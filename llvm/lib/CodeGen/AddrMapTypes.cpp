//===- AddrMapTypes.cpp - addrmap access path resolution ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/AddrMapTypes.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/Support/MathExtras.h"

using namespace llvm;
using namespace llvm::addrmap;

const DIType *addrmap::stripTypedefsAndQualifiers(const DIType *Ty) {
  while (auto *DT = dyn_cast_or_null<DIDerivedType>(Ty)) {
    switch (DT->getTag()) {
    case dwarf::DW_TAG_typedef:
    case dwarf::DW_TAG_const_type:
    case dwarf::DW_TAG_volatile_type:
    case dwarf::DW_TAG_restrict_type:
    case dwarf::DW_TAG_atomic_type:
      Ty = DT->getBaseType();
      continue;
    default:
      return Ty;
    }
  }
  return Ty;
}

const DIType *addrmap::getPointeeType(const DIType *Ty, bool &IsPointer) {
  auto *DT = dyn_cast_or_null<DIDerivedType>(stripTypedefsAndQualifiers(Ty));
  IsPointer = DT && DT->getTag() == dwarf::DW_TAG_pointer_type;
  return IsPointer ? DT->getBaseType() : nullptr;
}

namespace {

/// A type, or for a multi-dimensional array, the sub-array type left after
/// selecting the first Dim dimensions.
struct TypeView {
  const DIType *Ty = nullptr;
  unsigned Dim = 0;
};

} // end anonymous namespace

static const DICompositeType *getArrayType(TypeView V) {
  auto *CT =
      dyn_cast_or_null<DICompositeType>(stripTypedefsAndQualifiers(V.Ty));
  if (CT && CT->getTag() == dwarf::DW_TAG_array_type &&
      V.Dim < CT->getElements().size())
    return CT;
  return nullptr;
}

/// The number of elements of dimension \p Dim, or std::nullopt for a flexible
/// array member or a VLA.
static std::optional<uint64_t> getDimCount(const DICompositeType *CT,
                                           unsigned Dim) {
  auto *SR = dyn_cast_or_null<DISubrange>(CT->getElements()[Dim]);
  if (!SR)
    return std::nullopt;
  if (auto *Count = dyn_cast_if_present<ConstantInt *>(SR->getCount())) {
    int64_t C = Count->getSExtValue();
    // GNU zero-length arrays are used as flexible array members.
    if (C <= 0)
      return std::nullopt;
    return C;
  }
  if (auto *UB = dyn_cast_if_present<ConstantInt *>(SR->getUpperBound())) {
    int64_t C = UB->getSExtValue() + 1;
    if (C <= 0)
      return std::nullopt;
    return C;
  }
  return std::nullopt;
}

static TypeView getElementView(TypeView V) {
  const DICompositeType *CT = getArrayType(V);
  assert(CT && "not an array");
  if (V.Dim + 1 < CT->getElements().size())
    return {V.Ty, V.Dim + 1};
  return {CT->getBaseType(), 0};
}

static std::optional<uint64_t> getViewSize(TypeView V) {
  if (const DICompositeType *CT = getArrayType(V)) {
    std::optional<uint64_t> Count = getDimCount(CT, V.Dim);
    std::optional<uint64_t> ElemSize = getViewSize(getElementView(V));
    if (!Count || !ElemSize)
      return std::nullopt;
    return *Count * *ElemSize;
  }
  const DIType *Ty = stripTypedefsAndQualifiers(V.Ty);
  if (!Ty || Ty->isForwardDecl() || Ty->getSizeInBits() == 0)
    return std::nullopt;
  return Ty->getSizeInBits() / 8;
}

std::optional<uint64_t> addrmap::getTypeSize(const DIType *Ty) {
  return getViewSize({Ty, 0});
}

static std::string getArrayDimsSuffix(const DICompositeType *CT,
                                      unsigned FirstDim) {
  std::string S;
  for (unsigned D = FirstDim, E = CT->getElements().size(); D != E; ++D) {
    S += '[';
    if (std::optional<uint64_t> Count = getDimCount(CT, D))
      S += std::to_string(*Count);
    S += ']';
  }
  return S;
}

