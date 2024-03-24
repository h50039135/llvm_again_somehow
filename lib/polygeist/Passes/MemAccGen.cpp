#include "PassDetails.h"

#include "MemAcc/Dialect.h"
#include "MemAcc/Ops.h"
#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Affine/Passes.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "polygeist/Passes/Passes.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/Support/Debug.h"

#ifdef DEBUG
  #define PRINT(x) llvm::errs() << x << "\n"
#else
  #define PRINT(x)
#endif

// Use LLVM's data structures for convenience and performance
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"

#define DEBUG_TYPE "memory-access-generation"

using namespace mlir;
using namespace mlir::arith;
using namespace polygeist;
using namespace mlir::affine;
// Define the data structures at the beginning of your pass

namespace {
struct MemAccGenPass : public MemAccGenBase<MemAccGenPass> {
  void runOnOperation() override;
};
} // end namespace.

namespace {

llvm::SmallPtrSet<Operation *, 16> deepestLoads;
llvm::DenseMap<Operation *, llvm::SmallPtrSet<Operation *, 16>>
    loadOpToIndirectUses;
llvm::DenseMap<Operation *, llvm::SmallVector<Operation *, 16>>
    loadOpToIndirectChain;

static void postProcessDeepestLoads() {
  for (auto o : deepestLoads) {
    for (auto i : loadOpToIndirectUses[o]) {
      if (deepestLoads.count(i) > 0) {
        deepestLoads.erase(i);
      }
    }
  }
}

// Utility function to create a MemAcc::YieldOp
static void createMemAccYieldOp(PatternRewriter &rewriter, mlir::Location loc) {

  // Specify empty result types and operands for the yield operation
  mlir::TypeRange resultTypes;                     // No return types
  mlir::ValueRange operands;                       // No operands
  llvm::ArrayRef<mlir::NamedAttribute> attributes; // No attributes

  // Finally, build and insert the operation into the IR
  rewriter.create<MemAcc::YieldOp>(loc, resultTypes, operands, attributes);
}

// These unary op legalizations are identical for floating-point
// or quantized types
template <typename SrcOpType, typename DestOpType>
class ConvertArithToMemAccPattern : public OpRewritePattern<SrcOpType> {
public:
  using OpRewritePattern<SrcOpType>::OpRewritePattern;
  LogicalResult matchAndRewrite(SrcOpType op,
                                PatternRewriter &rewriter) const override {
    if (op->template getParentOfType<MemAcc::GenericLoadOp>()) {
      rewriter.replaceOpWithNewOp<DestOpType>(op, op.getResult().getType(),
                                              op.getOperands());
      return success();
    }
    return failure();
  }
};

class ConvertArithIndexCastToMemAccIndexCastPattern
    : public OpRewritePattern<arith::IndexCastOp> {
public:
  using OpRewritePattern<arith::IndexCastOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(arith::IndexCastOp op,
                                PatternRewriter &rewriter) const override {
    if (op->template getParentOfType<MemAcc::GenericLoadOp>()) {
      rewriter.replaceOpWithNewOp<MemAcc::IndexCastOp>(
          op, op.getResult().getType(), op.getOperand());
      return success();
    }
    return failure();
  }
};

template <typename StoreOpType>
struct StoreOpConversionPattern : public OpRewritePattern<StoreOpType> {
  using OpRewritePattern<StoreOpType>::OpRewritePattern;

  void rewriteStoreOp(StoreOpType storeOp, PatternRewriter &rewriter) const {
    rewriter.replaceOpWithNewOp<MemAcc::StoreOp>(
        storeOp, storeOp.getValueToStore(), storeOp.getMemRef(),
        storeOp.getIndices());
  }

  LogicalResult matchAndRewrite(StoreOpType storeOp,
                                PatternRewriter &rewriter) const override {
    // Check if the storeOp is contained within an affine.for operation
    if (!storeOp->template getParentOfType<AffineForOp>()) {
      return failure();
    }
    if (storeOp->template getParentOfType<MemAcc::GenericStoreOp>()) {
      rewriteStoreOp(storeOp, rewriter);
      return success();
    }

    // Create the new MemAcc::GenericStoreOp, wrapping the original store
    // operation
    Location loc = storeOp.getLoc();
    auto genericStoreOp = rewriter.create<MemAcc::GenericStoreOp>(
        loc /* other necessary parameters */);

    // Insert the original store operation into the body of the new
    // GenericStoreOp This assumes your GenericStoreOp has a region that can
    // contain the storeOp
    auto &region = genericStoreOp.getBody();
    auto *block = rewriter.createBlock(&region);

    // Remove the original store operation
    storeOp.getOperation()->moveBefore(block, block->end());

    createMemAccYieldOp(rewriter, loc);

    return success();
  }
};

template <typename LoadOpType>
struct LoadOpConversionPattern : public OpRewritePattern<LoadOpType> {
  using OpRewritePattern<LoadOpType>::OpRewritePattern;

