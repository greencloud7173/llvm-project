//===-- AbyssInstrInfo.td - Abyss instruction definitions --*- tablegen -*-===//

// Immediate operands.  The generated matcher calls AbyssOperand::isImm12(),
// isImm13(), isImm20(), isImm21(); implement those in AbyssAsmParser.cpp.
class AbyssImmAsmOperand<string name> : AsmOperandClass {
  let Name = name;
  let RenderMethod = "addImmOperands";
  let DiagnosticType = !strconcat("Invalid", name);
}

def Imm5AsmOperand : AbyssImmAsmOperand<"Imm5">;
def Imm12AsmOperand : AbyssImmAsmOperand<"Imm12">;
def Imm13AsmOperand : AbyssImmAsmOperand<"Imm13">;
def Imm20AsmOperand : AbyssImmAsmOperand<"Imm20">;
def Imm21AsmOperand : AbyssImmAsmOperand<"Imm21">;

def Imm5 : Operand<i32> {
  let ParserMatchClass = Imm5AsmOperand;
}
def Imm12 : Operand<i32> {
  let ParserMatchClass = Imm12AsmOperand;
}
def Imm13 : Operand<i32> {
  let ParserMatchClass = Imm13AsmOperand;
}
def Imm20 : Operand<i32> {
  let ParserMatchClass = Imm20AsmOperand;
}
def Imm21 : Operand<i32> {
  let ParserMatchClass = Imm21AsmOperand;
}

//===----------------------------------------------------------------------===//
// R-type: ADD SUB MUL DIV SLL SRL SRA
//===----------------------------------------------------------------------===//
def ADD : RType<0b0110011, 0b000, 0b0000000,
                (outs XRegs:$rd), (ins XRegs:$rs1, XRegs:$rs2),
                "add $rd, $rs1, $rs2">;
def SUB : RType<0b0110011, 0b000, 0b0100000,
                (outs XRegs:$rd), (ins XRegs:$rs1, XRegs:$rs2),
                "sub $rd, $rs1, $rs2">;
def MUL : RType<0b0110011, 0b000, 0b0000001,
                (outs XRegs:$rd), (ins XRegs:$rs1, XRegs:$rs2),
                "mul $rd, $rs1, $rs2">;
def DIV : RType<0b0110011, 0b100, 0b0000001,
                (outs XRegs:$rd), (ins XRegs:$rs1, XRegs:$rs2),
                "div $rd, $rs1, $rs2">;
def SLL : RType<0b0110011, 0b001, 0b0000000,
                (outs XRegs:$rd), (ins XRegs:$rs1, XRegs:$rs2),
                "sll $rd, $rs1, $rs2">;
def SRL : RType<0b0110011, 0b101, 0b0000000,
                (outs XRegs:$rd), (ins XRegs:$rs1, XRegs:$rs2),
                "srl $rd, $rs1, $rs2">;
def SRA : RType<0b0110011, 0b101, 0b0100000,
                (outs XRegs:$rd), (ins XRegs:$rs1, XRegs:$rs2),
                "sra $rd, $rs1, $rs2">;

//===----------------------------------------------------------------------===//
// I-type: ADDI LDR LB LH LBU LHU SLLI SRLI SRAI; S-type: STR SB SH
//===----------------------------------------------------------------------===//
def ADDI : IType<0b0010011, 0b000,
                 (outs XRegs:$rd), (ins XRegs:$rs1, Imm12:$imm),
                 "addi $rd, $rs1, #$imm">;
def SLLI : ShiftIType<0b0010011, 0b001, 0b0000000,
                      (outs XRegs:$rd), (ins XRegs:$rs1, Imm5:$shamt),
                      "slli $rd, $rs1, $shamt">;
def SRLI : ShiftIType<0b0010011, 0b101, 0b0000000,
                      (outs XRegs:$rd), (ins XRegs:$rs1, Imm5:$shamt),
                      "srli $rd, $rs1, $shamt">;
def SRAI : ShiftIType<0b0010011, 0b101, 0b0100000,
                      (outs XRegs:$rd), (ins XRegs:$rs1, Imm5:$shamt),
                      "srai $rd, $rs1, $shamt">;