std::string addrmap::getPointerTypeName(StringRef TypeName) {
  if (TypeName.ends_with("*"))
    return TypeName.str() + "*";
  return TypeName.str() + " *";
}

std::string addrmap::getCTypeName(const DIType *Ty) {
  if (!Ty)
    return "void";
  if (auto *DT = dyn_cast<DIDerivedType>(Ty)) {
    const DIType *Base = DT->getBaseType();
    switch (DT->getTag()) {
    case dwarf::DW_TAG_typedef:
      return DT->getName().str();
    case dwarf::DW_TAG_pointer_type:
      if (isa_and_nonnull<DISubroutineType>(Base))
        return "function *";
      return getPointerTypeName(getCTypeName(Base));
    case dwarf::DW_TAG_const_type:
    case dwarf::DW_TAG_volatile_type: {
      StringRef Qual =
          DT->getTag() == dwarf::DW_TAG_const_type ? "const" : "volatile";
      bool IsPointer;
      getPointeeType(Base, IsPointer);
      if (IsPointer)
        return getCTypeName(Base) + Qual.str();
      return Qual.str() + " " + getCTypeName(Base);
    }
    case dwarf::DW_TAG_atomic_type:
      return "_Atomic " + getCTypeName(Base);
    default:
      return getCTypeName(Base);
    }
  }
  if (auto *CT = dyn_cast<DICompositeType>(Ty)) {
    StringRef Prefix;
    switch (CT->getTag()) {
    case dwarf::DW_TAG_array_type:
      return getCTypeName(CT->getBaseType()) + getArrayDimsSuffix(CT, 0);
    case dwarf::DW_TAG_structure_type:
      Prefix = "struct ";
      break;
    case dwarf::DW_TAG_union_type:
      Prefix = "union ";
      break;
    case dwarf::DW_TAG_enumeration_type:
      Prefix = "enum ";
      break;
    default:
      break;
    }
    StringRef Name = CT->getName();
    return Prefix.str() + (Name.empty() ? "<anonymous>" : Name.str());
  }
  if (isa<DISubroutineType>(Ty))
    return "function";
  return Ty->getName().str();
}

static std::string getViewName(TypeView V) {
  if (V.Dim == 0)
    return getCTypeName(V.Ty);
  const DICompositeType *CT = getArrayType(V);
  return getCTypeName(CT->getBaseType()) + getArrayDimsSuffix(CT, V.Dim);
}

/// The members of a struct or union that a pointer can point to.
static SmallVector<const DIDerivedType *, 8>
getMembers(const DICompositeType *CT) {
  SmallVector<const DIDerivedType *, 8> Members;
  for (const DINode *N : CT->getElements()) {
    auto *M = dyn_cast_or_null<DIDerivedType>(N);
    if (M && M->getTag() == dwarf::DW_TAG_member && !M->isStaticMember() &&
        !M->isBitField())
      Members.push_back(M);
  }
  return Members;
}

static bool isRecord(const DICompositeType *CT) {
  return CT && (CT->getTag() == dwarf::DW_TAG_structure_type ||
                CT->getTag() == dwarf::DW_TAG_union_type ||
                CT->getTag() == dwarf::DW_TAG_class_type);
}

/// A pointer to the start of an aggregate also points to its first member or
/// element (R-PATH-5). Record those interpretations as alternatives. A union
/// branches into one alternative per member.
static void addOffsetZeroAlternatives(TypeView V, ResolvedPath &R) {
  std::string Reason = "offset 0 of " + getViewName(V);
  SmallVector<AltStep, 2> Chain;
  for (unsigned Depth = 0; Depth != 32; ++Depth) {
    if (getArrayType(V)) {
      Chain.push_back({false, "0"});
      V = getElementView(V);
      continue;
    }
    auto *CT =
        dyn_cast_or_null<DICompositeType>(stripTypedefsAndQualifiers(V.Ty));
    if (!isRecord(CT))
      break;
    SmallVector<const DIDerivedType *, 8> Members = getMembers(CT);
    if (CT->getTag() == dwarf::DW_TAG_union_type) {
      for (const DIDerivedType *M : Members) {
        Alternative A{Chain, Reason};
        A.Steps.push_back({true, M->getName().str()});
        R.Alternatives.push_back(std::move(A));
      }
      return;
    }
    if (Members.empty() || Members.front()->getOffsetInBits() != 0)
      break;
    Chain.push_back({true, Members.front()->getName().str()});
    V = {Members.front()->getBaseType(), 0};
  }
  if (!Chain.empty())
    R.Alternatives.push_back({Chain, Reason});
}

