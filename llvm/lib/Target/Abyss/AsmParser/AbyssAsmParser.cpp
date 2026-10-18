//===-- AbyssAsmParser.cpp - Parse Abyss assembly to MCInst ---------------===//

#include "MCTargetDesc/AbyssMCTargetDesc.h"
#include "AbyssTargetInfo.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCParser/AsmLexer.h"
#include "llvm/MC/MCParser/MCParsedAsmOperand.h"
#include "llvm/MC/MCParser/MCTargetAsmParser.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Compiler.h"

using namespace llvm;

namespace {

struct AbyssOperand : public MCParsedAsmOperand {
  enum KindTy { Token, Register, Immediate } Kind;

  SMLoc StartLoc, EndLoc;
  StringRef Tok;
  MCRegister Reg;
  const MCExpr *Imm = nullptr;

  explicit AbyssOperand(KindTy K) : Kind(K) {}

  bool isToken() const override { return Kind == Token; }
  bool isReg() const override { return Kind == Register; }
  bool isImm() const override { return Kind == Immediate; }
  bool isMem() const override { return false; }

  MCRegister getReg() const override { return Reg; }
  const MCExpr *getImm() const { return Imm; }
  StringRef getToken() const { return Tok; }

  SMLoc getStartLoc() const override { return StartLoc; }
  SMLoc getEndLoc() const override { return EndLoc; }

  bool isConstantImm() const {
    return isImm() && isa<MCConstantExpr>(Imm);
  }
  int64_t getConstantImm() const {
    return cast<MCConstantExpr>(Imm)->getValue();
  }
  bool isSymbolRef() const {
    return isImm() && isa<MCSymbolRefExpr>(Imm);
  }

  // Immediate predicates called by the generated matcher.
  bool isImm12() const {
    return isConstantImm() && getConstantImm() >= -2048 &&
           getConstantImm() <= 2047;
  }
  bool isImm13() const {
    if (isSymbolRef())
      return true;
    return isConstantImm() && getConstantImm() >= -4096 &&
           getConstantImm() <= 4094 && (getConstantImm() % 4 == 0);
  }
  bool isImm20() const {
    return isConstantImm() && getConstantImm() >= 0 &&
           getConstantImm() <= 1048575;
  }
  bool isImm5() const {
    return isConstantImm() && getConstantImm() >= 0 &&
           getConstantImm() <= 31;
  }
  bool isImm21() const {
    if (isSymbolRef())
      return true;
    return isConstantImm() && getConstantImm() >= -1048576 &&
           getConstantImm() <= 1048574 && (getConstantImm() % 2 == 0);
  }

  void print(raw_ostream &OS, const MCAsmInfo &MAI) const override {
    switch (Kind) {
    case Register:
      if (Reg == Abyss::X29)
        OS << "%blockIdx";
      else if (Reg == Abyss::X30)
        OS << "%blockDim";
      else if (Reg == Abyss::X31)
        OS << "%threadIdx";
      else
        OS << "<register " << Reg.id() << ">";
      break;
    case Immediate:
      MAI.printExpr(OS, *Imm);
      break;
    case Token:
      OS << "'" << Tok << "'";
      break;
    }
  }

  void addRegOperands(MCInst &Inst, unsigned N) const {
    assert(N == 1 && "Invalid number of operands!");
    Inst.addOperand(MCOperand::createReg(Reg));
  }

  void addImmOperands(MCInst &Inst, unsigned N) const {
    assert(N == 1 && "Invalid number of operands!");
    if (auto *CE = dyn_cast<MCConstantExpr>(Imm))
      Inst.addOperand(MCOperand::createImm(CE->getValue()));
    else
      Inst.addOperand(MCOperand::createExpr(Imm));
  }

  static std::unique_ptr<AbyssOperand> createToken(StringRef Str, SMLoc S) {
    auto Op = std::make_unique<AbyssOperand>(Token);
    Op->Tok = Str;
    Op->StartLoc = Op->EndLoc = S;
    return Op;
  }

  static std::unique_ptr<AbyssOperand> createReg(MCRegister Reg, SMLoc S,
                                                 SMLoc E) {
    auto Op = std::make_unique<AbyssOperand>(Register);
    Op->Reg = Reg;
    Op->StartLoc = S;
    Op->EndLoc = E;
    return Op;
  }

