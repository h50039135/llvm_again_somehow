//===- ParallelLower.cpp - Lower gpu code to triple nested loops ------ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements a pass to lower gpu kernels in NVVM/gpu dialects into
// a generic parallel for representation
//===----------------------------------------------------------------------===//

#include "PassDetails.h"
#include "mlir/Analysis/CallGraph.h"
#include "mlir/Dialect/Affine/Analysis/AffineAnalysis.h"
#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Async/IR/Async.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/GPU/IR/GPUDialect.h"
#include "mlir/Dialect/LLVMIR/NVVMDialect.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/IR/Dominance.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "mlir/Transforms/Passes.h"
#include "polygeist/Ops.h"
#include "polygeist/Passes/Passes.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include <algorithm>
#include <mutex>

#define DEBUG_TYPE "parallel-lower-opt"

using namespace mlir;
using namespace mlir::arith;
using namespace mlir::func;
using namespace polygeist;

namespace {
// The store to load forwarding relies on three conditions:
//
// 1) they need to have mathematically equivalent affine access functions
// (checked after full composition of load/store operands); this implies that
// they access the same single memref element for all iterations of the common
// surrounding loop,
//
// 2) the store op should dominate the load op,
//
// 3) among all op's that satisfy both (1) and (2), the one that postdominates
// all store op's that have a dependence into the load, is provably the last
// writer to the particular memref location being loaded at the load op, and its
// store value can be forwarded to the load. Note that the only dependences
// that are to be considered are those that are satisfied at the block* of the
// innermost common surrounding loop of the <store, load> being considered.
//
// (* A dependence being satisfied at a block: a dependence that is satisfied by
// virtue of the destination operation appearing textually / lexically after
// the source operation within the body of a 'affine.for' operation; thus, a
// dependence is always either satisfied by a loop or by a block).
//
// The above conditions are simple to check, sufficient, and powerful for most
// cases in practice - they are sufficient, but not necessary --- since they
// don't reason about loops that are guaranteed to execute at least once or
// multiple sources to forward from.
//
// TODO: more forwarding can be done when support for
// loop/conditional live-out SSA values is available.
// TODO: do general dead store elimination for memref's. This pass
// currently only eliminates the stores only if no other loads/uses (other
// than dealloc) remain.
//
// TODO do not take wrap argument, instead, always wrap and if we will be
// lowering to cpu, remove them before continuing
struct ParallelLower : public ParallelLowerBase<ParallelLower> {
  ParallelLower(bool wrapParallelOps, bool preserveGPUKernelStructure)
      : wrapParallelOps(wrapParallelOps),
        preserveGPUKernelStructure(preserveGPUKernelStructure) {}
  void runOnOperation() override;
  bool wrapParallelOps;
  bool preserveGPUKernelStructure;
};
struct CudaRTLower : public CudaRTLowerBase<CudaRTLower> {
  void runOnOperation() override;
};

} // end anonymous namespace

/// Creates a pass to perform optimizations relying on memref dataflow such as
/// store to load forwarding, elimination of dead stores, and dead allocs.
namespace mlir {
namespace polygeist {
std::unique_ptr<Pass> createCudaRTLowerPass() {
  return std::make_unique<CudaRTLower>();
}
std::unique_ptr<Pass> createParallelLowerPass(bool wrapParallelOps,
                                              bool preserveGPUKernelStructure) {
  return std::make_unique<ParallelLower>(wrapParallelOps,
                                         preserveGPUKernelStructure);
}
} // namespace polygeist
} // namespace mlir

#include "mlir/Transforms/InliningUtils.h"

struct AlwaysInlinerInterface : public InlinerInterface {
  using InlinerInterface::InlinerInterface;

  //===--------------------------------------------------------------------===//
  // Analysis Hooks
  //===--------------------------------------------------------------------===//

  /// All call operations within standard ops can be inlined.
  bool isLegalToInline(Operation *call, Operation *callable,
                       bool wouldBeCloned) const final {
    return true;
  }

  /// All operations within standard ops can be inlined.
  bool isLegalToInline(Region *, Region *, bool,
                       BlockAndValueMapping &) const final {
    return true;
  }

