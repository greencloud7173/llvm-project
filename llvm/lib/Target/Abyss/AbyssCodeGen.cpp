//===-- AbyssCodeGen.cpp - Direct IR to Abyss MCInst emitter --------------===//
//
// The Abyss MC layer is complete; this file adds a deliberately small code
// generator that lowers the integer subset used by the Abyss C tests directly
// from LLVM IR to Abyss MCInsts.  Values get stack slots for cross-block
// liveness; a block-local register allocator keeps values used inside a block
// in registers and removes most spill/reload traffic.
//
//===----------------------------------------------------------------------===//

#include "AbyssTargetMachineImpl.h"
#include "MCTargetDesc/AbyssMCTargetDesc.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Mangler.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstPrinter.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCObjectFileInfo.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Pass.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include <cassert>
#include <cstdint>
#include <functional>
#include <string>

using namespace llvm;

namespace {

// Registers available to the simple block-local register allocator.  X0 is
// hardwired zero, X1/X2 are RA/SP, X5-X9 are the emitter's scratch registers,
// X10 is A0, and X29-X31 are the read-only thread coordinates.
static const unsigned AbyssAllocRegs[] = {
    Abyss::X3,  Abyss::X4,  Abyss::X11, Abyss::X12, Abyss::X13,
    Abyss::X14, Abyss::X15, Abyss::X16, Abyss::X17, Abyss::X18,
    Abyss::X19, Abyss::X20, Abyss::X21, Abyss::X22, Abyss::X23,
    Abyss::X24, Abyss::X25, Abyss::X26, Abyss::X27, Abyss::X28,
};

class AbyssMC {
  Module &M;
  const Target &T;
  Triple TT;
  raw_pwrite_stream &Out;
  CodeGenFileType FileType;

  std::unique_ptr<MCRegisterInfo> MRI;
  std::unique_ptr<MCAsmInfo> MAI;
  std::unique_ptr<MCSubtargetInfo> STI;
  std::unique_ptr<MCInstrInfo> MII;
  std::unique_ptr<MCContext> Ctx;
  std::unique_ptr<MCObjectFileInfo> MOFI;
  std::unique_ptr<MCStreamer> Streamer;

  DenseMap<const Value *, unsigned> Slots;
  DenseMap<const AllocaInst *, int> AllocaOffsets;
  DenseMap<const GlobalVariable *, int> GlobalConstOffsets;
  DenseMap<const BasicBlock *, std::string> BlockNames;
  DenseSet<const Value *> NeedsSlot;
  DenseSet<const Value *> NeedOwnSlot;
  // No-op values (currently freeze) are aliases, not separate SSA values.
  DenseMap<const Value *, const Value *> Aliases;
  // Header PHIs of simple loops stay in a fixed register across the backedge.
  DenseMap<const Value *, unsigned> PinnedReg;
  DenseMap<const Value *, const Loop *> PinnedLoop;
  DenseMap<const BasicBlock *, SmallVector<const Value *, 2>> BlockPinned;
  DenseMap<const ConstantInt *, unsigned> PinnedConst;
  bool FunctionHasCalls = false;
  std::unique_ptr<DominatorTree> LoopDT;
  std::unique_ptr<LoopInfo> LoopAnalysis;
  DenseMap<const Value *, unsigned> RegAssign;
  DenseSet<const Value *> RegLive;
  SmallVector<const Value *, 32> BlockValues;
  SmallVector<MCInst, 64> PendingInsts;
  bool Scheduling = false;
  std::string FnName;
  unsigned NextSlot = 0;
  unsigned NextLabel = 0;
  unsigned CurFrameSize = 0;

  static constexpr unsigned X0 = Abyss::X0;
  static constexpr unsigned RA = Abyss::X1;
  static constexpr unsigned SP = Abyss::X2;
  static constexpr unsigned A0 = Abyss::X10;
  static constexpr unsigned T0 = Abyss::X5;
  static constexpr unsigned T1 = Abyss::X6;
  static constexpr unsigned T2 = Abyss::X7;
  static constexpr unsigned T3 = Abyss::X8;
  static constexpr unsigned T4 = Abyss::X9;

public:
  AbyssMC(Module &M, const Target &T, raw_pwrite_stream &Out,
          CodeGenFileType FileType);
  void emit();

private:
  MCOperand reg(unsigned R) const { return MCOperand::createReg(R); }
  MCOperand imm(int64_t I) const { return MCOperand::createImm(I); }
  MCOperand sym(MCSymbol *S) const {
    return MCOperand::createExpr(MCSymbolRefExpr::create(S, *Ctx));
  }

  void mc(unsigned Opcode, std::initializer_list<MCOperand> Ops) {
    MCInst Inst;
    Inst.setOpcode(Opcode);
    for (const MCOperand &Op : Ops)
      Inst.addOperand(Op);
    if (Scheduling)
      PendingInsts.push_back(Inst);
    else
      Streamer->emitInstruction(Inst, *STI);
  }
  void r(unsigned Opcode, unsigned Rd, unsigned Rs1, unsigned Rs2) {
    mc(Opcode, {reg(Rd), reg(Rs1), reg(Rs2)});
  }
  void i(unsigned Opcode, unsigned Rd, unsigned Rs1, int64_t I) {
    mc(Opcode, {reg(Rd), reg(Rs1), imm(I)});
  }
  void s(unsigned Opcode, unsigned Rs1, unsigned Rs2, int64_t I) {
    mc(Opcode, {reg(Rs1), reg(Rs2), imm(I)});
  }
  void b(unsigned Opcode, unsigned Rs1, unsigned Rs2, MCSymbol *Target) {
    mc(Opcode, {reg(Rs1), reg(Rs2), sym(Target)});
  }
  void jal(unsigned Rd, MCSymbol *Target) {
    mc(Abyss::JAL, {reg(Rd), sym(Target)});
  }
  // Return to the caller's RA.  The custom RET instruction is used by the
  // startup stub to stop the machine; functions need a real return.
  void ret() { mc(Abyss::JALR, {reg(X0), reg(RA), imm(0)}); }

  const Value *resolve(const Value *V) const;
  void beginSchedule();
  void flushSchedule();
  void scheduleAndEmit(SmallVectorImpl<MCInst> &Insts);
  void emitConst(unsigned Rd, int64_t Value);
  void emitConstToMemory(unsigned Base, const Constant *C, Type *Ty,
                         int64_t Offset);
  void loadValue(unsigned Rd, const Value *V);
  void storeSlot(const Value *V, unsigned Rs);
  unsigned destReg(const Value *V, unsigned Scratch) const;
  unsigned operandReg(unsigned Scratch, const Value *V);
  void beginBlockRegisters(BasicBlock &BB);
  void assignLoopRegisters(Function &F);
  void flushBlockValues(bool Force);
  unsigned slotOf(const Value *V) const;
  int slotOffset(const Value *V) const;

  void emitFunction(Function &F);
  void emitBlock(Function &F, BasicBlock &BB);
  void emitInstruction(Function &F, BasicBlock &BB, Instruction &I);
  void emitTerminator(Function &F, BasicBlock &BB, Instruction &I);
  void restoreStackPointer();
  void emitBinary(BinaryOperator &BO);
  bool emitPinnedPhiUpdate(BinaryOperator &BO, BasicBlock &BB);
  void emitICmp(ICmpInst &CI);
  void emitGEP(GetElementPtrInst &GEP);
  void emitLoad(LoadInst &LI);
  void emitStore(StoreInst &SI);
  void emitSelect(BasicBlock &BB, SelectInst &SI);
  void emitCall(CallInst &CI, BasicBlock &BB);
  void emitMemSet(CallInst &CI);
  void emitMinMax(CallInst &CI, BasicBlock &BB, bool IsMin, bool IsUnsigned);
  void emitCopy(Instruction &I, const Value *V);
  void emitPhiStores(BasicBlock *Pred, BasicBlock *Succ);
  bool edgeLeavesPinnedLoop(const BasicBlock *BB,
                            const BasicBlock *Succ) const;
  void emitPinnedExitStores(const BasicBlock *BB, const BasicBlock *Succ);
  void emitBranchPred(CmpInst::Predicate Pred, unsigned L, unsigned R,
                      MCSymbol *Target);

