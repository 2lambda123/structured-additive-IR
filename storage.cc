// Copyright 2020 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "storage.h"

#include "llvm/ADT/SmallString.h"
#include "loop_nest.h"

namespace sair {

Buffer::Buffer(mlir::Location loc, mlir::Type element_type,
               llvm::ArrayRef<mlir::StringAttr> loop_names,
               const LoopNest &loop_nest)
    : loc_(loc),
      element_type_(element_type),
      loop_nest_(loop_names.begin(), loop_names.end()) {
  assert(element_type != nullptr);

  // Prefix domain with loop nest domain.
  domain_.reserve(loop_nest.domain.size());
  int num_loops = loop_nest_.size();
  for (const ValueAccess &access : loop_nest.domain) {
    domain_.push_back(
        {access.value, access.mapping.ResizeUseDomain(num_loops)});
  }
}

Buffer::Buffer(FromToMemRefOp import_op,
               llvm::ArrayRef<mlir::StringAttr> loop_names,
               const LoopNest &loop_nest)
    : Buffer(import_op.getLoc(), import_op.MemRefType().getElementType(),
             loop_names, loop_nest) {
  import_op_ = import_op;
}

std::optional<int> Buffer::rank() const {
  return layout_.has_value() ? std::make_optional(layout_->size())
                             : std::nullopt;
}

void Buffer::SetLoopNest(const LoopNest &loop_nest) {
  int new_size = loop_nest.domain_to_loops.size();
  if (new_size == loop_nest_.size()) return;

  assert(new_size <= loop_nest_.size());
  loop_nest_.resize(new_size);
  if (domain_.empty()) return;

  // Compute dimensions to remove from the domain.
  llvm::SmallBitVector preserved_dims(domain_.size());
  preserved_dims.set(0, loop_nest.domain.size());
  if (layout_.has_value()) {
    preserved_dims |= layout_.value().DependencyMask();
  }

  // Trim domain from unused dimensions.
  mlir::MLIRContext *context = element_type_.getContext();
  auto none = MappingNoneExpr::get(context);
  llvm::SmallVector<MappingExpr> renaming(domain_.size(), none);

  llvm::SmallVector<ValueAccess> old_domain;
  std::swap(old_domain, domain_);
  for (int dim : preserved_dims.set_bits()) {
    // Already added to the new domain.
    if (renaming[dim].isa<MappingDimExpr>()) continue;
    renaming[dim] = MappingDimExpr::get(domain_.size(), context);
    domain_.push_back({
        .value = old_domain[dim].value,
        .mapping = old_domain[dim].mapping.ResizeUseDomain(new_size),
    });
  }

  if (layout_.has_value()) {
    auto renaming_mapping = MappingAttr::get(context, domain_.size(), renaming);
    layout_ = renaming_mapping.Compose(layout_.value());
  }
}

void Buffer::UnifyLayout(MappingAttr layout) {
  if (!layout_.has_value()) {
    layout_ = layout;
  } else {
    layout_ = layout_->Unify(layout);
  }
}

void Buffer::AddValue(mlir::Value value) {
  values_.push_back(value);
  auto defining_op = dyn_cast<ComputeOp>(value.getDefiningOp());
  if (defining_op != nullptr) {
    int position = value.cast<OpResult>().getResultNumber();
    writes_.emplace_back(defining_op, position);
  }
  for (mlir::OpOperand &use : value.getUses()) {
    auto user = dyn_cast<ComputeOp>(use.getOwner());
    if (user == nullptr) continue;
    ValueOperand sair_operand(&use);
    reads_.emplace_back(user, sair_operand.position());
  }
}

void Buffer::AddNonePrefixToLayout(int num_new_dims) {
  assert(layout_.has_value());
  assert(num_new_dims >= 0);
  mlir::MLIRContext *context = layout_->getContext();
  llvm::SmallVector<MappingExpr> prefix(num_new_dims,
                                        MappingNoneExpr::get(context));
  layout_ = layout_->AddPrefix(prefix);
}

void Buffer::AppendToDomain(llvm::ArrayRef<ValueAccess> new_values) {
  llvm::append_range(domain_, new_values);
  if (layout_.has_value()) {
    layout_ = layout_->ResizeUseDomain(domain_.size());
  }
}

MappingAttr BufferInstanceLayout(const Buffer &buffer,
                                 const LoopFusionAnalysis &fusion_analysis) {
  assert(buffer.layout().has_value());
  LoopNest loop_nest = fusion_analysis.GetLoopNest(buffer.loop_nest());
  return buffer.layout().value().AddPrefix(
      loop_nest.domain_to_loops.Dimensions());
}

StorageAnalysis::StorageAnalysis(mlir::Operation *operation)
    : StorageAnalysis(operation->getContext()) {
  mlir::LogicalResult result = Init(cast<SairProgramOp>(operation));
  assert(mlir::succeeded(result));
  (void)result;
}

std::optional<StorageAnalysis> StorageAnalysis::Create(SairProgramOp program) {
  StorageAnalysis analysis(program.getContext());
  if (mlir::failed(analysis.Init(program))) {
    return std::nullopt;
  }
  return analysis;
}

// Verifies that the storage attribute of the operation is well-formed:
// - that storage attributes are arrays of buffer or unit attributes,
// - that the number of entries in the storage array matches the number of,
//   results of the operation,
// - that indexes are not stored in memory,
// - that memory spaces referenced by the attribute exist,
// - that multi-dimensional buffers are not stored in registers,
// - that loops referenced by the attribute exist and
// - that the buffer has a name if and only if the memory space is addressable.
static mlir::LogicalResult VerifyStorageAttrWellFormed(ComputeOp op) {
  auto *sair_dialect = op.getContext()->getLoadedDialect<SairDialect>();

  llvm::Optional<mlir::ArrayAttr> storage_attr = op.storage();
  if (!op.storage().hasValue()) return mlir::success();
  llvm::ArrayRef<mlir::Attribute> storage = storage_attr.getValue().getValue();

  if (storage.size() != op->getNumResults()) {
    return op.emitError() << "wrong number of storage entries";
  }

  llvm::DenseSet<mlir::Attribute> loop_names;
  if (op.loop_nest().hasValue()) {
    for (mlir::Attribute attr : op.LoopNestLoops()) {
      LoopAttr loop = attr.cast<LoopAttr>();
      loop_names.insert(loop.name());
    }
  }

  llvm::DenseSet<mlir::Attribute> buffer_names;
  for (auto [attr, value] : llvm::zip(storage, op->getResults())) {
    if (attr.isa<UnitAttr>()) continue;
    BufferAttr buffer = attr.dyn_cast<BufferAttr>();
    if (buffer == nullptr) {
      return op.emitError() << "storage attribute must be an array of buffers "
                               "or unit attributes";
    }

    if (buffer.space() != sair_dialect->register_attr() &&
        buffer.space() != sair_dialect->memory_attr()) {
      return op.emitError() << "invalid memory space " << buffer.space();
    }

    ValueType type = value.getType().cast<ValueType>();
    if (buffer.space() == sair_dialect->memory_attr() &&
        (type.ElementType().isa<mlir::IndexType>() ||
         type.ElementType().isa<mlir::MemRefType>())) {
      return op.emitError()
             << "index and memref variables cannot be allocated in memory";
    }

    if ((buffer.space() == sair_dialect->memory_attr()) ^
        buffer.name() != nullptr) {
      return op.emitError() << "buffers must have a name if and only if they "
                               "are stored in memory";
    }

    if (buffer.name() != nullptr &&
        !buffer_names.insert(buffer.name()).second) {
      return op.emitError()
             << "operation cannot store two results in the same buffer";
    }

    if (buffer.layout() == nullptr) continue;

    if (buffer.layout().mapping().HasUnknownExprs()) {
      return op.emitError() << "layouts cannot contain `?` expressions";
    }

    if (buffer.space() == sair_dialect->register_attr() &&
        !buffer.layout().mapping().empty()) {
      return op.emitError() << "only 0D buffers can be stored in registers";
    }

    for (mlir::StringAttr loop_name : buffer.layout().names()) {
      if (!loop_names.contains(loop_name)) {
        return op.emitError() << "unknown loop name " << loop_name;
      }
    }
  }

  return mlir::success();
}

// Returns the layout of `buffer` as a mapping from the iteration space of
// `op` to buffer dimensions.
static MappingAttr GetBufferLayout(
    ComputeOp op, BufferAttr buffer,
    const IterationSpaceAnalysis &iteration_spaces) {
  if (buffer.layout() == nullptr) return nullptr;

  mlir::MLIRContext *context = op.getContext();
  auto none_expr = MappingNoneExpr::get(context);
  const IterationSpace &iter_space = iteration_spaces.Get(op.getOperation());
  MappingAttr mapping = buffer.layout().mapping();

  llvm::SmallVector<MappingExpr> loops_to_indexed_loops_exprs(
      mapping.UseDomainSize(), none_expr);
  for (auto p : llvm::enumerate(buffer.layout().names())) {
    auto it = llvm::find(iter_space.loop_names(), p.value());
    assert(it != iter_space.loop_names().end());
    int pos = std::distance(iter_space.loop_names().begin(), it);
    loops_to_indexed_loops_exprs[p.index()] = MappingDimExpr::get(pos, context);
  }

  auto loops_to_indexed_loops = MappingAttr::get(
      context, iter_space.mapping().size(), loops_to_indexed_loops_exprs);
  return loops_to_indexed_loops.Compose(mapping);
}

// Unifies the shape of `buffer` with the shape specified by attribute
// `buffer_attr` of `op`. Raises an error if shapes cannot be unified.
static mlir::LogicalResult UnifyBufferShape(
    mlir::StringAttr buffer_name, SairOp op, MappingAttr layout,
    const IterationSpace &op_iter_space,
    const LoopFusionAnalysis &loop_analysis, Buffer &buffer) {
  mlir::MLIRContext *context = op.getContext();
  auto none = MappingNoneExpr::get(context);

  LoopNest op_loop_nest = loop_analysis.GetLoopNest(op_iter_space.loop_names());
  LoopNest buffer_loop_nest = loop_analysis.GetLoopNest(buffer.loop_nest());

  // Creates a mapping that maps iter_space dimensions to op_loop_nest domain if
  // possible and op domain otherwise.
  int shift = op_loop_nest.domain.size();
  int concat_domain_size = shift + op.domain().size();

  llvm::SmallVector<MappingExpr> concat_exprs;
  concat_exprs.reserve(op_iter_space.mapping().size());
  llvm::append_range(concat_exprs, op_loop_nest.domain_to_loops);
  auto op_dimensions =
      op_iter_space.mapping().ShiftRight(shift).Dimensions().drop_front(
          op_loop_nest.domain_to_loops.size());
  llvm::append_range(concat_exprs, op_dimensions);
  auto concat_domains =
      MappingAttr::get(context, concat_domain_size, concat_exprs);
  MappingAttr concat_domains_to_layout =
      concat_domains.Compose(layout).Canonicalize();

  // Compute unification constraints. Dimensions used by the buffer loop nest
  // must be exactly the same for both uses.
  llvm::SmallVector<MappingExpr> constraints(concat_domain_size, none);
  for (int i = 0, e = buffer_loop_nest.domain.size(); i < e; ++i) {
    constraints[i] = MappingDimExpr::get(i, context);
  }
  if (buffer.layout().has_value()) {
    for (auto [old_expr, new_expr] :
         llvm::zip(buffer.layout().value(), concat_domains_to_layout)) {
      if (mlir::failed(
              UnificationConstraints(new_expr, old_expr, constraints))) {
        return op.emitError()
               << "buffer " << buffer_name
               << " layout is incompatible with previous occurences";
      }
    }
  }

  // Resolve constraints.
  std::string buffer_name_internal;
  llvm::raw_string_ostream buffer_name_str(buffer_name_internal);
  buffer_name_str << "buffer " << buffer_name;

  llvm::SmallBitVector indexed_dims = concat_domains_to_layout.DependencyMask();
  llvm::SmallVector<ValueAccess> new_domain;
  llvm::append_range(new_domain, buffer.domain());

  for (int dimension : indexed_dims.set_bits()) {
    ValueAccess dim_access;

    // Pick dimension from op_loop_nest domain or op domain.
    if (dimension < shift) {
      dim_access = op_loop_nest.domain[dimension];
    } else {
      dim_access.value = op.domain()[dimension - shift];
      MappingAttr dependency_mapping =
          op.shape().Dimension(dimension - shift).dependency_mapping();
      dim_access.mapping = op_iter_space.mapping().Inverse().Compose(
          dependency_mapping.ResizeUseDomain(op.domain().size()));
    }

    // Make sure that the dimension only depends on loop that are in the buffer
    // loop nest.
    dim_access.mapping =
        dim_access.mapping.ResizeUseDomain(buffer.loop_nest().size());
    if (mlir::failed(ResolveUnificationConstraint(
            op.getLoc(), buffer_name_str.str(), dim_access,
            constraints[dimension], new_domain))) {
      return mlir::failure();
    }
  }

  buffer.AppendToDomain(
      llvm::makeArrayRef(new_domain).drop_front(buffer.domain().size()));

  // Unify dimensions.
  auto renaming =
      MappingAttr::get(context, buffer.domain().size(), constraints);
  buffer.UnifyLayout(renaming.Compose(concat_domains_to_layout));

  return mlir::success();
}

// Trims `buffer` loop nest so that it can be accessed from the given iteration
// space, with the given layout. Layout is ignored if null.
static void TrimBufferLoopNestForAccess(
    const IterationSpace &iter_space, MappingAttr layout,
    const LoopFusionAnalysis &fusion_analysis, Buffer &buffer) {
  // Trims the buffer loop nest so that only common loops that are not indexed
  // by the layout remain.
  int max_loop_nest = iter_space.NumCommonLoops(buffer.loop_nest());
  if (layout != nullptr) {
    llvm::SmallBitVector indexed_loops = layout.DependencyMask();
    int first_indexed_loop = indexed_loops.find_first();
    if (first_indexed_loop >= 0 && first_indexed_loop < max_loop_nest) {
      max_loop_nest = first_indexed_loop;
    }
  }

  LoopNest new_loop_nest = fusion_analysis.GetLoopNest(
      iter_space.loop_names().take_front(max_loop_nest));
  buffer.SetLoopNest(new_loop_nest);
}

// Declares buffer `attr` in `buffer_map`. If the
// buffer is already present, ensure that rank and element type are coherent and
// trims the buffer loop nest to the common prefix with `op` loop nest.
static mlir::LogicalResult DeclareBuffer(
    ComputeOp op, int result, BufferAttr attr,
    const LoopFusionAnalysis &loop_analysis,
    const IterationSpaceAnalysis &iteration_spaces,
    llvm::DenseMap<mlir::Attribute, Buffer> &buffer_map) {
  if (attr == nullptr || attr.name() == nullptr) return mlir::success();
  mlir::Type element_type =
      op->getResult(result).getType().cast<ValueType>().ElementType();
  auto sair_op = cast<SairOp>(op.getOperation());
  const IterationSpace &iter_space = iteration_spaces.Get(sair_op);
  const LoopNest &loop_nest =
      loop_analysis.GetLoopNest(iter_space.loop_names());
  auto it = buffer_map.try_emplace(attr.name(), op.getLoc(), element_type,
                                   iter_space.loop_names(), loop_nest);
  Buffer &buffer = it.first->second;

  // Check that element types match.
  if (buffer.element_type() != element_type) {
    mlir::InFlightDiagnostic diag =
        op.emitError()
        << "buffer " << attr.name()
        << " has different element type than in previous occurence";
    diag.attachNote(buffer.getLoc()) << "previous occurence here";
    return mlir::failure();
  }

  // Ensure that the number of dimension is coherent.
  MappingAttr layout = GetBufferLayout(op, attr, iteration_spaces);
  if (buffer.rank().has_value() && layout != nullptr &&
      buffer.rank() != layout.size()) {
    mlir::InFlightDiagnostic diag = op.emitError()
                                    << "buffer " << attr.name()
                                    << " rank differs from previous occurence";
    diag.attachNote(buffer.getLoc()) << "previous occurence here";
    return mlir::failure();
  }

  TrimBufferLoopNestForAccess(iter_space, layout, loop_analysis, buffer);

  // Unify layouts.
  if (layout == nullptr) return mlir::success();
  return UnifyBufferShape(attr.name(), sair_op, layout, iter_space,
                          loop_analysis, buffer);
}

// Declare buffers used by `program` in `buffers`. If a buffer has multiple
// uses, chek that element type and rank are compatible.
static mlir::LogicalResult DeclareBuffers(
    SairProgramOp program, const IterationSpaceAnalysis &iteration_spaces,
    const LoopFusionAnalysis &fusion_analysis,
    llvm::DenseMap<mlir::Attribute, Buffer> &buffers) {
  mlir::MLIRContext *context = program.getContext();

  // Declare external buffers imported using from/to memref.
  mlir::WalkResult result =
      program.walk([&](FromToMemRefOp op) -> mlir::WalkResult {
        auto sair_op = cast<SairOp>(op.getOperation());
        auto name = mlir::StringAttr::get(op.getContext(), op.buffer_name());
        const IterationSpace &iter_space = iteration_spaces.Get(sair_op);
        const LoopNest &loop_nest =
            fusion_analysis.GetLoopNest(iter_space.loop_names());
        auto [buffer_it, was_inserted] =
            buffers.try_emplace(name, op, iter_space.loop_names(), loop_nest);
        if (!was_inserted)
          return op.emitError() << "buffer name is already used";

        int rank = op.memref_domain().size();
        int parallel_domain_size = op.parallel_domain().size();
        auto domain_to_layout = MappingAttr::GetIdentity(context, rank)
                                    .ShiftRight(parallel_domain_size);
        MappingAttr layout =
            iter_space.mapping().Inverse().Compose(domain_to_layout);

        return UnifyBufferShape(name, sair_op, layout, iter_space,
                                fusion_analysis, buffer_it->second);
      });
  if (result.wasInterrupted()) return mlir::failure();

  // Declare internal buffers.
  result = program.walk([&](ComputeOp op) -> mlir::WalkResult {
    for (int i = 0, e = op->getNumResults(); i < e; ++i) {
      BufferAttr buffer_attr = op.Storage(i);
      if (mlir::failed(DeclareBuffer(op, i, buffer_attr, fusion_analysis,
                                     iteration_spaces, buffers))) {
        return mlir::failure();
      }
    }
    return mlir::success();
  });
  if (result.wasInterrupted()) return mlir::failure();

  // Ensure all buffers layout is fully specified.
  for (auto [name, buffer] : buffers) {
    if (!buffer.layout().has_value()) continue;
    if (buffer.layout()->HasNoneExprs()) {
      return mlir::emitError(buffer.getLoc())
             << "buffer " << name << " layout is not fully specified";
    }
  }

  return mlir::failure(result.wasInterrupted());
}

// Computes how values are stored and stores the result into `value_storages`.
mlir::LogicalResult StorageAnalysis::ComputeValueStorages(
    SairProgramOp program, const LoopFusionAnalysis &fusion_analysis,
    const IterationSpaceAnalysis &iteration_spaces) {
  mlir::MLIRContext *context = program.getContext();
  auto *sair_dialect = context->getLoadedDialect<SairDialect>();
  mlir::StringAttr memory_space = sair_dialect->memory_attr();

  // Initialize storage information from compute operations.
  auto result = program.walk([&](ComputeOp op) -> mlir::WalkResult {
    for (int i = 0, e = op->getNumResults(); i < e; ++i) {
      BufferAttr buffer = op.Storage(i);
      if (buffer == nullptr) continue;
      MappingAttr layout = GetBufferLayout(op, buffer, iteration_spaces);
      ValueStorage storage(buffer.space(), buffer.name(), layout);
      if (mlir::failed(SetStorage(op->getResult(i), storage, fusion_analysis,
                                  iteration_spaces))) {
        return mlir::failure();
      }
    }
    return mlir::success();
  });
  if (result.wasInterrupted()) return mlir::failure();

  // Initialize from from_memref operations.
  result = program.walk([&](SairFromMemRefOp op) -> mlir::WalkResult {
    const IterationSpace &iter_space = iteration_spaces.Get(op);
    MappingAttr layout = iter_space.mapping().Inverse().Compose(op.Layout());
    ValueStorage storage(memory_space, op.buffer_nameAttr(), layout);
    return SetStorage(op.result(), storage, fusion_analysis, iteration_spaces);
  });
  if (result.wasInterrupted()) return mlir::failure();

  // Initialize from from_scalar operations.
  result = program.walk([&](SairFromScalarOp op) -> mlir::WalkResult {
    auto layout = MappingAttr::get(context, 0, {});
    ValueStorage storage(sair_dialect->register_attr(), nullptr, layout);
    return SetStorage(op.result(), storage, fusion_analysis, iteration_spaces);
  });
  if (result.wasInterrupted()) return mlir::failure();

  // Initialize from to_memref operations.
  result = program.walk([&](SairToMemRefOp op) -> mlir::WalkResult {
    const IterationSpace &iter_space = iteration_spaces.Get(op);
    MappingAttr layout = iter_space.mapping().Inverse().Compose(op.Layout());
    ValueStorage operand_storage(memory_space, op.buffer_nameAttr(), layout);
    mlir::Operation *defining_op = op.value().getDefiningOp();
    ValueStorage storage = operand_storage.Map(
        op, defining_op, op.Value().Mapping().Inverse(), iteration_spaces);
    return SetStorage(op.value(), storage, fusion_analysis, iteration_spaces);
  });
  if (result.wasInterrupted()) return mlir::failure();

  // Ensure all sair values have an entry.
  program.walk([&](SairOp op) {
    for (mlir::Value result : op->getResults()) {
      value_storages_.FindAndConstruct(result);
    }
  });

  return mlir::success();
}

mlir::LogicalResult StorageAnalysis::Init(SairProgramOp program) {
  // TODO(b/181938550): use cached analysis.
  LoopFusionAnalysis fusion_analysis(program);
  IterationSpaceAnalysis iteration_spaces(program);

  if (mlir::failed(DeclareBuffers(program, iteration_spaces, fusion_analysis,
                                  buffers_))) {
    return mlir::failure();
  }

  if (mlir::failed(
          ComputeValueStorages(program, fusion_analysis, iteration_spaces))) {
    return mlir::failure();
  }

  if (mlir::failed(VerifyAndMinimizeBufferLoopNests(fusion_analysis,
                                                    iteration_spaces))) {
    return mlir::failure();
  }

  // Ensure that writes to external buffers occure after the buffer is defined.
  for (auto &[name, buffer] : buffers_) {
    if (!buffer.is_external()) continue;
    mlir::Operation *defining_op =
        buffer.import_op().MemRef().value().getDefiningOp();
    // We only need to check writes as reads always occure after writes.
    for (auto write : buffer.writes()) {
      if (write.first->isBeforeInBlock(defining_op)) {
        mlir::InFlightDiagnostic diag = write.first.emitError()
                                        << "buffer " << name
                                        << " used before it is defined";
        diag.attachNote(defining_op->getLoc()) << "buffer defined here";
        return mlir::failure();
      }
    }
  }

  return mlir::success();
}

void StorageAnalysis::MergeStorage(
    mlir::Value value, const ValueStorage &new_storage,
    const LoopFusionAnalysis &fusion_analysis,
    const IterationSpaceAnalysis &iteration_spaces) {
  if (new_storage.buffer_name() != nullptr && new_storage.layout() != nullptr) {
    Buffer &buffer = buffers_.find(new_storage.buffer_name())->second;
    // Make sure that layout has the correct rank and initialize buffer layout
    // if needed.
    if (buffer.rank().has_value()) {
      assert(buffer.rank() == new_storage.layout().size());
    } else {
      assert(new_storage.layout().empty());
      auto empty_layout =
          MappingAttr::get(context_, buffer.domain().size(), {});
      buffer.UnifyLayout(empty_layout);
    }
  }
  AssertSuccess(
      SetStorage(value, new_storage, fusion_analysis, iteration_spaces));
}

mlir::StringAttr StorageAnalysis::GetFreshBufferName() {
  llvm::SmallString<10> name("buffer_");
  int original_size = name.size();
  mlir::StringAttr attr;
  do {
    name.resize(original_size);
    name += std::to_string(next_buffer_id_++);
    attr = mlir::StringAttr::get(context_, name);
  } while (buffers_.count(attr) > 0);
  return attr;
}

void StorageAnalysis::AddDimensionsToBuffer(
    mlir::StringAttr buffer_name, SairOp op,
    const IterationSpace &op_iter_space,
    const LoopFusionAnalysis &fusion_analysis, MappingAttr new_layout) {
  Buffer &buffer = buffers_.find(buffer_name)->second;
  assert(new_layout != nullptr);
  assert(buffer.layout().has_value());
  assert(new_layout.size() >= buffer.layout()->size());
  assert(!buffer.is_external());

  // Extend buffer domain.
  TrimBufferLoopNestForAccess(op_iter_space, new_layout, fusion_analysis,
                              buffer);
  int old_size = *buffer.rank();
  buffer.AddNonePrefixToLayout(new_layout.size() - old_size);
  AssertSuccess(UnifyBufferShape(buffer_name, op, new_layout, op_iter_space,
                                 fusion_analysis, buffer));

  // Add a dimension to values layout.
  for (mlir::Value value : buffer.values()) {
    ValueStorage &storage = value_storages_.find(value)->second;
    storage.AddUnknownPrefixToLayout(new_layout.size() - old_size);
  }
}

// Update the storage information for value. Updates buffers to register new
// buffer uses.
static mlir::LogicalResult UpdateStorage(
    mlir::Value value, const ValueStorage &new_storage,
    const LoopFusionAnalysis &fusion_analysis,
    const IterationSpaceAnalysis &iteration_spaces, ValueStorage &storage,
    llvm::DenseMap<mlir::Attribute, Buffer> &buffers) {
  if (storage.buffer_name() == nullptr &&
      new_storage.buffer_name() != nullptr) {
    Buffer &buffer = buffers.find(new_storage.buffer_name())->second;
    buffer.AddValue(value);
    // Trim buffer loop nest to ensure it can be used from value def and uses
    // iteration spaces.
    SairOp defining_op = value.getDefiningOp();
    assert(defining_op != nullptr);
    TrimBufferLoopNestForAccess(iteration_spaces.Get(defining_op), nullptr,
                                fusion_analysis, buffer);
    for (mlir::Operation *user : value.getUsers()) {
      TrimBufferLoopNestForAccess(iteration_spaces.Get(user), nullptr,
                                  fusion_analysis, buffer);
    }
  }

  if (mlir::failed(storage.MergeSpace(new_storage.space()))) {
    return value.getDefiningOp()->emitError()
           << "conflicting memory spaces: expected " << new_storage.space()
           << ", got " << storage.space();
  }
  if (mlir::failed(storage.MergeBufferName(new_storage.buffer_name()))) {
    return value.getDefiningOp()->emitError()
           << "conflicting buffer names: expected " << new_storage.buffer_name()
           << ", got " << storage.buffer_name();
  }
  if (mlir::failed(storage.MergeLayout(new_storage.layout()))) {
    return value.getDefiningOp()->emitError()
           << "conflicting layouts: expected " << new_storage.layout()
           << ", got " << storage.layout();
  }

  return mlir::success();
}

mlir::LogicalResult StorageAnalysis::SetStorage(
    mlir::Value value, ValueStorage storage,
    const LoopFusionAnalysis &fusion_analysis,
    const IterationSpaceAnalysis &iteration_spaces) {
  llvm::SmallVector<mlir::Value> work_list;

  // Merge storage information for a value with existing information. Fails and
  // emits an error in case of conflicts.
  auto update_storage = [&](mlir::Value value,
                            ValueStorage new_storage) -> mlir::LogicalResult {
    ValueStorage &storage = value_storages_[value];
    if (new_storage == storage) return mlir::success();

    work_list.push_back(value);
    return UpdateStorage(value, new_storage, fusion_analysis, iteration_spaces,
                         storage, buffers_);
  };

  if (mlir::failed(update_storage(value, storage))) return mlir::failure();

  // Propagate storage information.
  while (!work_list.empty()) {
    mlir::Value value = work_list.pop_back_val();
    ValueStorage storage = value_storages_[value];

    // Forward propagation.
    for (mlir::OpOperand &mlir_operand : value.getUses()) {
      mlir::Operation *user = mlir_operand.getOwner();
      ValueOperand operand(&mlir_operand);
      int result;
      if (isa<SairProjAnyOp, SairProjLastOp, SairFbyOp>(user)) {
        result = 0;
      } else if (auto map_reduce = dyn_cast<SairMapReduceOp>(user)) {
        if (operand.position() >= map_reduce.Inits().size()) continue;
        result = operand.position();
      } else {
        continue;
      }
      ValueStorage new_storage = storage.Map(operand, iteration_spaces);
      if (mlir::failed(update_storage(user->getResult(result), new_storage))) {
        return mlir::failure();
      }
    }

    // Backward propagation.
    mlir::Operation *defining_op = value.getDefiningOp();

    // Handle map-reduce separately.
    if (auto map_reduce = dyn_cast<SairMapReduceOp>(defining_op)) {
      int pos = value.cast<OpResult>().getResultNumber();
      ValueOperand operand = map_reduce.Inits()[pos];
      ValueStorage new_storage =
          storage.Map(defining_op, operand.value().getDefiningOp(),
                      operand.Mapping().Inverse(), iteration_spaces);
      if (mlir::failed(update_storage(operand.value(), new_storage))) {
        return mlir::failure();
      }
      continue;
    }

    if (!isa<SairProjAnyOp, SairProjLastOp, SairFbyOp>(defining_op)) continue;
    for (ValueOperand operand : cast<SairOp>(defining_op).ValueOperands()) {
      ValueStorage new_storage =
          storage.Map(defining_op, operand.value().getDefiningOp(),
                      operand.Mapping().Inverse(), iteration_spaces);
      if (mlir::failed(update_storage(operand.value(), new_storage))) {
        return mlir::failure();
      }
    }
  }

  return mlir::success();
}

// Ensures that we can insert a malloc operation for the buffer. Increases
// `min_num_loops` to make sure that a malloc operation can be inserted if
// needed.
static mlir::LogicalResult CheckMallocInsertionPoint(
    mlir::StringAttr buffer_name, const Buffer &buffer,
    const llvm::SmallBitVector &used_dimensions,
    const IterationSpaceAnalysis &iteration_spaces, int &min_num_loops) {
  // Find the first compute op writting to the buffer.
  ComputeOp first_write = buffer.writes().front().first;
  for (auto p : buffer.writes()) {
    if (p.first->isBeforeInBlock(first_write)) {
      first_write = p.first;
    }
  }

  llvm::ArrayRef<mlir::StringAttr> write_loops =
      iteration_spaces.Get(cast<SairOp>(first_write.getOperation()))
          .loop_names();
  for (int dim : used_dimensions.set_bits()) {
    auto dimension_op =
        cast<SairOp>(buffer.domain()[dim].value.getDefiningOp());
    if (first_write->isBeforeInBlock(dimension_op)) {
      mlir::InFlightDiagnostic diag =
          first_write.emitError()
          << "buffer " << buffer_name
          << " is used before one of its dimensions is defined";
      diag.attachNote(dimension_op.getLoc()) << "dimension defined here";
      return mlir::failure();
    }

    for (ValueOperand operand : dimension_op.ValueOperands()) {
      auto defining_op = cast<SairOp>(operand.value().getDefiningOp());
      llvm::ArrayRef<mlir::StringAttr> operand_loops =
          iteration_spaces.Get(defining_op).loop_names();
      int new_min = std::min(write_loops.size(), operand_loops.size());
      for (; new_min > 0; --new_min) {
        if (operand_loops[new_min - 1] == write_loops[new_min - 1]) break;
      }

      // TODO(b/170195606): this check is not enough if other operations are
      // present between the dimension definition and its arguments.
      if (new_min > buffer.loop_nest().size()) {
        mlir::InFlightDiagnostic diag =
            first_write.emitError()
            << "buffer " << buffer_name
            << " depends on a dimension that is defined after the buffer "
               "is allocated";
        diag.attachNote(dimension_op.getLoc()) << "dimension defined here";
        return mlir::failure();
      }

      min_num_loops = std::max(min_num_loops, new_min);
    }
  }
  return mlir::success();
}

mlir::LogicalResult StorageAnalysis::VerifyAndMinimizeBufferLoopNests(
    const LoopFusionAnalysis &fusion_analysis,
    const IterationSpaceAnalysis &iteration_spaces) {
  for (auto &[name_attr, buffer] : buffers_) {
    mlir::StringAttr name = name_attr.cast<mlir::StringAttr>();
    if (!buffer.layout().has_value()) continue;

    int min_num_loops = 0;

    // Update `min_num_loops` based on domain dimensions layout depends on.
    llvm::SmallBitVector used_dimensions = buffer.layout()->DependencyMask();
    for (int dim : used_dimensions.set_bits()) {
      MappingAttr dim_mapping = buffer.domain()[dim].mapping;
      if (dim_mapping.HasNoneExprs()) {
        return mlir::emitError(buffer.getLoc())
               << "buffer " << name
               << " layout depends on loops it cannot be nested in";
      }

      int new_min = buffer.domain()[dim].mapping.MinDomainSize();
      min_num_loops = std::max(new_min, min_num_loops);
    }

    // Update `min_num_loop` to account for dependencies accross layout and
    // loop-nest dimensions.
    MappingAttr mapping = BufferInstanceLayout(buffer, fusion_analysis);
    auto hr_shape =
        DomainShapeAttr::HyperRectangular(context_, buffer.domain().size());
    MappingAttr inverse = mapping.Inverse();
    for (MappingExpr layout_expr : buffer.layout()->Dimensions()) {
      DomainShapeDim shape_dim =
          layout_expr.AccessedShape(hr_shape.Dimensions(), inverse);
      int new_min = shape_dim.dependency_mapping().MinDomainSize();
      if (new_min > buffer.loop_nest().size()) {
        return mlir::emitError(buffer.getLoc())
               << "buffer " << name
               << " layout depends on loops it cannot be nested in";
      }
      min_num_loops = std::max(new_min, min_num_loops);
    }

    // We cannot minimize external buffers loop nests.
    if (buffer.is_external()) continue;

    if (mlir::failed(CheckMallocInsertionPoint(
            name, buffer, used_dimensions, iteration_spaces, min_num_loops))) {
      return mlir::failure();
    }

    // Minimize layout loop-nest.
    LoopNest new_loop_nest = fusion_analysis.GetLoopNest(
        buffer.loop_nest().take_front(min_num_loops));
    buffer.SetLoopNest(new_loop_nest);
  }

  return mlir::success();
}

void StorageAnalysis::CreateBuffer(
    mlir::Value value, llvm::ArrayRef<mlir::StringAttr> loop_names,
    const LoopFusionAnalysis &fusion_analysis,
    const IterationSpaceAnalysis &iteration_spaces) {
  mlir::StringAttr buffer_name = GetFreshBufferName();
  mlir::Type element_type = value.getType().cast<ValueType>().ElementType();
  LoopNest loop_nest = fusion_analysis.GetLoopNest(loop_names);
  buffers_.try_emplace(buffer_name, value.getLoc(), element_type, loop_names,
                       loop_nest);

  mlir::MLIRContext *context = value.getContext();
  auto *sair_dialect = context->getLoadedDialect<SairDialect>();

  ValueStorage storage = GetStorage(value);
  AssertSuccess(storage.MergeBufferName(buffer_name));
  AssertSuccess(storage.MergeSpace(sair_dialect->memory_attr()));
  MergeStorage(value, storage, fusion_analysis, iteration_spaces);
}

// Ensures that communication between the producer and the user of operand only
// occurs within the same loop iteration or along dimensions that are
// materialized in memory.
static mlir::LogicalResult VerifyCommunicationVolume(
    mlir::Location loc, const IterationSpace &use_iter_space,
    const ValueAccess &operand, const IterationSpaceAnalysis &iteration_spaces,
    const StorageAnalysis &storage_analysis) {
  const IterationSpace &def_iter_space =
      iteration_spaces.Get(operand.value.getDefiningOp());
  // Only check if loop nest are specified.
  if (!use_iter_space.fully_specified() || !def_iter_space.fully_specified()) {
    return mlir::success();
  }

  const ValueStorage &storage = storage_analysis.GetStorage(operand.value);
  // Success if storage is not yet specified.
  if (storage.layout() == nullptr) return mlir::success();

  MappingAttr communication_volume = CommunicationVolume(
      operand.mapping.size(), def_iter_space, use_iter_space);
  MappingAttr layout_to_operand =
      def_iter_space.mapping().Compose(storage.layout()).Inverse();
  MappingAttr layout_to_communication_volume =
      layout_to_operand.Compose(communication_volume).Canonicalize();

  // Check that the layout covers the sub-domain of the operand that is not
  // covered by common dimensions.
  if (layout_to_communication_volume.HasNoneExprs()) {
    mlir::InFlightDiagnostic diag =
        mlir::emitError(loc)
        << "operand storage must cover all operand dimensions "
           "that are not covered by loops common to both operand and user";
    diag.attachNote(operand.value.getDefiningOp()->getLoc())
        << "operand defined here";
    return mlir::failure();
  }

  return mlir::success();
}

// Ensures that communication between producers and users only occurs within the
// same loop iteration or along dimensions that are materialized in memory.
static mlir::LogicalResult VerifyCommunicationVolume(
    SairProgramOp program, const IterationSpaceAnalysis &iteration_spaces,
    const StorageAnalysis &storage_analysis) {
  // Ensure that values storage have enough dimensions.
  auto result = program.walk([&](SairOp op) -> mlir::WalkResult {
    const IterationSpace iter_space = iteration_spaces.Get(op);
    // Check dependencies for value operands.
    for (ValueOperand operand : op.ValueOperands()) {
      if (mlir::failed(
              VerifyCommunicationVolume(op.getLoc(), iter_space, operand.Get(),
                                        iteration_spaces, storage_analysis))) {
        return mlir::failure();
      }
    }
    // Check dependencies for domain dimensions.
    int domain_size = op.domain().size();
    for (int i = 0; i < domain_size; ++i) {
      auto dim_op = cast<SairOp>(op.domain()[i].getDefiningOp());
      const DomainShapeDim &shape_dim = op.shape().Dimension(i);
      MappingAttr dim_mapping =
          shape_dim.dependency_mapping().ResizeUseDomain(domain_size);
      for (ValueOperand operand : dim_op.ValueOperands()) {
        ValueAccess access = operand.Get();
        access.mapping = dim_mapping.Compose(access.mapping);
        if (mlir::failed(VerifyCommunicationVolume(
                op.getLoc(), iter_space, operand.Get(), iteration_spaces,
                storage_analysis))) {
          return mlir::failure();
        }
      }
    }
    return mlir::success();
  });
  return mlir::failure(result.wasInterrupted());
}

mlir::LogicalResult VerifyStorages(
    SairProgramOp program, const IterationSpaceAnalysis &iteration_spaces) {
  // Check storage attributes are well-formed.
  mlir::WalkResult result = program.walk([](ComputeOp op) -> mlir::WalkResult {
    return VerifyStorageAttrWellFormed(op);
  });
  if (result.wasInterrupted()) return mlir::failure();

  // Ensure storage attributes are compatibles with each other.
  auto analysis_result = StorageAnalysis::Create(program);
  if (!analysis_result.has_value()) return mlir::failure();
  StorageAnalysis analysis = std::move(analysis_result).value();

  // Ensure that operation updating a buffers in place use the same layout for
  // both inputs and outputs.
  result = program.walk([&](ComputeOp op) -> mlir::WalkResult {
    for (mlir::Value result : op->getResults()) {
      const ValueStorage &result_storage = analysis.GetStorage(result);
      if (result_storage.buffer_name() == nullptr) continue;
      auto sair_op = cast<SairOp>(op.getOperation());
      for (ValueOperand operand : sair_op.ValueOperands()) {
        const ValueStorage &operand_storage =
            analysis.GetStorage(operand.value());
        if (operand_storage.buffer_name() != result_storage.buffer_name()) {
          continue;
        }
        ValueStorage mapped_storage =
            operand_storage.Map(operand, iteration_spaces);
        if (mapped_storage.layout() != result_storage.layout()) {
          return op.emitError()
                 << "in-place update of buffer " << result_storage.buffer_name()
                 << " must use the same layout in input and output ("
                 << mapped_storage.layout() << " vs " << result_storage.layout()
                 << ")";
        }
      }
    }
    return mlir::success();
  });
  if (result.wasInterrupted()) return mlir::failure();

  // TODO(b/174127497): make sure that value is not ovewritten by another write.
  return VerifyCommunicationVolume(program, iteration_spaces, analysis);
}

BufferAttr GetRegister0DBuffer(mlir::MLIRContext *context) {
  auto *sair_dialect = context->getLoadedDialect<SairDialect>();
  return BufferAttr::get(/*space=*/sair_dialect->register_attr(),
                         /*name=*/nullptr,
                         /*layout=*/NamedMappingAttr::GetIdentity(context, {}),
                         context);
}

bool operator==(const ValueStorage &lhs, const ValueStorage &rhs) {
  return lhs.space() == rhs.space() && lhs.buffer_name() == rhs.buffer_name() &&
         lhs.layout() == rhs.layout();
}

bool operator!=(const ValueStorage &lhs, const ValueStorage &rhs) {
  return !(lhs == rhs);
}

mlir::LogicalResult ValueStorage::MergeSpace(mlir::StringAttr new_space) {
  if (new_space == nullptr) return mlir::success();
  if (space_ == nullptr) space_ = new_space;
  return mlir::success(space_ == new_space);
}

mlir::LogicalResult ValueStorage::MergeBufferName(mlir::StringAttr new_name) {
  if (new_name == nullptr) return mlir::success();
  if (buffer_name_ == nullptr) buffer_name_ = new_name;
  return mlir::success(buffer_name_ == new_name);
}

mlir::LogicalResult ValueStorage::MergeLayout(MappingAttr new_layout) {
  if (new_layout == nullptr) return mlir::success();
  if (layout_ == nullptr) {
    layout_ = new_layout;
    return mlir::success();
  }

  new_layout = new_layout.UnifyUnknownExprs(layout_);
  if (new_layout == nullptr) return mlir::failure();
  layout_ = new_layout;
  return mlir::success();
}

ValueStorage ValueStorage::Map(
    const ValueOperand &operand,
    const IterationSpaceAnalysis &iteration_spaces) const {
  return Map(operand.value().getDefiningOp(), operand.getOwner(),
             operand.Mapping(), iteration_spaces);
}

ValueStorage ValueStorage::Map(
    SairOp from, SairOp to, MappingAttr mapping,
    const IterationSpaceAnalysis &iteration_spaces) const {
  MappingAttr layout;
  if (layout_ != nullptr) {
    // We need to resize mapping to match operations domain size as values may
    // have a smaller rank than the operations that creates them.
    MappingAttr domain_mapping = mapping.Resize(from.domain().size())
                                     .ResizeUseDomain(to.domain().size());
    MappingAttr iter_space_mapping =
        iteration_spaces.TranslateMapping(to, from, domain_mapping);
    assert(iter_space_mapping != nullptr);
    layout = iter_space_mapping.Compose(layout_).Canonicalize();
  }
  return ValueStorage(space_, buffer_name_, layout);
}

void ValueStorage::AddUnknownPrefixToLayout(int num_new_dims) {
  assert(layout_ != nullptr);
  assert(num_new_dims >= 0);
  mlir::MLIRContext *context = layout_.getContext();
  llvm::SmallVector<MappingExpr> prefix(num_new_dims,
                                        MappingUnknownExpr::get(context));
  layout_ = layout_.AddPrefix(prefix);
}

MappingAttr CommunicationVolume(int value_rank,
                                const IterationSpace &def_iter_space,
                                const IterationSpace &use_iter_space) {
  int num_common_loops = def_iter_space.NumCommonLoops(use_iter_space);

  // Mapping from the domain of the operand to common loops.
  MappingAttr domain_to_common_loops = def_iter_space.mapping()
                                           .ResizeUseDomain(value_rank)
                                           .Resize(num_common_loops);
  // Extend `domain_to_common_loops` to cover the full operand domain then drop
  // common loops. This gives a mapping that only covers the sub-domain of the
  // operand that is not covered by common loops.
  return domain_to_common_loops.Inverse().MakeSurjective().Inverse().DropFront(
      num_common_loops);
}

}  // namespace sair
