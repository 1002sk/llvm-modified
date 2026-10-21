//===-- X86AddrMap.h - .llvm_addrmap section emission -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Collects addrmap records while the X86 AsmPrinter emits machine code and
// serializes them into the non-allocated ELF section .llvm_addrmap at the end
// of the translation unit.
//
// Target instructions are the ones that produce a pointer value:
//   - every LEA,
//   - MOVs that materialize a symbol address as an immediate,
//   - MOVs that load a symbol address from the GOT,
//   - MOVs that copy the stack or frame pointer (the address of a local).
// Each of them gets a temporary label right before it and one JSON record.
//
// Layout of one unit (all little endian):
//   header (48 bytes):
//     char[4]  magic "AMAP"
//     uint16   version
//     uint16   header_size
//     uint32   unit_size     (whole unit including padding, multiple of 8)
//     uint32   entry_count
//     uint32   json_offset   (from the start of the unit)
//     uint32   json_size
//     uint64   reserved
//     uint8[16] tu_id        (content hash)
//   entry_count index entries (16 bytes each):
//     uint64   pc            (R_X86_64_64 to a label before the instruction)
//     uint32   record_id
//     uint32   flags         (bit0: another entry follows for the same pc)
//   JSON Lines body, then zero padding to an 8 byte boundary.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_X86_X86ADDRMAP_H
#define LLVM_LIB_TARGET_X86_X86ADDRMAP_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/AddrMapTypes.h"
#include "llvm/IR/DebugLoc.h"
#include <optional>
#include <string>

namespace llvm {
class AsmPrinter;
class DIType;
class MachineBasicBlock;
class MachineFunction;
class MachineInstr;
class MCSymbol;
class Module;

class X86AddrMap {
public:
  static constexpr uint16_t Version = 1;
  static constexpr uint16_t HeaderSize = 48;
  static constexpr unsigned EntrySize = 16;

  /// If \p MI produces a pointer, emit a label right before it and record it.
  /// Must be called before anything else is emitted for \p MI, and for every
  /// instruction so that the registers holding recorded pointers are tracked.
  void handleInstruction(const MachineInstr &MI, AsmPrinter &AP);

  /// Emit the unit for this translation unit into .llvm_addrmap.
  void emitSection(const Module &M, AsmPrinter &AP);

  void reset() {
    Records.clear();
    Stats = {};
    resetFunction(nullptr);
  }

  /// The object a pointer points into and how to spell it.
  struct BaseObject {
    StringRef Kind = "unknown";
    std::optional<std::string> Name;
    /// The type of the object, or the pointee type for "pointee". Only
    /// meaningful if HasType is set; a null type is void.
    const DIType *Type = nullptr;
    bool HasType = false;
    std::optional<uint64_t> Size;
    std::optional<uint64_t> TypeSize;
    std::optional<StringRef> SizeUnknown;
    std::optional<std::string> Reason;
    /// Where the object is declared, if known.
    StringRef DeclFile;
    unsigned DeclLine = 0;
    /// The root of the access path, e.g. "g" or "p".
    std::string RootExpr;
    /// The origin is the value of a pointer (RootExpr) rather than an object.
    bool ViaPointer = false;
  };

  /// What a recorded instruction left in its destination register.
  struct PointerValue {
    /// The register holding the pointer; 0 if it is not tracked.
    unsigned Dest = 0;
    unsigned RecordId;
    BaseObject Base;
    std::optional<int64_t> ConstOffset;
    SmallVector<addrmap::VarTerm, 2> Vars;
    std::string PathStr;
    std::string ResultType;
  };

  /// One record, kept until the section is emitted because a later
  /// instruction may tell that it computed an intermediate value.
  struct RecordData {
    unsigned Id = 0;
    std::string Function;
    StringRef Kind;
    std::string Mnemonic;
    std::string DestReg;
    /// The address expression; empty registers are absent.
    std::string Segment, BaseReg, IndexReg;
    unsigned Scale = 1;
    int64_t Disp = 0;
    std::optional<std::string> Symbol;
    bool RipRelative = false;
    DebugLoc Loc;
    BaseObject Base;
    std::optional<std::string> SrcExpr, SrcVar, SrcType;
    std::optional<unsigned> DerivedFrom;
    addrmap::ResolvedPath Path;
    /// The access path was resolved from the type of the base.
    bool PathResolved = false;
    std::string PathStr;
    std::optional<int64_t> ConstOffset;
    SmallVector<addrmap::VarTerm, 2> Vars;
    std::string ResultType;
    SmallVector<std::string, 1> SharedSlotVars;
    bool OutOfBounds = false;
    StringRef Confidence;
    SmallVector<StringRef, 4> Provenance;
    std::optional<std::string> Reason;
    /// The record of the pointer this one is a step of the computation of.
    std::optional<unsigned> IntermediateOf;
  };

private:
  struct Record {
    MCSymbol *Label;
    RecordData Data;
  };
  SmallVector<Record, 0> Records;

  /// Mark the unresolved derive records that \p Final was computed from as
  /// intermediate values of it.
  void markIntermediates(const RecordData &Final);

  /// Counters for -addrmap-stats.
  struct {
    unsigned Addr = 0, AddrGOT = 0, Derive = 0, Mask = 0, NotPointer = 0;
    unsigned UnknownBase = 0;
    /// Target instructions that carried an annotation from the IR.
    unsigned Annotated = 0;
    /// Pointer arithmetic recorded without an annotation: either computed by
    /// a later machine pass or an annotation lost on the way.
    unsigned DeriveWithoutAnnotation = 0;
    /// Records found to compute an intermediate value of another one.
    unsigned Intermediate = 0;
  } Stats;

  /// Per-function tracking of the registers that hold the result of a
  /// recorded instruction, for derived_from (R-PTR-3). Only straight-line
  /// code within a basic block is followed.
  const MachineFunction *CurMF = nullptr;
  const MachineBasicBlock *CurMBB = nullptr;
  SmallVector<PointerValue, 0> Values;
  DenseMap<unsigned, unsigned> LiveValues;

  void resetFunction(const MachineFunction *MF);
  std::optional<PointerValue> recordInstruction(const MachineInstr &MI,
                                                AsmPrinter &AP);
  void updateLiveValues(const MachineInstr &MI,
                        std::optional<PointerValue> NewValue);
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_X86_X86ADDRMAP_H
