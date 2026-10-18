//===-- AbyssInstPrinter.cpp - Abyss MCInst to assembly -------------------===//

#include "AbyssInstPrinter.h"
#include "AbyssMCTargetDesc.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

#define PRINT_ALIAS_INSTR
#include "AbyssGenAsmWriter.inc"

void AbyssInstPrinter::printInst(const MCInst *MI, uint64_t Address,
                                 StringRef Annot, const MCSubtargetInfo &STI,
                                 raw_ostream &O) {
  if (printAliasInstr(MI, Address, O)) {
    printAnnotation(O, Annot);
    return;
  }

  printInstruction(MI, Address, O);
  printAnnotation(O, Annot);
}

void AbyssInstPrinter::printOperand(const MCInst *MI, unsigned OpNo,
                                    raw_ostream &O) {
  const MCOperand &Op = MI->getOperand(OpNo);
  if (Op.isReg()) {
    MCRegister Reg = Op.getReg();
    // Print the canonical SIMD special-register spelling.
    if (Reg == Abyss::X29)
      O << "%blockIdx";
    else if (Reg == Abyss::X30)
      O << "%blockDim";
    else if (Reg == Abyss::X31)
      O << "%threadIdx";
    else
      O << getRegisterName(Reg);
  } else if (Op.isImm()) {
    O << Op.getImm();
  } else if (Op.isExpr()) {
    MAI.printExpr(O, *Op.getExpr());
  } else {
    llvm_unreachable("unexpected operand kind");
  }
}
