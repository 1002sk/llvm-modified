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
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/TargetFrameLowering.h"
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

using BaseObject = X86AddrMap::BaseObject;
using PointerValue = X86AddrMap::PointerValue;

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

/// A variable whose stack slot contains the address of an instruction.
struct StackVariable {
  const DILocalVariable *Var;
  /// The offset of the address from the start of the variable.
  int64_t Offset;
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
  case X86::MOV64rr:
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
  B.RootExpr = *B.Name;
  const auto *GVar = dyn_cast<GlobalVariable>(&GV);
  if (!GVar) {
    B.Reason = isa<Function>(GV) ? "address of a function"
                                 : "address of a non-variable global value";
    return B;
  }

  SmallVector<DIGlobalVariableExpression *, 1> GVEs;
  GVar->getDebugInfo(GVEs);
  const DIGlobalVariable *DIGV =
      GVEs.empty() ? nullptr : GVEs[0]->getVariable();

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

  if (B.Kind == "string_literal" || B.Kind == "compound_literal") {
    B.Name.reset();
    B.RootExpr =
        B.Kind == "string_literal" ? "<string literal>" : "<compound literal>";
  } else if (DIGV && !DIGV->getName().empty()) {
    B.Name = DIGV->getName().str();
    B.RootExpr = *B.Name;
  } else if (GVar->hasLocalLinkage()) {
    B.Reason = "compiler-generated object without debug info";
  }

  if (DIGV) {
    B.Type = DIGV->getType();
    B.HasType = true;
    B.DeclFile = DIGV->getFilename();
    B.DeclLine = DIGV->getLine();
  }

  Type *Ty = GVar->getValueType();
  uint64_t Size = Ty->isSized() ? DL.getTypeAllocSize(Ty).getFixedValue() : 0;
  if (Size == 0 && GVar->isDeclaration())
    B.SizeUnknown = "incomplete type";
  else
    B.Size = Size;
  B.TypeSize = B.HasType ? addrmap::getTypeSize(B.Type) : B.Size;
  if (!B.TypeSize)
    B.TypeSize = B.Size;
  return B;
}

