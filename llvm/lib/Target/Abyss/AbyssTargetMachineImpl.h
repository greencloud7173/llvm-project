//===-- AbyssTargetMachineImpl.h - Minimal Abyss target machine -*- C++ -*-===//
//
// This is a deliberately small target machine.  The Abyss instruction set is
// already fully described by the MC layer, so this backend emits Abyss MCInsts
// directly from LLVM IR instead of going through SelectionDAG.  It is enough
// for the integer code used by the Abyss tests and keeps the build small.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_ABYSS_ABYSTARGETMACHINEIMPL_H
#define LLVM_LIB_TARGET_ABYSS_ABYSTARGETMACHINEIMPL_H

#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/CodeGen/CodeGenTargetMachineImpl.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include <memory>
#include <optional>

namespace llvm {

class AbyssSubtarget : public TargetSubtargetInfo {
public:
  explicit AbyssSubtarget(const Triple &TT)
      : TargetSubtargetInfo(TT, "", "", "", {}, {}, {}, nullptr, nullptr,
                            nullptr, nullptr, nullptr, nullptr) {}

  const TargetRegisterInfo *getRegisterInfo() const override { return nullptr; }
};

class AbyssTargetMachine : public CodeGenTargetMachineImpl {
  std::unique_ptr<TargetLoweringObjectFile> TLOF;
  AbyssSubtarget Subtarget;
  // TargetTransformInfo keeps a reference to the DataLayout, so keep a
  // stable copy here instead of passing a temporary.
  DataLayout AbyssDataLayout;

public:
  AbyssTargetMachine(const Target &T, const Triple &TT, StringRef CPU,
                     StringRef FS, const TargetOptions &Options,
                     std::optional<Reloc::Model> RM,
                     std::optional<CodeModel::Model> CM, CodeGenOptLevel OL,
                     bool JIT);
  ~AbyssTargetMachine() override;

  const AbyssSubtarget *getSubtargetImpl(const Function &F) const override {
    return &Subtarget;
  }

  TargetTransformInfo getTargetTransformInfo(
      const Function &F) const override;

  bool addPassesToEmitFile(PassManagerBase &PM, raw_pwrite_stream &Out,
                           raw_pwrite_stream *DwoOut, CodeGenFileType FileType,
                           bool DisableVerify = true,
                           MachineModuleInfoWrapperPass *MMIWP = nullptr) override;

  TargetLoweringObjectFile *getObjFileLowering() const override {
    return TLOF.get();
  }
};

ModulePass *createAbyssEmitPass(AbyssTargetMachine &TM,
                                raw_pwrite_stream &Out,
                                CodeGenFileType FileType);

} // namespace llvm

#endif
