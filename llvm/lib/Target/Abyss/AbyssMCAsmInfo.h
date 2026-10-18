#ifndef LLVM_LIB_TARGET_ABYSS_MCTARGETDESC_ABYSSMCASMINFO_H
#define LLVM_LIB_TARGET_ABYSS_MCTARGETDESC_ABYSSMCASMINFO_H

#include "llvm/MC/MCAsmInfoELF.h"
#include "llvm/TargetParser/Triple.h"

namespace llvm {

class AbyssMCAsmInfo : public MCAsmInfoELF {
public:
  explicit AbyssMCAsmInfo(const Triple &TT, const MCTargetOptions &Options) {
    CodePointerSize = 4;
    MinInstAlignment = 4;
    // '#' is used as the immediate prefix in Abyss assembly
    // (e.g. "addi R1, x0, #1"), so use ';' for comments.
    CommentString = ";";
    PrivateGlobalPrefix = ".L";
    PrivateLabelPrefix = ".L";
    SupportsDebugInformation = false;
    ExceptionsType = ExceptionHandling::None;
  }
};

} // namespace llvm

#endif