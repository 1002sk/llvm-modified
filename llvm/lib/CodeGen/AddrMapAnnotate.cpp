//===- AddrMapAnnotate.cpp - Annotate pointer arithmetic for addrmap ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Right before instruction selection, attach !addrmap annotations to the IR
// instructions that compute pointers (GEPs, inttoptr) and to the integer
// arithmetic that may become a LEA. Instruction selection carries them to the
// MachineInstr that defines the value, so that the AsmPrinter knows which
// machine instructions do pointer arithmetic and what the result points to,
// even when the base pointer was loaded from memory or comes from another
// basic block.
//
// Every value is described relative to a root: a global variable, a local
// variable with a stack slot (dbg.declare), or the pointee of a pointer value
// (an argument or variable described by dbg.value, or a pointer loaded from a
// typed location). Types come from debug info, never from the IR types.
//
// The pass only adds metadata, so it does not change the generated code.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/CodeGen/AddrMapTypes.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"

using namespace llvm;
using namespace llvm::addrmap;

#define DEBUG_TYPE "addrmap-annotate"

namespace {

/// What a pointer value is, relative to its root.
struct PtrInfo {
  Annotation A;
  /// Computed while a PHI it depends on was still being visited, so it is
  /// incomplete and must not be cached.
  bool Partial = false;
};

class Annotator {
public:
  explicit Annotator(const DataLayout &DL) : DL(DL) {}
  PtrInfo get(const Value *V, unsigned Depth = 0);

private:
  const DataLayout &DL;
  DenseMap<const Value *, PtrInfo> Cache;
  SmallPtrSet<const Value *, 8> InProgress;

  PtrInfo compute(const Value *V, unsigned Depth);
  std::optional<PtrInfo> fromDebugValue(const Value *V);
  PtrInfo fromLoad(const LoadInst &LI, unsigned Depth);
  PtrInfo merge(ArrayRef<const Value *> Incoming, unsigned Depth);
};

} // end anonymous namespace

/// The type of the object at the root of \p A, and whether the origin is the
/// value of a pointer rather than the object itself.
static bool getRootType(const Annotation &A, const DIType *&Ty,
                        bool &ViaPointer) {
  ViaPointer = false;
  switch (A.RootKind) {
  case Annotation::Unknown:
    return false;
  case Annotation::Global: {
    SmallVector<DIGlobalVariableExpression *, 1> GVEs;
    A.GV->getDebugInfo(GVEs);
    if (GVEs.empty())
      return false;
    Ty = GVEs[0]->getVariable()->getType();
    return true;
  }
  case Annotation::Variable:
    Ty = A.Var->getType();
    return true;
  case Annotation::Pointee:
    Ty = A.PointeeType;
    ViaPointer = true;
    return true;
  }
  llvm_unreachable("unknown root kind");
}

static std::string getRootExpr(const Annotation &A) {
  switch (A.RootKind) {
  case Annotation::Global: {
    SmallVector<DIGlobalVariableExpression *, 1> GVEs;
    A.GV->getDebugInfo(GVEs);
    if (!GVEs.empty())
      return GVEs[0]->getVariable()->getName().str();
    return A.GV->getName().str();
  }
  case Annotation::Variable:
    return A.Var->getName().str();
  default:
    return A.RootExpr;
  }
}

static bool sameRoot(const Annotation &L, const Annotation &R) {
  return L.RootKind == R.RootKind && L.GV == R.GV && L.Var == R.Var &&
         L.PointeeType == R.PointeeType && L.RootExpr == R.RootExpr;
}

/// A pointer variable whose value \p V is, from #dbg_value records.
std::optional<PtrInfo> Annotator::fromDebugValue(const Value *V) {
  SmallVector<DbgVariableRecord *, 2> Records;
  findDbgValues(const_cast<Value *>(V), Records);
  for (const DbgVariableRecord *DVR : Records) {
    if (DVR->hasArgList() || DVR->getExpression()->getNumElements() != 0)
      continue;
    const DILocalVariable *Var = DVR->getVariable();
    bool IsPointer;
    const DIType *Pointee = getPointeeType(Var->getType(), IsPointer);
    if (!IsPointer)
      continue;
    PtrInfo P;
    P.A.RootKind = Annotation::Pointee;
    P.A.PointeeType = Pointee;
    P.A.RootExpr = Var->getName().str();
    P.A.SrcType = Var->getType();
    P.A.Offset = 0;
    return P;
  }
  return std::nullopt;
}