def LDR  : IType<0b0000011, 0b010,
                 (outs XRegs:$rd), (ins XRegs:$rs1, Imm12:$imm),
                 "ldr $rd, ${imm}(${rs1})">;
def LB   : IType<0b0000011, 0b000,
                 (outs XRegs:$rd), (ins XRegs:$rs1, Imm12:$imm),
                 "lb $rd, ${imm}(${rs1})">;
def LH   : IType<0b0000011, 0b001,
                 (outs XRegs:$rd), (ins XRegs:$rs1, Imm12:$imm),
                 "lh $rd, ${imm}(${rs1})">;
def LBU  : IType<0b0000011, 0b100,
                 (outs XRegs:$rd), (ins XRegs:$rs1, Imm12:$imm),
                 "lbu $rd, ${imm}(${rs1})">;
def LHU  : IType<0b0000011, 0b101,
                 (outs XRegs:$rd), (ins XRegs:$rs1, Imm12:$imm),
                 "lhu $rd, ${imm}(${rs1})">;
def STR  : SType<0b0100011, 0b010,
                 (outs), (ins XRegs:$rs1, XRegs:$rs2, Imm12:$imm),
                 "str $rs2, ${imm}(${rs1})">;
def SB   : SType<0b0100011, 0b000,
                 (outs), (ins XRegs:$rs1, XRegs:$rs2, Imm12:$imm),
                 "sb $rs2, ${imm}(${rs1})">;
def SH   : SType<0b0100011, 0b001,
                 (outs), (ins XRegs:$rs1, XRegs:$rs2, Imm12:$imm),
                 "sh $rs2, ${imm}(${rs1})">;

//===----------------------------------------------------------------------===//
// B-type: six conditional branches
//===----------------------------------------------------------------------===//
def BEQ  : BType<0b1100011, 0b000,
                 (outs), (ins XRegs:$rs1, XRegs:$rs2, Imm13:$imm),
                 "beq $rs1, $rs2, $imm">;
def BNE  : BType<0b1100011, 0b001,
                 (outs), (ins XRegs:$rs1, XRegs:$rs2, Imm13:$imm),
                 "bne $rs1, $rs2, $imm">;
def BLT  : BType<0b1100011, 0b100,
                 (outs), (ins XRegs:$rs1, XRegs:$rs2, Imm13:$imm),
                 "blt $rs1, $rs2, $imm">;
def BGE  : BType<0b1100011, 0b101,
                 (outs), (ins XRegs:$rs1, XRegs:$rs2, Imm13:$imm),
                 "bge $rs1, $rs2, $imm">;
def BLTU : BType<0b1100011, 0b110,
                 (outs), (ins XRegs:$rs1, XRegs:$rs2, Imm13:$imm),
                 "bltu $rs1, $rs2, $imm">;
def BGEU : BType<0b1100011, 0b111,
                 (outs), (ins XRegs:$rs1, XRegs:$rs2, Imm13:$imm),
                 "bgeu $rs1, $rs2, $imm">;

//===----------------------------------------------------------------------===//
// U-type and J-type
//===----------------------------------------------------------------------===//
def LUI   : UType<0b0110111,
                  (outs XRegs:$rd), (ins Imm20:$imm20),
                  "lui $rd, $imm20">;
def AUIPC : UType<0b0010111,
                  (outs XRegs:$rd), (ins Imm20:$imm20),
                  "auipc $rd, $imm20">;
def JAL   : JType<0b1101111,
                  (outs XRegs:$rd), (ins Imm21:$imm),
                  "jal $rd, $imm">;
def JALR  : IType<0b1100111, 0b000,
                  (outs XRegs:$rd), (ins XRegs:$rs1, Imm12:$imm),
                  "jalr $rd, ${imm}(${rs1})">;

//===----------------------------------------------------------------------===//
// GPU control instruction
//===----------------------------------------------------------------------===//
def RET : RetType;

//===----------------------------------------------------------------------===//
// Pseudo-instructions (assembler aliases, no separate encoding)
//===----------------------------------------------------------------------===//
def : InstAlias<"nop", (ADDI X0, X0, 0), 3>;
def : InstAlias<"li $rd, #$imm", (ADDI XRegs:$rd, X0, Imm12:$imm), 2>;
def : InstAlias<"j $imm", (JAL X0, Imm21:$imm)>;