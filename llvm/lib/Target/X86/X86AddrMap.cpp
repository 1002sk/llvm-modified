//===-- X86AddrMap.cpp - .llvm_addrmap section emission -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "X86AddrMap.h"
#include "MCTargetDesc/X86ATTInstPrinter.h"
#include "MCTargetDesc/X86BaseInfo.h"
#include "X86InstrInfo.h"
#include "X86RegisterInfo.h"
#include "X86Subtarget.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCSectionELF.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MD5.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include <optional>

using namespace llvm;

namespace {

/// The address expression of a target instruction, in the form of an x86
/// memory operand. Instructions that take a symbol as an immediate only fill
/// in Sym and Disp.
struct AddrExpr {
  Register Base;
  Register Index;
  Register Segment;
  unsigned Scale = 1;
  int64_t Disp = 0;
  /// The symbolic part of the displacement or immediate, if any.
  const MachineOperand *Sym = nullptr;
};

/// The object a pointer points into, as far as it can be told from the
/// machine operands.
struct BaseObject {
  StringRef Kind = "unknown";
  std::optional<std::string> Name;
  std::optional<uint64_t> Size;
  /// Why Size is unknown. Only meaningful when Size is not set.
  std::optional<StringRef> SizeUnknown;
  /// Why the base is "unknown".
  std::optional<StringRef> Reason;
};

} // end anonymous namespace

static const char *getMnemonic(unsigned Opcode) {
  switch (Opcode) {
  case X86::LEA64r:
  case X86::TLS_addr64:
  case X86::TLS_base_addr64:
  case X86::TLS_desc64:
    return "leaq";
  case X86::LEA32r:
  case X86::LEA64_32r:
    return "leal";
  case X86::LEA16r:
    return "leaw";
  case X86::MOV64ri:
    return "movabsq";
  case X86::MOV64ri32:
  case X86::MOV64rm:
    return "movq";
  case X86::MOV32ri:
    return "movl";
  default:
    return nullptr;
  }
}

static bool isSymbolOperand(const MachineOperand &MO) {
  return MO.isGlobal() || MO.isSymbol() || MO.isCPI() || MO.isJTI() ||
         MO.isBlockAddress() || MO.isMCSymbol();
}

static bool isGOTLoad(const MachineOperand &MO) {
  switch (MO.getTargetFlags()) {
  case X86II::MO_GOTPCREL:
  case X86II::MO_GOTPCREL_NORELAX:
  case X86II::MO_GOTTPOFF:
    return true;
  default:
    return false;
  }
}

/// Does the base register of a memory operand with symbol \p MO add a variable
/// offset to the symbol? For TLS symbols it holds the thread pointer instead.
static bool isBaseRegVarOffset(const MachineOperand &MO, Register Base) {
  if (!Base || Base == X86::RIP || isGOTLoad(MO))
    return false;
  if (!MO.isGlobal())
    return true;
  return !MO.getGlobal()->isThreadLocal();
}

static int64_t getSymbolOffset(const MachineOperand &MO) {
  return MO.isJTI() ? 0 : MO.getOffset();
}

static StringRef getRegName(Register Reg) {
  return X86ATTInstPrinter::getRegisterName(Reg.asMCReg());
}

static MCSymbol *getOperandSymbol(const MachineOperand &MO, AsmPrinter &AP) {
  switch (MO.getType()) {
  case MachineOperand::MO_GlobalAddress:
    return AP.getSymbol(MO.getGlobal());
  case MachineOperand::MO_ExternalSymbol:
    return AP.GetExternalSymbolSymbol(MO.getSymbolName());
  case MachineOperand::MO_ConstantPoolIndex:
    return AP.GetCPISymbol(MO.getIndex());
  case MachineOperand::MO_JumpTableIndex:
    return AP.GetJTISymbol(MO.getIndex());
  case MachineOperand::MO_BlockAddress:
    return AP.GetBlockAddressSymbol(MO.getBlockAddress());
  case MachineOperand::MO_MCSymbol:
    return MO.getMCSymbol();
  default:
    llvm_unreachable("not a symbol operand");
  }
}