/// A pointer loaded from a location whose type is known.
PtrInfo Annotator::fromLoad(const LoadInst &LI, unsigned Depth) {
  PtrInfo P;
  PtrInfo Src = get(LI.getPointerOperand(), Depth + 1);
  P.Partial = Src.Partial;
  const DIType *RootTy;
  bool ViaPointer;
  if (!Src.A.Offset || !getRootType(Src.A, RootTy, ViaPointer))
    return P;

  SmallVector<VarTerm, 2> Vars;
  for (int64_t S : Src.A.Scales)
    Vars.push_back({"", unsigned(S)});
  ResolvedPath Path = resolvePath(RootTy, *Src.A.Offset, Vars, ViaPointer);
  if (Path.Unresolved || Path.HasUnmatchedVar || Path.ByteOffsetInField != 0)
    return P;

  std::string Root = getRootExpr(Src.A);
  const DIType *Loaded = Path.FinalType;
  std::string Expr;
  bool IsPointer;
  getPointeeType(Loaded, IsPointer);
  if (IsPointer) {
    Expr = formatPath(Root, ViaPointer, Path.Steps);
  } else {
    // At offset 0 the location is also the first member (or element).
    for (const Alternative &Alt : Path.Alternatives) {
      getPointeeType(Alt.Type, IsPointer);
      if (IsPointer) {
        Loaded = Alt.Type;
        Expr = formatPath(Root, ViaPointer, Path.Steps, Alt.Steps);
        break;
      }
    }
  }
  if (!IsPointer)
    return P;
  // Loading through the pointer itself: (*p).
  if (Expr == Root && ViaPointer)
    Expr = "(*" + Expr + ")";

  P.A.RootKind = Annotation::Pointee;
  P.A.PointeeType = getPointeeType(Loaded, IsPointer);
  P.A.RootExpr = Expr;
  P.A.SrcType = Loaded;
  P.A.Offset = 0;
  return P;
}

/// A PHI or select: the incoming values must share the root.
PtrInfo Annotator::merge(ArrayRef<const Value *> Incoming, unsigned Depth) {
  PtrInfo P;
  std::optional<Annotation> Common;
  bool SameOffset = true;
  for (const Value *In : Incoming) {
    if (InProgress.contains(In)) {
      // A loop: the value moves away from the incoming ones.
      P.Partial = true;
      SameOffset = false;
      continue;
    }
    PtrInfo I = get(In, Depth + 1);
    P.Partial |= I.Partial;
    if (I.A.RootKind == Annotation::Unknown)
      return PtrInfo{{}, P.Partial};
    if (!Common) {
      Common = I.A;
      continue;
    }
    if (!sameRoot(*Common, I.A))
      return PtrInfo{{}, P.Partial};
    if (Common->Offset != I.A.Offset || Common->Scales != I.A.Scales)
      SameOffset = false;
    Common->Heuristic |= I.A.Heuristic;
  }
  if (!Common)
    return P;
  P.A = *Common;
  if (!SameOffset) {
    P.A.Offset.reset();
    P.A.Scales.clear();
  }
  return P;
}

PtrInfo Annotator::get(const Value *V, unsigned Depth) {
  auto It = Cache.find(V);
  if (It != Cache.end())
    return It->second;
  if (Depth > 32 || !InProgress.insert(V).second)
    return PtrInfo{{}, true};
  PtrInfo P = compute(V, Depth);
  InProgress.erase(V);
  if (!P.Partial)
    Cache[V] = P;
  return P;
}

