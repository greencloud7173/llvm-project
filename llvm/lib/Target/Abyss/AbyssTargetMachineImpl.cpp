//===-- AbyssTargetMachineImpl.cpp - Minimal Abyss target machine ---------===//

#include "AbyssTargetMachineImpl.h"
#include "AbyssTargetInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/CodeGen/TargetLoweringObjectFileImpl.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"
#include <optional>

using namespace llvm;

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeAbyssTarget() {
  RegisterTargetMachine<AbyssTargetMachine> X(getTheAbyssTarget());
}

AbyssTargetMachine::AbyssTargetMachine(const Target &T, const Triple &TT,
                                       StringRef CPU, StringRef FS,
                                       const TargetOptions &Options,
                                       std::optional<Reloc::Model> RM,
                                       std::optional<CodeModel::Model> CM,
                                       CodeGenOptLevel OL, bool JIT)
    : CodeGenTargetMachineImpl(T, TT.computeDataLayout(), TT, /*CPU=*/StringRef(),
                               FS, Options, Reloc::Static, CodeModel::Small,
                               OL),
      TLOF(std::make_unique<TargetLoweringObjectFileELF>()),
      Subtarget(TT), AbyssDataLayout(TT.computeDataLayout()) {
  initAsmInfo();
}

AbyssTargetMachine::~AbyssTargetMachine() = default;

TargetTransformInfo
AbyssTargetMachine::getTargetTransformInfo(const Function &F) const {
  // The minimal backend does not have a SelectionDAG/TTI implementation.
  // A baseline TTI is sufficient for Clang's IR optimizations.
  return TargetTransformInfo(AbyssDataLayout);
}

bool AbyssTargetMachine::addPassesToEmitFile(
    PassManagerBase &PM, raw_pwrite_stream &Out, raw_pwrite_stream *DwoOut,
    CodeGenFileType FileType, bool DisableVerify,
    MachineModuleInfoWrapperPass *MMIWP) {
  if (FileType == CodeGenFileType::Null)
    return true;
  PM.add(createAbyssEmitPass(*this, Out, FileType));
  return false;
}
