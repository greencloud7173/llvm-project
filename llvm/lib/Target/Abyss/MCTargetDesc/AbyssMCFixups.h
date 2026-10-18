//===-- AbyssMCFixups.h - Abyss-specific fixup kinds ------------*- C++ -*-===//

#ifndef LLVM_LIB_TARGET_ABYSS_MCTARGETDESC_ABYSSMCFIXUPS_H
#define LLVM_LIB_TARGET_ABYSS_MCTARGETDESC_ABYSSMCFIXUPS_H

#include "llvm/MC/MCFixup.h"

namespace llvm {
namespace Abyss {

enum Fixups {
  // PC-relative 13-bit B-type branch offset (scattered in the instruction).
  FK_ABYSS_BRANCH = FirstTargetFixupKind,
  // PC-relative 21-bit J-type JAL offset (scattered in the instruction).
  FK_ABYSS_JAL,
  NumTargetFixupKinds
};

} // namespace Abyss
} // namespace llvm

#endif
