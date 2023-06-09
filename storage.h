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

#ifndef SAIR_STORAGE_H_
#define SAIR_STORAGE_H_

#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/Operation.h"
#include "loop_nest.h"
#include "mapped_domain.h"
#include "sair_op_interfaces.h"
#include "sair_ops.h"
#include "sequence.h"

namespace sair {

class SairDialect;

// Verifies that storage attributes in the program are correct. Assumes that
// Sair operands are defined in the same program.
mlir::LogicalResult VerifyStorages(
    SairProgramOp program, const LoopFusionAnalysis &fusion_analysis,
    const IterationSpaceAnalysis &iteration_spaces,
    const SequenceAnalysis &sequence_analysis);

// Returns the buffer attribute representing a 0-dimensional register.
BufferAttr GetRegister0DBuffer(mlir::MLIRContext *context);

// A buffer declared by one or more storage attributes.
class Buffer : public MappedDomain {
 public:
  // Create a new buffer written to by the given operation.
  Buffer(mlir::Location loc, mlir::StringAttr name, mlir::Type element_type,
         const LoopNest &loop_nest);
  Buffer(FromToMemRefOp import_op, mlir::StringAttr name,
         const LoopNest &loop_nest);

  // Number of dimensions in the buffer layout.
  int rank() const { return mapping().size(); }

  // Types of the scalars stored in the buffer.
  mlir::Type element_type() const { return element_type_; }

  // Indicates if the buffer is declared outside the Sair program.
  bool is_external() const { return import_op_ != nullptr; }

  // In the case where `is_external` is true, operation that imports the memref
  // in the sair program.
  FromToMemRefOp import_op() const { return import_op_; }

  // List of operations that write to the buffer, with the position of the
  // result stored in the buffer. Non-external buffers must have at least one
  // write.
  llvm::ArrayRef<std::pair<ComputeOpInstance, int>> writes() const {
    return writes_;
  }

  // List of operations that read from the buffer, with the position of the Sair
  // value operand.
  llvm::ArrayRef<std::pair<ComputeOpInstance, int>> reads() const {
    return reads_;
  }

  // List of values stored in the buffer.
  llvm::ArrayRef<ResultInstance> values() const { return values_; }

  // Registers a value stored in the buffer.
  void AddValue(ResultInstance value);

 private:
  mlir::Type element_type_;
  FromToMemRefOp import_op_ = nullptr;

  llvm::SmallVector<std::pair<ComputeOpInstance, int>> writes_;
  llvm::SmallVector<std::pair<ComputeOpInstance, int>> reads_;
  llvm::SmallVector<ResultInstance> values_;
};

// Describes how a value is stored. Attributes may be null if the buffer is not
// yet specified. Merge* methods replace null attributes by a new value or
// verify that the new value is the same as the existing one if both old and
// new values are not null.
class ValueStorage {
 public:
  ValueStorage() {}
  ValueStorage(mlir::StringAttr space, mlir::StringAttr buffer_name,
               MappingAttr layout)
      : space_(space), buffer_name_(buffer_name), layout_(layout) {}

  // Memory space the value is stored in. May be null if not yet specified.
  mlir::StringAttr space() const { return space_; }
  mlir::LogicalResult MergeSpace(mlir::StringAttr new_space);

  // Name of the buffer where the value is stored, if specified.
  mlir::StringAttr buffer_name() const { return buffer_name_; }
  mlir::LogicalResult MergeBufferName(mlir::StringAttr new_name);

  // Mapping from the iteration space of the value to buffer dimensions.
  // MergeLayout unifies layout by substituting `?` expressions only.
  MappingAttr layout() const { return layout_; }
  mlir::LogicalResult MergeLayout(MappingAttr new_layout);

  // Adds a dimension at the front of the layout. Fills the dimension with the
  // `?` mapping expression.
  void AddUnknownPrefixToLayout(int num_new_dims);

  // Converts a value storage from the domain of the value to the domain of the
  // operand. Returns nullopt if the operand value is not yet specified.
  std::optional<ValueStorage> Map(
      const OperandInstance &operand,
      const IterationSpaceAnalysis &iteration_spaces) const;
  // Converts a value storage from the domain of `from` to the domain of `to`
  // given a mapping from the domain of `to` to the domain of `from`.
  ValueStorage Map(const OpInstance &from, const OpInstance &to,
                   MappingAttr mapping,
                   const IterationSpaceAnalysis &iteration_spaces) const;

 private:
  mlir::StringAttr space_;
  mlir::StringAttr buffer_name_;
  MappingAttr layout_;
};

bool operator==(const ValueStorage &lhs, const ValueStorage &rhs);
bool operator!=(const ValueStorage &lhs, const ValueStorage &rhs);

// Returns a mapping from the domain of a value defined in `def_iter_space` to a
// space that represents the sub-domain of the value that must be stored
// so that it can be used from `use_iter_space`.
MappingAttr CommunicationVolume(int value_rank,
                                const IterationSpace &def_iter_space,
                                const IterationSpace &use_iter_space);

// Computes buffers metadata and storage information for each value.
class StorageAnalysis {
 public:
  // Creates and populates the analysis. `operation` must be a sair.program
  // operation. Asserts that the analysis succeeded.
  explicit StorageAnalysis(mlir::Operation *operation);