static BaseObject getSymbolBase(const MachineOperand &MO, const DataLayout &DL,
                                StringRef SymbolName) {
  if (MO.isGlobal())
    return getGlobalBase(*MO.getGlobal(), DL);
  BaseObject B;
  B.RootExpr = SymbolName.str();
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

static bool isScopeWithin(const DILocalScope *Scope, const DIScope *Outer) {
  for (const DIScope *S = Scope; S; S = S->getScope()) {
    if (S == Outer)
      return true;
    if (isa<DISubprogram>(S))
      break;
  }
  return false;
}

/// Find the variables whose stack slot contains Base + Disp. The frame index
/// operand is gone after prologue/epilogue insertion, but the slots of the
/// variables described by dbg.declare are still known, so the address can be
/// matched against them.
static SmallVector<StackVariable, 1>
findStackVariables(const MachineFunction &MF, Register Base, int64_t Disp,
                   const DILocation *Loc) {
  const MachineFrameInfo &MFI = MF.getFrameInfo();
  const TargetFrameLowering *TFI = MF.getSubtarget().getFrameLowering();
  const auto *TRI = MF.getSubtarget<X86Subtarget>().getRegisterInfo();
  SmallVector<StackVariable, 1> All, InScope;

  for (const MachineFunction::VariableDbgInfo &VI :
       MF.getInStackSlotVariableDbgInfo()) {
    int FI = VI.getStackSlot();
    if (FI < MFI.getObjectIndexBegin() || FI >= MFI.getObjectIndexEnd() ||
        MFI.isDeadObjectIndex(FI))
      continue;
    int64_t ExprOffset = 0;
    if (VI.Expr && !VI.Expr->extractIfOffset(ExprOffset))
      continue;
    int64_t Size =
        addrmap::getTypeSize(VI.Var->getType()).value_or(MFI.getObjectSize(FI));

    auto Matches = [&](Register Reg, StackOffset Off) {
      int64_t Start = Off.getFixed() + ExprOffset;
      return Reg == Base && Disp >= Start &&
             Disp < Start + std::max<int64_t>(Size, 1);
    };
    Register Reg;
    StackOffset Off = TFI->getFrameIndexReference(MF, FI, Reg);
    if (!Matches(Reg, Off) && Base == TRI->getStackRegister() && Reg != Base) {
      Off = TFI->getFrameIndexReferencePreferSP(MF, FI, Reg,
                                                /*IgnoreSPUpdates=*/false);
    }
    if (!Matches(Reg, Off))
      continue;
    if (any_of(All, [&](const StackVariable &S) { return S.Var == VI.Var; }))
      continue;

    StackVariable SV{VI.Var, Disp - (Off.getFixed() + ExprOffset)};
    All.push_back(SV);
    if (Loc && VI.Loc && VI.Loc->getInlinedAt() == Loc->getInlinedAt() &&
        isScopeWithin(Loc->getScope(), VI.Var->getScope()))
      InScope.push_back(SV);
  }
  return InScope.empty() ? All : InScope;
}

/// Find the variable whose value \p Reg holds at \p MI, from the DBG_VALUEs
/// earlier in the same basic block.
static const DILocalVariable *findVariableInRegister(const MachineInstr &MI,
                                                     Register Reg) {
  const MachineBasicBlock &MBB = *MI.getParent();
  const TargetRegisterInfo *TRI =
      MBB.getParent()->getSubtarget().getRegisterInfo();
  for (auto It = std::next(MI.getReverseIterator()), E = MBB.instr_rend();
       It != E; ++It) {
    const MachineInstr &Prev = *It;
    if (Prev.isNonListDebugValue()) {
      const MachineOperand &Op = Prev.getDebugOperand(0);
      if (Op.isReg() && Op.getReg() == Reg && !Prev.isIndirectDebugValue() &&
          Prev.getDebugExpression()->getNumElements() == 0)
        return Prev.getDebugVariable();
      continue;
    }
    if (Prev.isDebugInstr())
      continue;
    if (Prev.modifiesRegister(Reg, TRI))
      return nullptr;
  }
  return nullptr;
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

template <typename T>
static void writeOptional(json::OStream &J, StringRef Key,
                          const std::optional<T> &V) {
  if (V)
    J.attribute(Key, *V);
  else
    J.attribute(Key, nullptr);
}

static void writePathStep(json::OStream &J, const addrmap::PathStep &S) {
  J.object([&] {
    if (S.Kind == addrmap::PathStep::Field) {
      J.attribute("op", "field");
      if (S.Name.empty())
        J.attribute("name", nullptr);
      else
        J.attribute("name", S.Name);
      J.attribute("record", S.Record);
    } else {
      J.attribute("op", "index");
      writeOptional(J, "index", S.IndexValue);
      writeOptional(J, "index_reg", S.IndexReg);
    }
    J.attribute("type", S.Type);
    writeOptional(J, "offset_in_parent", S.OffsetInParent);
    writeOptional(J, "offset", S.Offset);
    writeOptional(J, "size", S.Size);
    if (S.IsArray) {
      writeOptional(J, "length", S.Length);
      J.attribute("stride", S.Stride);
      if (S.Flexible)
        J.attribute("flexible", true);
    }
  });
}

void X86AddrMap::resetFunction(const MachineFunction *MF) {
  CurMF = MF;
  CurMBB = nullptr;
  Values.clear();
  LiveValues.clear();
}

void X86AddrMap::handleInstruction(const MachineInstr &MI, AsmPrinter &AP) {
  if (MI.getMF() != CurMF)
    resetFunction(MI.getMF());
  if (MI.getParent() != CurMBB) {
    CurMBB = MI.getParent();
    LiveValues.clear();
  }
  updateLiveValues(MI, recordInstruction(MI, AP));
}

void X86AddrMap::updateLiveValues(const MachineInstr &MI,
                                  std::optional<PointerValue> NewValue) {
  const TargetRegisterInfo *TRI = MI.getMF()->getSubtarget().getRegisterInfo();
  SmallVector<unsigned, 4> Clobbered;
  for (const MachineOperand &MO : MI.operands()) {
    for (auto [Reg, Idx] : LiveValues) {
      if ((MO.isRegMask() && MO.clobbersPhysReg(Reg)) ||
          (MO.isReg() && MO.isDef() && MO.getReg().isPhysical() &&
           TRI->regsOverlap(Reg, MO.getReg())))
        Clobbered.push_back(Reg);
    }
  }
  for (unsigned Reg : Clobbered)
    LiveValues.erase(Reg);

  if (NewValue && NewValue->Dest) {
    LiveValues[NewValue->Dest] = Values.size();
    Values.push_back(std::move(*NewValue));
  }
}

std::optional<PointerValue>
X86AddrMap::recordInstruction(const MachineInstr &MI, AsmPrinter &AP) {
  // Prologue and epilogue frame manipulation is out of scope.
  if (MI.getFlag(MachineInstr::FrameSetup) ||
      MI.getFlag(MachineInstr::FrameDestroy))
    return std::nullopt;

  unsigned Opcode = MI.getOpcode();
  const char *Mnemonic = getMnemonic(Opcode);
  if (!Mnemonic)
    return std::nullopt;

  const MachineFunction &MF = *MI.getMF();
  AddrExpr Addr;
  bool IsMem = Opcode != X86::MOV64ri && Opcode != X86::MOV64ri32 &&
               Opcode != X86::MOV32ri && Opcode != X86::MOV64rr;
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
        return std::nullopt;
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
  } else if (Opcode == X86::MOV64rr) {
    // A copy of the stack or frame pointer is the address of the object at
    // offset 0 from it, e.g. "movq %rsp, %rdi" for "sink(a)".
    Register Src = MI.getOperand(1).getReg();
    if (!isFrameRegister(Src, MF))
      return std::nullopt;
    Addr.Base = Src;
  } else {
    const MachineOperand &ImmMO = MI.getOperand(1);
    if (!isSymbolOperand(ImmMO))
      return std::nullopt;
    Addr.Sym = &ImmMO;
    Addr.Disp = getSymbolOffset(ImmMO);
  }

  // Only loads from the GOT produce an address; other loads are not targets.
  bool IsLoad = Opcode == X86::MOV64rm;
  if (IsLoad && !(Addr.Sym && isGOTLoad(*Addr.Sym)))
    return std::nullopt;

  Register Dest = Opcode == X86::TLS_desc64 ? Register(X86::RAX)
                  : IsTLSPseudo             ? Register(X86::RDI)
                                            : MI.getOperand(0).getReg();
  bool Is64BitDest = X86::GR64RegClass.contains(Dest);
  bool IsStack = !Addr.Sym && isFrameRegister(Addr.Base, MF);
  std::string SymbolName;
  if (Addr.Sym)
    SymbolName = getOperandSymbol(*Addr.Sym, AP)->getName().str();

  // Classify the instruction. Without annotations from earlier stages, a LEA
  // on plain registers is assumed to derive a pointer from its base register
  // unless its shape says it is integer arithmetic.
  StringRef Kind;
  std::optional<std::string> NotPointerReason;
  bool NotPointerFromWidth = false;
  if (IsLoad) {
    Kind = "addr_got";
  } else if (Addr.Sym) {
    Kind = "addr";
  } else if (!Is64BitDest) {
    // Pointers are 64 bits wide on x86-64.
    Kind = "not_pointer";
    NotPointerFromWidth = true;
    NotPointerReason = "result narrower than a pointer";
  } else if (IsStack) {
    Kind = "addr";
  } else if (!Addr.Base || Addr.Base == Addr.Index) {
    Kind = "not_pointer";
    NotPointerReason = "integer arithmetic shape (no base, or base == index)";
  } else {
    Kind = "derive";
  }

  // Find the base object, the offset from its origin, and the variable terms.
  PointerValue V;
  V.RecordId = Records.size();
  if (!IsTLSPseudo && Kind != "not_pointer")
    V.Dest = Dest;
  BaseObject &Base = V.Base;
  if (Addr.Index)
    V.Vars.push_back({getRegName(Addr.Index).str(), Addr.Scale});

  SmallVector<StringRef, 3> Provenance = {"machine-operand"};
  SmallVector<std::string, 1> SharedSlotVars;
  std::optional<unsigned> DerivedFrom;
  std::optional<std::string> SrcExpr, SrcVar, SrcType;
  bool Ambiguous = false;

  if (Opcode == X86::TLS_base_addr64 ||
      (IsTLSPseudo && Addr.Sym && !Addr.Sym->isGlobal())) {
    Base.Reason = "TLS module base (local dynamic)";
    Base.RootExpr = SymbolName;
  } else if (Addr.Sym) {
    Base = getSymbolBase(*Addr.Sym, MF.getDataLayout(), SymbolName);
    V.ConstOffset = Kind == "addr_got" ? 0 : Addr.Disp;
    if (isBaseRegVarOffset(*Addr.Sym, Addr.Base))
      V.Vars.push_back({getRegName(Addr.Base).str(), 1});
  } else if (IsStack) {
    SmallVector<StackVariable, 1> Vars =
        findStackVariables(MF, Addr.Base, Addr.Disp, MI.getDebugLoc().get());
    if (Vars.empty()) {
      Base.Reason = "stack object without a variable";
    } else {
      const DILocalVariable *Var = Vars.front().Var;
      Base.Kind = Var->isParameter() ? "param" : "local";
      Base.Name = Var->getName().str();
      Base.RootExpr = *Base.Name;
      Base.Type = Var->getType();
      Base.HasType = true;
      Base.Size = Base.TypeSize = addrmap::getTypeSize(Base.Type);
      if (!Base.Size)
        Base.SizeUnknown = "unknown";
      Base.DeclFile = Var->getFilename();
      Base.DeclLine = Var->getLine();
      V.ConstOffset = Vars.front().Offset;
      Provenance.push_back("debuginfo-variable");
      // Variables sharing a stack slot (StackColoring).
      for (const StackVariable &Other : drop_begin(Vars)) {
        addrmap::ResolvedPath P = addrmap::resolvePath(
            Other.Var->getType(), Other.Offset, V.Vars, /*ViaPointer=*/false);
        StringRef Name = Other.Var->getName();
        SharedSlotVars.push_back(
            P.Unresolved || P.HasUnmatchedVar
                ? Name.str()
                : addrmap::formatPath(Name, false, P.Steps));
      }
      Ambiguous = !SharedSlotVars.empty();
    }
  } else if (Kind == "derive") {
    // The offset from the source pointer, unless it is known to point into
    // an object.
    V.ConstOffset = Addr.Disp;
    auto It = LiveValues.find(Addr.Base);
    if (It != LiveValues.end()) {
      // Derived from a pointer made by an earlier recorded instruction.
      const PointerValue &Src = Values[It->second];
      Base = Src.Base;
      if (Src.ConstOffset)
        V.ConstOffset = *Src.ConstOffset + Addr.Disp;
      else
        V.ConstOffset.reset();
      SmallVector<addrmap::VarTerm, 2> Vars(Src.Vars);
      append_range(Vars, V.Vars);
      V.Vars = std::move(Vars);
      DerivedFrom = Src.RecordId;
      SrcExpr = Src.PathStr;
      SrcType = Src.ResultType;
      Provenance.push_back("derived-from-record");
    } else if (const DILocalVariable *Var =
                   findVariableInRegister(MI, Addr.Base)) {
      SrcExpr = SrcVar = Var->getName().str();
      SrcType = addrmap::getCTypeName(Var->getType());
      Provenance.push_back("debuginfo-variable");
      bool IsPointer;
      const DIType *Pointee =
          addrmap::getPointeeType(Var->getType(), IsPointer);
      Base.Kind = "pointee";
      Base.SizeUnknown = "pointee";
      if (IsPointer) {
        Base.Type = Pointee;
        Base.HasType = true;
        Base.TypeSize = addrmap::getTypeSize(Pointee);
        Base.RootExpr = *SrcVar;
        Base.ViaPointer = true;
      } else if (isa_and_nonnull<DIBasicType>(
                     addrmap::stripTypedefsAndQualifiers(Var->getType()))) {
        // The LEA computes on an integer variable, e.g. "a + b" or "n - 1".
        Kind = "not_pointer";
        NotPointerReason = "base register holds integer variable " + *SrcVar;
        V.Dest = 0;
      } else {
        Base.Reason = "source value " + *SrcVar + " is not a pointer";
      }
    } else {
      Base.Kind = "pointee";
      Base.SizeUnknown = "pointee";
      Base.Reason = "source pointer not known";
    }
  }

  // Turn the offset into an access path using the type of the base.
  addrmap::ResolvedPath Path;
  bool Resolved = false;
  if (Base.HasType && V.ConstOffset) {
    Path = addrmap::resolvePath(Base.Type, *V.ConstOffset, V.Vars,
                                Base.ViaPointer);
    // A variable term that is not an array subscript means the address is an
    // intermediate value of a longer computation, which has no access path.
    if (Path.HasUnmatchedVar) {
      Path.Steps.clear();
      Path.Alternatives.clear();
      Path.ByteOffsetInField = 0;
      Path.OutOfBounds = false;
    }
    Resolved = !Path.Unresolved && !Path.HasUnmatchedVar;
    if (Resolved)
      Provenance.push_back("debuginfo-type");
  }
  if (Resolved) {
    V.PathStr = addrmap::formatPath(Base.RootExpr, Base.ViaPointer, Path.Steps);
    if (Path.ByteOffsetInField == 0)
      V.ResultType = addrmap::getPointerTypeName(Path.FinalTypeName);
  } else if (Base.Name && V.ConstOffset == 0 && V.Vars.empty()) {
    V.PathStr = *Base.Name;
  }

  bool OutOfBounds = Path.OutOfBounds;
  if (!Resolved && V.ConstOffset && V.Vars.empty() && Base.Size &&
      !Base.ViaPointer)
    OutOfBounds = *V.ConstOffset < 0 || uint64_t(*V.ConstOffset) >= *Base.Size;

  std::optional<std::string> Reason = Base.Reason;
  if (!Reason && Path.Unresolved)
    Reason = Path.Unresolved;
  if (!Reason && Path.HasUnmatchedVar)
    Reason = "variable offset not matched to an array subscript";
  if (!Reason && Base.HasType && !V.ConstOffset)
    Reason = "constant offset not known";
  if (!Reason && Ambiguous)
    Reason = "stack slot shared by several variables";

  bool BaseKnown =
      Base.Kind != "unknown" && !(Base.Kind == "pointee" && !Base.HasType);
  StringRef Confidence;
  if (Kind == "not_pointer")
    Confidence = NotPointerFromWidth ? "derived" : "heuristic";
  else
    Confidence = BaseKnown && !Path.HasUnmatchedVar && !Ambiguous ? "derived"
                                                                  : "heuristic";

  // Serialize the record.
  std::string Json;
  raw_string_ostream OS(Json);
  json::OStream J(OS);
  J.object([&] {
    J.attribute("type", "record");
    J.attribute("id", V.RecordId);
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
          J.attribute("symbol", SymbolName);
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
      writeOptional(J, "name", Base.Name);
      if (Base.HasType)
        J.attribute("type", addrmap::getCTypeName(Base.Type));
      else
        J.attribute("type", nullptr);
      writeOptional(J, "type_size", Base.TypeSize);
      J.attributeObject("object", [&] {
        writeOptional(J, "size", Base.Size);
        if (!Base.Size && Base.SizeUnknown)
          J.attribute("size_unknown", *Base.SizeUnknown);
      });
      if (Base.DeclLine)
        J.attributeObject("decl", [&] {
          J.attribute("file", Base.DeclFile);
          J.attribute("line", Base.DeclLine);
        });
      else
        J.attribute("decl", nullptr);
    });
    if (Kind == "derive") {
      J.attributeObject("src_ptr", [&] {
        writeOptional(J, "expr", SrcExpr);
        writeOptional(J, "var", SrcVar);
        writeOptional(J, "type", SrcType);
        J.attributeArray("derived_from", [&] {
          if (DerivedFrom)
            J.value(*DerivedFrom);
        });
      });
    }
    J.attributeArray("path", [&] {
      for (const addrmap::PathStep &S : Path.Steps)
        writePathStep(J, S);
    });
    J.attribute("path_str", V.PathStr);
    J.attributeObject("ptr_offset", [&] {
      writeOptional(J, "const", V.ConstOffset);
      J.attributeArray("var", [&] {
        for (const addrmap::VarTerm &T : V.Vars)
          J.object([&] {
            J.attribute("reg", T.Reg);
            J.attribute("scale", T.Scale);
          });
      });
    });
    J.attribute("byte_offset_in_field", Path.ByteOffsetInField);
    J.attribute("result_type", V.ResultType);
    J.attribute("source_expr", nullptr);
    J.attributeArray("alternatives", [&] {
      for (const addrmap::Alternative &A : Path.Alternatives)
        J.object([&] {
          J.attribute("path_str",
                      addrmap::formatPath(Base.RootExpr, Base.ViaPointer,
                                          Path.Steps, A.Steps));
          J.attribute("reason", A.Reason);
        });
      for (const std::string &Name : SharedSlotVars)
        J.object([&] {
          J.attribute("path_str", Name);
          J.attribute("reason", "shares the stack slot");
        });
    });
    J.attribute("out_of_bounds", OutOfBounds);
    J.attribute("confidence", Confidence);
    J.attributeArray("provenance", [&] {
      for (StringRef P : Provenance)
        J.value(P);
    });
    writeOptional(J, "reason", Reason);
  });

  MCSymbol *Label = AP.OutContext.createTempSymbol("addrmap");
  AP.OutStreamer->emitLabel(Label);
  Records.push_back({Label, std::move(Json)});
  if (Kind == "not_pointer")
    return std::nullopt;
  return V;
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
  OS.emitBytes(
      StringRef(reinterpret_cast<const char *>(TUId.data()), TUId.size()));

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