/// Is \p Reg a register that addresses the stack frame of the function?
static bool isFrameRegister(Register Reg, const MachineFunction &MF) {
  if (!Reg)
    return false;
  const auto *TRI = MF.getSubtarget<X86Subtarget>().getRegisterInfo();
  if (Reg == TRI->getStackRegister() || Reg == TRI->getFrameRegister(MF))
    return true;
  return TRI->hasBasePointer(MF) && Reg == TRI->getBaseRegister();
}

static BaseObject getGlobalBase(const GlobalValue &GV, const DataLayout &DL) {
  BaseObject B;
  B.Name = GV.getName().str();
  const auto *GVar = dyn_cast<GlobalVariable>(&GV);
  if (!GVar) {
    B.Reason = isa<Function>(GV) ? "address of a function"
                                 : "address of a non-variable global value";
    return B;
  }

  SmallVector<DIGlobalVariableExpression *, 1> GVEs;
  GVar->getDebugInfo(GVEs);
  const DIGlobalVariable *DIGV = GVEs.empty() ? nullptr : GVEs[0]->getVariable();

  StringRef IRName = GVar->getName();
  if (GVar->isThreadLocal())
    B.Kind = "tls";
  else if (IRName.starts_with(".str"))
    B.Kind = "string_literal";
  else if (IRName.starts_with(".compoundliteral"))
    B.Kind = "compound_literal";
  else if (DIGV && isa_and_nonnull<DILocalScope>(DIGV->getScope()))
    B.Kind = "static_local";
  else
    B.Kind = "global";

  if (B.Kind == "string_literal" || B.Kind == "compound_literal")
    B.Name.reset();
  else if (DIGV && !DIGV->getName().empty())
    B.Name = DIGV->getName().str();
  else if (GVar->hasLocalLinkage())
    B.Reason = "compiler-generated object without debug info";

  Type *Ty = GVar->getValueType();
  uint64_t Size = Ty->isSized() ? DL.getTypeAllocSize(Ty).getFixedValue() : 0;
  if (Size == 0 && GVar->isDeclaration())
    B.SizeUnknown = "incomplete type";
  else
    B.Size = Size;
  return B;
}

static BaseObject getSymbolBase(const MachineOperand &MO, const DataLayout &DL) {
  if (MO.isGlobal())
    return getGlobalBase(*MO.getGlobal(), DL);
  BaseObject B;
  if (MO.isSymbol()) {
    B.Name = MO.getSymbolName();
    B.Reason = "external symbol";
  } else if (MO.isCPI()) {
    B.Reason = "constant pool entry";
  } else if (MO.isJTI()) {
    B.Reason = "jump table";
  } else if (MO.isBlockAddress()) {
    B.Reason = "address of a basic block";
  } else {
    B.Reason = "symbol without IR object";
  }
  return B;
}

static void writeSourceLocation(json::OStream &J, const DebugLoc &DL) {
  const DILocation *Loc = DL.get();
  if (!Loc) {
    J.attribute("source", nullptr);
    return;
  }
  J.attributeObject("source", [&] {
    J.attribute("file", Loc->getFilename());
    J.attribute("line", Loc->getLine());
    J.attribute("column", Loc->getColumn());
    J.attributeArray("inlined_at", [&] {
      for (const DILocation *IA = Loc->getInlinedAt(); IA;
           IA = IA->getInlinedAt()) {
        J.object([&] {
          // IA is the call site, so its scope is the caller.
          J.attribute("function", IA->getScope()->getSubprogram()->getName());
          J.attribute("file", IA->getFilename());
          J.attribute("line", IA->getLine());
          J.attribute("column", IA->getColumn());
        });
      }
    });
  });
}

static void writeOptionalReg(json::OStream &J, StringRef Key, Register Reg) {
  if (Reg)
    J.attribute(Key, getRegName(Reg));
  else
    J.attribute(Key, nullptr);
}