  /// All operations within standard ops can be inlined.
  bool isLegalToInline(Operation *, Region *, bool,
                       BlockAndValueMapping &) const final {
    return true;
  }

  //===--------------------------------------------------------------------===//
  // Transformation Hooks
  //===--------------------------------------------------------------------===//

  /// Handle the given inlined terminator by replacing it with a new operation
  /// as necessary.
  void handleTerminator(Operation *op, Block *newDest) const final {
    // Only "std.return" needs to be handled here.
    auto returnOp = dyn_cast<func::ReturnOp>(op);
    if (!returnOp)
      return;

    // Replace the return with a branch to the dest.
    OpBuilder builder(op);
    builder.create<cf::BranchOp>(op->getLoc(), newDest, returnOp.getOperands());
    op->erase();
  }

  /// Handle the given inlined terminator by replacing it with a new operation
  /// as necessary.
  void handleTerminator(Operation *op,
                        ArrayRef<Value> valuesToRepl) const final {
    // Only "std.return" needs to be handled here.
    auto returnOp = cast<func::ReturnOp>(op);

    // Replace the values directly with the return operands.
    assert(returnOp.getNumOperands() == valuesToRepl.size());
    for (const auto &it : llvm::enumerate(returnOp.getOperands()))
      valuesToRepl[it.index()].replaceAllUsesWith(it.value());
  }
};

// TODO
mlir::Value callMalloc(mlir::OpBuilder &ibuilder, mlir::ModuleOp module,
                       mlir::Location loc, mlir::Value arg) {
  static std::mutex _mutex;
  std::unique_lock<std::mutex> lock(_mutex);

  mlir::OpBuilder builder(module.getContext());
  SymbolTableCollection symbolTable;
  std::vector args = {arg};
  if (auto fn = dyn_cast_or_null<func::FuncOp>(symbolTable.lookupSymbolIn(
          module, builder.getStringAttr("malloc")))) {
    return ibuilder.create<mlir::func::CallOp>(loc, fn, args)->getResult(0);
  }
  if (!dyn_cast_or_null<LLVM::LLVMFuncOp>(symbolTable.lookupSymbolIn(
          module, builder.getStringAttr("malloc")))) {
    auto *ctx = module->getContext();
    mlir::Type types[] = {mlir::IntegerType::get(ctx, 64)};
    auto llvmFnType = LLVM::LLVMFunctionType::get(
        LLVM::LLVMPointerType::get(mlir::IntegerType::get(ctx, 8)), types,
        false);

    LLVM::Linkage lnk = LLVM::Linkage::External;
    builder.setInsertionPointToStart(module.getBody());
    builder.create<LLVM::LLVMFuncOp>(module.getLoc(), "malloc", llvmFnType,
                                     lnk);
  }

  auto fn = cast<LLVM::LLVMFuncOp>(
      symbolTable.lookupSymbolIn(module, builder.getStringAttr("malloc")));
  return ibuilder.create<mlir::LLVM::CallOp>(loc, fn, args)->getResult(0);
}
mlir::LLVM::LLVMFuncOp GetOrCreateFreeFunction(ModuleOp module) {
  static std::mutex _mutex;
  std::unique_lock<std::mutex> lock(_mutex);

  mlir::OpBuilder builder(module.getContext());
  SymbolTableCollection symbolTable;
  if (auto fn = dyn_cast_or_null<LLVM::LLVMFuncOp>(
          symbolTable.lookupSymbolIn(module, builder.getStringAttr("free"))))
    return fn;
  auto *ctx = module->getContext();
  auto llvmFnType = LLVM::LLVMFunctionType::get(
      LLVM::LLVMVoidType::get(ctx),
      ArrayRef<mlir::Type>(LLVM::LLVMPointerType::get(builder.getI8Type())),
      false);

  LLVM::Linkage lnk = LLVM::Linkage::External;
  builder.setInsertionPointToStart(module.getBody());
  return builder.create<LLVM::LLVMFuncOp>(module.getLoc(), "free", llvmFnType,
                                          lnk);
}

LogicalResult fixupGetFunc(LLVM::CallOp, OpBuilder &rewriter,
                           SmallVectorImpl<Value> &);