PtrInfo Annotator::compute(const Value *V, unsigned Depth) {
  PtrInfo P;
  V = V->stripPointerCastsSameRepresentation();

  if (auto *GV = dyn_cast<GlobalVariable>(V)) {
    P.A.RootKind = Annotation::Global;
    P.A.GV = GV;
    P.A.Offset = 0;
    return P;
  }

  if (auto *GEP = dyn_cast<GEPOperator>(V)) {
    PtrInfo Base = get(GEP->getPointerOperand(), Depth + 1);
    unsigned BitWidth = DL.getIndexTypeSizeInBits(GEP->getType());
    SmallMapVector<Value *, APInt, 4> VarOffsets;
    APInt ConstOffset(BitWidth, 0);
    P = Base;
    if (!GEP->collectOffset(DL, BitWidth, VarOffsets, ConstOffset)) {
      P.A.Offset.reset();
      P.A.Scales.clear();
      return P;
    }
    if (P.A.Offset)
      P.A.Offset = *P.A.Offset + ConstOffset.getSExtValue();
    for (auto &[Index, Scale] : VarOffsets)
      P.A.Scales.push_back(Scale.getSExtValue());
    return P;
  }

  // A pointer held by a variable is spelled by the variable name.
  if (std::optional<PtrInfo> D = fromDebugValue(V))
    return *D;

  if (auto *AI = dyn_cast<AllocaInst>(V)) {
    for (const DbgVariableRecord *DVR :
         findDVRDeclares(const_cast<AllocaInst *>(AI))) {
      if (DVR->getExpression()->getNumElements() != 0)
        continue;
      P.A.RootKind = Annotation::Variable;
      P.A.Var = DVR->getVariable();
      P.A.Offset = 0;
      return P;
    }
    return P;
  }

  if (auto *LI = dyn_cast<LoadInst>(V))
    return fromLoad(*LI, Depth);

  if (auto *PN = dyn_cast<PHINode>(V)) {
    SmallVector<const Value *, 4> Incoming(PN->incoming_values());
    return merge(Incoming, Depth);
  }
  if (auto *SI = dyn_cast<SelectInst>(V))
    return merge({SI->getTrueValue(), SI->getFalseValue()}, Depth);

  // (T *)((uintptr_t)p + C), (T *)((uintptr_t)p & M)
  if (auto *ITP = dyn_cast<IntToPtrInst>(V)) {
    const Value *Int = ITP->getOperand(0);
    if (auto *PTI = dyn_cast<PtrToIntOperator>(Int)) {
      P = get(PTI->getPointerOperand(), Depth + 1);
      P.A.Heuristic = true;
      return P;
    }
    auto *BO = dyn_cast<BinaryOperator>(Int);
    if (!BO)
      return P;
    auto *PTI = dyn_cast<PtrToIntOperator>(BO->getOperand(0));
    auto *C = dyn_cast<ConstantInt>(BO->getOperand(1));
    if (!PTI)
      return P;
    P = get(PTI->getPointerOperand(), Depth + 1);
    P.A.Heuristic = true;
    switch (BO->getOpcode()) {
    case Instruction::Add:
    case Instruction::Sub:
      if (C && P.A.Offset) {
        int64_t D = C->getSExtValue();
        P.A.Offset =
            *P.A.Offset + (BO->getOpcode() == Instruction::Add ? D : -D);
      } else {
        P.A.Offset.reset();
      }
      return P;
    case Instruction::And:
    case Instruction::Or:
      P.A.Kind = Annotation::Mask;
      P.A.Offset.reset();
      P.A.Scales.clear();
      return P;
    default:
      P.A.Offset.reset();
      P.A.Scales.clear();
      return P;
    }
  }
  return P;
}

static bool isIntegerArithmetic(const Instruction &I) {
  switch (I.getOpcode()) {
  case Instruction::Add:
  case Instruction::Sub:
  case Instruction::Mul:
  case Instruction::Shl:
  case Instruction::Or:
    break;
  default:
    return false;
  }
  if (!I.getType()->isIntegerTy())
    return false;
  // Arithmetic on an address that is turned back into a pointer is pointer
  // arithmetic; the inttoptr carries the annotation.
  return none_of(I.users(), [](const User *U) { return isa<IntToPtrInst>(U); });
}

static bool annotateFunction(Function &F) {
  Annotator Ann(F.getDataLayout());
  LLVMContext &Ctx = F.getContext();
  unsigned KindID = Ctx.getMDKindID(AnnotationKindName);
  MDNode *IntMD = nullptr;
  bool Changed = false;
  for (Instruction &I : instructions(F)) {
    MDNode *MD = nullptr;
    if (isa<GetElementPtrInst>(I) || isa<IntToPtrInst>(I)) {
      PtrInfo P = Ann.get(&I);
      MD = createAnnotation(Ctx, P.A);
    } else if (isIntegerArithmetic(I)) {
      if (!IntMD) {
        Annotation A;
        A.Kind = Annotation::Integer;
        IntMD = createAnnotation(Ctx, A);
      }
      MD = IntMD;
    }
    if (MD) {
      I.setMetadata(KindID, MD);
      Changed = true;
    }
  }
  return Changed;
}

namespace {

class AddrMapAnnotateLegacy : public FunctionPass {
public:
  static char ID;
  AddrMapAnnotateLegacy() : FunctionPass(ID) {}

  StringRef getPassName() const override { return "AddrMap Annotate"; }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesAll();
  }

  bool runOnFunction(Function &F) override { return annotateFunction(F); }
};

} // end anonymous namespace

char AddrMapAnnotateLegacy::ID = 0;

INITIALIZE_PASS(AddrMapAnnotateLegacy, DEBUG_TYPE,
                "Annotate pointer arithmetic for the addrmap", false, false)

FunctionPass *llvm::createAddrMapAnnotatePass() {
  return new AddrMapAnnotateLegacy();
}
