//===- llvm/CodeGen/AddrMapTypes.h - addrmap path resolution -- -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Helpers for the addrmap (.llvm_addrmap) emission that work on debug info
// types: naming C types, and turning "base object + byte offset (+ variable
// terms)" into an access path of field selections and array subscripts, with
// the range of every sub-object on the way.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CODEGEN_ADDRMAPTYPES_H
#define LLVM_CODEGEN_ADDRMAPTYPES_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Compiler.h"
#include <cstdint>
#include <optional>
#include <string>

namespace llvm {
class DILocalVariable;
class DIType;
class GlobalVariable;
class LLVMContext;
class MDNode;

namespace addrmap {

/// Strip typedefs and qualifiers.
LLVM_ABI const DIType *stripTypedefsAndQualifiers(const DIType *Ty);

/// If \p Ty is (a typedef of, or a qualified) pointer type, return the type
/// it points to, which is null for void. Otherwise set \p IsPointer to false.
LLVM_ABI const DIType *getPointeeType(const DIType *Ty, bool &IsPointer);

/// The size of \p Ty in bytes, or std::nullopt if it is not known statically
/// (void, incomplete types, flexible array members, VLAs).
LLVM_ABI std::optional<uint64_t> getTypeSize(const DIType *Ty);

/// The C spelling of \p Ty, e.g. "struct Point", "int *", "int[4][2]".
LLVM_ABI std::string getCTypeName(const DIType *Ty);

/// The C spelling of a pointer to the type spelled \p TypeName.
LLVM_ABI std::string getPointerTypeName(StringRef TypeName);

/// A term of the offset that is only known at run time: Reg * Scale.
struct VarTerm {
  std::string Reg;
  unsigned Scale;
};

/// One step of an access path.
struct PathStep {
  enum StepKind { Field, Index } Kind;
  /// Field: the member name; empty for an anonymous struct/union member.
  std::string Name;
  /// Field: the type of the enclosing struct/union.
  std::string Record;
  /// Index: the subscript if it is a constant.
  std::optional<int64_t> IndexValue;
  /// Index: the register holding a variable subscript.
  std::optional<std::string> IndexReg;
  /// The C type of the selected sub-object.
  std::string Type;
  /// Offset from the enclosing sub-object, unknown below a variable subscript
  /// only for Index steps.
  std::optional<int64_t> OffsetInParent;
  /// Offset from the origin of the base, unknown below a variable subscript.
  std::optional<int64_t> Offset;
  std::optional<uint64_t> Size;
  /// Set for a step selecting an array (a field of array type, or an element
  /// that is itself an array).
  bool IsArray = false;
  std::optional<uint64_t> Length;
  uint64_t Stride = 0;
  bool Flexible = false;
};

/// A step of an alternative interpretation, appended to the resolved path.
struct AltStep {
  bool IsField;
  std::string Name;
};

struct Alternative {
  SmallVector<AltStep, 2> Steps;
  std::string Reason;
  /// The type selected by the alternative path, if it is a plain type.
  const DIType *Type = nullptr;
};

struct ResolvedPath {
  SmallVector<PathStep, 4> Steps;
  /// Bytes from the start of the innermost selected sub-object to the pointer
  /// (R-PATH-2), when the pointer lands inside a scalar or padding.
  int64_t ByteOffsetInField = 0;
  bool OutOfBounds = false;
  /// Other paths that denote the same address.
  SmallVector<Alternative, 1> Alternatives;
  /// The variable terms that could not be matched to an array subscript.
  bool HasUnmatchedVar = false;
  /// The C type of the innermost selected sub-object (the root type if Steps
  /// is empty).
  std::string FinalTypeName;
  /// The same type, unless it is the sub-array of a multi-dimensional array.
  const DIType *FinalType = nullptr;
  /// Why no path could be computed, if so.
  std::optional<std::string> Unresolved;
  /// The root is the pointee of a void or char pointer, whose arithmetic is a
  /// byte offset only (R-PTR-6).
  bool ByteAddressed = false;
};

/// Resolve the access path for a pointer to \p Root + \p Offset + sum(\p Vars).
/// When \p ViaPointer is set, \p Root is the pointee of a pointer whose value
/// is the origin, so offsets outside of it (and variable terms that do not
/// fall into an array) are taken as subscripts of the pointer itself.
LLVM_ABI ResolvedPath resolvePath(const DIType *Root, int64_t Offset,
                                  ArrayRef<VarTerm> Vars, bool ViaPointer);

/// Spell a path like "p->items[?].pos" or "g.b.y". \p ViaPointer selects "->"
/// (or a subscript) for the first step.
LLVM_ABI std::string formatPath(StringRef RootExpr, bool ViaPointer,
                                ArrayRef<PathStep> Steps,
                                ArrayRef<AltStep> Extra = {});

/// The !addrmap annotation that the AddrMapAnnotate pass attaches to IR
/// instructions and instruction selection carries to the MachineInstr that
/// defines the value. It describes the value computed by the instruction:
///   !{!"int"}  an integer computation (a LEA made from it is not a pointer)
///   !{!"ptr" | !"mask", !"<root kind>", <root>, i64 <offset> | null,
///     !{i64 <scale>, ...}, !"<root expr>", <source pointer type> | null,
///     i1 <heuristic>}
/// where the root is a GlobalVariable ("global"), a DILocalVariable whose
/// stack slot is the object ("var"), or the pointee DIType of a pointer value
/// spelled by the root expression ("pointee", null for void).
struct Annotation {
  enum KindTy { Pointer, Mask, Integer } Kind = Pointer;
  enum RootKindTy { Unknown, Global, Variable, Pointee } RootKind = Unknown;
  const GlobalVariable *GV = nullptr;
  const DILocalVariable *Var = nullptr;
  const DIType *PointeeType = nullptr;
  std::string RootExpr;
  const DIType *SrcType = nullptr;
  std::optional<int64_t> Offset;
  SmallVector<int64_t, 2> Scales;
  bool Heuristic = false;
};

/// The metadata kind name of the annotation.
inline constexpr StringRef AnnotationKindName = "addrmap";

LLVM_ABI MDNode *createAnnotation(LLVMContext &Ctx, const Annotation &A);
LLVM_ABI std::optional<Annotation> parseAnnotation(const MDNode *MD);

} // namespace addrmap
} // namespace llvm

#endif // LLVM_CODEGEN_ADDRMAPTYPES_H