  MCSymbol *blockSymbol(const BasicBlock *BB) const {
    return Ctx->getOrCreateSymbol(BlockNames.lookup(BB));
  }
  MCSymbol *newLabel(const BasicBlock *BB, StringRef Suffix);
};

AbyssMC::AbyssMC(Module &M, const Target &T, raw_pwrite_stream &Out,
                 CodeGenFileType FileType)
    : M(M), T(T), TT(M.getTargetTriple()), Out(Out), FileType(FileType) {
  if (TT.getArch() == Triple::UnknownArch)
    TT = Triple("abyss");

  MRI.reset(T.createMCRegInfo(TT));
  MCTargetOptions MCOptions;
  MAI.reset(T.createMCAsmInfo(*MRI, TT, MCOptions));
  STI.reset(T.createMCSubtargetInfo(TT, "", ""));
  MII.reset(T.createMCInstrInfo());

  Ctx = std::make_unique<MCContext>(TT, MAI.get(), MRI.get(), STI.get(),
                                    nullptr, &MCOptions);
  MOFI.reset(T.createMCObjectFileInfo(*Ctx, /*PIC=*/false, /*LargeCM=*/false));
  Ctx->setObjectFileInfo(MOFI.get());

  auto MAB = std::unique_ptr<MCAsmBackend>(
      T.createMCAsmBackend(*STI, *MRI, MCOptions));
  auto MCE = std::unique_ptr<MCCodeEmitter>(T.createMCCodeEmitter(*MII, *Ctx));
  assert(MAB && MCE && "Abyss MC backend not registered");

  if (FileType == CodeGenFileType::AssemblyFile) {
    auto IP = std::unique_ptr<MCInstPrinter>(
        T.createMCInstPrinter(TT, 0, *MAI, *MII, *MRI));
    auto FOut = std::make_unique<formatted_raw_ostream>(Out);
    Streamer.reset(T.createAsmStreamer(*Ctx, std::move(FOut), std::move(IP),
                                       std::move(MCE), std::move(MAB)));
  } else {
    auto OW = MAB->createObjectWriter(Out);
    Streamer.reset(T.createMCObjectStreamer(TT, *Ctx, std::move(MAB),
                                            std::move(OW), std::move(MCE),
                                            *STI));
  }

  Streamer->initSections(false, *STI);
  Streamer->switchSection(Ctx->getObjectFileInfo()->getTextSection());
}

const Value *AbyssMC::resolve(const Value *V) const {
  for (unsigned Depth = 0; Depth != 8; ++Depth) {
    auto It = Aliases.find(V);
    if (It == Aliases.end())
      break;
    V = It->second;
  }
  return V;
}

static bool isAbyssLoad(unsigned Opcode) {
  switch (Opcode) {
  case Abyss::LDR:
  case Abyss::LB:
  case Abyss::LH:
  case Abyss::LBU:
  case Abyss::LHU:
    return true;
  default:
    return false;
  }
}

static bool isAbyssStore(unsigned Opcode) {
  switch (Opcode) {
  case Abyss::STR:
  case Abyss::SB:
  case Abyss::SH:
    return true;
  default:
    return false;
  }
}

void AbyssMC::beginSchedule() {
  PendingInsts.clear();
  // The list scheduler below is kept as an option, but is disabled by default:
  // measured 1.8% slower on raster64x64.  With eight resident threads the
  // original source order already hides most ALU latency, and reordering adds
  // I-cache pressure.
  Scheduling = false;
}

void AbyssMC::scheduleAndEmit(SmallVectorImpl<MCInst> &Insts) {
  unsigned N = Insts.size();
  if (N < 2) {
    for (MCInst &Inst : Insts)
      Streamer->emitInstruction(Inst, *STI);
    return;
  }

  struct Info {
    SmallVector<unsigned, 2> Defs;
    SmallVector<unsigned, 3> Uses;
    bool Memory = false;
    unsigned Latency = 1;
  };
  SmallVector<Info, 64> Infos(N);
  for (unsigned I = 0; I != N; ++I) {
    const MCInstrDesc &Desc = MII->get(Insts[I].getOpcode());
    unsigned NumDefs = Desc.getNumDefs();
    for (unsigned Op = 0; Op != Insts[I].getNumOperands(); ++Op) {
      const MCOperand &MO = Insts[I].getOperand(Op);
      if (!MO.isReg() || MO.getReg() == X0)
        continue;
      if (Op < NumDefs)
        Infos[I].Defs.push_back(MO.getReg());
      else
        Infos[I].Uses.push_back(MO.getReg());
    }
    unsigned Opc = Insts[I].getOpcode();
    Infos[I].Memory = isAbyssLoad(Opc) || isAbyssStore(Opc);
    if (Opc == Abyss::MUL)
      Infos[I].Latency = 3;
    else if (Opc == Abyss::DIV || Opc == Abyss::DIVU || Opc == Abyss::REM ||
             Opc == Abyss::REMU)
      Infos[I].Latency = 8;
    else if (isAbyssLoad(Opc))
      Infos[I].Latency = 4;
    else if (isAbyssStore(Opc))
      Infos[I].Latency = 2;
  }

  SmallVector<SmallVector<unsigned, 4>, 64> Succs(N);
  SmallVector<unsigned, 64> DepCount(N, 0);
  for (unsigned I = 0; I != N; ++I) {
    for (unsigned J = I + 1; J != N; ++J) {
      bool Dep = Infos[I].Memory && Infos[J].Memory;
      for (unsigned D : Infos[I].Defs) {
        if (Dep)
          break;
        for (unsigned U : Infos[J].Uses)
          if (D == U) {
            Dep = true;
            break;
          }
        for (unsigned D2 : Infos[J].Defs)
          if (D == D2) {
            Dep = true;
            break;
          }
      }
      if (!Dep)
        for (unsigned U : Infos[I].Uses)
          for (unsigned D : Infos[J].Defs)
            if (U == D) {
              Dep = true;
              break;
            }
      if (Dep) {
        Succs[I].push_back(J);
        ++DepCount[J];
      }
    }
  }

  SmallVector<unsigned, 64> Height(N, 0);
  for (unsigned I = N; I-- > 0;) {
    Height[I] = Infos[I].Latency;
    for (unsigned J : Succs[I])
      Height[I] = std::max(Height[I], Infos[I].Latency + Height[J]);
  }

  SmallVector<unsigned, 64> Ready;
  for (unsigned I = 0; I != N; ++I)
    if (!DepCount[I])
      Ready.push_back(I);

  while (!Ready.empty()) {
    unsigned Best = 0;
    for (unsigned I = 1; I != Ready.size(); ++I) {
      unsigned A = Ready[I], B = Ready[Best];
      if (Height[A] > Height[B] ||
          (Height[A] == Height[B] && A < B))
        Best = I;
    }
    unsigned I = Ready[Best];
    Ready.erase(Ready.begin() + Best);
    Streamer->emitInstruction(Insts[I], *STI);
    for (unsigned J : Succs[I])
      if (--DepCount[J] == 0)
        Ready.push_back(J);
  }
}

void AbyssMC::flushSchedule() {
  if (!Scheduling)
    return;
  Scheduling = false;
  scheduleAndEmit(PendingInsts);
  PendingInsts.clear();
}

void AbyssMC::emitConstToMemory(unsigned Base, const Constant *C, Type *Ty,
                                int64_t Offset) {
  auto Store = [&](unsigned Opcode, unsigned Value, int64_t Off) {
    if (Off >= -2048 && Off <= 2047)
      s(Opcode, Base, Value, Off);
    else {
      i(Abyss::ADDI, T2, Base, Off);
      s(Opcode, T2, Value, 0);
    }
  };

  if (auto *CI = dyn_cast<ConstantInt>(C)) {
    emitConst(T1, CI->getSExtValue());
    unsigned Op = Abyss::STR;
    if (Ty->isIntegerTy(8))
      Op = Abyss::SB;
    else if (Ty->isIntegerTy(16))
      Op = Abyss::SH;
    Store(Op, T1, Offset);
    return;
  }
  if (isa<UndefValue>(C) || isa<PoisonValue>(C))
    C = Constant::getNullValue(C->getType());

  const DataLayout &DL = M.getDataLayout();
  if (isa<ConstantAggregateZero>(C)) {
    Type *T = C->getType();
    if (auto *AT = dyn_cast<ArrayType>(T)) {
      Type *ET = AT->getElementType();
      uint64_t ES = DL.getTypeAllocSize(ET).getFixedValue();
      Constant *Zero = Constant::getNullValue(ET);
      for (uint64_t I = 0; I != AT->getNumElements(); ++I)
        emitConstToMemory(Base, Zero, ET,
                          Offset + static_cast<int64_t>(I * ES));
      return;
    }
    if (auto *ST = dyn_cast<StructType>(T)) {
      const StructLayout *SL = DL.getStructLayout(ST);
      for (unsigned I = 0; I != ST->getNumElements(); ++I) {
        Type *ET = ST->getElementType(I);
        emitConstToMemory(Base, Constant::getNullValue(ET), ET,
                          Offset + static_cast<int64_t>(SL->getElementOffset(I)));
      }
      return;
    }
    emitConst(T1, 0);
    Store(Abyss::STR, T1, Offset);
    return;
  }

  if (auto *CA = dyn_cast<ConstantArray>(C)) {
    Type *ET = CA->getType()->getElementType();
    uint64_t ES = DL.getTypeAllocSize(ET).getFixedValue();
    for (unsigned I = 0; I != CA->getNumOperands(); ++I)
      emitConstToMemory(Base, CA->getOperand(I), ET,
                        Offset + static_cast<int64_t>(I * ES));
    return;
  }
  if (auto *CS = dyn_cast<ConstantStruct>(C)) {
    auto *ST = CS->getType();
    const StructLayout *SL = DL.getStructLayout(ST);
    for (unsigned I = 0; I != CS->getNumOperands(); ++I)
      emitConstToMemory(Base, CS->getOperand(I), ST->getElementType(I),
                        Offset + static_cast<int64_t>(SL->getElementOffset(I)));
    return;
  }
  if (auto *CD = dyn_cast<ConstantDataSequential>(C)) {
    Type *ET = CD->getElementType();
    uint64_t ES = DL.getTypeAllocSize(ET).getFixedValue();
    for (unsigned I = 0; I != CD->getNumElements(); ++I)
      emitConstToMemory(Base, CD->getElementAsConstant(I), ET,
                        Offset + static_cast<int64_t>(I * ES));
    return;
  }
  report_fatal_error("Abyss: unsupported constant aggregate");
}

void AbyssMC::emitConst(unsigned Rd, int64_t Value) {
  uint32_t U = static_cast<uint32_t>(Value);
  int32_t S = static_cast<int32_t>(U);
  if (S >= -2048 && S <= 2047) {
    i(Abyss::ADDI, Rd, X0, S);
    return;
  }

  uint32_t Hi = ((U + 0x800u) >> 12) & 0xFFFFFu;
  int32_t Lo = static_cast<int32_t>(U - (Hi << 12));
  if (Hi)
    mc(Abyss::LUI, {reg(Rd), imm(Hi)});
  if (Lo)
    i(Abyss::ADDI, Rd, Hi ? Rd : X0, Lo);
}

unsigned AbyssMC::slotOf(const Value *V) const {
  V = resolve(V);
  auto It = Slots.find(V);
  assert(It != Slots.end() && "value has no stack slot");
  return It->second;
}

int AbyssMC::slotOffset(const Value *V) const {
  return -4 * static_cast<int>(slotOf(V) + 1);
}

void AbyssMC::loadValue(unsigned Rd, const Value *V) {
  V = resolve(V);
  if (auto *GV = dyn_cast<GlobalVariable>(V)) {
    auto It = GlobalConstOffsets.find(GV);
    if (It == GlobalConstOffsets.end())
      report_fatal_error("Abyss: non-constant or unused global variable");
    i(Abyss::ADDI, Rd, SP, It->second);
    return;
  }
  if (auto *CI = dyn_cast<ConstantInt>(V)) {
    auto It = PinnedConst.find(CI);
    if (It != PinnedConst.end()) {
      if (Rd != It->second)
        i(Abyss::ADDI, Rd, It->second, 0);
      return;
    }
    emitConst(Rd, CI->getSExtValue());
    return;
  }
  if (isa<ConstantPointerNull>(V) || isa<UndefValue>(V) ||
      isa<PoisonValue>(V)) {
    emitConst(Rd, 0);
    return;
  }
  if (auto *CE = dyn_cast<ConstantExpr>(V)) {
    if (CE->getOpcode() == Instruction::IntToPtr &&
        isa<ConstantInt>(CE->getOperand(0))) {
      emitConst(Rd, cast<ConstantInt>(CE->getOperand(0))->getSExtValue());
      return;
    }
  }
  auto RegIt = RegAssign.find(V);
  if (RegIt != RegAssign.end()) {
    if (!RegLive.count(V)) {
      i(Abyss::LDR, RegIt->second, SP, slotOffset(V));
      RegLive.insert(V);
    }
    if (Rd != RegIt->second)
      i(Abyss::ADDI, Rd, RegIt->second, 0);
    return;
  }
  i(Abyss::LDR, Rd, SP, slotOffset(V));
}

void AbyssMC::storeSlot(const Value *V, unsigned Rs) {
  V = resolve(V);
  auto RegIt = RegAssign.find(V);
  if (RegIt != RegAssign.end()) {
    RegLive.insert(V);
    if (Rs != RegIt->second)
      i(Abyss::ADDI, RegIt->second, Rs, 0);
    return;
  }
  s(Abyss::STR, SP, Rs, slotOffset(V));
}

unsigned AbyssMC::destReg(const Value *V, unsigned Scratch) const {
  V = resolve(V);
  auto It = RegAssign.find(V);
  return It == RegAssign.end() ? Scratch : It->second;
}

unsigned AbyssMC::operandReg(unsigned Scratch, const Value *V) {
  V = resolve(V);
  if (auto *CI = dyn_cast<ConstantInt>(V)) {
    auto It = PinnedConst.find(CI);
    if (It != PinnedConst.end())
      return It->second;
    if (CI->isZero())
      return X0;
  }
  auto It = RegAssign.find(V);
  if (It != RegAssign.end() && RegLive.count(V))
    return It->second;
  loadValue(Scratch, V);
  return Scratch;
}

void AbyssMC::beginBlockRegisters(BasicBlock &BB) {
  RegAssign.clear();
  RegLive.clear();
  BlockValues.clear();

  DenseSet<unsigned> Reserved;
  for (const Value *V : BlockPinned.lookup(&BB)) {
    unsigned Reg = PinnedReg.lookup(V);
    RegAssign[V] = Reg;
    RegLive.insert(V);
    Reserved.insert(Reg);
  }
  // Constant homes live from the prologue through every block of a call-free
  // function, so they must not be reused outside the loop either.
  for (auto &KV : PinnedConst)
    Reserved.insert(KV.second);

  // Compute local live intervals.  A value starts at its definition and ends
  // at its last use in this block.  Live-out values are extended to the end so
  // they can be stored once, after all in-block uses.
  DenseMap<const Value *, unsigned> Start, End;
  DenseMap<const Value *, unsigned> EntryUses, EntryEnd;
  unsigned Index = 0;
  for (Instruction &I : BB) {
    if (!I.getType()->isVoidTy() && !isa<PHINode>(I) &&
        !isa<AllocaInst>(I) &&
        (I.getType()->isIntegerTy() || I.getType()->isPointerTy())) {
      Start[&I] = Index;
      End[&I] = Index;
    }
    for (Value *Op : I.operands()) {
      auto It = End.find(Op);
      if (It != End.end() && Index > It->second)
        It->second = Index;

      // Cache frequently used values defined outside this block (loop-carried
      // PHIs, pointers, arguments) in a register on first use.
      if (It == End.end() && !isa<Constant>(Op) &&
          (Op->getType()->isIntegerTy() || Op->getType()->isPointerTy())) {
        ++EntryUses[Op];
        EntryEnd[Op] = Index;
      }
    }
    ++Index;
  }

  unsigned BlockEnd = Index;
  for (auto &KV : End)
    if (NeedsSlot.count(KV.first) && KV.second > Start.lookup(KV.first))
      KV.second = BlockEnd;

  struct Interval {
    unsigned Start;
    unsigned End;
    const Value *V;
    bool Local;
  };
  SmallVector<Interval, 32> Intervals;
  for (auto &KV : Start) {
    unsigned S = KV.second;
    unsigned E = End.lookup(KV.first);
    if (E > S || NeedsSlot.count(KV.first))
      Intervals.push_back({S, E == S ? BlockEnd : E, KV.first, true});
  }
  for (auto &KV : EntryEnd)
    if (EntryUses.lookup(KV.first) > 1 && !PinnedReg.count(KV.first))
      Intervals.push_back({0, KV.second, KV.first, false});
  llvm::sort(Intervals, [](const Interval &A, const Interval &B) {
    return A.Start < B.Start;
  });

  // Classic linear scan over a small fixed register set.  A register can be
  // reused once its previous interval has ended before the new interval
  // starts.
  DenseMap<unsigned, unsigned> RegEnd;
  for (const Interval &It : Intervals) {
    if (PinnedReg.count(It.V))
      continue;
    for (unsigned Reg : AbyssAllocRegs) {
      if (Reserved.count(Reg))
        continue;
      auto Active = RegEnd.find(Reg);
      if (Active != RegEnd.end() && Active->second >= It.Start)
        continue;

      RegAssign[It.V] = Reg;
      RegEnd[Reg] = It.End;
      if (It.Local)
        BlockValues.push_back(It.V);
      break;
    }
  }
}

void AbyssMC::assignLoopRegisters(Function &F) {
  PinnedReg.clear();
  PinnedLoop.clear();
  BlockPinned.clear();

  LoopDT = std::make_unique<DominatorTree>(F);
  LoopAnalysis = std::make_unique<LoopInfo>(*LoopDT);
  LoopInfo &LI = *LoopAnalysis;
  DenseSet<unsigned> GlobalUsed;
  std::function<void(Loop *)> Visit = [&](Loop *L) {
    // Pinning an outer loop while an inner loop is also pinned would reserve
    // too many registers.  Start with innermost loops, which carry the hot
    // induction variables in the raster kernels.
    if (!L->getSubLoops().empty()) {
      for (Loop *Sub : L->getSubLoops())
        Visit(Sub);
      return;
    }

    // Calls clobber the allocatable register set, so they are incompatible
    // with values held live across a whole loop.
    for (BasicBlock *B : L->blocks())
      for (Instruction &I : *B)
        if (auto *CB = dyn_cast<CallBase>(&I))
          if (CB->getIntrinsicID() == Intrinsic::not_intrinsic &&
              !isa<InlineAsm>(CB->getCalledOperand()))
            return;

    BasicBlock *Header = L->getHeader();
    SmallVector<PHINode *, 2> Pinned;
    for (PHINode &PN : Header->phis()) {
      if (!PN.getType()->isIntegerTy(32) || Pinned.size() == 3)
        continue;
      for (unsigned Reg : AbyssAllocRegs) {
        if (GlobalUsed.count(Reg))
          continue;
        GlobalUsed.insert(Reg);
        PinnedReg[&PN] = Reg;
        PinnedLoop[&PN] = L;
        Pinned.push_back(&PN);
        break;
      }
    }
    for (BasicBlock *B : L->blocks())
      for (PHINode *PN : Pinned)
        BlockPinned[B].push_back(PN);

    // Hoist a few hot loop constants.  Large constants cost LUI+ADDI on every
    // use; keeping them in a register is a net win even after reserving the
    // register.  Only do this in call-free functions so the homes survive from
    // the prologue to the loop.
    if (!FunctionHasCalls) {
      DenseMap<const ConstantInt *, unsigned> Count, Cost;
      for (BasicBlock *B : L->blocks())
        for (Instruction &I : *B)
          for (Value *Op : I.operands())
            if (auto *CI = dyn_cast<ConstantInt>(Op)) {
              if (CI->isZero())
                continue;
              ++Count[CI];
              int64_t V = CI->getSExtValue();
              Cost[CI] = (V >= -2048 && V <= 2047) ? 1 : 2;
            }

      SmallVector<std::pair<unsigned, const ConstantInt *>, 16> Ranked;
      for (auto &KV : Count)
        Ranked.push_back({KV.second * Cost.lookup(KV.first), KV.first});
      llvm::sort(Ranked, [](const auto &A, const auto &B) {
        if (A.first != B.first)
          return A.first > B.first;
        int64_t AV = A.second->getSExtValue();
        int64_t BV = B.second->getSExtValue();
        return AV > BV;
      });

      // Keep roughly one third of the register file free for temporaries.
      constexpr unsigned MaxPinnedRegs = 14;
      for (auto &Entry : Ranked) {
        if (GlobalUsed.size() >= MaxPinnedRegs)
          break;
        const ConstantInt *CI = Entry.second;
        if (PinnedConst.count(CI) || CI->isOne() || CI->isMinusOne())
          continue;
        unsigned Reg = 0;
        for (unsigned Candidate : AbyssAllocRegs) {
          if (!GlobalUsed.count(Candidate)) {
            Reg = Candidate;
            break;
          }
        }
        if (!Reg)
          break;
        GlobalUsed.insert(Reg);
        PinnedConst[CI] = Reg;
      }
    }
  };

  for (Loop *L : LI)
    Visit(L);
}

void AbyssMC::flushBlockValues(bool Force) {
  for (const Value *V : BlockValues) {
    if (!RegLive.count(V))
      continue;
    if (Force || NeedOwnSlot.count(V))
      s(Abyss::STR, SP, RegAssign[V], slotOffset(V));
  }
  if (Force)
    RegLive.clear();
}

MCSymbol *AbyssMC::newLabel(const BasicBlock *BB, StringRef Suffix) {
  std::string Name = BlockNames.lookup(BB) + "." + Suffix.str() + "." +
                     std::to_string(NextLabel++);
  return Ctx->getOrCreateSymbol(Name);
}

void AbyssMC::emit() {
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    emitFunction(F);
  }
  Streamer->finish();
}