static void setArrayInfo(PathStep &S, TypeView V) {
  const DICompositeType *CT = getArrayType(V);
  if (!CT)
    return;
  S.IsArray = true;
  S.Length = getDimCount(CT, V.Dim);
  S.Flexible = !S.Length;
  S.Stride = getViewSize(getElementView(V)).value_or(0);
}

ResolvedPath addrmap::resolvePath(const DIType *Root, int64_t Offset,
                                  ArrayRef<VarTerm> Vars, bool ViaPointer) {
  ResolvedPath R;
  TypeView Cur{Root, 0};
  R.FinalTypeName = getCTypeName(Root);

  std::optional<uint64_t> RootSize = getViewSize(Cur);
  const DIType *Stripped = stripTypedefsAndQualifiers(Root);
  if (!Stripped || !RootSize || *RootSize == 0) {
    R.Unresolved = "size of " + R.FinalTypeName + " is unknown";
    R.HasUnmatchedVar = !Vars.empty();
    return R;
  }
  // Arithmetic on a void or char pointer is only a byte offset (R-PTR-6).
  if (ViaPointer && isa<DIBasicType>(Stripped) && *RootSize == 1) {
    R.Unresolved = "byte-addressed pointee " + R.FinalTypeName;
    R.HasUnmatchedVar = !Vars.empty();
    return R;
  }

  size_t NextVar = 0;
  bool AbsKnown = true;
  int64_t Abs = 0;
  int64_t Off = Offset;

  // Descend into element \p Elem (of size \p ElemSize) of the array \p Count
  // elements long that starts at the current position. Returns false if the
  // subscript is out of bounds.
  auto PushIndex = [&](TypeView Elem, uint64_t ElemSize,
                       std::optional<uint64_t> Count, bool UseVar) {
    int64_t K = divideFloorSigned(Off, int64_t(ElemSize));
    PathStep S;
    S.Kind = PathStep::Index;
    S.Type = getViewName(Elem);
    S.Size = ElemSize;
    setArrayInfo(S, Elem);
    if (UseVar) {
      S.IndexReg = Vars[NextVar++].Reg;
      AbsKnown = false;
    } else {
      S.IndexValue = K;
      S.OffsetInParent = K * int64_t(ElemSize);
      if (AbsKnown)
        S.Offset = Abs + K * int64_t(ElemSize);
    }
    R.Steps.push_back(std::move(S));
    Abs += K * int64_t(ElemSize);
    Off -= K * int64_t(ElemSize);
    Cur = Elem;
    if (UseVar)
      return true;
    if (K < 0 || (Count && uint64_t(K) >= *Count)) {
      R.OutOfBounds = true;
      return false;
    }
    return true;
  };

  // Does the pending variable term step through elements of \p ElemSize?
  // A scale of 1 is a byte offset computed earlier, which is taken to be the
  // subscript of the outermost array.
  auto VarMatches = [&](uint64_t ElemSize) {
    return NextVar < Vars.size() &&
           (Vars[NextVar].Scale == ElemSize || Vars[NextVar].Scale == 1);
  };

  bool Done = false;
  if (ViaPointer) {
    // Pointer arithmetic on the pointer itself: p + i, p - 1.
    bool UseVar = NextVar < Vars.size() && Vars[NextVar].Scale == *RootSize;
    if (UseVar || Off < 0 || uint64_t(Off) >= *RootSize)
      Done = !PushIndex(Cur, *RootSize, std::nullopt, UseVar);
  } else if (!getArrayType(Cur) && NextVar == Vars.size() &&
             (Off < 0 || uint64_t(Off) >= *RootSize)) {
    R.OutOfBounds = true;
    Done = true;
  }

  for (unsigned Depth = 0; !Done && Depth != 64; ++Depth) {
    if (const DICompositeType *CT = getArrayType(Cur)) {
      TypeView Elem = getElementView(Cur);
      std::optional<uint64_t> ElemSize = getViewSize(Elem);
      if (!ElemSize || *ElemSize == 0)
        break;
      bool UseVar = VarMatches(*ElemSize);
      if (Off == 0 && !UseVar) {
        addOffsetZeroAlternatives(Cur, R);
        break;
      }
      if (!PushIndex(Elem, *ElemSize, getDimCount(CT, Cur.Dim), UseVar))
        break;
      continue;
    }

    if (Off == 0 && NextVar == Vars.size()) {
      addOffsetZeroAlternatives(Cur, R);
      break;
    }
    auto *CT =
        dyn_cast_or_null<DICompositeType>(stripTypedefsAndQualifiers(Cur.Ty));
    if (!isRecord(CT)) {
      R.ByteOffsetInField = Off;
      break;
    }

    SmallVector<const DIDerivedType *, 2> Candidates;
    for (const DIDerivedType *M : getMembers(CT)) {
      int64_t MOff = M->getOffsetInBits() / 8;
      std::optional<uint64_t> MSize = getTypeSize(M->getBaseType());
      const DICompositeType *MArray = getArrayType({M->getBaseType(), 0});
      bool Flexible = MArray && !getDimCount(MArray, 0);
      if (Off >= MOff && (Flexible || (MSize && Off < MOff + int64_t(*MSize))))
        Candidates.push_back(M);
    }
    if (Candidates.empty()) {
      // Padding.
      R.ByteOffsetInField = Off;
      break;
    }
    if (Candidates.size() > 1) {
      // Overlapping union members (R-PATH-4).
      for (const DIDerivedType *M : Candidates)
        R.Alternatives.push_back(
            {{{true, M->getName().str()}}, "member of " + getViewName(Cur)});
      break;
    }

    const DIDerivedType *M = Candidates.front();
    int64_t MOff = M->getOffsetInBits() / 8;
    PathStep S;
    S.Kind = PathStep::Field;
    S.Name = M->getName().str();
    S.Record = getViewName(Cur);
    S.Type = getCTypeName(M->getBaseType());
    S.OffsetInParent = MOff;
    if (AbsKnown)
      S.Offset = Abs + MOff;
    S.Size = getTypeSize(M->getBaseType());
    setArrayInfo(S, {M->getBaseType(), 0});
    R.Steps.push_back(std::move(S));
    Abs += MOff;
    Off -= MOff;
    Cur = {M->getBaseType(), 0};
  }

  R.HasUnmatchedVar = NextVar < Vars.size();
  R.FinalTypeName = getViewName(Cur);
  return R;
}

std::string addrmap::formatPath(StringRef RootExpr, bool ViaPointer,
                                ArrayRef<PathStep> Steps,
                                ArrayRef<AltStep> Extra) {
  std::string S = RootExpr.str();
  bool First = true;
  auto Emit = [&](bool IsField, StringRef Text) {
    if (IsField) {
      // Members of anonymous structs and unions are accessed directly.
      if (Text.empty())
        return;
      S += First && ViaPointer ? "->" : ".";
      S += Text;
    } else {
      S += "[" + Text.str() + "]";
    }
    First = false;
  };
  for (const PathStep &Step : Steps) {
    if (Step.Kind == PathStep::Field)
      Emit(true, Step.Name);
    else
      Emit(false, Step.IndexValue ? std::to_string(*Step.IndexValue) : "?");
  }
  for (const AltStep &Step : Extra)
    Emit(Step.IsField, Step.Name);
  return S;
}