  static std::unique_ptr<AbyssOperand> createImm(const MCExpr *Val, SMLoc S,
                                                 SMLoc E) {
    auto Op = std::make_unique<AbyssOperand>(Immediate);
    Op->Imm = Val;
    Op->StartLoc = S;
    Op->EndLoc = E;
    return Op;
  }
};

class AbyssAsmParser : public MCTargetAsmParser {
  SMLoc getLoc() const { return getParser().getTok().getLoc(); }

  bool matchAndEmitInstruction(SMLoc IDLoc, unsigned &Opcode,
                               OperandVector &Operands, MCStreamer &Out,
                               uint64_t &ErrorInfo,
                               bool MatchingInlineAsm) override;

  bool parseRegister(MCRegister &Reg, SMLoc &StartLoc, SMLoc &EndLoc) override;
  ParseStatus tryParseRegister(MCRegister &Reg, SMLoc &StartLoc,
                               SMLoc &EndLoc) override;

  bool parseInstruction(ParseInstructionInfo &Info, StringRef Name,
                        SMLoc NameLoc, OperandVector &Operands) override;

  ParseStatus parseRegisterOperand(OperandVector &Operands);
  ParseStatus parseImmediateOperand(OperandVector &Operands);

#define GET_ASSEMBLER_HEADER
#include "AbyssGenAsmMatcher.inc"

public:
  enum AbyssMatchResultTy {
    Match_Dummy = FIRST_TARGET_MATCH_RESULT_TY,
#define GET_OPERAND_DIAGNOSTIC_TYPES
#include "AbyssGenAsmMatcher.inc"
#undef GET_OPERAND_DIAGNOSTIC_TYPES
  };

  AbyssAsmParser(const MCSubtargetInfo &STI, MCAsmParser &Parser,
                 const MCInstrInfo &MII, const MCTargetOptions &Options)
      : MCTargetAsmParser(Options, STI, MII) {
    setAvailableFeatures(ComputeAvailableFeatures(STI.getFeatureBits()));
  }
};

} // namespace

#define GET_REGISTER_MATCHER
#define GET_MATCHER_IMPLEMENTATION
#include "AbyssGenAsmMatcher.inc"

// AltNames are not part of the generated MatchRegisterName table, so accept
// the R29/R30/R31 spellings explicitly in addition to blockIdx/blockDim/
// threadIdx.
static MCRegister matchAbyssRegisterName(StringRef Name) {
  if (MCRegister Reg = MatchRegisterName(Name))
    return Reg;
  return StringSwitch<MCRegister>(Name)
      .Case("R29", Abyss::X29)
      .Case("R30", Abyss::X30)
      .Case("R31", Abyss::X31)
      .Default(Abyss::NoRegister);
}

ParseStatus AbyssAsmParser::tryParseRegister(MCRegister &Reg, SMLoc &StartLoc,
                                             SMLoc &EndLoc) {
  const AsmToken &Tok = getParser().getTok();
  StartLoc = Tok.getLoc();
  EndLoc = Tok.getEndLoc();
  Reg = Abyss::NoRegister;

  // %blockIdx / %blockDim / %threadIdx
  if (Tok.is(AsmToken::Percent)) {
    getParser().Lex();
    if (getLexer().isNot(AsmToken::Identifier))
      return ParseStatus::NoMatch;
    Reg = matchAbyssRegisterName(getLexer().getTok().getIdentifier());
    if (!Reg)
      return ParseStatus::NoMatch;
    getParser().Lex();
    return ParseStatus::Success;
  }

  if (Tok.isNot(AsmToken::Identifier))
    return ParseStatus::NoMatch;

  Reg = matchAbyssRegisterName(Tok.getIdentifier());
  if (!Reg)
    return ParseStatus::NoMatch;

  getParser().Lex();
  return ParseStatus::Success;
}

bool AbyssAsmParser::parseRegister(MCRegister &Reg, SMLoc &StartLoc,
                                   SMLoc &EndLoc) {
  if (!tryParseRegister(Reg, StartLoc, EndLoc).isSuccess())
    return Error(StartLoc, "invalid register name");
  return false;
}