void AbyssMC::emitFunction(Function &F) {
  Slots.clear();
  AllocaOffsets.clear();
  GlobalConstOffsets.clear();
  Aliases.clear();
  PinnedReg.clear();
  PinnedLoop.clear();
  BlockPinned.clear();
  PinnedConst.clear();
  LoopDT.reset();
  LoopAnalysis.reset();
  BlockNames.clear();
  NeedsSlot.clear();
  NeedOwnSlot.clear();
  RegAssign.clear();
  RegLive.clear();
  BlockValues.clear();
  NextSlot = 1; // slot 0 is the saved return address
  NextLabel = 0;
  FnName = F.getName().str();
  FunctionHasCalls = false;
  for (BasicBlock &BB : F)
    for (Instruction &I : BB)
      if (auto *CB = dyn_cast<CallBase>(&I))
        if (CB->getIntrinsicID() == Intrinsic::not_intrinsic &&
            !isa<InlineAsm>(CB->getCalledOperand()))
          FunctionHasCalls = true;

  for (Argument &A : F.args())
    Slots[&A] = NextSlot++;

  for (BasicBlock &BB : F)
    for (Instruction &I : BB)
      if (isa<PHINode>(I) || !I.getType()->isVoidTy())
        Slots[&I] = NextSlot++;

  // A value that is only used inside its defining block can stay in a
  // register.  Values with a cross-block use must be visible in their stack
  // slot at the end of the defining block.
  for (BasicBlock &BB : F)
    for (Instruction &I : BB) {
      if (I.getType()->isVoidTy() || isa<PHINode>(I) || isa<AllocaInst>(I))
        continue;
      bool CrossesBlock = false;
      for (User *U : I.users()) {
        auto *UI = dyn_cast<Instruction>(U);
        if (!UI || UI->getParent() != &BB) {
          CrossesBlock = true;
          // A PHI edge can read the register directly in emitPhiStores, but
          // only when this block is the start of that edge.  Any other use
          // (a non-PHI, or a PHI reached through a different predecessor)
          // needs the value in its stack slot by the end of this block.
          bool ReadOnEdge = false;
          if (auto *PN = dyn_cast<PHINode>(U))
            ReadOnEdge = PN->getBasicBlockIndex(&BB) >= 0;
          if (!ReadOnEdge)
            NeedOwnSlot.insert(&I);
        }
      }
      if (CrossesBlock)
        NeedsSlot.insert(&I);
    }

  unsigned BI = 0;
  for (BasicBlock &BB : F) {
    std::string N = FnName + "." + BB.getName().str() + "." +
                    std::to_string(BI++);
    BlockNames[&BB] = N;
  }

  CurFrameSize = alignTo(4 * NextSlot, 16);
  // Reserve memory for allocas below the slot area.
  const DataLayout &DL = M.getDataLayout();
  int Used = 4 * NextSlot;
  for (BasicBlock &BB : F)
    for (Instruction &I : BB) {
      auto *AI = dyn_cast<AllocaInst>(&I);
      if (!AI)
        continue;
      TypeSize Size = DL.getTypeAllocSize(AI->getAllocatedType());
      if (Size.isScalable())
        report_fatal_error("Abyss: scalable alloca is not supported");
      unsigned Align = std::max<unsigned>(4, AI->getAlign().value());
      Used = alignTo(Used, Align) + static_cast<int>(Size.getFixedValue());
      AllocaOffsets[AI] = -Used;
    }

  // C initializers such as `int a[4] = {...}` are promoted to private constant
  // globals by LLVM.  Materialize those constants into this thread's frame and
  // let GEP/load use the frame address.  This keeps the normal C array syntax
  // usable without adding a read-only data section to the Abyss memory map.
  DenseSet<const GlobalVariable *> UsedGlobals;
  for (BasicBlock &BB : F)
    for (Instruction &I : BB)
      for (Value *Op : I.operands())
        if (auto *GV = dyn_cast<GlobalVariable>(Op))
          UsedGlobals.insert(GV);
  for (const GlobalVariable *GV : UsedGlobals) {
    if (!GV->hasInitializer() || GV->isDeclaration() || !GV->isConstant())
      report_fatal_error("Abyss: non-constant global is not supported");
    TypeSize Size = DL.getTypeAllocSize(GV->getValueType());
    if (Size.isScalable())
      report_fatal_error("Abyss: scalable global is not supported");
    unsigned Align = 4;
    if (MaybeAlign GVAlign = GV->getAlign())
      Align = std::max<unsigned>(4, GVAlign->value());
    Used = alignTo(Used, Align) + static_cast<int>(Size.getFixedValue());
    GlobalConstOffsets[GV] = -Used;
  }
  CurFrameSize = alignTo(Used, 16);
  assignLoopRegisters(F);
  MCSymbol *FnSym = Ctx->getOrCreateSymbol(FnName);
  // Keep assembly symbols local: the test flow concatenates start.s and the
  // kernel before assembling, and a global symbol would make the assembler
  // leave a relocation for the start stub's JAL instead of resolving it.
  if (FileType == CodeGenFileType::ObjectFile)
    Streamer->emitSymbolAttribute(FnSym, MCSA_Global);
  if (Ctx->isELF())
    Streamer->emitSymbolAttribute(FnSym, MCSA_ELF_TypeFunction);
  Streamer->emitLabel(FnSym);
  Streamer->emitCodeAlignment(Align(4), STI.get());

  if (CurFrameSize) {
    // All threads share one data memory.  Give each thread its own frame
    // below the caller-provided stack pointer using the linear thread id
    // blockIdx * blockDim + threadIdx.
    i(Abyss::ADDI, T0, Abyss::X29, 0);
    i(Abyss::ADDI, T1, Abyss::X30, 0);
    r(Abyss::MUL, T0, T0, T1);
    i(Abyss::ADDI, T1, Abyss::X31, 0);
    r(Abyss::ADD, T0, T0, T1);
    i(Abyss::ADDI, T0, T0, 1);
    emitConst(T1, CurFrameSize);
    r(Abyss::MUL, T0, T0, T1);
    r(Abyss::SUB, SP, SP, T0);
  }

  unsigned ArgNo = 0;
  // Slot 0 is the caller's return address.  Helpers called from -O0 code
  // overwrite RA, so every function saves and restores it.
  s(Abyss::STR, SP, RA, -4);
  for (Argument &A : F.args()) {
    if (ArgNo >= 8) {
      report_fatal_error("Abyss: more than 8 arguments are not supported");
    }
    if (A.getType()->isIntegerTy(32)) {
      s(Abyss::STR, SP, A0 + ArgNo, slotOffset(&A));
    }
    ++ArgNo;
  }

  // Materialize alloca addresses now that the frame is set up.
  for (auto &KV : AllocaOffsets) {
    i(Abyss::ADDI, T0, SP, KV.second);
    storeSlot(KV.first, T0);
  }

  // Materialize the global constant initializers into the per-thread frame.
  for (auto &KV : GlobalConstOffsets) {
    const GlobalVariable *GV = KV.first;
    i(Abyss::ADDI, T0, SP, KV.second);
    emitConstToMemory(T0, GV->getInitializer(), GV->getValueType(), 0);
  }

  // Constant homes are loaded once in the prologue and never clobbered in a
  // call-free function.
  for (auto &KV : PinnedConst)
    emitConst(KV.second, KV.first->getSExtValue());

  for (BasicBlock &BB : F)
    emitBlock(F, BB);
}