void ParallelLower::runOnOperation() {
  // The inliner should only be run on operations that define a symbol table,
  // as the callgraph will need to resolve references.

  SymbolTableCollection symbolTable;
  symbolTable.getSymbolTable(getOperation());

  getOperation()->walk([&](CallOp bidx) {
    if (bidx.getCallee() == "cudaThreadSynchronize")
      bidx.erase();
  });

  std::function<void(LLVM::CallOp)> LLVMcallInliner;
  std::function<void(CallOp)> callInliner = [&](CallOp caller) {
    // Build the inliner interface.
    AlwaysInlinerInterface interface(&getContext());

    auto callable = caller.getCallableForCallee();
    CallableOpInterface callableOp;
    if (SymbolRefAttr symRef = callable.dyn_cast<SymbolRefAttr>()) {
      if (!symRef.isa<FlatSymbolRefAttr>())
        return;
      auto *symbolOp =
          symbolTable.lookupNearestSymbolFrom(getOperation(), symRef);
      callableOp = dyn_cast_or_null<CallableOpInterface>(symbolOp);
    } else {
      return;
    }
    Region *targetRegion = callableOp.getCallableRegion();
    if (!targetRegion)
      return;
    if (targetRegion->empty())
      return;
    {
      SmallVector<CallOp> ops;
      callableOp.walk([&](CallOp caller) { ops.push_back(caller); });
      for (auto op : ops)
        callInliner(op);
    }
    {
      SmallVector<LLVM::CallOp> ops;
      callableOp.walk([&](LLVM::CallOp caller) { ops.push_back(caller); });
      for (auto op : ops)
        LLVMcallInliner(op);
    }
    OpBuilder b(caller);
    auto allocScope = b.create<memref::AllocaScopeOp>(caller.getLoc(),
                                                      caller.getResultTypes());
    allocScope.getRegion().push_back(new Block());
    b.setInsertionPointToStart(&allocScope.getRegion().front());
    auto exOp = b.create<scf::ExecuteRegionOp>(caller.getLoc(),
                                               caller.getResultTypes());
    Block *blk = new Block();
    exOp.getRegion().push_back(blk);
    caller->moveBefore(blk, blk->begin());
    caller.replaceAllUsesWith(allocScope.getResults());
    b.setInsertionPointToEnd(blk);
    b.create<scf::YieldOp>(caller.getLoc(), caller.getResults());
    if (inlineCall(interface, caller, callableOp, targetRegion,
                   /*shouldCloneInlinedRegion=*/true)
            .succeeded()) {
      caller.erase();
    }
    b.setInsertionPointToEnd(&allocScope.getRegion().front());
    b.create<memref::AllocaScopeReturnOp>(allocScope.getLoc(),
                                          exOp.getResults());
  };
  LLVMcallInliner = [&](LLVM::CallOp caller) {
    // Build the inliner interface.
    AlwaysInlinerInterface interface(&getContext());

    auto callable = caller.getCallableForCallee();
    CallableOpInterface callableOp;
    if (SymbolRefAttr symRef = callable.dyn_cast<SymbolRefAttr>()) {
      if (!symRef.isa<FlatSymbolRefAttr>())
        return;
      auto *symbolOp =
          symbolTable.lookupNearestSymbolFrom(getOperation(), symRef);
      callableOp = dyn_cast_or_null<CallableOpInterface>(symbolOp);
    } else {
      return;
    }
    Region *targetRegion = callableOp.getCallableRegion();
    if (!targetRegion)
      return;
    if (targetRegion->empty())
      return;
    {
      SmallVector<CallOp> ops;
      callableOp.walk([&](CallOp caller) { ops.push_back(caller); });
      for (auto op : ops)
        callInliner(op);
    }
    {
      SmallVector<LLVM::CallOp> ops;
      callableOp.walk([&](LLVM::CallOp caller) { ops.push_back(caller); });
      for (auto op : ops)
        LLVMcallInliner(op);
    }
    OpBuilder b(caller);
    auto allocScope = b.create<memref::AllocaScopeOp>(caller.getLoc(),
                                                      caller.getResultTypes());
    allocScope.getRegion().push_back(new Block());
    b.setInsertionPointToStart(&allocScope.getRegion().front());
    auto exOp = b.create<scf::ExecuteRegionOp>(caller.getLoc(),
                                               caller.getResultTypes());
    Block *blk = new Block();
    exOp.getRegion().push_back(blk);
    caller->moveBefore(blk, blk->begin());
    caller.replaceAllUsesWith(allocScope.getResults());
    b.setInsertionPointToEnd(blk);
    b.create<scf::YieldOp>(caller.getLoc(), caller.getResults());
    if (inlineCall(interface, caller, callableOp, targetRegion,
                   /*shouldCloneInlinedRegion=*/true)
            .succeeded()) {
      caller.erase();
    }
    b.setInsertionPointToEnd(&allocScope.getRegion().front());
    b.create<memref::AllocaScopeReturnOp>(allocScope.getLoc(),
                                          exOp.getResults());
  };

  {
    SmallVector<CallOp> dimsToInline;
    getOperation()->walk([&](CallOp bidx) {
      if (bidx.getCallee() == "_ZN4dim3C1EOS_" ||
          bidx.getCallee() == "_ZN4dim3C1Ejjj")
        dimsToInline.push_back(bidx);
    });
    for (auto op : dimsToInline)
      callInliner(op);
  }

  {

    SmallVector<Operation *> inlineOps;
    SmallVector<mlir::Value> toFollowOps;
    SetVector<FunctionOpInterface> toinl;

    getOperation().walk(
        [&](mlir::gpu::ThreadIdOp bidx) { inlineOps.push_back(bidx); });
    getOperation().walk(
        [&](mlir::gpu::GridDimOp bidx) { inlineOps.push_back(bidx); });
    getOperation().walk(
        [&](mlir::NVVM::Barrier0Op bidx) { inlineOps.push_back(bidx); });

    SymbolUserMap symbolUserMap(symbolTable, getOperation());
    while (inlineOps.size()) {
      auto op = inlineOps.back();
      inlineOps.pop_back();
      auto lop = op->getParentOfType<gpu::LaunchOp>();
      auto fop = op->getParentOfType<FunctionOpInterface>();
      if (!lop || lop->isAncestor(fop)) {
        toinl.insert(fop);
        for (Operation *m : symbolUserMap.getUsers(fop)) {
          if (isa<LLVM::CallOp, func::CallOp>(m))
            inlineOps.push_back(m);
          else if (isa<polygeist::GetFuncOp>(m)) {
            toFollowOps.push_back(m->getResult(0));
          }
        }
      }
    }
    for (auto F : toinl) {
      SmallVector<LLVM::CallOp> ltoinl;
      SmallVector<func::CallOp> mtoinl;
      SymbolUserMap symbolUserMap(symbolTable, getOperation());
      for (Operation *m : symbolUserMap.getUsers(F)) {
        if (auto l = dyn_cast<LLVM::CallOp>(m))
          ltoinl.push_back(l);
        else if (auto mc = dyn_cast<func::CallOp>(m))
          mtoinl.push_back(mc);
      }
      for (auto l : ltoinl) {
        LLVMcallInliner(l);
      }
      for (auto m : mtoinl) {
        callInliner(m);
      }
    }
    while (toFollowOps.size()) {
      auto op = toFollowOps.back();
      toFollowOps.pop_back();
      SmallVector<LLVM::CallOp> ltoinl;
      SmallVector<func::CallOp> mtoinl;
      bool inlined = false;
      for (auto u : op.getUsers()) {
        if (auto cop = dyn_cast<LLVM::CallOp>(u)) {
          if (!cop.getCallee() && cop->getOperand(0) == op) {
            OpBuilder builder(cop);
            SmallVector<Value> vals;
            if (fixupGetFunc(cop, builder, vals).succeeded()) {
              if (vals.size())
                cop.getResult().replaceAllUsesWith(vals[0]);
              cop.erase();
              inlined = true;
              break;
            }
          } else if (cop.getCallee())
            ltoinl.push_back(cop);
        } else if (auto cop = dyn_cast<func::CallOp>(u)) {
          mtoinl.push_back(cop);
        } else {
          for (auto r : u->getResults())
            toFollowOps.push_back(r);
        }
      }
      for (auto l : ltoinl) {
        LLVMcallInliner(l);
        inlined = true;
      }
      for (auto m : mtoinl) {
        callInliner(m);
        inlined = true;
      }
      if (inlined)
        toFollowOps.push_back(op);
    }
  }

  // Only supports single block functions at the moment.

  SmallVector<gpu::LaunchOp> toHandle;
  getOperation().walk(
      [&](gpu::LaunchOp launchOp) { toHandle.push_back(launchOp); });
  for (gpu::LaunchOp launchOp : toHandle) {
    {
      SmallVector<CallOp> ops;
      launchOp.walk([&](CallOp caller) { ops.push_back(caller); });
      for (auto op : ops)
        callInliner(op);
    }
    {
      SmallVector<LLVM::CallOp> lops;
      launchOp.walk([&](LLVM::CallOp caller) { lops.push_back(caller); });
      for (auto op : lops)
        LLVMcallInliner(op);
    }

    mlir::IRRewriter builder(launchOp.getContext());
    auto loc = launchOp.getLoc();

    builder.setInsertionPoint(launchOp->getBlock(), launchOp->getIterator());
    auto zindex = builder.create<ConstantIndexOp>(loc, 0);

    auto oneindex = builder.create<ConstantIndexOp>(loc, 1);

    async::ExecuteOp asyncOp = nullptr;
    if (!launchOp.getAsyncDependencies().empty()) {
      SmallVector<Value> dependencies;
      for (auto v : launchOp.getAsyncDependencies()) {
        auto tok = v.getDefiningOp<polygeist::StreamToTokenOp>();
        dependencies.push_back(builder.create<polygeist::StreamToTokenOp>(
            tok.getLoc(), builder.getType<async::TokenType>(),
            tok.getSource()));
      }
      asyncOp = builder.create<mlir::async::ExecuteOp>(
          loc, /*results*/ TypeRange(), /*dependencies*/ dependencies,
          /*operands*/ ValueRange());
      Block *blockB = asyncOp.getBody();
      builder.setInsertionPointToStart(blockB);
    }

    if (wrapParallelOps) {
      auto pw = builder.create<polygeist::GPUWrapperOp>(
          loc, ValueRange({launchOp.getBlockSizeX(), launchOp.getBlockSizeY(),
                           launchOp.getBlockSizeZ()}));
      builder.setInsertionPointToStart(pw.getBody());
    }

    auto block = builder.create<mlir::scf::ParallelOp>(
        loc, std::vector<Value>({zindex, zindex, zindex}),
        std::vector<Value>({launchOp.getGridSizeX(), launchOp.getGridSizeY(),
                            launchOp.getGridSizeZ()}),
        std::vector<Value>({oneindex, oneindex, oneindex}));
    Block *blockB = &block.getRegion().front();
    builder.setInsertionPointToStart(blockB);

    if (preserveGPUKernelStructure) {
      auto gpuBlock = builder.create<polygeist::GPUBlockOp>(
          loc, blockB->getArguments()[0], blockB->getArguments()[1],
          blockB->getArguments()[2]);
      builder.setInsertionPointToStart(&gpuBlock.getRegion().front());
    }

    auto threadr = builder.create<mlir::scf::ParallelOp>(
        loc, std::vector<Value>({zindex, zindex, zindex}),
        std::vector<Value>({launchOp.getBlockSizeX(), launchOp.getBlockSizeY(),
                            launchOp.getBlockSizeZ()}),
        std::vector<Value>({oneindex, oneindex, oneindex}));
    Block *threadB = &threadr.getRegion().front();
    builder.setInsertionPointToStart(threadB);
    Operation *mergeLoc = threadB->getTerminator();

    if (preserveGPUKernelStructure) {
      auto gpuThread = builder.create<polygeist::GPUThreadOp>(
          loc, threadB->getArguments()[0], threadB->getArguments()[1],
          threadB->getArguments()[2]);
      builder.setInsertionPointToStart(&gpuThread.getRegion().front());
      mergeLoc = gpuThread.getRegion().front().getTerminator();
    }

    launchOp.getRegion().front().getTerminator()->erase();

    SmallVector<Value> launchArgs;
    llvm::append_range(launchArgs, blockB->getArguments());
    llvm::append_range(launchArgs, threadB->getArguments());
    launchArgs.push_back(launchOp.getGridSizeX());
    launchArgs.push_back(launchOp.getGridSizeY());
    launchArgs.push_back(launchOp.getGridSizeZ());
    launchArgs.push_back(launchOp.getBlockSizeX());
    launchArgs.push_back(launchOp.getBlockSizeY());
    launchArgs.push_back(launchOp.getBlockSizeZ());
    builder.mergeBlockBefore(&launchOp.getRegion().front(), mergeLoc,
                             launchArgs);

    auto container = threadr;

    container.walk([&](mlir::gpu::BlockIdOp bidx) {
      int idx = -1;
      if (bidx.getDimension() == gpu::Dimension::x)
        idx = 0;
      else if (bidx.getDimension() == gpu::Dimension::y)
        idx = 1;
      else if (bidx.getDimension() == gpu::Dimension::z)
        idx = 2;
      else
        assert(0 && "illegal dimension");
      builder.replaceOp(bidx,
                        ValueRange((mlir::Value)blockB->getArgument(idx)));
    });

    container.walk([&](mlir::memref::AllocaOp alop) {
      if (auto ia =
              alop.getType().getMemorySpace().dyn_cast_or_null<IntegerAttr>())
        if (ia.getValue() == 5) {
          builder.setInsertionPointToStart(blockB);
          auto newAlloca = builder.create<memref::AllocaOp>(
              alop.getLoc(),
              MemRefType::get(alop.getType().getShape(),
                              alop.getType().getElementType(),
                              alop.getType().getLayout(), Attribute()));
          builder.replaceOpWithNewOp<memref::CastOp>(alop, alop.getType(),
                                                     newAlloca);
        }
    });

    container.walk([&](mlir::LLVM::AllocaOp alop) {
      auto PT = alop.getType().cast<LLVM::LLVMPointerType>();
      if (PT.getAddressSpace() == 5) {
        builder.setInsertionPointToStart(blockB);
        auto newAlloca = builder.create<LLVM::AllocaOp>(
            alop.getLoc(), LLVM::LLVMPointerType::get(PT.getElementType(), 0),
            alop.getArraySize());
        builder.replaceOpWithNewOp<LLVM::AddrSpaceCastOp>(alop, PT, newAlloca);
      }
    });

    container.walk([&](mlir::gpu::ThreadIdOp bidx) {
      int idx = -1;
      if (bidx.getDimension() == gpu::Dimension::x)
        idx = 0;
      else if (bidx.getDimension() == gpu::Dimension::y)
        idx = 1;
      else if (bidx.getDimension() == gpu::Dimension::z)
        idx = 2;
      else
        assert(0 && "illegal dimension");
      builder.replaceOp(bidx, ValueRange(threadB->getArgument(idx)));
    });

    container.walk([&](mlir::NVVM::Barrier0Op op) {
      builder.setInsertionPoint(op);
      builder.replaceOpWithNewOp<mlir::polygeist::BarrierOp>(
          op, threadB->getArguments());
    });

    container.walk([&](gpu::GridDimOp bidx) {
      Value val = nullptr;
      if (bidx.getDimension() == gpu::Dimension::x)
        val = launchOp.getGridSizeX();
      else if (bidx.getDimension() == gpu::Dimension::y)
        val = launchOp.getGridSizeY();
      else if (bidx.getDimension() == gpu::Dimension::z)
        val = launchOp.getGridSizeZ();
      else
        assert(0 && "illegal dimension");
      builder.replaceOp(bidx, val);
    });

    container.walk([&](gpu::BlockDimOp bidx) {
      Value val = nullptr;
      if (bidx.getDimension() == gpu::Dimension::x)
        val = launchOp.getBlockSizeX();
      else if (bidx.getDimension() == gpu::Dimension::y)
        val = launchOp.getBlockSizeY();
      else if (bidx.getDimension() == gpu::Dimension::z)
        val = launchOp.getBlockSizeZ();
      else
        assert(0 && "illegal dimension");
      builder.replaceOp(bidx, val);
    });

    container.walk([&](AffineStoreOp storeOp) {
      builder.setInsertionPoint(storeOp);
      auto map = storeOp.getAffineMap();
      std::vector<Value> indices;
      for (size_t i = 0; i < map.getNumResults(); i++) {
        auto apply = builder.create<AffineApplyOp>(
            storeOp.getLoc(), map.getSliceMap(i, 1), storeOp.getMapOperands());
        indices.push_back(apply->getResult(0));
      }
      builder.replaceOpWithNewOp<memref::StoreOp>(storeOp, storeOp.getValue(),
                                                  storeOp.getMemref(), indices);
    });

    container.walk([&](AffineLoadOp storeOp) {
      builder.setInsertionPoint(storeOp);
      auto map = storeOp.getAffineMap();
      std::vector<Value> indices;
      for (size_t i = 0; i < map.getNumResults(); i++) {
        auto apply = builder.create<AffineApplyOp>(
            storeOp.getLoc(), map.getSliceMap(i, 1), storeOp.getMapOperands());
        indices.push_back(apply->getResult(0));
      }
      builder.replaceOpWithNewOp<memref::LoadOp>(storeOp, storeOp.getMemref(),
                                                 indices);
    });
    builder.eraseOp(launchOp);
  }

  // Fold the copy memtype cast
  {
    mlir::RewritePatternSet rpl(getOperation()->getContext());
    GreedyRewriteConfig config;
    (void)applyPatternsAndFoldGreedily(getOperation(), std::move(rpl), config);
  }
}

