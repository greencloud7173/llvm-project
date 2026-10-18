//===-- AbyssInstrFormats.td - Abyss instruction formats --*- tablegen -*-===//
//
// Bit layouts follow the 32-bit Abyss ISA (see docs/isa.md).
//
//===----------------------------------------------------------------------===//

class AbyssInstr<dag outs, dag ins, string asmstr, list<dag> pattern = []>
    : Instruction {
  field bits<32> Inst;
  let Size = 4;
  let Namespace = "Abyss";
  let DecoderNamespace = "Abyss";

  dag OutOperandList = outs;
  dag InOperandList = ins;
  let AsmString = asmstr;
  let Pattern = pattern;
}

// R-type: funct7 | rs2 | rs1 | funct3 | rd | opcode
class RType<bits<7> opcode, bits<3> funct3, bits<7> funct7,
            dag outs, dag ins, string asmstr>
    : AbyssInstr<outs, ins, asmstr> {
  bits<5> rd;
  bits<5> rs1;
  bits<5> rs2;

  let Inst{6-0}   = opcode;
  let Inst{11-7}  = rd;
  let Inst{14-12} = funct3;
  let Inst{19-15} = rs1;
  let Inst{24-20} = rs2;
  let Inst{31-25} = funct7;
}

// I-type: imm[11:0] | rs1 | funct3 | rd | opcode
class IType<bits<7> opcode, bits<3> funct3,
            dag outs, dag ins, string asmstr>
    : AbyssInstr<outs, ins, asmstr> {
  bits<5>  rd;
  bits<5>  rs1;
  bits<12> imm;

  let Inst{6-0}   = opcode;
  let Inst{11-7}  = rd;
  let Inst{14-12} = funct3;
  let Inst{19-15} = rs1;
  let Inst{31-20} = imm;
}

// I-type shift: imm[11:5] is a fixed funct7-like field, imm[4:0] is shamt.
class ShiftIType<bits<7> opcode, bits<3> funct3, bits<7> imm11_5,
                 dag outs, dag ins, string asmstr>
    : AbyssInstr<outs, ins, asmstr> {
  bits<5> rd;
  bits<5> rs1;
  bits<5> shamt;

  let Inst{6-0}   = opcode;
  let Inst{11-7}  = rd;
  let Inst{14-12} = funct3;
  let Inst{19-15} = rs1;
  let Inst{24-20} = shamt;
  let Inst{31-25} = imm11_5;
}

// S-type: imm[11:5] | rs2 | rs1 | funct3 | imm[4:0] | opcode
class SType<bits<7> opcode, bits<3> funct3,
            dag outs, dag ins, string asmstr>
    : AbyssInstr<outs, ins, asmstr> {
  bits<5>  rs1;
  bits<5>  rs2;
  bits<12> imm;

  let Inst{6-0}   = opcode;
  let Inst{11-7}  = imm{4-0};
  let Inst{14-12} = funct3;
  let Inst{19-15} = rs1;
  let Inst{24-20} = rs2;
  let Inst{31-25} = imm{11-5};
}

// B-type: imm[12] | imm[10:5] | rs2 | rs1 | funct3 | imm[4:1] | imm[11] | opcode
class BType<bits<7> opcode, bits<3> funct3,
            dag outs, dag ins, string asmstr>
    : AbyssInstr<outs, ins, asmstr> {
  bits<5>  rs1;
  bits<5>  rs2;
  bits<13> imm;

  let Inst{6-0}   = opcode;
  let Inst{7}     = imm{11};
  let Inst{11-8}  = imm{4-1};
  let Inst{14-12} = funct3;
  let Inst{19-15} = rs1;
  let Inst{24-20} = rs2;
  let Inst{30-25} = imm{10-5};
  let Inst{31}    = imm{12};
}

// U-type: imm[31:12] | rd | opcode
class UType<bits<7> opcode,
            dag outs, dag ins, string asmstr>
    : AbyssInstr<outs, ins, asmstr> {
  bits<5>  rd;
  bits<20> imm20;

  let Inst{6-0}   = opcode;
  let Inst{11-7}  = rd;
  let Inst{31-12} = imm20;
}

// J-type: imm[20] | imm[10:1] | imm[11] | imm[19:12] | rd | opcode
class JType<bits<7> opcode,
            dag outs, dag ins, string asmstr>
    : AbyssInstr<outs, ins, asmstr> {
  bits<5>  rd;
  bits<21> imm;

  let Inst{6-0}   = opcode;
  let Inst{11-7}  = rd;
  let Inst{19-12} = imm{19-12};
  let Inst{20}    = imm{11};
  let Inst{30-21} = imm{10-1};
  let Inst{31}    = imm{20};
}

// RET: custom-2 opcode 1011011, funct3 000, every other field zero.
class RetType : AbyssInstr<(outs), (ins), "ret"> {
  let Inst{6-0}   = 0b1011011;
  let Inst{14-12} = 0b000;
}

