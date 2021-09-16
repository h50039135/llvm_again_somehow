/// Utility functions shared among mlir-clang library sources.

#ifndef MLIR_TOOLS_MLIRCLANG_UTILS_H
#define MLIR_TOOLS_MLIRCLANG_UTILS_H

#include "llvm/ADT/ArrayRef.h"

namespace mlir {
class Operation;
class FuncOp;
class Value;
class OpBuilder;
class AbstractOperation;
class Type;
} // namespace mlir

namespace llvm {
class StringRef;
} // namespace llvm

namespace mlirclang {

/// Replace the given function by the operation with the given name, and use the
/// same argument list. For example, if the function is @foo(%a, %b) and opName
/// is "bar.baz", we will create an operator baz of the bar dialect, with
/// operands %a and %b. The new op will be inserted at where the insertion point
/// of the provided OpBuilder is.
mlir::Operation *
replaceFuncByOperation(mlir::FuncOp f, llvm::StringRef opName,
                       mlir::OpBuilder &b,
                       llvm::SmallVectorImpl<mlir::Value> &input,
                       llvm::SmallVectorImpl<mlir::Value> &output);

mlir::Operation *buildLinalgOp(const mlir::AbstractOperation *op,
                               mlir::OpBuilder &b,
                               llvm::SmallVectorImpl<mlir::Value> &input,
                               llvm::SmallVectorImpl<mlir::Value> &output);

} // namespace mlirclang

#endif