void AbyssMC::emitBlock(Function &F, BasicBlock &BB) {
  Streamer->emitLabel(blockSymbol(&BB));
  beginBlockRegisters(BB);
  beginSchedule();
  // A conditional branch materializes its own operands.  Do not also emit the
  // ICmp as a value: that produced a second SLT plus a second copy of the
  // comparison constants on every loop iteration.
  const ICmpInst *BranchCond = nullptr;
  if (auto *BI = dyn_cast<BranchInst>(BB.getTerminator()))
    if (BI->isConditional())
      if (auto *Cmp = dyn_cast<ICmpInst>(BI->getCondition()))
        if (Cmp->hasOneUse())
          BranchCond = Cmp;

  for (Instruction &I : BB) {
    if (I.isTerminator()) {
      flushSchedule();
      flushBlockValues(/*Force=*/false);
      emitTerminator(F, BB, I);
      break;
    }
    if (&I == BranchCond)
      continue;
    emitInstruction(F, BB, I);
  }
}

void AbyssMC::restoreStackPointer() {
  if (!CurFrameSize)
    return;
  i(Abyss::ADDI, T0, Abyss::X29, 0);
  i(Abyss::ADDI, T1, Abyss::X30, 0);
  r(Abyss::MUL, T0, T0, T1);
  i(Abyss::ADDI, T1, Abyss::X31, 0);
  r(Abyss::ADD, T0, T0, T1);
  i(Abyss::ADDI, T0, T0, 1);
  emitConst(T1, CurFrameSize);
  r(Abyss::MUL, T0, T0, T1);
  r(Abyss::ADD, SP, SP, T0);
}

