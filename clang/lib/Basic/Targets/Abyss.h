//===--- Abyss.h - Declare Abyss target feature support --------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file declares Abyss TargetInfo objects.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LIB_BASIC_TARGETS_ABYSS_H
#define LLVM_CLANG_LIB_BASIC_TARGETS_ABYSS_H

#include "clang/Basic/TargetInfo.h"
#include "clang/Basic/TargetOptions.h"
#include "llvm/Support/Compiler.h"
#include "llvm/TargetParser/Triple.h"

namespace clang {
namespace targets {

// Abyss target: 32-bit little-endian GPU ISA.
class LLVM_LIBRARY_VISIBILITY AbyssTargetInfo : public TargetInfo {
public:
  AbyssTargetInfo(const llvm::Triple &Triple, const TargetOptions &)
      : TargetInfo(Triple) {
    TLSSupported = false;
    NoAsmVariants = true;

    ShortWidth = 16;
    ShortAlign = 16;
    IntWidth = 32;
    IntAlign = 32;
    LongWidth = 32;
    LongAlign = 32;
    LongLongWidth = 64;
    LongLongAlign = 64;
    PointerWidth = 32;
    PointerAlign = 32;
    SuitableAlign = 32;

    HalfWidth = 16;
    HalfAlign = 16;
    FloatWidth = 32;
    FloatAlign = 32;
    DoubleWidth = 64;
    DoubleAlign = 64;
    LongDoubleWidth = 64;
    LongDoubleAlign = 64;
    FloatFormat = &llvm::APFloat::IEEEsingle();
    DoubleFormat = &llvm::APFloat::IEEEdouble();
    LongDoubleFormat = &llvm::APFloat::IEEEdouble();

    SizeType = UnsignedInt;
    IntMaxType = SignedLongLong;
    IntPtrType = SignedInt;
    PtrDiffType = SignedInt;
    WCharType = SignedInt;
    WIntType = SignedInt;
    Char16Type = UnsignedShort;
    Char32Type = UnsignedInt;
    SigAtomicType = SignedInt;

    resetDataLayout(
        "e-m:e-p:32:32-i8:8:32-i16:16:32-i32:32:32-i64:64:64-n32-S32");
  }

  void getTargetDefines(const LangOptions &Opts,
                        MacroBuilder &Builder) const override;

  llvm::SmallVector<Builtin::InfosShard> getTargetBuiltins() const override {
    return {};
  }

  bool allowsLargerPreferedTypeAlignment() const override { return false; }

  StringRef getABI() const override { return "ilp32"; }

  bool hasFeature(StringRef Feature) const override {
    return Feature == "abyss";
  }

  BuiltinVaListKind getBuiltinVaListKind() const override {
    return TargetInfo::VoidPtrBuiltinVaList;
  }

  std::string_view getClobbers() const override { return ""; }

  bool validateAsmConstraint(const char *&Name,
                             TargetInfo::ConstraintInfo &Info) const override {
    switch (*Name) {
    case 'r':
      Info.setAllowsRegister();
      return true;
    case 'm':
      Info.setAllowsMemory();
      return true;
    case 'i':
      Info.setRequiresImmediate();
      return true;
    default:
      return false;
    }
  }

  ArrayRef<const char *> getGCCRegNames() const override;
  ArrayRef<TargetInfo::GCCRegAlias> getGCCRegAliases() const override;
};

} // namespace targets
} // namespace clang

#endif // LLVM_CLANG_LIB_BASIC_TARGETS_ABYSS_H