void CudaRTLower::runOnOperation() {
  // The inliner should only be run on operations that define a symbol table,
  // as the callgraph will need to resolve references.

  SymbolTableCollection symbolTable;
  symbolTable.getSymbolTable(getOperation());

  std::function<void(Operation * call, StringRef callee)> replace =
      [&](Operation *call, StringRef callee) {
        if (callee == "cudaMemcpy" || callee == "cudaMemcpyAsync") {
          OpBuilder bz(call);
          auto falsev = bz.create<ConstantIntOp>(call->getLoc(), false, 1);
          auto dst = call->getOperand(0);
          if (auto mt = dst.getType().dyn_cast<MemRefType>()) {
            dst = bz.create<polygeist::Memref2PointerOp>(
                call->getLoc(),
                LLVM::LLVMPointerType::get(mt.getElementType(),
                                           mt.getMemorySpaceAsInt()),
                dst);
          }
          auto src = call->getOperand(1);
          if (auto mt = src.getType().dyn_cast<MemRefType>()) {
            src = bz.create<polygeist::Memref2PointerOp>(
                call->getLoc(),
                LLVM::LLVMPointerType::get(mt.getElementType(),
                                           mt.getMemorySpaceAsInt()),
                src);
          }
          bz.create<LLVM::MemcpyOp>(call->getLoc(), dst, src,
                                    call->getOperand(2),
                                    /*isVolatile*/ falsev);
          call->replaceAllUsesWith(bz.create<ConstantIntOp>(
              call->getLoc(), 0, call->getResult(0).getType()));
          call->erase();
        } else if (callee == "cudaMemcpyToSymbol") {
          OpBuilder bz(call);
          auto falsev = bz.create<ConstantIntOp>(call->getLoc(), false, 1);
          auto dst = call->getOperand(0);
          if (auto mt = dst.getType().dyn_cast<MemRefType>()) {
            dst = bz.create<polygeist::Memref2PointerOp>(
                call->getLoc(),
                LLVM::LLVMPointerType::get(mt.getElementType(),
                                           mt.getMemorySpaceAsInt()),
                dst);
          }
          auto src = call->getOperand(1);
          if (auto mt = src.getType().dyn_cast<MemRefType>()) {
            src = bz.create<polygeist::Memref2PointerOp>(
                call->getLoc(),
                LLVM::LLVMPointerType::get(mt.getElementType(),
                                           mt.getMemorySpaceAsInt()),
                src);
          }
          bz.create<LLVM::MemcpyOp>(
              call->getLoc(),
              bz.create<LLVM::GEPOp>(call->getLoc(), dst.getType(), dst,
                                     std::vector<Value>({call->getOperand(3)})),
              src, call->getOperand(2),
              /*isVolatile*/ falsev);
          call->replaceAllUsesWith(bz.create<ConstantIntOp>(
              call->getLoc(), 0, call->getResult(0).getType()));
          call->erase();
        } else if (callee == "cudaMemset") {
          OpBuilder bz(call);
          auto falsev = bz.create<ConstantIntOp>(call->getLoc(), false, 1);
          auto dst = call->getOperand(0);
          if (auto mt = dst.getType().dyn_cast<MemRefType>()) {
            dst = bz.create<polygeist::Memref2PointerOp>(
                call->getLoc(),
                LLVM::LLVMPointerType::get(mt.getElementType(),
                                           mt.getMemorySpaceAsInt()),
                dst);
          }
          bz.create<LLVM::MemsetOp>(call->getLoc(), dst,
                                    bz.create<TruncIOp>(call->getLoc(),
                                                        bz.getI8Type(),
                                                        call->getOperand(1)),
                                    call->getOperand(2),
                                    /*isVolatile*/ falsev);
          call->replaceAllUsesWith(bz.create<ConstantIntOp>(
              call->getLoc(), 0, call->getResult(0).getType()));
          call->erase();
        } else if (callee == "cudaMalloc" || callee == "cudaMallocHost") {
          OpBuilder bz(call);
          Value arg = call->getOperand(1);
          if (arg.getType().cast<IntegerType>().getWidth() < 64)
            arg =
                bz.create<arith::ExtUIOp>(call->getLoc(), bz.getI64Type(), arg);
          mlir::Value alloc =
              callMalloc(bz, getOperation(), call->getLoc(), arg);
          bz.create<LLVM::StoreOp>(call->getLoc(), alloc, call->getOperand(0));
          {
            auto retv = bz.create<ConstantIntOp>(
                call->getLoc(), 0,
                call->getResult(0).getType().cast<IntegerType>().getWidth());
            Value vals[] = {retv};
            call->replaceAllUsesWith(ArrayRef<Value>(vals));
            call->erase();
          }
        } else if (callee == "cudaFree" || callee == "cudaFreeHost") {
          auto mf = GetOrCreateFreeFunction(getOperation());
          OpBuilder bz(call);
          Value args[] = {call->getOperand(0)};
          bz.create<mlir::LLVM::CallOp>(call->getLoc(), mf, args);
          {
            auto retv = bz.create<ConstantIntOp>(
                call->getLoc(), 0,
                call->getResult(0).getType().cast<IntegerType>().getWidth());
            Value vals[] = {retv};
            call->replaceAllUsesWith(ArrayRef<Value>(vals));
            call->erase();
          }
        } else if (callee == "cudaDeviceSynchronize") {
          OpBuilder bz(call);
          auto retv = bz.create<ConstantIntOp>(
              call->getLoc(), 0,
              call->getResult(0).getType().cast<IntegerType>().getWidth());
          Value vals[] = {retv};
          call->replaceAllUsesWith(ArrayRef<Value>(vals));
          call->erase();
        } else if (callee == "cudaGetLastError" ||
                   callee == "cudaPeekAtLastError") {
          OpBuilder bz(call);
          auto retv = bz.create<ConstantIntOp>(
              call->getLoc(), 0,
              call->getResult(0).getType().cast<IntegerType>().getWidth());
          Value vals[] = {retv};
          call->replaceAllUsesWith(ArrayRef<Value>(vals));
          call->erase();
        }
      };

  getOperation()->walk([&](CallOp bidx) {
    if (bidx.getCallee() == "cudaThreadSynchronize")
      bidx.erase();
  });
  getOperation().walk([&](LLVM::CallOp call) {
    if (!call.getCallee())
      return;
    replace(call, *call.getCallee());
  });

  getOperation().walk([&](CallOp call) { replace(call, call.getCallee()); });

  // Fold the copy memtype cast
  {
    mlir::RewritePatternSet rpl(getOperation()->getContext());
    GreedyRewriteConfig config;
    (void)applyPatternsAndFoldGreedily(getOperation(), std::move(rpl), config);
  }
}
