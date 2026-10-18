#include "AbyssTargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"
#include "llvm/TargetParser/Triple.h"

using namespace llvm;

Target &llvm::getTheAbyssTarget() {
  static Target TheAbyssTarget;
  return TheAbyssTarget;
}

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void LLVMInitializeAbyssTargetInfo() {
  TargetRegistry::RegisterTarget(getTheAbyssTarget(), "abyss", "Abyss", "Abyss", 
  [](Triple::ArchType) { return false; }, 
  /*HasJIT=*/false);
}