  void rewriteLoadOp(LoadOpType loadOp, PatternRewriter &rewriter) const {
    rewriter.replaceOpWithNewOp<MemAcc::LoadOp>(loadOp, loadOp.getMemRef(),
                                                loadOp.getIndices());
  }

  SmallVector<Type, 4> getGenericLoadOpResultTypes(Operation *loadOp) const {
    SmallVector<Type, 4> resultTypes;
    auto &indirectLoadUseChain = loadOpToIndirectChain[loadOp];
    loadOpToIndirectUses[loadOp].insert(loadOp);
    for (int i = indirectLoadUseChain.size() - 1; i >= 0; i--) {
      auto I = indirectLoadUseChain[i];
      PRINT(*I);
      for (auto U : I->getUsers()) {
        PRINT("User: " << *U);
        if (loadOpToIndirectUses[loadOp].count(U) == 0) {
          PRINT("External User: " << *U);
          resultTypes.push_back(I->getResult(0).getType());
          break;
        }
      }
    }
    return resultTypes;
  }

  SmallVector<Value, 4> populateGenericLoadOp(
      llvm::SmallVector<Operation *, 16> &indirectLoadUseChain,
      PatternRewriter &rewriter, MemAcc::GenericLoadOp genericLoadOp) const {
    SmallVector<Value, 4> resultVals;
    auto &region = genericLoadOp.getBody();

    // Create a block inside the GenericLoadOp's region
    auto *block = rewriter.createBlock(&region);

    // Move the operations from the indirectLoadUseChain into the block
    for (int i = indirectLoadUseChain.size() - 1; i >= 0; i--) {
      auto clonedOp = rewriter.clone(*indirectLoadUseChain[i]);
      indirectLoadUseChain[i]->getResult(0).replaceAllUsesWith(
          clonedOp->getResult(0));
      rewriter.eraseOp(indirectLoadUseChain[i]);
    }

    for (auto &I : *block) {
      bool hasExternalUses = false;
      for (auto U : I.getUsers()) {
        if (block != U->getBlock()) {
          hasExternalUses = true;
          break;
        }
      }
      if (hasExternalUses) {
        resultVals.push_back(I.getResult(0));
      }
    }
    return resultVals;
  }

  void updateExternelUses(MemAcc::GenericLoadOp genericLoadOp) const {
    // Update uses of inner-block values
    auto block = &genericLoadOp.getBody().getBlocks().front();
    int idx = 0;
    for (auto &I : *block) {
      bool hasExternalUses = false;
      for (auto U : I.getUsers()) {
        if (block != U->getBlock()) {
          for (unsigned int operandIndex = 0;
               operandIndex < U->getNumOperands(); ++operandIndex) {
            if (U->getOperand(operandIndex) == I.getResult(0)) {
              // Update the operand with a new value
              U->setOperand(operandIndex, genericLoadOp->getResult(idx));
              hasExternalUses = true;
              break; // Break after updating the operand
            }
          } // for
        }   // if
      }
      if (hasExternalUses) {
        idx++;
      }
    }
  }

