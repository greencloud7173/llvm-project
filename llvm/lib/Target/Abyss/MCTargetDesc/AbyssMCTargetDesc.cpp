//===-- AbyssMCTargetDesc.cpp - Abyss target descriptions -----------------===//

#include "AbyssMCTargetDesc.h"
#include "AbyssInstPrinter.h"
#include "AbyssMCAsmInfo.h"
#include "AbyssTargetInfo.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"
#include "llvm/TargetParser/Triple.h"

#define GET_INSTRINFO_MC_DESC
#define ENABLE_INSTR_PREDICATE_VERIFIER
#include "AbyssGenInstrInfo.inc"

#define GET_SUBTARGETINFO_MC_DESC
#include "AbyssGenSubtargetInfo.inc"

#define GET_REGINFO_MC_DESC
#include "AbyssGenRegisterInfo.inc"

using namespace llvm;

static MCInstrInfo *createAbyssMCInstrInfo() {
  auto *X = new MCInstrInfo();
  InitAbyssMCInstrInfo(X);
  return X;
}

static MCRegisterInfo *createAbyssMCRegisterInfo(const Triple &) {
  auto *X = new MCRegisterInfo();
  // X1 is the return-address register used for unwind information.
  InitAbyssMCRegisterInfo(X, Abyss::X1);
  return X;
}

static MCSubtargetInfo *createAbyssMCSubtargetInfo(const Triple &TT,
                                                   StringRef CPU, StringRef FS) {
  return createAbyssMCSubtargetInfoImpl(TT, CPU, /*TuneCPU=*/CPU, FS);
}

static MCStreamer *
createAbyssMCStreamer(const Triple &, MCContext &Ctx,
                      std::unique_ptr<MCAsmBackend> &&MAB,
                      std::unique_ptr<MCObjectWriter> &&OW,
                      std::unique_ptr<MCCodeEmitter> &&Emitter) {
  return createELFStreamer(Ctx, std::move(MAB), std::move(OW),
                           std::move(Emitter));
}

static MCInstPrinter *createAbyssMCInstPrinter(const Triple &,
                                               unsigned SyntaxVariant,
                                               const MCAsmInfo &MAI,
                                               const MCInstrInfo &MII,
                                               const MCRegisterInfo &MRI) {
  return SyntaxVariant == 0 ? new AbyssInstPrinter(MAI, MII, MRI) : nullptr;
}

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeAbyssTargetMC() {
  Target &T = getTheAbyssTarget();

  RegisterMCAsmInfo<AbyssMCAsmInfo> X(T);
  TargetRegistry::RegisterMCInstrInfo(T, createAbyssMCInstrInfo);
  TargetRegistry::RegisterMCRegInfo(T, createAbyssMCRegisterInfo);
  TargetRegistry::RegisterMCSubtargetInfo(T, createAbyssMCSubtargetInfo);
  TargetRegistry::RegisterELFStreamer(T, createAbyssMCStreamer);
  TargetRegistry::RegisterMCInstPrinter(T, createAbyssMCInstPrinter);
  TargetRegistry::RegisterMCCodeEmitter(T, createAbyssMCCodeEmitter);
  TargetRegistry::RegisterMCAsmBackend(T, createAbyssAsmBackend);
}
