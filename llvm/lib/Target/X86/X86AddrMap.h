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
//   - MOVs that load a symbol address from the GOT.
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

#include "llvm/ADT/SmallVector.h"
#include <string>

namespace llvm {
class AsmPrinter;
class MachineInstr;
class MCSymbol;
class Module;

class X86AddrMap {
public:
  static constexpr uint16_t Version = 1;
  static constexpr uint16_t HeaderSize = 48;
  static constexpr unsigned EntrySize = 16;

  /// If \p MI produces a pointer, emit a label right before it and record it.
  /// Must be called before anything else is emitted for \p MI.
  void handleInstruction(const MachineInstr &MI, AsmPrinter &AP);

  /// Emit the unit for this translation unit into .llvm_addrmap.
  void emitSection(const Module &M, AsmPrinter &AP);

  void reset() { Records.clear(); }

private:
  struct Record {
    MCSymbol *Label;
    /// The serialized JSON object, without the trailing newline.
    std::string Json;
  };
  SmallVector<Record, 0> Records;
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_X86_X86ADDRMAP_H