void AbyssMC::emitCopy(Instruction &I, const Value *V) {
  unsigned D = destReg(&I, T0);
  loadValue(D, V);
  storeSlot(&I, D);
}

bool AbyssMC::emitPinnedPhiUpdate(BinaryOperator &BO, BasicBlock &BB) {
  if (BO.getOpcode() != Instruction::Add &&
      BO.getOpcode() != Instruction::Sub)
    return false;
  if (!BO.hasOneUse())
    return false;
  auto *PN = dyn_cast<PHINode>(*BO.user_begin());
  if (!PN || !PinnedReg.count(PN))
    return false;
  // The update only folds into the PHI when this block is the matching edge.
  // getIncomingValueForBlock() asserts otherwise, and valid IR may define the
  // update in a block that only dominates the predecessor.
  int Incoming = PN->getBasicBlockIndex(&BB);
  if (Incoming < 0 || PN->getIncomingValue(Incoming) != &BO)
    return false;

  auto *C0 = dyn_cast<ConstantInt>(BO.getOperand(0));
  auto *C1 = dyn_cast<ConstantInt>(BO.getOperand(1));
  const Value *Base = nullptr;
  int64_t Delta = 0;
  if (C1) {
    Base = BO.getOperand(0);
    Delta = C1->getSExtValue();
    if (BO.getOpcode() == Instruction::Sub)
      Delta = -Delta;
  } else if (C0 && BO.getOpcode() == Instruction::Add) {
    Base = BO.getOperand(1);
    Delta = C0->getSExtValue();
  } else {
    return false;
  }
  if (resolve(Base) != PN)
    return false;

  unsigned Reg = PinnedReg.lookup(PN);
  if (Delta >= -2048 && Delta <= 2047) {
    i(Abyss::ADDI, Reg, Reg, Delta);
  } else {
    emitConst(T0, Delta);
    r(Abyss::ADD, Reg, Reg, T0);
  }
  // The update is now the new PHI value; no separate SSA value or spill.
  Aliases[&BO] = PN;
  return true;
}

void AbyssMC::emitBinary(BinaryOperator &BO) {
  auto *C0 = dyn_cast<ConstantInt>(BO.getOperand(0));
  auto *C1 = dyn_cast<ConstantInt>(BO.getOperand(1));
  unsigned D = destReg(&BO, T2);
  auto IsImm12 = [](int64_t V) { return V >= -2048 && V <= 2047; };
  auto Finish = [&]() { storeSlot(&BO, D); };

  // ADDI / ANDI / ORI / XORI / shifts avoid materializing a constant in a
  // register.  Constants in the left operand are commutative for add/and/or/
  // xor, so canonicalize them to the right.
  switch (BO.getOpcode()) {
  case Instruction::Add:
    if (C1 && IsImm12(C1->getSExtValue())) {
      i(Abyss::ADDI, D, operandReg(T0, BO.getOperand(0)),
        C1->getSExtValue());
      Finish();
      return;
    }
    if (C0 && IsImm12(C0->getSExtValue())) {
      i(Abyss::ADDI, D, operandReg(T0, BO.getOperand(1)),
        C0->getSExtValue());
      Finish();
      return;
    }
    break;
  case Instruction::Sub:
    if (C1 && IsImm12(-C1->getSExtValue())) {
      i(Abyss::ADDI, D, operandReg(T0, BO.getOperand(0)),
        -C1->getSExtValue());
      Finish();
      return;
    }
    break;
  case Instruction::Mul: {
    // Multiplication by a power of two is a shift; by 0/1 is a move.
    auto TryMul = [&](ConstantInt *C, const Value *Other) {
      int64_t V = C->getSExtValue();
      if (V == 0) {
        i(Abyss::ADDI, D, X0, 0);
        Finish();
        return true;
      }
      if (V == 1) {
        i(Abyss::ADDI, D, operandReg(T0, Other), 0);
        Finish();
        return true;
      }
      if (V == -1) {
        r(Abyss::SUB, D, X0, operandReg(T0, Other));
        Finish();
        return true;
      }
      if (V > 0 && isPowerOf2_64(static_cast<uint64_t>(V))) {
        i(Abyss::SLLI, D, operandReg(T0, Other), Log2_64(V));
        Finish();
        return true;
      }
      return false;
    };
    if (C1 && TryMul(C1, BO.getOperand(0)))
      return;
    if (C0 && TryMul(C0, BO.getOperand(1)))
      return;
    break;
  }
  case Instruction::And:
  case Instruction::Or:
  case Instruction::Xor: {
    unsigned Op = BO.getOpcode() == Instruction::And
                      ? Abyss::ANDI
                      : BO.getOpcode() == Instruction::Or ? Abyss::ORI
                                                          : Abyss::XORI;
    if (C1 && IsImm12(C1->getSExtValue())) {
      i(Op, D, operandReg(T0, BO.getOperand(0)), C1->getSExtValue());
      Finish();
      return;
    }
    if (C0 && IsImm12(C0->getSExtValue())) {
      i(Op, D, operandReg(T0, BO.getOperand(1)), C0->getSExtValue());
      Finish();
      return;
    }
    break;
  }
  case Instruction::Shl:
  case Instruction::LShr:
  case Instruction::AShr:
    if (C1 && C1->getZExtValue() < 32) {
      unsigned Op = BO.getOpcode() == Instruction::Shl
                        ? Abyss::SLLI
                        : BO.getOpcode() == Instruction::LShr ? Abyss::SRLI
                                                              : Abyss::SRAI;
      i(Op, D, operandReg(T0, BO.getOperand(0)),
        static_cast<int64_t>(C1->getZExtValue()));
      Finish();
      return;
    }
    break;
  case Instruction::SDiv:
  case Instruction::SRem:
  case Instruction::UDiv:
  case Instruction::URem: {
    bool IsRem = BO.getOpcode() == Instruction::SRem ||
                 BO.getOpcode() == Instruction::URem;
    bool IsSigned = BO.getOpcode() == Instruction::SDiv ||
                    BO.getOpcode() == Instruction::SRem;
    int64_t Div = C1 ? C1->getSExtValue() : 0;
    // Unsigned division is always strength-reduced.  Signed division normally
    // keeps native DIV (the sign-correction sequence is slower on Abyss), but
    // a known-non-negative numerator has exactly the unsigned semantics.
    bool NonNegative =
        !IsSigned ||
        computeKnownBits(BO.getOperand(0), M.getDataLayout(), nullptr, &BO)
            .isNonNegative();
    if (!IsSigned || NonNegative) {
      if (Div > 0 && isPowerOf2_64(static_cast<uint64_t>(Div))) {
        unsigned K = Log2_64(static_cast<uint64_t>(Div));
        uint64_t Mask = static_cast<uint64_t>(Div) - 1;
        unsigned L = operandReg(T0, BO.getOperand(0));

        if (Div == 1) {
          if (IsRem)
            i(Abyss::ADDI, D, X0, 0);
          else
            i(Abyss::ADDI, D, L, 0);
          Finish();
          return;
        }
        if (!IsRem) {
          i(Abyss::SRLI, D, L, K);
        } else if (Mask <= 2047) {
          i(Abyss::ANDI, D, L, static_cast<int64_t>(Mask));
        } else {
          emitConst(T2, static_cast<int64_t>(Mask));
          r(Abyss::AND, D, L, T2);
        }
        Finish();
        return;
      }
    }
    break;
  }
  default:
    break;
  }

  unsigned L = operandReg(T0, BO.getOperand(0));
  unsigned R = operandReg(T1, BO.getOperand(1));
  switch (BO.getOpcode()) {
  case Instruction::Add:
    r(Abyss::ADD, D, L, R);
    break;
  case Instruction::Sub:
    r(Abyss::SUB, D, L, R);
    break;
  case Instruction::Mul:
    r(Abyss::MUL, D, L, R);
    break;
  case Instruction::And:
    r(Abyss::AND, D, L, R);
    break;
  case Instruction::Or:
    r(Abyss::OR, D, L, R);
    break;
  case Instruction::Xor:
    r(Abyss::XOR, D, L, R);
    break;
  case Instruction::Shl:
    r(Abyss::SLL, D, L, R);
    break;
  case Instruction::LShr:
    r(Abyss::SRL, D, L, R);
    break;
  case Instruction::AShr:
    r(Abyss::SRA, D, L, R);
    break;
  case Instruction::SDiv:
    r(Abyss::DIV, D, L, R);
    break;
  case Instruction::SRem:
    r(Abyss::REM, D, L, R);
    break;
  case Instruction::UDiv:
    r(Abyss::DIVU, D, L, R);
    break;
  case Instruction::URem:
    r(Abyss::REMU, D, L, R);
    break;
  default:
    report_fatal_error("Abyss: unsupported binary operator");
  }
  Finish();
}