  // Creates and populates the analysis. Returns `nullopt` and emits an error if
  // the analysis fails because storage attributes are invalid.
  static std::optional<StorageAnalysis> Create(SairProgramOp program);

  // Retrieves the analysis result for a buffer.
  const Buffer &GetBuffer(mlir::StringAttr buffer) const {
    return buffers_.find(buffer)->second;
  }

  // List of buffers indexed by name.
  const llvm::DenseMap<mlir::Attribute, Buffer> &buffers() const {
    return buffers_;
  }

  // Retrieves the storage of a value.
  const ValueStorage &GetStorage(ResultInstance value) const {
    return value_storages_.find(value)->second;
  }

  // Creates a new memory buffer, assigns it to the value storage and propagates
  // the information. This does not modify the IR, only the analysis.
  void CreateBuffer(ResultInstance value,
                    llvm::ArrayRef<mlir::StringAttr> loop_names,
                    const LoopFusionAnalysis &fusion_analysis,
                    const IterationSpaceAnalysis &iteration_spaces);

  // Updates the storage of a value with new information and propagates to other
  // values. The new information must be compatible with existing information.
  void MergeStorage(ResultInstance value, const ValueStorage &new_storage,
                    const LoopFusionAnalysis &fusion_analysis,
                    const IterationSpaceAnalysis &iteration_spaces);

  // Returns a fresh buffer name. May be called multiple times without
  // invalidating the analysis.
  mlir::StringAttr GetFreshBufferName();

  // Verifies that buffer loop nests are valid and minimizes their size if
  // possible. This is automatically called when creating StorageAnalysis. It
  // should only be manually called when the storage analysis is modified by
  // passes.
  mlir::LogicalResult VerifyAndMinimizeBufferLoopNests(
      const LoopFusionAnalysis &fusion_analysis,
      const IterationSpaceAnalysis &iteration_spaces,
      const SequenceAnalysis &sequence_analysis);

  // Extends the layout of a value by adding dimensions at the front of the
  // buffer layout. The previous layout must be a suffix of the new one. The
  // layout is given as a mapping from op_iter_space to buffer dimensions.
  void AddDimensionsToBuffer(mlir::StringAttr buffer_name, const OpInstance &op,
                             const IterationSpace &op_iter_space,
                             const LoopFusionAnalysis &fusion_analysis,
                             MappingAttr new_layout);

 private:
  // Creates an empty analysis.
  StorageAnalysis(mlir::MLIRContext *context) : context_(context){};

  // Populates the analysis.
  mlir::LogicalResult Init(SairProgramOp program);

  // Fills value_storages_.
  mlir::LogicalResult ComputeValueStorages(
      SairProgramOp program, const LoopFusionAnalysis &fusion_analysis,
      const IterationSpaceAnalysis &iteration_spaces);

  // Sets the storage of a value and propagates the information to other values.
  // Emits an error if the new storage conflicts with existing storage.
  mlir::LogicalResult SetStorage(
      ResultInstance value, ValueStorage storage,
      const LoopFusionAnalysis &fusion_analysis,
      const IterationSpaceAnalysis &iteration_spaces);

  mlir::MLIRContext *context_;
  int next_buffer_id_ = 0;
  llvm::DenseMap<mlir::Attribute, Buffer> buffers_;
  llvm::DenseMap<ResultInstance, ValueStorage> value_storages_;
};

// Verifies that values are not overwritten by another operation before they are
// used.
mlir::LogicalResult VerifyValuesNotOverwritten(
    const LoopFusionAnalysis &fusion_analysis,
    const IterationSpaceAnalysis &iteration_spaces,
    const StorageAnalysis &storage_analysis,
    const SequenceAnalysis &sequence_analysis);

// Verifies that the storage attribute is well-formed:
// - that storage attributes are arrays of buffer or unit attributes,
// - that the number of entries in the storage array matches the number of,
//   results of the operation,
// - that indexes are not stored in memory,
// - that memory spaces referenced by the attribute exist,
// - that multi-dimensional buffers are not stored in registers,
// - that loops referenced by the attribute exist and
// - that the buffer has a name if and only if the memory space is addressable.
mlir::LogicalResult VerifyStorageAttrWellFormed(
    mlir::Location loc, SairDialect *sair_dialect, mlir::TypeRange result_types,
    llvm::DenseSet<mlir::Attribute> loop_names,
    llvm::ArrayRef<mlir::Attribute> storage);

}  // namespace sair

#endif  // SAIR_STORAGE_H_