ParseStatus AbyssAsmParser::parseRegisterOperand(OperandVector &Operands) {
  SMLoc S = getLoc();
  MCRegister Reg;
  SMLoc E;
  if (!tryParseRegister(Reg, S, E).isSuccess())
    return ParseStatus::NoMatch;
  Operands.push_back(AbyssOperand::createReg(Reg, S, E));
  return ParseStatus::Success;
}

ParseStatus AbyssAsmParser::parseImmediateOperand(OperandVector &Operands) {
  switch (getLexer().getKind()) {
  default:
    return ParseStatus::NoMatch;
  case AsmToken::LParen:
  case AsmToken::Minus:
  case AsmToken::Plus:
  case AsmToken::Integer:
  case AsmToken::Identifier:
    break;
  }

  SMLoc S = getLoc();
  const MCExpr *Val = nullptr;
  if (getParser().parseExpression(Val))
    return ParseStatus::Failure;

  SMLoc E = SMLoc::getFromPointer(S.getPointer() - 1);
  Operands.push_back(AbyssOperand::createImm(Val, S, E));
  return ParseStatus::Success;
}

bool AbyssAsmParser::parseInstruction(ParseInstructionInfo &Info,
                                      StringRef Name, SMLoc NameLoc,
                                      OperandVector &Operands) {
  Operands.push_back(AbyssOperand::createToken(Name, NameLoc));

  while (!getLexer().is(AsmToken::EndOfStatement)) {
    if (getLexer().is(AsmToken::Comma)) {
      getLexer().Lex();
      continue;
    }

    // Immediate prefix used by ADDI / LI.
    if (getLexer().is(AsmToken::Hash)) {
      Operands.push_back(AbyssOperand::createToken("#", getLoc()));
      getLexer().Lex();
      continue;
    }

    // LDR/STR memory operands: imm(rs1)
    if (getLexer().is(AsmToken::LParen)) {
      Operands.push_back(AbyssOperand::createToken("(", getLoc()));
      getLexer().Lex();
      continue;
    }
    if (getLexer().is(AsmToken::RParen)) {
      Operands.push_back(AbyssOperand::createToken(")", getLoc()));
      getLexer().Lex();
      continue;
    }

    if (parseRegisterOperand(Operands).isSuccess())
      continue;
    if (parseImmediateOperand(Operands).isSuccess())
      continue;

    return Error(getLexer().getLoc(), "unexpected token");
  }

  getParser().Lex(); // Eat EndOfStatement.
  return false;
}

bool AbyssAsmParser::matchAndEmitInstruction(
    SMLoc IDLoc, unsigned &Opcode, OperandVector &Operands, MCStreamer &Out,
    uint64_t &ErrorInfo, bool MatchingInlineAsm) {
  MCInst Inst;

  switch (MatchInstructionImpl(Operands, Inst, ErrorInfo, MatchingInlineAsm)) {
  case Match_Success:
    Inst.setLoc(IDLoc);
    Out.emitInstruction(Inst, getSTI());
    return false;
  case Match_MissingFeature:
    return Error(IDLoc, "instruction requires an unavailable feature");
  case Match_MnemonicFail:
    return Error(IDLoc, "unrecognized instruction mnemonic");
  case Match_InvalidOperand:
    return Error(IDLoc, "invalid operand for instruction");
  case Match_InvalidImm5:
    return Error(IDLoc, "shift amount out of range (0..31)");
  case Match_InvalidImm12:
    return Error(IDLoc, "immediate out of range (signed 12-bit)");
  case Match_InvalidImm13:
    return Error(IDLoc,
                 "branch offset must be a multiple of 4 in [-4096, 4094]");
  case Match_InvalidImm20:
    return Error(IDLoc, "immediate out of range (unsigned 20-bit)");
  case Match_InvalidImm21:
    return Error(IDLoc,
                 "JAL offset must be a multiple of 2 in [-1048576, 1048574]");
  case Match_InvalidTiedOperand:
    return Error(IDLoc, "invalid tied operand");
  default:
    return Error(IDLoc, "invalid instruction");
  }
}

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeAbyssAsmParser() {
  RegisterMCAsmParser<AbyssAsmParser> X(getTheAbyssTarget());
}