void AbyssMC::emitICmp(ICmpInst &CI) {
  unsigned D = destReg(&CI, T2);
  auto Finish = [&]() { storeSlot(&CI, D); };

  // SLTI/SLTIU and XORI avoid materializing small right-hand constants.
  if (auto *CR = dyn_cast<ConstantInt>(CI.getOperand(1))) {
    int64_t K = CR->getSExtValue();
    if (K >= 0 && K <= 2047) {
      unsigned L = operandReg(T0, CI.getOperand(0));
      switch (CI.getPredicate()) {
      case CmpInst::ICMP_EQ:
        i(Abyss::XORI, D, L, K);
        i(Abyss::SLTIU, D, D, 1);
        Finish();
        return;
      case CmpInst::ICMP_NE:
        i(Abyss::XORI, D, L, K);
        r(Abyss::SLTU, D, X0, D);
        Finish();
        return;
      case CmpInst::ICMP_SLT:
        i(Abyss::SLTI, D, L, K);
        Finish();
        return;
      case CmpInst::ICMP_SGE:
        i(Abyss::SLTI, D, L, K);
        i(Abyss::XORI, D, D, 1);
        Finish();
        return;
      case CmpInst::ICMP_ULT:
        i(Abyss::SLTIU, D, L, K);
        Finish();
        return;
      case CmpInst::ICMP_UGE:
        i(Abyss::SLTIU, D, L, K);
        i(Abyss::XORI, D, D, 1);
        Finish();
        return;
      case CmpInst::ICMP_SLE:
      case CmpInst::ICMP_SGT:
      case CmpInst::ICMP_ULE:
      case CmpInst::ICMP_UGT:
        if (K < 2047) {
          bool Unsigned = CI.getPredicate() == CmpInst::ICMP_ULE ||
                          CI.getPredicate() == CmpInst::ICMP_UGT;
          unsigned Op = Unsigned ? Abyss::SLTIU : Abyss::SLTI;
          i(Op, D, L, K + 1);
          if (CI.getPredicate() == CmpInst::ICMP_SGT ||
              CI.getPredicate() == CmpInst::ICMP_UGT)
            i(Abyss::XORI, D, D, 1);
          Finish();
          return;
        }
        break;
      default:
        break;
      }
    }
  }

  unsigned L = operandReg(T0, CI.getOperand(0));
  unsigned R = operandReg(T1, CI.getOperand(1));

  switch (CI.getPredicate()) {
  case CmpInst::ICMP_EQ:
    r(Abyss::XOR, D, L, R);
    i(Abyss::SLTIU, D, D, 1);
    break;
  case CmpInst::ICMP_NE:
    r(Abyss::XOR, D, L, R);
    r(Abyss::SLTU, D, X0, D);
    break;
  case CmpInst::ICMP_SLT:
    r(Abyss::SLT, D, L, R);
    break;
  case CmpInst::ICMP_SGT:
    r(Abyss::SLT, D, R, L);
    break;
  case CmpInst::ICMP_SLE:
    r(Abyss::SLT, D, R, L);
    i(Abyss::XORI, D, D, 1);
    break;
  case CmpInst::ICMP_SGE:
    r(Abyss::SLT, D, L, R);
    i(Abyss::XORI, D, D, 1);
    break;
  case CmpInst::ICMP_ULT:
    r(Abyss::SLTU, D, L, R);
    break;
  case CmpInst::ICMP_UGT:
    r(Abyss::SLTU, D, R, L);
    break;
  case CmpInst::ICMP_ULE:
    r(Abyss::SLTU, D, R, L);
    i(Abyss::XORI, D, D, 1);
    break;
  case CmpInst::ICMP_UGE:
    r(Abyss::SLTU, D, L, R);
    i(Abyss::XORI, D, D, 1);
    break;
  default:
    report_fatal_error("Abyss: unsupported icmp predicate");
  }
  storeSlot(&CI, D);
}

void AbyssMC::emitGEP(GetElementPtrInst &GEP) {
  const DataLayout &DL = M.getDataLayout();
  Type *Ty = GEP.getSourceElementType();

  // The common `p + index * sizeof(T)` form with a null base needs no zero
  // accumulator and no final ADD: scale the index directly.
  if (isa<ConstantPointerNull>(GEP.getPointerOperand()) &&
      GEP.getNumOperands() == 2) {
    unsigned D = destReg(&GEP, T0);
    unsigned Src = operandReg(D, GEP.getOperand(1));
    uint64_t Size = DL.getTypeAllocSize(Ty).getFixedValue();
    if (Size == 1) {
      if (Src != D)
        i(Abyss::ADDI, D, Src, 0);
    } else if (isPowerOf2_64(Size)) {
      i(Abyss::SLLI, D, Src, Log2_64(Size));
    } else {
      emitConst(T1, static_cast<int64_t>(Size));
      r(Abyss::MUL, D, Src, T1);
    }
    storeSlot(&GEP, D);
    return;
  }

  // Address accumulator lives in T0.
  loadValue(T0, GEP.getPointerOperand());

  auto AddScaledIndex = [&](Value *Idx, uint64_t Size) {
    loadValue(T1, Idx);
    if (Size != 1) {
      if (isPowerOf2_64(Size)) {
        i(Abyss::SLLI, T1, T1, Log2_64(Size));
      } else {
        emitConst(T2, static_cast<int64_t>(Size));
        r(Abyss::MUL, T1, T1, T2);
      }
    }
    r(Abyss::ADD, T0, T0, T1);
  };

  // The first GEP index walks over whole source elements.
  AddScaledIndex(GEP.getOperand(1),
                 DL.getTypeAllocSize(Ty).getFixedValue());

  for (unsigned OpNo = 2; OpNo < GEP.getNumOperands(); ++OpNo) {
    if (auto *ST = dyn_cast<StructType>(Ty)) {
      auto *CI = dyn_cast<ConstantInt>(GEP.getOperand(OpNo));
      if (!CI)
        report_fatal_error("Abyss: non-constant struct GEP index");
      unsigned Field = CI->getZExtValue();
      int64_t Offset = DL.getStructLayout(ST)->getElementOffset(Field);
      if (Offset) {
        emitConst(T1, Offset);
        r(Abyss::ADD, T0, T0, T1);
      }
      Ty = ST->getElementType(Field);
      continue;
    }

    if (auto *AT = dyn_cast<ArrayType>(Ty)) {
      AddScaledIndex(GEP.getOperand(OpNo),
                     DL.getTypeAllocSize(AT->getElementType()).getFixedValue());
      Ty = AT->getElementType();
      continue;
    }

    report_fatal_error("Abyss: unsupported GEP aggregate type");
  }

  storeSlot(&GEP, T0);
}

