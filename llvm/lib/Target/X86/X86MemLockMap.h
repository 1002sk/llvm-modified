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
// The labels are emitted per MCInst, right before the AsmPrinter emits it (or
// before the prefixes emitted as separate MCInsts in front of it), so that
// the instructions emitted for a MachineInstr by custom lowering are listed
// too.
//
// The memory accesses are the instructions with mayLoadOrStore(), the ones
// accessing the stack implicitly (PUSH, POP, CALL, RET, LEAVE) and inline
// assembly (as a range). The MCInst lowered from a MachineInstr in the usual
// way is classified by the MachineInstr; an instruction needs no lock only if
// each of its accesses is one of:
//   A. a local variable (alloca) that is not captured,
//   B. a stack slot created by the compiler,
//   C. the stack, accessed implicitly through the stack pointer,
//   D. a read of the GOT, the constant pool or a jump table.
// Everything else, including missing information, needs a lock. The memory
// accesses among the other MCInsts (custom lowering such as TLS_addr64, XRay
// sleds, KCFI checks, FAULTING_OP, STATEPOINT) need a lock (0x07, or 0x09 for
// the TLSDESC call).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_X86_X86MEMLOCKMAP_H
#define LLVM_LIB_TARGET_X86_X86MEMLOCKMAP_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/Target/TargetOptions.h"
#include <cstdint>
#include <optional>

namespace llvm {
class AllocaInst;
class AsmPrinter;
class MachineFunction;
class MachineInstr;
class MachineMemOperand;
class MCInst;
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

  /// Start emitting \p MI. Must be called before any MCInst is emitted for
  /// \p MI.
  void handleInstruction(const MachineInstr &MI, AsmPrinter &AP);

  /// If \p Inst accesses memory, emit a label for it and record it. Must be
  /// called right before \p Inst is emitted, for every MCInst emitted by the
  /// AsmPrinter except nops.
  void handleMCInst(const MCInst &Inst, AsmPrinter &AP);

  /// Set while the MCInst lowered from the current MachineInstr in the usual
  /// way is emitted. That one is classified by the MachineInstr.
  void setDirect(bool D) { Direct = D; }

  /// Emit a label at the start of an inline assembly block.
  void beginInlineAsm(AsmPrinter &AP);

  /// Record the inline assembly block started by beginInlineAsm() as a range.
  /// \p MI is null for module level inline assembly.
  void endInlineAsm(const MachineInstr *MI, AsmPrinter &AP);

  /// Emit one record per fragment of the current function. Must be called
  /// after the function body has been emitted.
  void endFunction(AsmPrinter &AP);

  /// Whether \p Opcode accesses the stack through the stack pointer without
  /// an explicit memory operand (rule C).
  static bool isImplicitStackAccess(unsigned Opcode);

private:
  /// The reason code for \p MI, which accesses memory.
  uint8_t classify(const MachineInstr &MI);
  uint8_t classifyMemOperand(const MachineMemOperand &MMO,
                             const MachineFunction &MF);
  bool isCaptured(const AllocaInst *AI);

  struct Entry {
    MCSymbol *Label;
    EntryKind Kind;
    /// The end of the range, for KindRange.
    MCSymbol *End;
    uint8_t Reason;
  };

  /// Entries of the current function, per fragment.
  MapVector<MBBSectionID, SmallVector<Entry, 16>> Entries;
  /// Capture results for the allocas of the current function.
  DenseMap<const AllocaInst *, bool> CaptureCache;
  /// The start of the inline assembly block being emitted.
  MCSymbol *InlineAsmBegin = nullptr;
  /// The last MachineInstr of the current function, if any.
  const MachineInstr *CurMI = nullptr;
  /// The reason code of CurMI, if it accesses memory and its own MCInst has
  /// not been emitted yet.
  std::optional<uint8_t> CurReason;
  /// The label in front of the prefixes emitted so far for the next MCInst.
  MCSymbol *PrefixLabel = nullptr;
  bool Direct = false;
  MemLockMapMode Mode;
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_X86_X86MEMLOCKMAP_H