void X86AddrMap::handleInstruction(const MachineInstr &MI, AsmPrinter &AP) {
  // Prologue and epilogue frame manipulation is out of scope.
  if (MI.getFlag(MachineInstr::FrameSetup) ||
      MI.getFlag(MachineInstr::FrameDestroy))
    return;

  unsigned Opcode = MI.getOpcode();
  const char *Mnemonic = getMnemonic(Opcode);
  if (!Mnemonic)
    return;

  const MachineFunction &MF = *MI.getMF();
  AddrExpr Addr;
  bool IsMem = Opcode != X86::MOV64ri && Opcode != X86::MOV64ri32 &&
               Opcode != X86::MOV32ri;
  // The TLS pseudos are lowered to a LEA of the TLS symbol into a fixed
  // register (followed by a call), so the label lands on that LEA.
  bool IsTLSPseudo = Opcode == X86::TLS_addr64 ||
                     Opcode == X86::TLS_base_addr64 ||
                     Opcode == X86::TLS_desc64;
  if (IsMem) {
    const MCInstrDesc &Desc = MI.getDesc();
    int MemOp = 0;
    if (!IsTLSPseudo) {
      MemOp = X86II::getMemoryOperandNo(Desc.TSFlags);
      if (MemOp < 0)
        return;
      MemOp += X86II::getOperandBias(Desc);
    }
    const MachineOperand &DispMO = MI.getOperand(MemOp + X86::AddrDisp);
    Addr.Base = IsTLSPseudo ? Register(X86::RIP)
                            : MI.getOperand(MemOp + X86::AddrBaseReg).getReg();
    Addr.Scale = MI.getOperand(MemOp + X86::AddrScaleAmt).getImm();
    Addr.Index = MI.getOperand(MemOp + X86::AddrIndexReg).getReg();
    Addr.Segment = MI.getOperand(MemOp + X86::AddrSegmentReg).getReg();
    if (DispMO.isImm()) {
      Addr.Disp = DispMO.getImm();
    } else if (isSymbolOperand(DispMO)) {
      Addr.Sym = &DispMO;
      Addr.Disp = getSymbolOffset(DispMO);
    }
  } else {
    const MachineOperand &ImmMO = MI.getOperand(1);
    if (!isSymbolOperand(ImmMO))
      return;
    Addr.Sym = &ImmMO;
    Addr.Disp = getSymbolOffset(ImmMO);
  }

  // Only loads from the GOT produce an address; other loads are not targets.
  bool IsLoad = Opcode == X86::MOV64rm;
  if (IsLoad && !(Addr.Sym && isGOTLoad(*Addr.Sym)))
    return;

  Register Dest = Opcode == X86::TLS_desc64 ? Register(X86::RAX)
                 : IsTLSPseudo                ? Register(X86::RDI)
                                              : MI.getOperand(0).getReg();
  bool Is64BitDest = X86::GR64RegClass.contains(Dest);
  bool IsStack = !Addr.Sym && isFrameRegister(Addr.Base, MF);

  // Classify the instruction. Without annotations from earlier stages, a LEA
  // on plain registers is assumed to derive a pointer from its base register
  // unless its shape says it is integer arithmetic.
  StringRef Kind;
  StringRef Confidence = "heuristic";
  std::optional<StringRef> NotPointerReason;
  if (IsLoad) {
    Kind = "addr_got";
    Confidence = "derived";
  } else if (Addr.Sym) {
    Kind = "addr";
    Confidence = "derived";
  } else if (!Is64BitDest) {
    // Pointers are 64 bits wide on x86-64.
    Kind = "not_pointer";
    Confidence = "derived";
    NotPointerReason = "result narrower than a pointer";
  } else if (IsStack) {
    Kind = "addr";
  } else if (!Addr.Base || Addr.Base == Addr.Index) {
    Kind = "not_pointer";
    NotPointerReason = "integer arithmetic shape (no base, or base == index)";
  } else {
    Kind = "derive";
  }

  BaseObject Base;
  if (Opcode == X86::TLS_base_addr64 ||
      (IsTLSPseudo && Addr.Sym && !Addr.Sym->isGlobal())) {
    Base.Reason = "TLS module base (local dynamic)";
  } else if (Addr.Sym) {
    Base = getSymbolBase(*Addr.Sym, MF.getDataLayout());
  } else if (IsStack) {
    Base.Reason = "stack object; frame index already eliminated";
  } else {
    Base.Kind = "pointee";
    Base.SizeUnknown = "pointee";
    Base.Reason = "pointee type not available";
  }

  // The constant offset from the start of the base object (or from the source
  // pointer for "pointee"). A stack-relative displacement says nothing about
  // the object, so it is left unknown.
  std::optional<int64_t> ConstOffset;
  if (!IsStack)
    ConstOffset = Kind == "addr_got" ? 0 : Addr.Disp;
  bool BaseIsVar = Addr.Sym && isBaseRegVarOffset(*Addr.Sym, Addr.Base);
  bool HasVarOffset = Addr.Index || BaseIsVar;
  bool OutOfBounds = ConstOffset && !HasVarOffset && Base.Size &&
                     (*ConstOffset < 0 || uint64_t(*ConstOffset) >= *Base.Size);

  // Serialize the record.
  unsigned Id = Records.size();
  std::string Json;
  raw_string_ostream OS(Json);
  json::OStream J(OS);
  J.object([&] {
    J.attribute("type", "record");
    J.attribute("id", Id);
    J.attribute("function", MF.getName());
    J.attribute("kind", Kind);
    J.attributeObject("insn", [&] {
      J.attribute("mnemonic", Mnemonic);
      J.attribute("dest_reg", getRegName(Dest));
      J.attributeObject("addr", [&] {
        writeOptionalReg(J, "segment", Addr.Segment);
        writeOptionalReg(J, "base_reg", Addr.Base);
        writeOptionalReg(J, "index_reg", Addr.Index);
        J.attribute("scale", Addr.Scale);
        J.attribute("disp", Addr.Disp);
        if (Addr.Sym)
          J.attribute("symbol", getOperandSymbol(*Addr.Sym, AP)->getName());
        else
          J.attribute("symbol", nullptr);
        J.attribute("rip_relative", Addr.Base == X86::RIP);
      });
    });
    writeSourceLocation(J, MI.getDebugLoc());
    if (Kind == "not_pointer") {
      J.attribute("confidence", Confidence);
      J.attribute("reason", *NotPointerReason);
      return;
    }

    J.attributeObject("base", [&] {
      J.attribute("kind", Base.Kind);
      if (Base.Name)
        J.attribute("name", *Base.Name);
      else
        J.attribute("name", nullptr);
      J.attribute("type", nullptr);
      if (Base.Size)
        J.attribute("type_size", *Base.Size);
      else
        J.attribute("type_size", nullptr);
      J.attributeObject("object", [&] {
        if (Base.Size) {
          J.attribute("size", *Base.Size);
        } else {
          J.attribute("size", nullptr);
          if (Base.SizeUnknown)
            J.attribute("size_unknown", *Base.SizeUnknown);
        }
      });
    });
    if (Kind == "derive") {
      J.attributeObject("src_ptr", [&] {
        J.attribute("expr", nullptr);
        J.attribute("var", nullptr);
        J.attribute("type", nullptr);
        J.attributeArray("derived_from", [] {});
      });
    }
    J.attributeArray("path", [] {});
    // Without type information only the whole object can be named.
    bool IsWholeObject = Base.Name && ConstOffset == 0 && !HasVarOffset;
    J.attribute("path_str", IsWholeObject ? *Base.Name : "");
    J.attributeObject("ptr_offset", [&] {
      if (ConstOffset)
        J.attribute("const", *ConstOffset);
      else
        J.attribute("const", nullptr);
      J.attributeArray("var", [&] {
        if (Addr.Index)
          J.object([&] {
            J.attribute("reg", getRegName(Addr.Index));
            J.attribute("scale", Addr.Scale);
          });
        if (BaseIsVar)
          J.object([&] {
            J.attribute("reg", getRegName(Addr.Base));
            J.attribute("scale", 1);
          });
      });
    });
    J.attribute("result_type", "");
    J.attribute("source_expr", nullptr);
    J.attributeArray("alternatives", [] {});
    J.attribute("out_of_bounds", OutOfBounds);
    J.attribute("confidence", Confidence);
    J.attributeArray("provenance", [&] { J.value("machine-operand"); });
    if (Base.Reason)
      J.attribute("reason", *Base.Reason);
    else
      J.attribute("reason", nullptr);
  });

  MCSymbol *Label = AP.OutContext.createTempSymbol("addrmap");
  AP.OutStreamer->emitLabel(Label);
  Records.push_back({Label, std::move(Json)});
}