  LogicalResult matchAndRewrite(LoadOpType loadOp,
                                PatternRewriter &rewriter) const override {
    // Check if the loadOp is contained within an affine.for operation
    if (!loadOp->template getParentOfType<AffineForOp>()) {
      return failure();
    }
    if (loadOp->template getParentOfType<MemAcc::GenericLoadOp>()) {
      rewriteLoadOp(loadOp, rewriter);
      return success();
    }

    // only consider the deepest loads
    if (deepestLoads.count(loadOp) == 0) {
      return failure();
    }
    SmallVector<Value, 4> resultVals;
    auto &indirectLoadUseChain = loadOpToIndirectChain[loadOp];

    // get result types of generic load op
    auto resultTypes = getGenericLoadOpResultTypes(loadOp);
    // Count the number of loads in the indirectLoadUseChain for indirection
    // level
    uint64_t indirectionLevel = 0;
    // Calculate the indirection level based on the number of loads in the
    // indirectLoadUseChain
    for (Operation *op : indirectLoadUseChain) {
      if (isa<memref::LoadOp>(op) || isa<affine::AffineLoadOp>(op)) {
        indirectionLevel++;
      }
    }
    auto indirectionAttr = IntegerAttr::get(
        IntegerType::get(rewriter.getContext(), 64), indirectionLevel - 1);

    Location loc = loadOp.getLoc();

    // Start creating the GenericLoadOp
    auto genericLoadOp = rewriter.create<MemAcc::GenericLoadOp>(
        loc, resultTypes, indirectionAttr);

    // Populate the GenericLoadOp with the operations from the
    // indirectLoadUseChain
    resultVals =
        populateGenericLoadOp(indirectLoadUseChain, rewriter, genericLoadOp);

    rewriter.create<MemAcc::YieldOp>(loc, resultTypes, resultVals);

    // Update uses of inner-block values
    updateExternelUses(genericLoadOp);

    return success();
  }
};

// Modified to populate the mapping
void markIndirectLoadUsers(Operation *op,
                           llvm::SmallPtrSetImpl<Operation *> &visited,
                           Operation *originalLoadOp) {

  if (!op || !visited.insert(op).second)
    return;

  if (isa<memref::LoadOp>(op) || isa<affine::AffineLoadOp>(op) ||
      isa<arith::ArithDialect>(op->getDialect())) {
    loadOpToIndirectUses[originalLoadOp].insert(op);
    loadOpToIndirectChain[originalLoadOp].push_back(op);
  } else {
    return;
  }

  for (auto operand : op->getOperands()) {
    markIndirectLoadUsers(operand.getDefiningOp(), visited, originalLoadOp);
  }
}

void analyzeLoadOps(Operation *op,
                    llvm::SmallPtrSet<Operation *, 16> &deepestLoads) {
  llvm::SmallPtrSet<Operation *, 16> visited;
  op->walk([&](Operation *currentOp) {
    if (isa<memref::LoadOp>(currentOp) ||
        isa<affine::AffineLoadOp>(currentOp)) {
      visited.clear();
      loadOpToIndirectChain[currentOp].push_back(currentOp);
      // Check all users of the load operation to see if it indirectly
      // contributes to another load
      for (auto operand : currentOp->getOperands()) {
        markIndirectLoadUsers(operand.getDefiningOp(), visited, currentOp);
      }
      deepestLoads.insert(currentOp);
    }
  });
  postProcessDeepestLoads();
}

void MemAccGenPass::runOnOperation() {
  deepestLoads.clear();
  loadOpToIndirectUses.clear();
  loadOpToIndirectChain.clear();
  mlir::MLIRContext *context = getOperation()->getContext();

  analyzeLoadOps(getOperation(), deepestLoads);

  // context->loadDialect<mlir::MemAcc::MemAccDialect>();
  mlir::RewritePatternSet patterns(context);
  patterns.add<StoreOpConversionPattern<memref::StoreOp>>(context);
  patterns.add<StoreOpConversionPattern<affine::AffineStoreOp>>(context);
  patterns.add<LoadOpConversionPattern<memref::LoadOp>>(context);
  patterns.add<LoadOpConversionPattern<affine::AffineLoadOp>>(context);
  patterns.add<ConvertArithToMemAccPattern<arith::MulIOp, MemAcc::MulIOp>>(
      context);
  patterns.add<ConvertArithToMemAccPattern<arith::AddIOp, MemAcc::AddIOp>>(
      context);
  patterns.add<ConvertArithToMemAccPattern<arith::SubIOp, MemAcc::SubIOp>>(
      context);
  patterns.add<ConvertArithIndexCastToMemAccIndexCastPattern>(context);
  GreedyRewriteConfig config;
  (void)applyPatternsAndFoldGreedily(getOperation(), std::move(patterns),
                                     config);
}
} // end anonymous namespace

namespace mlir {
namespace polygeist {
std::unique_ptr<Pass> mlir::polygeist::createMemAccGenPass() {
  return std::make_unique<MemAccGenPass>();
}
} // namespace polygeist
} // namespace mlir