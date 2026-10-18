include "llvm/Target/Target.td"
include "AbyssRegisterInfo.td"
include "AbyssInstrFormats.td"
include "AbyssInstrInfo.td"

def AbyssInstrInfo : InstrInfo;

defm : RemapAllTargetPseudoPointerOperands<XRegs>;

class Proc<string Name, list<SubtargetFeature> Features>
    : Processor<Name, NoItineraries, Features>;

def : Proc<"generic", []>;

def AbyssInstPrinter : AsmWriter {
  string AsmWriterClassName = "InstPrinter";
  bit isMCAsmWriter = 1;
}

def AbyssAsmParser : AsmParser {
  bit HasMnemonicFirst = 1;
}

def AbyssAsmParserVariant : AsmParserVariant {
  int Variant = 0;
  string Name = "ABYSS";
}

// Keep the target record name in sync with the "Abyss" namespace so that
// generated symbols are AbyssInstPrinter, InitAbyssMCInstrInfo, etc.
def Abyss : Target {
  let InstructionSet = AbyssInstrInfo;
  let AssemblyWriters = [AbyssInstPrinter];
  let AssemblyParsers = [AbyssAsmParser];
  let AssemblyParserVariants = [AbyssAsmParserVariant];
}