static StringRef getOptLevelName(CodeGenOptLevel Level) {
  switch (Level) {
  case CodeGenOptLevel::None:
    return "O0";
  case CodeGenOptLevel::Less:
    return "O1";
  case CodeGenOptLevel::Default:
    return "O2";
  case CodeGenOptLevel::Aggressive:
    return "O3";
  }
  llvm_unreachable("unknown optimization level");
}

void X86AddrMap::emitSection(const Module &M, AsmPrinter &AP) {
  // Record lines. These, together with the target, are the input of the
  // content hash, so the source path does not affect tu_id.
  std::string RecordLines;
  for (const Record &R : Records) {
    RecordLines += R.Json;
    RecordLines += '\n';
  }

  StringRef Target = AP.TM.getTargetTriple().str();
  MD5 Hash;
  Hash.update(Target);
  Hash.update(ArrayRef<uint8_t>{0});
  Hash.update(RecordLines);
  MD5::MD5Result TUId = Hash.final();

  std::string Json;
  raw_string_ostream JS(Json);
  {
    json::OStream J(JS);
    J.object([&] {
      J.attribute("type", "header");
      J.attribute("version", Version);
      J.attribute("tu_id", TUId.digest().str());
      J.attribute("source", M.getSourceFileName());
      J.attribute("target", Target);
      J.attribute("opt_level", getOptLevelName(AP.TM.getOptLevel()));
      J.attribute("producer", "LLVM " LLVM_VERSION_STRING " addrmap-patch");
    });
  }
  JS << '\n' << RecordLines;

  uint32_t EntryCount = Records.size();
  uint32_t JsonOffset = HeaderSize + EntryCount * EntrySize;
  uint32_t JsonSize = Json.size();
  uint32_t UnitSize = alignTo(JsonOffset + JsonSize, 8);

  MCStreamer &OS = *AP.OutStreamer;
  OS.switchSection(
      AP.OutContext.getELFSection(".llvm_addrmap", ELF::SHT_PROGBITS, 0));
  OS.emitValueToAlignment(Align(8));

  // Header.
  OS.emitBytes("AMAP");
  OS.emitInt16(Version);
  OS.emitInt16(HeaderSize);
  OS.emitInt32(UnitSize);
  OS.emitInt32(EntryCount);
  OS.emitInt32(JsonOffset);
  OS.emitInt32(JsonSize);
  OS.emitInt64(0);
  OS.emitBytes(StringRef(reinterpret_cast<const char *>(TUId.data()),
                         TUId.size()));

  // Index entries. There is one record per instruction so far, so flags is
  // always 0.
  for (auto [Id, R] : enumerate(Records)) {
    OS.emitSymbolValue(R.Label, 8);
    OS.emitInt32(Id);
    OS.emitInt32(0);
  }

  // JSON Lines body and padding.
  OS.emitBytes(Json);
  OS.emitZeros(UnitSize - JsonOffset - JsonSize);
}