void AbyssMC::emitLoad(LoadInst &LI) {
  unsigned P = operandReg(T0, LI.getPointerOperand());
  Type *Ty = LI.getType();
  bool IsUnsigned = false;
  for (User *U : LI.users())
    if (isa<ZExtInst>(U))
      IsUnsigned = true;

  unsigned Op = Abyss::LDR;
  if (Ty->isPointerTy())
    Op = Abyss::LDR;
  else if (Ty->isIntegerTy(8))
    Op = IsUnsigned ? Abyss::LBU : Abyss::LB;
  else if (Ty->isIntegerTy(16))
    Op = IsUnsigned ? Abyss::LHU : Abyss::LH;
  else if (!Ty->isIntegerTy(32))
    report_fatal_error("Abyss: unsupported load type");
  unsigned D = destReg(&LI, T2);
  i(Op, D, P, 0);
  storeSlot(&LI, D);
}

void AbyssMC::emitStore(StoreInst &SI) {
  Type *Ty = SI.getValueOperand()->getType();
  unsigned Op = Abyss::STR;
  if (Ty->isPointerTy())
    Op = Abyss::STR;
  else if (Ty->isIntegerTy(8))
    Op = Abyss::SB;
  else if (Ty->isIntegerTy(16))
    Op = Abyss::SH;
  else if (!Ty->isIntegerTy(32))
    report_fatal_error("Abyss: unsupported store type");

  unsigned V = operandReg(T0, SI.getValueOperand());
  unsigned P = operandReg(T1, SI.getPointerOperand());
  s(Op, P, V, 0);
}

void AbyssMC::emitSelect(BasicBlock &BB, SelectInst &SI) {
  (void)BB;
  // Branchless select.  Abyss cores serialize divergent threads, so emitting
  // a real branch here is not only slower but can expose stale stores on the
  // inactive path.  Build an all-ones/all-zero mask instead:
  //   mask = -(cond)
  //   result = (true & mask) | (false & ~mask)
  // The condition is i1, so it is already 0/1; SUB is enough (no SLTU).
  unsigned C = operandReg(T0, SI.getCondition());
  r(Abyss::SUB, T0, X0, C);

  auto IsZero = [](const Value *V) {
    if (auto *CI = dyn_cast<ConstantInt>(V))
      return CI->isZero();
    return false;
  };
  bool TVZero = IsZero(SI.getTrueValue());
  bool FVZero = IsZero(SI.getFalseValue());
  unsigned D = destReg(&SI, T3);

  if (TVZero && FVZero) {
    i(Abyss::ADDI, D, X0, 0);
  } else if (FVZero) {
    // result = true & mask
    unsigned TV = operandReg(T1, SI.getTrueValue());
    r(Abyss::AND, D, TV, T0);
  } else if (TVZero) {
    // result = false & ~mask
    i(Abyss::XORI, T4, T0, -1);
    unsigned FV = operandReg(T1, SI.getFalseValue());
    r(Abyss::AND, D, FV, T4);
  } else {
    unsigned TV = operandReg(T1, SI.getTrueValue());
    unsigned FV = operandReg(T2, SI.getFalseValue());
    r(Abyss::AND, T3, TV, T0);
    i(Abyss::XORI, T4, T0, -1);
    r(Abyss::AND, T1, FV, T4);
    r(Abyss::OR, D, T3, T1);
  }
  storeSlot(&SI, D);
}

void AbyssMC::emitMinMax(CallInst &CI, BasicBlock &, bool IsMin,
                         bool IsUnsigned) {
  if (CI.arg_size() != 2)
    report_fatal_error("Abyss: min/max intrinsic expects two arguments");
  unsigned L = operandReg(T0, CI.getArgOperand(0));
  unsigned R = operandReg(T1, CI.getArgOperand(1));

  // Branchless min/max, for the same reason as emitSelect.  Keep the two
  // ANDs independent after the mask: on the in-order Abyss pipeline this is
  // faster than a shorter but more serial XOR-select chain.
  unsigned Cmp = IsUnsigned ? Abyss::SLTU : Abyss::SLT;
  if (IsMin)
    r(Cmp, T2, L, R);
  else
    r(Cmp, T2, R, L);
  r(Abyss::SUB, T2, X0, T2);

  r(Abyss::AND, T3, L, T2);
  i(Abyss::XORI, T4, T2, -1);
  r(Abyss::AND, T1, R, T4);
  unsigned D = destReg(&CI, T3);
  r(Abyss::OR, D, T3, T1);
  storeSlot(&CI, D);
}

void AbyssMC::emitMemSet(CallInst &CI) {
  if (CI.arg_size() < 3)
    report_fatal_error("Abyss: malformed memset");
  auto *Size = dyn_cast<ConstantInt>(CI.getArgOperand(2));
  auto *Value = dyn_cast<ConstantInt>(CI.getArgOperand(1));
  if (!Size || !Value)
    report_fatal_error("Abyss: non-constant memset size/value");
  uint64_t Bytes = Size->getZExtValue();
  uint64_t V = Value->getZExtValue() & 0xFF;
  unsigned D = operandReg(T0, CI.getArgOperand(0));
  uint64_t Off = 0;
  for (; Off + 4 <= Bytes; Off += 4) {
    emitConst(T1, static_cast<int64_t>(V | (V << 8) | (V << 16) |
                                       (V << 24)));
    s(Abyss::STR, D, T1, static_cast<int64_t>(Off));
  }
  if (Off != Bytes) {
    emitConst(T1, static_cast<int64_t>(V));
    s(Abyss::SB, D, T1, static_cast<int64_t>(Off));
  }
}

void AbyssMC::emitCall(CallInst &CI, BasicBlock &BB) {
  if (auto *IA = dyn_cast<InlineAsm>(CI.getCalledOperand())) {
    if (CI.getType()->isVoidTy())
      return;
    StringRef Asm = IA->getAsmString();
    unsigned Src = 0;
    if (Asm.contains("x29"))
      Src = Abyss::X29;
    else if (Asm.contains("x30"))
      Src = Abyss::X30;
    else if (Asm.contains("x31"))
      Src = Abyss::X31;
    else
      report_fatal_error("Abyss: unsupported inline asm: " + Asm);
    i(Abyss::ADDI, T0, Src, 0);
    storeSlot(&CI, T0);
    return;
  }

  Function *Callee = CI.getCalledFunction();
  if (!Callee)
    report_fatal_error("Abyss: indirect calls are not supported");
  if (auto *II = dyn_cast<IntrinsicInst>(&CI)) {
    switch (II->getIntrinsicID()) {
    case Intrinsic::umin:
      emitMinMax(CI, BB, /*IsMin=*/true, /*IsUnsigned=*/true);
      return;
    case Intrinsic::umax:
      emitMinMax(CI, BB, /*IsMin=*/false, /*IsUnsigned=*/true);
      return;
    case Intrinsic::smin:
      emitMinMax(CI, BB, /*IsMin=*/true, /*IsUnsigned=*/false);
      return;
    case Intrinsic::smax:
      emitMinMax(CI, BB, /*IsMin=*/false, /*IsUnsigned=*/false);
      return;
    case Intrinsic::memset:
      emitMemSet(CI);
      return;
    default:
      break;
    }
  }
  if (Callee->isIntrinsic()) {
    // Debug/lifetime and other no-op intrinsics have no machine code.
    return;
  }

  // Calls may clobber every allocatable register, so spill live block-local
  // values before the call and reload them lazily afterwards.  They are also
  // scheduling barriers.  This has to run before the argument registers are
  // loaded: A1..A7 are X11..X17, which the allocator may already be using.
  flushSchedule();
  flushBlockValues(/*Force=*/true);

  unsigned ArgNo = 0;
  for (Value *Arg : CI.args()) {
    if (ArgNo >= 8)
      report_fatal_error("Abyss: more than 8 call arguments are not supported");
    loadValue(A0 + ArgNo, Arg);
    ++ArgNo;
  }

  MCSymbol *CalleeSym = Ctx->getOrCreateSymbol(Callee->getName());
  jal(RA, CalleeSym);
  if (!CI.getType()->isVoidTy())
    storeSlot(&CI, A0);
  beginSchedule();
}

void AbyssMC::emitInstruction(Function &F, BasicBlock &BB, Instruction &I) {
  (void)F;
  (void)BB;
  if (isa<PHINode>(I))
    return;
  if (auto *BO = dyn_cast<BinaryOperator>(&I)) {
    if (emitPinnedPhiUpdate(*BO, BB))
      return;
    emitBinary(*BO);
    return;
  }
  if (auto *CI = dyn_cast<ICmpInst>(&I)) {
    emitICmp(*CI);
    return;
  }
  if (auto *GEP = dyn_cast<GetElementPtrInst>(&I)) {
    emitGEP(*GEP);
    return;
  }
  if (auto *LI = dyn_cast<LoadInst>(&I)) {
    emitLoad(*LI);
    return;
  }
  if (auto *SI = dyn_cast<StoreInst>(&I)) {
    emitStore(*SI);
    return;
  }
  if (auto *SI = dyn_cast<SelectInst>(&I)) {
    emitSelect(BB, *SI);
    return;
  }
  if (auto *CI = dyn_cast<CallInst>(&I)) {
    emitCall(*CI, BB);
    return;
  }
  if (auto *FI = dyn_cast<FreezeInst>(&I)) {
    // Keep freeze as a real copy: aliasing it to the operand would extend the
    // operand's live range past the point recorded by the block allocator.
    emitCopy(I, FI->getOperand(0));
    return;
  }
  if (auto *Cast = dyn_cast<CastInst>(&I)) {
    emitCopy(I, Cast->getOperand(0));
    return;
  }
  if (auto *AI = dyn_cast<AllocaInst>(&I)) {
    (void)AI;
    // The pointer value was initialized in the prologue.
    return;
  }
  if (isa<DbgInfoIntrinsic>(I))
    return;

  report_fatal_error(Twine("Abyss: unsupported instruction: ") +
                     I.getOpcodeName());
}

