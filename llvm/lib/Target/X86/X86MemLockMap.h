//===-- X86MemLockMap.h - .memlock_map section emission ---------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Lists the memory accesses that may touch memory shared between threads in
// the non-allocated ELF section .memlock_map. A debugging tool takes a lock
// only for the listed instructions.
//
// There is one .memlock_map section per text section (SHF_LINK_ORDER, in the
// COMDAT group of the text section), and one record per function fragment
// (all little endian):
//   uint8    version        (1)
//   uint8    flags          (bit0: all mode)
//   uint64   base_address   (R_X86_64_64 to the fragment start)
//   ULEB128  fragment_size
//   ULEB128  name length, then the function name (no NUL)
//   ULEB128  num_entries
//   entries, in increasing offset order:
//     ULEB128  offset       (fragment start to the instruction start)
//     uint8    kind         (0: instruction, 1: range)
//     ULEB128  length       (only if kind is 1)
//     uint8    reason       (bit7 set: no lock needed)
// Offsets and sizes are label differences resolved by the assembler.
//
// FIXME: The reason codes are placeholders; the sharing analysis is not
// implemented yet. Inline assembly and instructions emitted by the MC layer
// are not listed yet.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_X86_X86MEMLOCKMAP_H
#define LLVM_LIB_TARGET_X86_X86MEMLOCKMAP_H

#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/Target/TargetOptions.h"
#include <cstdint>

namespace llvm {
class AsmPrinter;
class MachineInstr;
class MCSymbol;

class X86MemLockMap {
public:
  static constexpr uint8_t Version = 1;

  enum : uint8_t { FlagAll = 1 };

  enum EntryKind : uint8_t { KindInstruction = 0, KindRange = 1 };

  /// Reason codes. Bit 7 set means that no lock is needed.
  enum Reason : uint8_t {
    NoMemOperand = 0x01,
    NotAlloca = 0x02,
    Captured = 0x03,
    InlineAsm = 0x05,
    OtherPseudoSource = 0x06,
    MCLayer = 0x07,
    Unexpected = 0x09,
    LocalNotCaptured = 0x81,
    StackSlot = 0x82,
    StackPointerImplicit = 0x83,
    ReadOnly = 0x84,
  };

  static bool needsLock(uint8_t R) { return !(R & 0x80); }

  explicit X86MemLockMap(MemLockMapMode Mode = MemLockMapMode::None)
      : Mode(Mode) {}

  void setMode(MemLockMapMode M) { Mode = M; }
  bool isEnabled() const { return Mode != MemLockMapMode::None; }

  /// If \p MI accesses memory, emit a label right before it and record it.
  /// Must be called before anything else is emitted for \p MI.
  void handleInstruction(const MachineInstr &MI, AsmPrinter &AP);

  /// Emit one record per fragment of the current function. Must be called
  /// after the function body has been emitted.
  void endFunction(AsmPrinter &AP);

private:
  struct Entry {
    MCSymbol *Label;
    EntryKind Kind;
    /// The end of the range, for KindRange.
    MCSymbol *End;
    uint8_t Reason;
  };

  /// Entries of the current function, per fragment.
  MapVector<MBBSectionID, SmallVector<Entry, 16>> Entries;
  MemLockMapMode Mode;
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_X86_X86MEMLOCKMAP_H
