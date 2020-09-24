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

#include "transforms/default_lowering_attributes.h"

#include <memory>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/SmallVector.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/Function.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Pass/PassManager.h"
#include "sair_attributes.h"
#include "sair_op_interfaces.h"
#include "sair_ops.h"

namespace sair {
namespace {

// Include passes base class declaration generated by MLIR. This file should not
// be included anywhere else with GEN_PASS_CLASSES set. The #define in front
// selects the part of the file to include (pass base class declaration or pass
// registration). See
// https://mlir.llvm.org/docs/PassManagement/#declarative-pass-specification for
// more information.
#define GEN_PASS_CLASSES
#include "transforms/default_lowering_attributes.h.inc"

// Assigns the default memory space to sair values. The default memory space is
// `kRegister` for 0D variables and `kMemory` for others.
class DefaultMemorySpace
    : public DefaultMemorySpacePassBase<DefaultMemorySpace> {
 public:
  void runOnFunction() override {
    getFunction().walk([](ValueProducerOp op) {
      mlir::Operation *operation = op.getOperation();
      for (int i = 0, e = operation->getNumResults(); i < e; ++i) {
        if (op.IsMemorySpaceSet(i)) continue;
        ValueType type = operation->getResult(i).getType().cast<ValueType>();
        int memory_space = type.Shape().Is0d() ? ValueProducerOp::kRegister
                                               : ValueProducerOp::kMemory;
        op.SetMemorySpace(i, memory_space);
      }
    });
  }
};

// Sets the `loop_nest` attribute to its default value. The default loop nest
// iterates over each dimension of the domain, in order, without
// rematerialization or strip-mining.
class DefaultLoopNest : public DefaultLoopNestPassBase<DefaultLoopNest> {
 public:
  void runOnFunction() override {
    getFunction().walk([](ComputeOp op) {
      if (op.loop_nest().hasValue()) return;
      SairOp sair_op = cast<SairOp>(op.getOperation());
      SairProgramOp program_op = cast<SairProgramOp>(op.getParentOp());
      int num_dimensions = sair_op.shape().NumDimensions();
      op.setLoopNest(GetDefaultLoopNest(program_op, num_dimensions));
    });
  }
};

}  // namespace

mlir::ArrayAttr GetDefaultLoopNest(SairProgramOp program, int num_dimensions,
                                   llvm::ArrayRef<mlir::Attribute> prefix) {
  mlir::MLIRContext *context = program.getContext();
  llvm::SmallVector<mlir::Attribute, 8> loop_nest(prefix.begin(), prefix.end());

  // List dimensions that are already covered by the prefix.
  llvm::BitVector covered_dims(num_dimensions);
  for (mlir::Attribute attr : loop_nest) {
    LoopAttr loop = attr.cast<LoopAttr>();
    if (loop.iter().Rematerialize()) continue;
    if (loop.iter().Step() != 1) continue;
    covered_dims.set(loop.iter().Dimension());
  }

  for (int i = 0; i < num_dimensions; ++i) {
    if (covered_dims.test(i)) continue;
    IteratorAttr iterator = IteratorAttr::get(context, i);
    mlir::StringAttr name = program.GenLoopName("loop");
    loop_nest.push_back(LoopAttr::get(name, iterator, context));
  }
  return mlir::ArrayAttr::get(loop_nest, context);
}

std::unique_ptr<mlir::Pass> CreateDefaultMemorySpacePass() {
  return std::make_unique<DefaultMemorySpace>();
}

std::unique_ptr<mlir::Pass> CreateDefaultLoopNestPass() {
  return std::make_unique<DefaultLoopNest>();
}

void CreateDefaultLoweringAttributesPipeline(mlir::OpPassManager *pm) {
  pm->addPass(CreateDefaultLoopNestPass());
  pm->addPass(CreateDefaultMemorySpacePass());
}

}  // namespace sair
