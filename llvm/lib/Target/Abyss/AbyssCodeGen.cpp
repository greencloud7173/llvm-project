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
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
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
  DenseMap<const BasicBlock *, std::string> BlockNames;
  DenseSet<const Value *> NeedsSlot;
  DenseSet<const Value *> NeedOwnSlot;
  DenseMap<const Value *, unsigned> RegAssign;
  DenseSet<const Value *> RegLive;
  SmallVector<const Value *, 32> BlockValues;
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

  void emitConst(unsigned Rd, int64_t Value);
  void loadValue(unsigned Rd, const Value *V);
  void storeSlot(const Value *V, unsigned Rs);
  unsigned destReg(const Value *V, unsigned Scratch) const;
  unsigned operandReg(unsigned Scratch, const Value *V);
  void beginBlockRegisters(BasicBlock &BB);
  void flushBlockValues(bool Force);
  unsigned slotOf(const Value *V) const;
  int slotOffset(const Value *V) const;

  void emitFunction(Function &F);
  void emitBlock(Function &F, BasicBlock &BB);
  void emitInstruction(Function &F, BasicBlock &BB, Instruction &I);
  void emitTerminator(Function &F, BasicBlock &BB, Instruction &I);
  void restoreStackPointer();
  void emitBinary(BinaryOperator &BO);
  void emitICmp(ICmpInst &CI);
  void emitGEP(GetElementPtrInst &GEP);
  void emitLoad(LoadInst &LI);
  void emitStore(StoreInst &SI);
  void emitSelect(BasicBlock &BB, SelectInst &SI);
  void emitCall(CallInst &CI, BasicBlock &BB);
  void emitMinMax(CallInst &CI, BasicBlock &BB, bool IsMin, bool IsUnsigned);
  void emitCopy(Instruction &I, const Value *V);
  void emitPhiStores(BasicBlock *Pred, BasicBlock *Succ);
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
  auto It = Slots.find(V);
  assert(It != Slots.end() && "value has no stack slot");
  return It->second;
}

int AbyssMC::slotOffset(const Value *V) const {
  return -4 * static_cast<int>(slotOf(V) + 1);
}