void AbyssMC::emitBranchPred(CmpInst::Predicate Pred, unsigned L, unsigned R,
                             MCSymbol *Target) {
  switch (Pred) {
  case CmpInst::ICMP_EQ:
    b(Abyss::BEQ, L, R, Target);
    break;
  case CmpInst::ICMP_NE:
    b(Abyss::BNE, L, R, Target);
    break;
  case CmpInst::ICMP_SLT:
    b(Abyss::BLT, L, R, Target);
    break;
  case CmpInst::ICMP_SGT:
    b(Abyss::BLT, R, L, Target);
    break;
  case CmpInst::ICMP_SLE:
    b(Abyss::BGE, R, L, Target);
    break;
  case CmpInst::ICMP_SGE:
    b(Abyss::BGE, L, R, Target);
    break;
  case CmpInst::ICMP_ULT:
    b(Abyss::BLTU, L, R, Target);
    break;
  case CmpInst::ICMP_UGT:
    b(Abyss::BLTU, R, L, Target);
    break;
  case CmpInst::ICMP_ULE:
    b(Abyss::BGEU, R, L, Target);
    break;
  case CmpInst::ICMP_UGE:
    b(Abyss::BGEU, L, R, Target);
    break;
  default:
    report_fatal_error("Abyss: unsupported branch predicate");
  }
}

void AbyssMC::emitPhiStores(BasicBlock *Pred, BasicBlock *Succ) {
  struct PinnedMove {
    PHINode *PN;
    unsigned Reg;
    const Value *In;
  };
  SmallVector<PinnedMove, 2> Moves;

  // First store every non-pinned PHI.  A pinned move can clobber a register
  // that still holds another PHI's incoming value, so it must run afterwards.
  for (PHINode &PN : Succ->phis()) {
    const Value *In = PN.getIncomingValueForBlock(Pred);
    if (!In)
      continue;
    In = resolve(In);

    auto PinIt = PinnedReg.find(&PN);
    if (PinIt != PinnedReg.end()) {
      Moves.push_back({&PN, PinIt->second, In});
      continue;
    }

    loadValue(T0, In);
    // PHI values are block-entry values: the target block always reloads them
    // from their stack slot, even if they are cached in a register here.
    s(Abyss::STR, SP, T0, slotOffset(&PN));
  }

  // Load all pinned sources into stable scratch registers before any move can
  // overwrite another pinned destination.
  for (unsigned I = 0; I != Moves.size(); ++I)
    if (Moves[I].In != Moves[I].PN)
      loadValue(T1 + I, Moves[I].In);

  for (unsigned I = 0; I != Moves.size(); ++I) {
    PinnedMove &M = Moves[I];
    if (M.In != M.PN)
      i(Abyss::ADDI, M.Reg, T1 + I, 0);
    // The value stays live in M.Reg inside the loop.  Only an edge leaving
    // the loop needs the stack slot for later readers.
    const Loop *L = PinnedLoop.lookup(M.PN);
    if (!L || !L->contains(Succ))
      s(Abyss::STR, SP, M.Reg, slotOffset(M.PN));
  }
}

bool AbyssMC::edgeLeavesPinnedLoop(const BasicBlock *BB,
                                   const BasicBlock *Succ) const {
  for (auto &KV : PinnedLoop)
    if (KV.second->contains(BB) && !KV.second->contains(Succ))
      return true;
  return false;
}

// Pinned loop PHIs live only in their registers while the loop runs.  An edge
// that leaves the loop has to materialize them into their stack slots, even
// when the successor has no PHIs of its own: emitPhiStores() is only reached
// through a successor's PHIs, so a plain exit block would otherwise read an
// uninitialized slot.
void AbyssMC::emitPinnedExitStores(const BasicBlock *BB,
                                   const BasicBlock *Succ) {
  for (auto &KV : PinnedLoop) {
    const Loop *L = KV.second;
    if (!L->contains(BB) || L->contains(Succ))
      continue;
    s(Abyss::STR, SP, PinnedReg.lookup(KV.first), slotOffset(KV.first));
  }
}

void AbyssMC::emitTerminator(Function &F, BasicBlock &BB, Instruction &I) {
  (void)F;
  if (auto *RI = dyn_cast<ReturnInst>(&I)) {
    if (Value *RV = RI->getReturnValue())
      loadValue(A0, RV);
    i(Abyss::LDR, RA, SP, -4);
    restoreStackPointer();
    ret();
    return;
  }

  if (auto *BI = dyn_cast<BranchInst>(&I)) {
    auto HasPhi = [](const BasicBlock *B) { return !B->phis().empty(); };

    if (BI->isUnconditional()) {
      BasicBlock *Succ = BI->getSuccessor(0);
      if (!HasPhi(Succ) && !edgeLeavesPinnedLoop(&BB, Succ)) {
        jal(X0, blockSymbol(Succ));
        return;
      }
      MCSymbol *Edge = newLabel(&BB, "edge.u");
      jal(X0, Edge);
      Streamer->emitLabel(Edge);
      emitPinnedExitStores(&BB, Succ);
      emitPhiStores(&BB, Succ);
      jal(X0, blockSymbol(Succ));
      return;
    }

    BasicBlock *TrueBB = BI->getSuccessor(0);
    BasicBlock *FalseBB = BI->getSuccessor(1);
    if (TrueBB == FalseBB) {
      jal(X0, blockSymbol(TrueBB));
      return;
    }

    // Branch away on the false condition and fall through into the true
    // successor.  This removes the unconditional jump that used to sit between
    // the branch and the true edge.  When a successor has no PHI and the edge
    // stays inside a pinned loop, branch to its block directly instead of
    // creating a one-instruction trampoline.
    bool TruePhi = HasPhi(TrueBB);
    bool FalsePhi = HasPhi(FalseBB);
    bool TrueExit = edgeLeavesPinnedLoop(&BB, TrueBB);
    bool FalseExit = edgeLeavesPinnedLoop(&BB, FalseBB);
    MCSymbol *TrueEdge =
        (TruePhi || TrueExit) ? newLabel(&BB, "edge.t") : nullptr;
    MCSymbol *FalseEdge =
        (FalsePhi || FalseExit) ? newLabel(&BB, "edge.f") : nullptr;
    MCSymbol *FalseTarget = FalseEdge ? FalseEdge : blockSymbol(FalseBB);

    if (auto *Cmp = dyn_cast<ICmpInst>(BI->getCondition())) {
      unsigned L = operandReg(T0, Cmp->getOperand(0));
      unsigned R = operandReg(T1, Cmp->getOperand(1));
      emitBranchPred(CmpInst::getInversePredicate(Cmp->getPredicate()), L, R,
                     FalseTarget);
    } else {
      loadValue(T0, BI->getCondition());
      b(Abyss::BEQ, T0, X0, FalseTarget);
    }

    if (TrueEdge) {
      Streamer->emitLabel(TrueEdge);
      emitPinnedExitStores(&BB, TrueBB);
      emitPhiStores(&BB, TrueBB);
    }
    jal(X0, blockSymbol(TrueBB));

    if (FalseEdge) {
      Streamer->emitLabel(FalseEdge);
      emitPinnedExitStores(&BB, FalseBB);
      emitPhiStores(&BB, FalseBB);
      jal(X0, blockSymbol(FalseBB));
    }
    return;
  }

  if (isa<UnreachableInst>(&I)) {
    i(Abyss::LDR, RA, SP, -4);
    restoreStackPointer();
    ret();
    return;
  }

  report_fatal_error("Abyss: unsupported terminator");
}

class AbyssEmitPass : public ModulePass {
  AbyssTargetMachine &TM;
  raw_pwrite_stream &Out;
  CodeGenFileType FileType;

public:
  static char ID;
  AbyssEmitPass(AbyssTargetMachine &TM, raw_pwrite_stream &Out,
                CodeGenFileType FileType)
      : ModulePass(ID), TM(TM), Out(Out), FileType(FileType) {}
  bool runOnModule(Module &M) override {
    AbyssMC MC(M, TM.getTarget(), Out, FileType);
    MC.emit();
    return false;
  }
};

char AbyssEmitPass::ID = 0;

} // namespace

ModulePass *llvm::createAbyssEmitPass(AbyssTargetMachine &TM,
                                      raw_pwrite_stream &Out,
                                      CodeGenFileType FileType) {
  return new AbyssEmitPass(TM, Out, FileType);
}