void AbyssMC::loadValue(unsigned Rd, const Value *V) {
  if (auto *CI = dyn_cast<ConstantInt>(V)) {
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
  auto It = RegAssign.find(V);
  return It == RegAssign.end() ? Scratch : It->second;
}

unsigned AbyssMC::operandReg(unsigned Scratch, const Value *V) {
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
    if (EntryUses.lookup(KV.first) > 1)
      Intervals.push_back({0, KV.second, KV.first, false});
  llvm::sort(Intervals, [](const Interval &A, const Interval &B) {
    return A.Start < B.Start;
  });

  // Classic linear scan over a small fixed register set.  A register can be
  // reused once its previous interval has ended before the new interval
  // starts.
  DenseMap<unsigned, unsigned> RegEnd;
  for (const Interval &It : Intervals) {
    for (unsigned Reg : AbyssAllocRegs) {
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
  BlockNames.clear();
  NeedsSlot.clear();
  NeedOwnSlot.clear();
  RegAssign.clear();
  RegLive.clear();
  BlockValues.clear();
  NextSlot = 1; // slot 0 is the saved return address
  NextLabel = 0;
  FnName = F.getName().str();

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
          // A PHI edge can read the register directly in emitPhiStores.
          if (!isa<PHINode>(U))
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
  CurFrameSize = alignTo(Used, 16);
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

  for (BasicBlock &BB : F)
    emitBlock(F, BB);
}

void AbyssMC::emitBlock(Function &F, BasicBlock &BB) {
  Streamer->emitLabel(blockSymbol(&BB));
  beginBlockRegisters(BB);
  for (Instruction &I : BB) {
    if (I.isTerminator()) {
      flushBlockValues(/*Force=*/false);
      emitTerminator(F, BB, I);
      break;
    }
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

void AbyssMC::emitBinary(BinaryOperator &BO) {
  unsigned L = operandReg(T0, BO.getOperand(0));
  unsigned R = operandReg(T1, BO.getOperand(1));
  unsigned D = destReg(&BO, T2);

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
  storeSlot(&BO, D);
}

void AbyssMC::emitICmp(ICmpInst &CI) {
  unsigned L = operandReg(T0, CI.getOperand(0));
  unsigned R = operandReg(T1, CI.getOperand(1));
  unsigned D = destReg(&CI, T2);

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
  //   mask = -(cond != 0)
  //   result = (true & mask) | (false & ~mask)
  loadValue(T0, SI.getCondition());
  r(Abyss::SLTU, T0, X0, T0);
  r(Abyss::SUB, T0, X0, T0);

  unsigned TV = operandReg(T1, SI.getTrueValue());
  unsigned FV = operandReg(T2, SI.getFalseValue());
  r(Abyss::AND, T3, TV, T0);
  i(Abyss::XORI, T4, T0, -1);
  r(Abyss::AND, T1, FV, T4);
  unsigned D = destReg(&SI, T3);
  r(Abyss::OR, D, T3, T1);
  storeSlot(&SI, D);
}

void AbyssMC::emitMinMax(CallInst &CI, BasicBlock &, bool IsMin,
                         bool IsUnsigned) {
  if (CI.arg_size() != 2)
    report_fatal_error("Abyss: min/max intrinsic expects two arguments");
  loadValue(T0, CI.getArgOperand(0));
  loadValue(T1, CI.getArgOperand(1));

  // Branchless min/max, for the same reason as emitSelect.
  unsigned Cmp = IsUnsigned ? Abyss::SLTU : Abyss::SLT;
  if (IsMin)
    r(Cmp, T2, T0, T1);
  else
    r(Cmp, T2, T1, T0);
  r(Abyss::SUB, T2, X0, T2);

  r(Abyss::AND, T3, T0, T2);
  i(Abyss::XORI, T4, T2, -1);
  r(Abyss::AND, T1, T1, T4);
  unsigned D = destReg(&CI, T3);
  r(Abyss::OR, D, T3, T1);
  storeSlot(&CI, D);
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
    default:
      break;
    }
  }
  if (Callee->isIntrinsic()) {
    // Debug/lifetime and other no-op intrinsics have no machine code.
    return;
  }

  unsigned ArgNo = 0;
  for (Value *Arg : CI.args()) {
    if (ArgNo >= 8)
      report_fatal_error("Abyss: more than 8 call arguments are not supported");
    loadValue(A0 + ArgNo, Arg);
    ++ArgNo;
  }

  // Calls may clobber every allocatable register, so spill live block-local
  // values before the call and reload them lazily afterwards.
  flushBlockValues(/*Force=*/true);
  MCSymbol *CalleeSym = Ctx->getOrCreateSymbol(Callee->getName());
  jal(RA, CalleeSym);
  if (!CI.getType()->isVoidTy())
    storeSlot(&CI, A0);
}

void AbyssMC::emitInstruction(Function &F, BasicBlock &BB, Instruction &I) {
  (void)F;
  (void)BB;
  if (isa<PHINode>(I))
    return;
  if (auto *BO = dyn_cast<BinaryOperator>(&I)) {
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
  for (PHINode &PN : Succ->phis()) {
    Value *In = PN.getIncomingValueForBlock(Pred);
    if (!In)
      continue;
    loadValue(T0, In);
    // PHI values are block-entry values: the target block always reloads them
    // from their stack slot, even if they are cached in a register here.
    s(Abyss::STR, SP, T0, slotOffset(&PN));
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
    if (BI->isUnconditional()) {
      BasicBlock *Succ = BI->getSuccessor(0);
      MCSymbol *Edge = newLabel(&BB, "edge.u");
      jal(X0, Edge);
      Streamer->emitLabel(Edge);
      emitPhiStores(&BB, Succ);
      jal(X0, blockSymbol(Succ));
      return;
    }

    BasicBlock *TrueBB = BI->getSuccessor(0);
    BasicBlock *FalseBB = BI->getSuccessor(1);
    MCSymbol *TrueEdge = newLabel(&BB, "edge.t");
    MCSymbol *FalseEdge = newLabel(&BB, "edge.f");

    if (auto *Cmp = dyn_cast<ICmpInst>(BI->getCondition())) {
      loadValue(T0, Cmp->getOperand(0));
      loadValue(T1, Cmp->getOperand(1));
      emitBranchPred(Cmp->getPredicate(), T0, T1, TrueEdge);
    } else {
      loadValue(T0, BI->getCondition());
      b(Abyss::BNE, T0, X0, TrueEdge);
    }
    jal(X0, FalseEdge);

    Streamer->emitLabel(TrueEdge);
    emitPhiStores(&BB, TrueBB);
    jal(X0, blockSymbol(TrueBB));

    Streamer->emitLabel(FalseEdge);
    emitPhiStores(&BB, FalseBB);
    jal(X0, blockSymbol(FalseBB));
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
