#include <iostream>
#include <iomanip>
#include <algorithm>
#include "recompiler.h"
// Helper function to map Capstone's register enum to the correct 0-31 GPR index.
// This function should be placed in main.cpp, typically above the main() function.
int get_gpr_index(mips_reg capstone_reg) {
    switch (capstone_reg) {
        // General Purpose Registers (GPRs)
        case MIPS_REG_ZERO: return 0; // $zero
        case MIPS_REG_AT:   return 1; // $at
        case MIPS_REG_V0:   return 2; // $v0
        case MIPS_REG_V1:   return 3; // $v1
        case MIPS_REG_A0:   return 4; // $a0
        case MIPS_REG_A1:   return 5; // $a1
        case MIPS_REG_A2:   return 6; // $a2
        case MIPS_REG_A3:   return 7; // $a3
        case MIPS_REG_T0:   return 8; // $t0
        case MIPS_REG_T1:   return 9; // $t1
        case MIPS_REG_T2:   return 10; // $t2
        case MIPS_REG_T3:   return 11; // $t3
        case MIPS_REG_T4:   return 12; // $t4
        case MIPS_REG_T5:   return 13; // $t5
        case MIPS_REG_T6:   return 14; // $t6
        case MIPS_REG_T7:   return 15; // $t7
        case MIPS_REG_S0:   return 16; // $s0
        case MIPS_REG_S1:   return 17; // $s1
        case MIPS_REG_S2:   return 18; // $s2
        case MIPS_REG_S3:   return 19; // $s3
        case MIPS_REG_S4:   return 20; // $s4
        case MIPS_REG_S5:   return 21; // $s5
        case MIPS_REG_S6:   return 22; // $s6
        case MIPS_REG_S7:   return 23; // $s7
        case MIPS_REG_T8:   return 24; // $t8
        case MIPS_REG_T9:   return 25; // $t9
        case MIPS_REG_K0:   return 26; // $k0
        case MIPS_REG_K1:   return 27; // $k1
        case MIPS_REG_GP:   return 28; // $gp
        case MIPS_REG_SP:   return 29; // $sp
        case MIPS_REG_S8:   return 30; // $fp (or $s8)
        case MIPS_REG_RA:   return 31; // $ra

        // If you encounter other types of registers (e.g., floating point, COP0),
        // you'll need to add cases for them and handle them appropriately.
        // For now, return -1 for unhandled or invalid registers to indicate an error.
        default:
            std::cerr << "ERROR: Unhandled Capstone register enum: " << capstone_reg << std::endl;
            return -1; // Or throw an exception
    }
}

// This function is now the heart of the recompiler.
// This is the single source of truth for translating any MIPS instruction.
void translate_instruction_block(std::ofstream& out_file, cs_insn* insn) {
    cs_mips& mips_details = insn->detail->mips;

    std::cerr << "DEBUG [" << insn->mnemonic << "]: " << mips_details.op_count << " operands." << std::endl;

    switch (insn->id) {
        // --- JUMP & BRANCH INSTRUCTIONS (Special Handling) ---
        case MIPS_INS_JR: {
            std::cerr << "  Operand 0 [target]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            const auto& target_reg_capstone = mips_details.operands[0].reg;
            int target_reg_index = get_gpr_index(target_reg_capstone);

            out_file << "    host_dispatch_jump(context.cpuRegs.GPR.r[" << target_reg_index << "].UD[0]);" << std::endl;
            out_file << "    return;" << std::endl;
            break;
        }
        case MIPS_INS_BEQ: {
            std::cerr << "  Operand 0 [rs]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [rt]: type=" << mips_details.operands[1].type << ", reg=" << mips_details.operands[1].reg << std::endl;
            std::cerr << "  Operand 2 [offset]: type=" << mips_details.operands[2].type << ", imm=" << mips_details.operands[2].imm << std::endl;
            const auto& rs_capstone = mips_details.operands[0].reg;
            const auto& rt_capstone = mips_details.operands[1].reg;
            const auto& off = mips_details.operands[2].imm;
            int rs_index = get_gpr_index(rs_capstone);
            int rt_index = get_gpr_index(rt_capstone);

            u32 target = calculate_target(*insn);

            out_file << "    if (context.cpuRegs.GPR.r[" << rs_index << "].UD[0] == context.cpuRegs.GPR.r[" << rt_index << "].UD[0]) {" << std::endl;
            out_file << "        func_" << std::hex << target << "();" << std::endl;
            out_file << "        return;" << std::endl;
            out_file << "    }" << std::endl;
            break;
        }
        case MIPS_INS_BNE: {
            std::cerr << "  Operand 0 [rs]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [rt]: type=" << mips_details.operands[1].type << ", reg=" << mips_details.operands[1].reg << std::endl;
            std::cerr << "  Operand 2 [offset]: type=" << mips_details.operands[2].type << ", imm=" << mips_details.operands[2].imm << std::endl;
            const auto& rs_capstone = mips_details.operands[0].reg;
            const auto& rt_capstone = mips_details.operands[1].reg;
            const auto& off = mips_details.operands[2].imm;
            int rs_index = get_gpr_index(rs_capstone);
            int rt_index = get_gpr_index(rt_capstone);

            u32 target = calculate_target(*insn);

            out_file << "    if (context.cpuRegs.GPR.r[" << rs_index << "].UD[0] != context.cpuRegs.GPR.r[" << rt_index << "].UD[0]) {" << std::endl;
            out_file << "        func_" << std::hex << target << "();" << std::endl;
            out_file << "        return;" << std::endl;
            out_file << "    }" << std::endl;
            break;
        }

        // --- STANDARD INSTRUCTIONS (Default Handling) ---
        case MIPS_INS_ADDIU: {
            std::cerr << "  Operand 0 [dest]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [source]: type=" << mips_details.operands[1].type << ", reg=" << mips_details.operands[1].reg << std::endl;
            std::cerr << "  Operand 2 [imm]: type=" << mips_details.operands[2].type << ", imm=" << mips_details.operands[2].imm << std::endl;
            const auto& dest_capstone = mips_details.operands[0].reg;
            const auto& source_capstone = mips_details.operands[1].reg;
            const auto& imm = mips_details.operands[2].imm;
            int dest_index = get_gpr_index(dest_capstone);
            int source_index = get_gpr_index(source_capstone);

            out_file << "context.cpuRegs.GPR.r[" << dest_index << "].SD[0] = (s64)(s32)(context.cpuRegs.GPR.r[" << source_index << "].SD[0] + (s16)" << imm << ");" << std::endl;
            break;
        }
        case MIPS_INS_LW: {
            std::cerr << "  Operand 0 [dest]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [mem]: type=" << mips_details.operands[1].type << ", base=" << mips_details.operands[1].mem.base << ", disp=" << mips_details.operands[1].mem.disp << std::endl;
            const auto& dest_capstone = mips_details.operands[0].reg;
            const auto& base_capstone = mips_details.operands[1].mem.base;
            const auto& offset = mips_details.operands[1].mem.disp;
            int dest_index = get_gpr_index(dest_capstone);
            int base_index = get_gpr_index(base_capstone);

            out_file << "{"
            out_file << "    u32 address = context.cpuRegs.GPR.r[" << base_index << "].UD[0] + " << offset << ";" << std::endl;
            out_file << "    context.cpuRegs.GPR.r[" << dest_index << "].SD[0] = (s64)(s32)ReadMemory32(address);" << std::endl;
            out_file << "}" << std::endl;
            break;
        }
        case MIPS_INS_SW: {
            std::cerr << "  Operand 0 [source]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [mem]: type=" << mips_details.operands[1].type << ", base=" << mips_details.operands[1].mem.base << ", disp=" << mips_details.operands[1].mem.disp << std::endl;
            const auto& source_capstone = mips_details.operands[0].reg;
            const auto& base_capstone = mips_details.operands[1].mem.base;
            const auto& offset = mips_details.operands[1].mem.disp;
            int source_index = get_gpr_index(source_capstone);
            int base_index = get_gpr_index(base_capstone);

            out_file << "{"
            out_file << "    u32 address = context.cpuRegs.GPR.r[" << base_index << "].UD[0] + " << offset << ";" << std::endl;
            out_file << "    WriteMemory32(address, (u32)context.cpuRegs.GPR.r[" << source_index << "].UD[0]);" << std::endl;
            out_file << "}" << std::endl;
            break;
        }
        case MIPS_INS_OR: {
            std::cerr << "  Operand 0 [dest]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [r1]: type=" << mips_details.operands[1].type << ", reg=" << mips_details.operands[1].reg << std::endl;
            std::cerr << "  Operand 2 [r2]: type=" << mips_details.operands[2].type << ", reg=" << mips_details.operands[2].reg << std::endl;
            const auto& dest_capstone = mips_details.operands[0].reg;
            const auto& r1_capstone = mips_details.operands[1].reg;
            const auto& r2_capstone = mips_details.operands[2].reg;
            int dest_index = get_gpr_index(dest_capstone);
            int r1_index = get_gpr_index(r1_capstone);
            int r2_index = get_gpr_index(r2_capstone);

            out_file << "context.cpuRegs.GPR.r[" << dest_index << "].UD[0] = context.cpuRegs.GPR.r[" << r1_index << "].UD[0] | context.cpuRegs.GPR.r[" << r2_index << "].UD[0];" << std::endl;
            break;
        }
        case MIPS_INS_LUI: {
            std::cerr << "  Operand 0 [rt]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [imm]: type=" << mips_details.operands[1].type << ", imm=" << mips_details.operands[1].imm << std::endl;
            const auto& rt_capstone = mips_details.operands[0].reg;
            const auto& imm = mips_details.operands[1].imm;
            int rt_index = get_gpr_index(rt_capstone);

            out_file << "context.cpuRegs.GPR.r[" << rt_index << "].SD[0] = (s64)(s32)(" << imm << " << 16);" << std::endl;
            break;
        }
        case MIPS_INS_SLL: {
            std::cerr << "  Operand 0 [rd]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [rt]: type=" << mips_details.operands[1].type << ", reg=" << mips_details.operands[1].reg << std::endl;
            std::cerr << "  Operand 2 [sa]: type=" << mips_details.operands[2].type << ", imm=" << mips_details.operands[2].imm << std::endl;
            const auto& rd_capstone = mips_details.operands[0].reg;
            const auto& rt_capstone = mips_details.operands[1].reg;
            const auto& sa = mips_details.operands[2].imm;
            int rd_index = get_gpr_index(rd_capstone);
            int rt_index = get_gpr_index(rt_capstone);

            out_file << "context.cpuRegs.GPR.r[" << rd_index << "].SD[0] = (s64)(s32)((u32)context.cpuRegs.GPR.r[" << rt_index << "].UD[0] << " << " << sa << ");" << std::endl;
            break;
        }
        case MIPS_INS_NOP: {
            out_file << "// NOP" << std::endl;
            break;
        }
        case MIPS_INS_ORI: {
            std::cerr << "  Operand 0 [dest]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [source]: type=" << mips_details.operands[1].type << ", reg=" << mips_details.operands[1].reg << std::endl;
            std::cerr << "  Operand 2 [imm]: type=" << mips_details.operands[2].type << ", imm=" << mips_details.operands[2].imm << std::endl;
            const auto& dest_reg = mips_details.operands[0].reg;
            const auto& source_reg = mips_details.operands[1].reg;
            const auto& imm = mips_details.operands[2].imm;

            int dest_index = get_gpr_index(dest_reg);
            int source_index = get_gpr_index(source_reg);

            out_file << "context.cpuRegs.GPR.r[" << dest_index << "].UD[0] = context.cpuRegs.GPR.r["<< source_index << "].UD[0] | (u32)("<< imm << ");"<< std::endl;
            break;
        }
        case MIPS_INS_ADDU: {
            std::cerr << "  Operand 0 [dest]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [reg1]: type=" << mips_details.operands[1].type << ", reg=" << mips_details.operands[1].reg << std::endl;
            std::cerr << "  Operand 2 [reg2]: type=" << mips_details.operands[2].type << ", reg=" << mips_details.operands[2].reg << std::endl;
            const auto& dest_reg = mips_details.operands[0].reg;
            const auto& reg1 = mips_details.operands[1].reg;
            const auto& reg2 = mips_details.operands[2].reg;

            int dest_index = get_gpr_index(dest_reg);
            int reg1_index = get_gpr_index(reg1);
            int reg2_index = get_gpr_index(reg2);

            out_file << "context.cpuRegs.GPR.r[" << dest_index << "].UD[0] = (u64)(u32)(context.cpuRegs.GPR.r[" << reg1_index << "].UD[0] + context.cpuRegs.GPR.r["<< reg2_index << "].UD[0]);"<< std::endl;
            break;

        }
        case MIPS_INS_SUBU: {
            std::cerr << "  Operand 0 [dest]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [reg1]: type=" << mips_details.operands[1].type << ", reg=" << mips_details.operands[1].reg << std::endl;
            std::cerr << "  Operand 2 [reg2]: type=" << mips_details.operands[2].type << ", reg=" << mips_details.operands[2].reg << std::endl;
            const auto& dest_reg = mips_details.operands[0].reg;
            const auto& reg1 = mips_details.operands[1].reg;
            const auto& reg2 = mips_details.operands[2].reg;

            int dest_index = get_gpr_index(dest_reg);
            int reg1_index = get_gpr_index(reg1);
            int reg2_index = get_gpr_index(reg2);

            out_file << "context.cpuRegs.GPR.r[" << dest_index << "].UD[0] = (u64)(u32)(context.cpuRegs.GPR.r[" << reg1_index << "].UD[0] - context.cpuRegs.GPR.r["<< reg2_index << "].UD[0]);"<< std::endl;
            break;

        }
        case MIPS_INS_SLT: {
            std::cerr << "  Operand 0 [dest]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [reg1]: type=" << mips_details.operands[1].type << ", reg=" << mips_details.operands[1].reg << std::endl;
            std::cerr << "  Operand 2 [reg2]: type=" << mips_details.operands[2].type << ", reg=" << mips_details.operands[2].reg << std::endl;
            const auto& dest_reg = mips_details.operands[0].reg;
            const auto& reg1 = mips_details.operands[1].reg;
            const auto& reg2 = mips_details.operands[2].reg;

            int dest_index = get_gpr_index(dest_reg);
            int reg1_index = get_gpr_index(reg1);
            int reg2_index = get_gpr_index(reg2);

            out_file << "context.cpuRegs.GPR.r[" << dest_index << "].SD[0] = (s64)((s32)context.cpuRegs.GPR.r[" << reg1_index << "].SD[0] < (s32)context.cpuRegs.GPR.r["<< reg2_index << "].SD[0] ? 1 : 0);"<< std::endl;
            break;
        }
        case MIPS_INS_SLTI: {
            std::cerr << "  Operand 0 [dest]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [source]: type=" << mips_details.operands[1].type << ", reg=" << mips_details.operands[1].reg << std::endl;
            std::cerr << "  Operand 2 [imm]: type=" << mips_details.operands[2].type << ", imm=" << mips_details.operands[2].imm << std::endl;
            const auto& dest_reg = mips_details.operands[0].reg;
            const auto& source_reg = mips_details.operands[1].reg;
            const auto& imm = mips_details.operands[2].imm;

            int dest_index = get_gpr_index(dest_reg);
            int source_index = get_gpr_index(source_reg);

            out_file << "context.cpuRegs.GPR.r[" << dest_index << "].SD[0] = (s64)((s32)context.cpuRegs.GPR.r[" << source_index << "].SD[0] < (s32)" << imm << " ? 1 : 0);"<< std::endl;
            break;
        }
        case MIPS_INS_MULT : {
            std::cerr << "  Operand 0 [dest]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [source]: type=" << mips_details.operands[1].type << ", reg=" << mips_details.operands[1].reg << std::endl;
            const auto& dest_reg = mips_details.operands[0].reg;
            const auto& source_reg = mips_details.operands[1].reg;

            int dest_index = get_gpr_index(dest_reg);
            int source_index = get_gpr_index(source_reg);

            /*
            {
                s32 op1 = context.cpuRegs.GPR.r[dest_index].SD[0];
                s32 op2 = context.cpuRegs.GPR.r[source_index].SD[0];
                s64 product = op1 * op2;
                context.cpuRegs.GPR.r[dest_index].LO.SD[0] = (s64)(s32)product;
                context.cpuRegs.GPR.r[dest_index].HI.SD[0] = (s64)(s32)(product >> 32);
            
            }
            */
           out_file << "{"
           out_file << "   s32 op1 = context.cpuRegs.GPR.r[ " << dest_index << "].SD[0];" << std::endl;
           out_file << "   s32 op2 = context.cpuRegs.GPR.r[ " << source_index <<" ].SD[0];" << std::endl;
           out_file << "   s64 product = op1 * op2;" << std::endl;
           out_file << "   context.cpuRegs.LO.SD[0] = (s64)(s32)product;" << std::endl;
           out_file << "   context.cpuRegs.HI.SD[0] = (s64)(s32)(product >> 32);" << std::endl;
           out_file << "}" << std::endl;
           break;
        }
        case MIPS_INS_DIV : {
            std::cerr << "  Operand 0 [dest]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [source]: type=" << mips_details.operands[1].type << ", reg=" << mips_details.operands[1].reg << std::endl;
            const auto& dest_reg = mips_details.operands[0].reg;
            const auto& source_reg = mips_details.operands[1].reg;

            int dest_index = get_gpr_index(dest_reg);
            int source_index = get_gpr_index(source_reg);

            /*
            {
                s32 num = context.cpuRegs.GPR.r[dest_index].SD[0];
                s32 den = context.cpuRegs.GPR.r[source_index].SD[0];

                if (den != 0){
                    s32 HI_ans = num % den;
                    s32 LO_ans = num / den;
                    context.cpuRegs.LO.SD[0] = (s64)(s32)LO_ans;
                    context.cpuRegs.HI.SD[0] = (s64)(s32)(HI_ans);
                }
            
            }
            */
           out_file << "{"
           out_file << "   s32 num = (s32)context.cpuRegs.GPR.r[ " << dest_index << "].SD[0];" << std::endl;
           out_file << "   s32 den = (s32)context.cpuRegs.GPR.r[ " << source_index <<" ].SD[0];" << std::endl;
           out_file << "   if (den != 0){ " << std::endl;
           out_file << "       s32 HI_ans = num " << "%" << " den;"  <<std::endl;
           out_file << "       s32 LO_ans = num / den;" << std::endl;
           out_file << "       context.cpuRegs.LO.SD[0] = (s64)(s32)LO_ans;" << std::endl;
           out_file << "       context.cpuRegs.HI.SD[0] = (s64)(s32)(HI_ans);" << std::endl;
           out_file << "   }" << std::endl;
           out_file << "}" << std::endl;
           break;
        }
        case MIPS_INS_XOR : {
            std::cerr << "  Operand 0 [rd]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [rs]: type=" << mips_details.operands[1].type << ", reg=" << mips_details.operands[1].reg << std::endl;
            std::cerr << "  Operand 2 [rt]: type=" << mips_details.operands[2].type << ", reg=" << mips_details.operands[2].reg << std::endl;
            const auto& rd_reg = mips_details.operands[0].reg;
            const auto& rs_reg = mips_details.operands[1].reg;
            const auto& rt_reg = mips_details.operands[2].reg;

            int rd_index = get_gpr_index(rd_reg);
            int rs_index = get_gpr_index(rs_reg);
            int rt_index = get_gpr_index(rt_reg);

            /*
            {
                u64 rs = context.cpuRegs.GPR.r[rs_index].UD[0];
                u64 rt = context.cpuRegs.GPR.r[rt_index].UD[0];
                context.cpuRegs.GPT.r[rd_index].UD[0] = rs ^ rt;
            
            }
            */
           out_file << "{"
           out_file << "   u64 rs_val = context.cpuRegs.GPR.r["<< rs_index <<"].UD[0];" << std::endl;
           out_file << "   u64 rt_val = context.cpuRegs.GPR.r["<< rt_index <<"].UD[0];" << std::endl;
           out_file << "   context.cpuRegs.GPR.r[" << rd_index << "].UD[0] = rs_val ^ rt_val;" << std::endl;
           out_file << "}" << std::endl;
           break;
        }
        case MIPS_INS_NOR : {
            std::cerr << "  Operand 0 [rd]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [rs]: type=" << mips_details.operands[1].type << ", reg=" << mips_details.operands[1].reg << std::endl;
            std::cerr << "  Operand 2 [rt]: type=" << mips_details.operands[2].type << ", reg=" << mips_details.operands[2].reg << std::endl;
            const auto& rd_reg = mips_details.operands[0].reg;
            const auto& rs_reg = mips_details.operands[1].reg;
            const auto& rt_reg = mips_details.operands[2].reg;

            int rd_index = get_gpr_index(rd_reg);
            int rs_index = get_gpr_index(rs_reg);
            int rt_index = get_gpr_index(rt_reg);

            /*
            {
                u64 rs = context.cpuRegs.GPR.r[rs_index].UD[0];
                u64 rt = context.cpuRegs.GPR.r[rt_index].UD[0];
                context.cpuRegs.GPT.r[rd_index].UD[0] = ~(rs | rt);
            
            }
            */
           out_file << "{"
           out_file << "   u64 rs_val = context.cpuRegs.GPR.r["<< rs_index <<"].UD[0];" << std::endl;
           out_file << "   u64 rt_val = context.cpuRegs.GPR.r["<< rt_index <<"].UD[0];" << std::endl;
           out_file << "   context.cpuRegs.GPR.r[" << rd_index << "].UD[0] = ~(rs_val | rt_val);" << std::endl;
           out_file << "}" << std::endl;
           break;
        }
        case MIPS_INS_SRL : {
            std::cerr << "  Operand 0 [rd]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [rt]: type=" << mips_details.operands[1].type << ", reg=" << mips_details.operands[1].reg << std::endl;
            std::cerr << "  Operand 2 [imm]: type=" << mips_details.operands[2].type << ", imm=" << mips_details.operands[2].imm << std::endl;
            const auto& rd_reg = mips_details.operands[0].reg;
            const auto& rt_reg = mips_details.operands[1].reg;
            const auto& imm = mips_details.operands[2].imm;

            int rd_index = get_gpr_index(rd_reg);
            int rt_index = get_gpr_index(rt_reg);

            /*

                u64 rt_val = context.cpuRegs.GPR.r[rt_index].UD[0];
                context.cpuRegs.GPT.r[rd_index].UD[0] = u64(rt_val >> imm);
            
            */
           out_file << "{"
           out_file << "   u64 rt_val = context.cpuRegs.GPR.r[" << rt_index << "].UD[0];" << std::endl;
           out_file << "   context.cpuRegs.GPR.r[" << rd_index << "].UD[0] = (u64)((u32)rt_val >>" << imm << ");" << std::endl;
           out_file << "}" << std::endl;
           break;
        }
        case MIPS_INS_SRA : {
            std::cerr << "  Operand 0 [rd]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [rt]: type=" << mips_details.operands[1].type << ", reg=" << mips_details.operands[1].reg << std::endl;
            std::cerr << "  Operand 2 [imm]: type=" << mips_details.operands[2].type << ", imm=" << mips_details.operands[2].imm << std::endl;
            const auto& rd_reg = mips_details.operands[0].reg;
            const auto& rt_reg = mips_details.operands[1].reg;
            const auto& imm = mips_details.operands[2].imm;

            int rd_index = get_gpr_index(rd_reg);
            int rt_index = get_gpr_index(rt_reg);

            /*

                u64 rt_val = context.cpuRegs.GPR.r[rt_index].UD[0];
                context.cpuRegs.GPT.r[rd_index].SD[0] = (s64)(s32)((u32)(rt_val) >> imm);
            
            */
           out_file << "{"
           out_file << "   s64 rt_val = context.cpuRegs.GPR.r["<< rt_index <<"].UD[0];" << std::endl;
           out_file << "   context.cpuRegs.GPR.r[" << rd_index << "].SD[0] = (s64)((s32)(rt_val) >>" << imm << ");" << std::endl;
           out_file << "}" << std::endl;
           break;
        }
        case MIPS_INS_LB : {
            std::cerr << "  Operand 0 [dest]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [mem]: type=" << mips_details.operands[1].type << ", base=" << mips_details.operands[1].mem.base << ", disp=" << mips_details.operands[1].mem.disp << std::endl;
            const auto& dest_capstone = mips_details.operands[0].reg;
            const auto& base_capstone = mips_details.operands[1].mem.base;
            const auto& offset = mips_details.operands[1].mem.disp;
            int dest_index = get_gpr_index(dest_capstone);
            int base_index = get_gpr_index(base_capstone);

            out_file << "{"
            out_file << "    u32 address = context.cpuRegs.GPR.r[" << base_index << "].UD[0] + " << offset << ";" << std::endl;
            out_file << "    context.cpuRegs.GPR.r[" << dest_index << "].SD[0] = (s64)(s32)ReadMemory8(address);" << std::endl;
            out_file << "}" << std::endl;
            break;
        }
        case MIPS_INS_LBU : {
            std::cerr << "  Operand 0 [dest]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [mem]: type=" << mips_details.operands[1].type << ", base=" << mips_details.operands[1].mem.base << ", disp=" << mips_details.operands[1].mem.disp << std::endl;
            const auto& dest_capstone = mips_details.operands[0].reg;
            const auto& base_capstone = mips_details.operands[1].mem.base;
            const auto& offset = mips_details.operands[1].mem.disp;
            int dest_index = get_gpr_index(dest_capstone);
            int base_index = get_gpr_index(base_capstone);

            out_file << "{"
            out_file << "    u32 address = context.cpuRegs.GPR.r[" << base_index << "].UD[0] + " << offset << ";" << std::endl;
            out_file << "    context.cpuRegs.GPR.r[" << dest_index << "].UD[0] = (u64)(u32)ReadMemory8(address);" << std::endl;
            out_file << "}" << std::endl;
            break;
        }
        case MIPS_INS_LH: {
            const auto& rt_capstone = insn->detail->mips.operands[0].reg;
            const auto& base_capstone = insn->detail->mips.operands[1].mem.base;
            const auto& offset = insn->detail->mips.operands[1].mem.disp; // Capstone gives disp for offset(base) 

            int rt_index = get_gpr_index(rt_capstone);
            int base_index = get_gpr_index(base_capstone);

            // Debug prints (optional)
            std::cerr << "  Operand 0 [rt]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [mem]: type=" << mips_details.operands[1].type << ", base=" << mips_details.operands[1].mem.base << ", disp=" << mips_details.operands[1].mem.disp << std::endl;

            out_file << "{"
            out_file << "    u32 address = context.cpuRegs.GPR.r[" << base_index << "].UD[0] + " << offset << ";" << std::endl;

            // Alignment Check: Address must be 2-byte aligned (address % 2 == 0)
            out_file << "    if (address % 2 != 0) {" << std::endl;
            out_file << "        std::cerr << \"FATAL ERROR: Unaligned memory access for LH at address: 0x\" << std::hex << address << std::endl;" << std::endl;
            out_file << "        exit(1);" << std::endl;
            out_file << "    }" << std::endl;

            // Read the 16-bit value and cast it to a signed 16-bit integer (s16)
            out_file << "    s16 value = (s16)ReadMemory16(address);" << std::endl;
            // Assign the signed 16-bit value to the signed 64-bit register. C++ handles the sign extension.
            out_file << "    context.cpuRegs.GPR.r[" << rt_index << "].SD[0] = (s64)value;" << std::endl;
            out_file << "}" << std::endl;
            break;
        }
        case MIPS_INS_LHU : {
            const auto& rt_capstone = insn->detail->mips.operands[0].reg;
            const auto& base_capstone = insn->detail->mips.operands[1].mem.base;
            const auto& offset = insn->detail->mips.operands[1].mem.disp; // Capstone gives disp for offset(base) 

            int rt_index = get_gpr_index(rt_capstone);
            int base_index = get_gpr_index(base_capstone);

            // Debug prints (optional)
            std::cerr << "  Operand 0 [rt]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [mem]: type=" << mips_details.operands[1].type << ", base=" << mips_details.operands[1].mem.base << ", disp=" << mips_details.operands[1].mem.disp << std::endl;

            out_file << "{"
            out_file << "    u32 address = context.cpuRegs.GPR.r[" << base_index << "].UD[0] + " << offset << ";" << std::endl;

            // Alignment Check: Address must be 2-byte aligned (address % 2 == 0)
            out_file << "    if (address % 2 != 0) {" << std::endl;
            out_file << "        std::cerr << \"FATAL ERROR: Unaligned memory access for LH at address: 0x\" << std::hex << address << std::endl;" << std::endl;
            out_file << "        exit(1);" << std::endl;
            out_file << "    }" << std::endl;

            // Read the 16-bit value and cast it to a signed 16-bit integer (s16)
            out_file << "    u16 value = ReadMemory16(address);" << std::endl;
            // Assign the signed 16-bit value to the signed 64-bit register. C++ handles the sign extension.
            out_file << "    context.cpuRegs.GPR.r[" << rt_index << "].UD[0] = (u64)value;" << std::endl;
            out_file << "}" << std::endl;
            break;
        }
        case MIPS_INS_SB : {
            std::cerr << "  Operand 0 [source]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [mem]: type=" << mips_details.operands[1].type << ", base=" << mips_details.operands[1].mem.base << ", disp=" << mips_details.operands[1].mem.disp << std::endl;
            const auto& source_capstone = mips_details.operands[0].reg;
            const auto& base_capstone = mips_details.operands[1].mem.base;
            const auto& offset = mips_details.operands[1].mem.disp;
            int source_index = get_gpr_index(source_capstone);
            int base_index = get_gpr_index(base_capstone);

            out_file << "{"
            out_file << "    u32 address = context.cpuRegs.GPR.r[" << base_index << "].UD[0] + " << offset << ";" << std::endl;
            out_file << "    WriteMemory8(address, (u8)context.cpuRegs.GPR.r[" << source_index << "].UD[0]);" << std::endl;
            out_file << "}" << std::endl;
            break;
        }
        case MIPS_INS_SH : {
            std::cerr << "  Operand 0 [source]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [mem]: type=" << mips_details.operands[1].type << ", base=" << mips_details.operands[1].mem.base << ", disp=" << mips_details.operands[1].mem.disp << std::endl;
            const auto& source_capstone = mips_details.operands[0].reg;
            const auto& base_capstone = mips_details.operands[1].mem.base;
            const auto& offset = mips_details.operands[1].mem.disp;
            int source_index = get_gpr_index(source_capstone);
            int base_index = get_gpr_index(base_capstone);

            out_file << "{"
            out_file << "    u32 address = context.cpuRegs.GPR.r[" << base_index << "].UD[0] + " << offset << ";" << std::endl;
        
            // Alignment Check: Address must be 2-byte aligned (address % 2 == 0)
            out_file << "    if (address % 2 != 0) {" << std::endl;
            out_file << "        std::cerr << \"FATAL ERROR: Unaligned memory access for LH at address: 0x\" << std::hex << address << std::endl;" << std::endl;
            out_file << "        exit(1);" << std::endl;
            out_file << "    }" << std::endl;
            out_file << "    WriteMemory16(address, (u16)context.cpuRegs.GPR.r[" << source_index << "].UD[0]);" << std::endl;
            out_file << "}" << std::endl;
            break;
        }
        case MIPS_INS_BGTZ : {
            std::cerr << "  Operand 0 [rs]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 2 [offset]: type=" << mips_details.operands[1].type << ", imm=" << mips_details.operands[2].imm << std::endl;
            const auto& rs_capstone = mips_details.operands[0].reg;
            const auto& off = mips_details.operands[1].imm;
            int rs_index = get_gpr_index(rs_capstone);
            u32 target = calculate_target(*insn);

            

            out_file << "    if ((s64)context.cpuRegs.GPR.r[" << rs_index << "].SD[0] > 0) {" << std::endl;
            out_file << "        func_0x" << std::hex << target << "();" << std::endl;
            out_file << "        return;" << std::endl;
            out_file << "    }" << std::endl;
            break;
        }
        case MIPS_INS_BLEZ : {
            std::cerr << "  Operand 0 [rs]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 2 [offset]: type=" << mips_details.operands[1].type << ", imm=" << mips_details.operands[2].imm << std::endl;
            const auto& rs_capstone = mips_details.operands[0].reg;
            const auto& off = mips_details.operands[1].imm;
            int rs_index = get_gpr_index(rs_capstone);

            u32 target = calculate_target(*insn);

            out_file << "    if ((s64)context.cpuRegs.GPR.r[" << rs_index << "].SD[0] <= 0) {" << std::endl;
            out_file << "        func_0x" << std::hex << target << "();" << std::endl;
            out_file << "        return;" << std::endl;
            out_file << "    }" << std::endl;
            break;
        }
        case MIPS_INS_JAL : {
            std::cerr << "  Operand 0 [target]: type=" << mips_details.operands[0].type << ", imm=" << mips_details.operands[0].imm << std::endl;
            const auto& target_imm = mips_details.operands[0].imm;

            u32 target = calculate_target(*insn);
            out_file << "    context.cpuRegs.GPR.r[31].UD[0] = " << insn->address << "+ 8;" << std::endl;
            /*                      0xFFFFFFFF                              0x02FFFFFF
                                    0xF0000000                              0x0FFFFFF0


            context.cpuRegs.pc = (insn->address & 0xF0000000) | (target_reg_index << 2);
            */
            out_file << "    func_0x" << std::hex << target << "();" << std::endl;
            out_file << "    return;" << std::endl;
            break;
        }
        case MIPS_INS_J : {
            std::cerr << "  Operand 0 [target]: type=" << mips_details.operands[0].type << ", imm=" << mips_details.operands[0].imm << std::endl;
            const auto& target_imm = mips_details.operands[0].imm;

            u32 target = calculate_target(*insn);

            /*                      0xFFFFFFFF                              0x02FFFFFF
                                    0xF0000000                              0x0FFFFFF0


            context.cpuRegs.pc = (insn->address & 0xF0000000) | (target_reg_index << 2);
            */
           out_file << "    func_0x" << std::hex << target << "();" << std::endl;
           out_file << "    return;" << std::endl;
           break;
        }
        case MIPS_INS_JALR : {
            std::cerr << "  Operand 0 [rd]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [rt]: type=" << mips_details.operands[1].type << ", reg=" << mips_details.operands[1].reg << std::endl;
            const auto& rd_reg = mips_details.operands[0].reg;
            const auto& rs_reg = mips_details.operands[1].reg;

            int rd_index = get_gpr_index(rd_reg);
            int rs_index = get_gpr_index(rs_reg);
            /*
            So when we are jumping, we are jumping somewhere else in that code and then we want to return from that jump we need to know where we want to jump back to.
            We save this place we want to return in the rd register. We dont save the next instruction because that is already done since J instructions do the next immediate
            instruction before the jump so we store the one after. 

            context.cpuRegs.GPR.r[rd_index].UD[0] = insn->address + 8;
            context.cpuRegs.pc = context.cpuRegs.GPR.r[rs_index];
            */

            out_file << "    context.cpuRegs.GPR.r[ " << rd_index <<" ].UD[0] = " << std::hex << insn->address <<  " + 8;" << std::endl;
            out_file << "    host_dispatch_jump(context.cpuRegs.GPR.r[ " << rs_index <<" ].UD[0]);" << std::endl;
            out_file << "    return;" << std::endl;
            break;
        }
        case MIPS_INS_SYSCALL : {

            /*
            
                SYS operation: v0 -> $2
                Parameters: a0-a3 ->  $4, $5, $6, $7




                Mode SWITCH
                context.cpuRegs.CP0.n.EPC = current_insn + 4;
                context.cpuRegs.CP0.n.Cause = syscall_code;
                                              |      |
                1111 1111 1111 1111 1111 1111 1000 0011
                8 =                               01010
                Need to shift by 2 -> 8 << 2

                                              |      |
                1111 1111 1111 1111 1111 1111 1000 0011
                                              |      |
                8 =                            010 10__


                // sys_handlers returns the address to go to
                sys_handler(cs_mips& context_details);
            
            */

            out_file << "context.cpuRegs.CP0.n.EPC = " << insn->address <<" + 4;" << std::endl;
            out_file << "context.cpuRegs.CP0.n.Cause = (context.cpuRegs.CP0.n.Cause & 0xFFFFFF83) | (8 << 2);" << std::endl;
            out_file << "sys_handler(context);" << std::endl;


            break;
        }
        case MIPS_INS_MFC0 : {
            std::cerr << "  Operand 0 [rt]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [rd]: type=" << mips_details.operands[1].type << ", reg=" << mips_details.operands[1].reg << std::endl;
            const auto& rt_reg = mips_details.operands[0].reg;
            const auto& rd_reg = mips_details.operands[1].reg;

            int rt_index = get_gpr_index(rt_reg);

            /*
            
            context.cpuRegs.GPR.r[rt_index].SD[0] = (s64)(s32)context.cpuRegs.CP0.r[rd_reg];
            */

            out_file << "context.cpuRegs.GPR.r["<< rt_index <<"].SD[0] = (s64)(s32)context.cpuRegs.CP0.r["<< rd_reg <<" ];" << std::endl;
            break;
        }
        case MIPS_INS_MTC0 : {
            std::cerr << "  Operand 0 [rt]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [rd]: type=" << mips_details.operands[1].type << ", reg=" << mips_details.operands[1].reg << std::endl;
            const auto& rt_reg = mips_details.operands[0].reg;
            const auto& rd_reg = mips_details.operands[1].reg;

            int rt_index = get_gpr_index(rt_reg);

            /*

            context.cpuRegs.CP0.r[rd_reg] = (s32)context.cpuRegs.GPR.r[rt_index];
            */

            out_file << "context.cpuRegs.CP0.r["<< rd_reg <<"] = (u32)context.cpuRegs.GPR.r["<< rt_index <<"].UD[0];" << std::endl;
            break;
        }


        // --- SPECIAL TABLE (Function based) ---
        case MIPS_INS_SLLV: {
            std::cerr << "  Operand 0 [rd]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [rt]: type=" << mips_details.operands[1].type << ", reg=" << mips_details.operands[1].reg << std::endl;
            std::cerr << "  Operand 2 [rs]: type=" << mips_details.operands[2].type << ", reg=" << mips_details.operands[2].reg << std::endl;
            // TODO: Implement SLLV (Shift Left Logical Variable) ONLY CARE ABOUT THE LOWER 5 BITS OF RS
            // MIPS: sllv rd, rt, rs

            const auto& rd_reg = mips_details.operands[0].reg;
            const auto& rt_reg = mips_details.operands[1].reg;
            const auto& rs_reg = mips_details.operands[2].reg;

            int rd_index = get_gpr_index(rd_reg);
            int rt_index = get_gpr_index(rt_reg);
            int rs_index = get_gpr_index(rs_reg);

            /*
                0000 0000 0000 0000 0000 0000 0001 1111
                context.cpuRegs.GPR[rd_index].UD[0] = (s64)((s32)context.cpuRegs.GPR[rt_index].UD[0] <<  (s32)(context.cpuRegs.GPR[rs_index].SD[0] & 0x1F));
            */

            out_file << "context.cpuRegs.GPR["<< rd_index <<"].UD[0] = (u64)((u32)context.cpuRegs.GPR["<< rt_index <<"].UD[0] << (u32)(context.cpuRegs.GPR["<< rs_index <<"].UD[0] & 0x1F));" << std::endl;
            break;
        }
        case MIPS_INS_SRLV: {
            std::cerr << "  Operand 0 [rd]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [rt]: type=" << mips_details.operands[1].type << ", reg=" << mips_details.operands[1].reg << std::endl;
            std::cerr << "  Operand 2 [rs]: type=" << mips_details.operands[2].type << ", reg=" << mips_details.operands[2].reg << std::endl;
            // TODO: Implement SRLV (Shift Right Logical Variable) ONLY CARE ABOUT THE LOWER 5 BITS OF RS
            // MIPS: srlv rd, rt, rs

            const auto& rd_reg = mips_details.operands[0].reg;
            const auto& rt_reg = mips_details.operands[1].reg;
            const auto& rs_reg = mips_details.operands[2].reg;

            int rd_index = get_gpr_index(rd_reg);
            int rt_index = get_gpr_index(rt_reg);
            int rs_index = get_gpr_index(rs_reg);

            /*
                0000 0000 0000 0000 0000 0000 0001 1111
                context.cpuRegs.GPR[rd_index].SD[0] = (s64)((s32)context.cpuRegs.GPR[rt_index].UD[0] >>  (s32)(context.cpuRegs.GPR[rs_index].SD[0] & 0x1F));
            */

            out_file << "context.cpuRegs.GPR["<< rd_index <<"].UD[0] = (u64)((u32)context.cpuRegs.GPR["<< rt_index <<"].UD[0] >> (u32)(context.cpuRegs.GPR["<< rs_index <<"].UD[0] & 0x1F));" << std::endl;
            break;
        }
        case MIPS_INS_SRAV: {
            std::cerr << "  Operand 0 [rd]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [rt]: type=" << mips_details.operands[1].type << ", reg=" << mips_details.operands[1].reg << std::endl;
            std::cerr << "  Operand 2 [rs]: type=" << mips_details.operands[2].type << ", reg=" << mips_details.operands[2].reg << std::endl;
            // TODO: Implement SRAV (Shift Right Arithmetic Variable)
            // MIPS: srav rd, rt, rs

            const auto& rd_reg = mips_details.operands[0].reg;
            const auto& rt_reg = mips_details.operands[1].reg;
            const auto& rs_reg = mips_details.operands[2].reg;

            int rd_index = get_gpr_index(rd_reg);
            int rt_index = get_gpr_index(rt_reg);
            int rs_index = get_gpr_index(rs_reg);

            /*
                0000 0000 0000 0000 0000 0000 0011 1111
                context.cpuRegs.GPR[rd_index].SD[0] = (s64)((s32)context.cpuRegs.GPR[rt_index].UD[0] >>  (s32)(context.cpuRegs.GPR[rs_index].SD[0] & 0x1F));
            */

            out_file << "context.cpuRegs.GPR["<< rd_index <<"].SD[0] = (s64)((s32)context.cpuRegs.GPR["<< rt_index <<"].SD[0] >> (s32)(context.cpuRegs.GPR["<< rs_index <<"].SD[0] & 0x1F));" << std::endl;
            break;
        }
        case MIPS_INS_DSLLV: {
            std::cerr << "  Operand 0 [rd]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [rt]: type=" << mips_details.operands[1].type << ", reg=" << mips_details.operands[1].reg << std::endl;
            std::cerr << "  Operand 2 [rs]: type=" << mips_details.operands[2].type << ", reg=" << mips_details.operands[2].reg << std::endl;
            // TODO: Implement DSLLV (Doubleword Shift Left Logical Variable) Im guessing its 8 bits of rs
            // MIPS: dsllv rd, rt, rs

            const auto& rd_reg = mips_details.operands[0].reg;
            const auto& rt_reg = mips_details.operands[1].reg;
            const auto& rs_reg = mips_details.operands[2].reg;

            int rd_index = get_gpr_index(rd_reg);
            int rt_index = get_gpr_index(rt_reg);
            int rs_index = get_gpr_index(rs_reg);

            /*
                0000 0000 0000 0000 0000 0000 1111 1111
                context.cpuRegs.GPR[rd_index].UD[0] = (s64)((s32)context.cpuRegs.GPR[rt_index].UD[0] <<  (s32)(context.cpuRegs.GPR[rs_index].SD[0] & 0x3F));
            */

            out_file << "context.cpuRegs.GPR["<< rd_index <<"].UD[0] = (u64)((u32)context.cpuRegs.GPR["<< rt_index <<"].UD[0] << (u32)(context.cpuRegs.GPR["<< rs_index <<"].UD[0] & 0x3F));" << std::endl;
            break;
        }
        case MIPS_INS_DSRLV: {
            std::cerr << "  Operand 0 [rd]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [rt]: type=" << mips_details.operands[1].type << ", reg=" << mips_details.operands[1].reg << std::endl;
            std::cerr << "  Operand 2 [rs]: type=" << mips_details.operands[2].type << ", reg=" << mips_details.operands[2].reg << std::endl;
            // TODO: Implement DSRLV (Doubleword Shift Right Logical Variable)
            // MIPS: dsrlv rd, rt, rs
            const auto& rd_reg = mips_details.operands[0].reg;
            const auto& rt_reg = mips_details.operands[1].reg;
            const auto& rs_reg = mips_details.operands[2].reg;

            int rd_index = get_gpr_index(rd_reg);
            int rt_index = get_gpr_index(rt_reg);
            int rs_index = get_gpr_index(rs_reg);

            /*
                0000 0000 0000 0000 0000 0000 0001 1111
                context.cpuRegs.GPR[rd_index].SD[0] = (s64)((s32)context.cpuRegs.GPR[rt_index].UD[0] >>  (s32)(context.cpuRegs.GPR[rs_index].SD[0] & 0x1F));
            */

            out_file << "context.cpuRegs.GPR["<< rd_index <<"].UD[0] = (u64)((u32)context.cpuRegs.GPR["<< rt_index <<"].UD[0] >> (u32)(context.cpuRegs.GPR["<< rs_index <<"].UD[0] & 0x3F));" << std::endl;
            break;
            break;
        }
        case MIPS_INS_DSRAV: {
            std::cerr << "  Operand 0 [rd]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [rt]: type=" << mips_details.operands[1].type << ", reg=" << mips_details.operands[1].reg << std::endl;
            std::cerr << "  Operand 2 [rs]: type=" << mips_details.operands[2].type << ", reg=" << mips_details.operands[2].reg << std::endl;
            // TODO: Implement DSRAV (Doubleword Shift Right Arithmetic Variable)
            // MIPS: dsrav rd, rt, rs

            const auto& rd_reg = mips_details.operands[0].reg;
            const auto& rt_reg = mips_details.operands[1].reg;
            const auto& rs_reg = mips_details.operands[2].reg;

            int rd_index = get_gpr_index(rd_reg);
            int rt_index = get_gpr_index(rt_reg);
            int rs_index = get_gpr_index(rs_reg);

            /*
                0000 0000 0000 0000 0000 0000 0011 1111
                context.cpuRegs.GPR[rd_index].SD[0] = (s64)((s32)context.cpuRegs.GPR[rt_index].UD[0] >>  (s32)(context.cpuRegs.GPR[rs_index].SD[0] & 0x1F));
            */

            out_file << "context.cpuRegs.GPR["<< rd_index <<"].SD[0] = (s64)((s32)context.cpuRegs.GPR["<< rt_index <<"].SD[0] >> (s32)(context.cpuRegs.GPR["<< rs_index <<"].SD[0] & 0x3F));" << std::endl;
            break;
            break;
        }

        case MIPS_INS_MOVZ: {
            std::cerr << "  Operand 0 [rd]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [rs]: type=" << mips_details.operands[1].type << ", reg=" << mips_details.operands[1].reg << std::endl;
            std::cerr << "  Operand 2 [rt]: type=" << mips_details.operands[2].type << ", reg=" << mips_details.operands[2].reg << std::endl;
            // TODO: Implement MOVZ (Move conditional on Zero)
            // MIPS: movz rd, rs, rt
            // C++: if (rt == 0) rd = rs

            const auto& rd_reg = mips_details.operands[0].reg;
            const auto& rt_reg = mips_details.operands[1].reg;
            const auto& rs_reg = mips_details.operands[2].reg;

            int rd_index = get_gpr_index(rd_reg);
            int rt_index = get_gpr_index(rt_reg);
            int rs_index = get_gpr_index(rs_reg);

            /*
            {
                if (context.cpuRegs.GPR.r[rt_index].UD[0] == 0){
                    context.cpuRegs.GPR.r[rd_index].UD[0] = context.cpuRegs.GPR.r[rs_index].UD[0];
                }
            }
            */
           
            out_file << "{"
            out_file << "  if (context.cpuRegs.GPR.r["<< rt_index <<"].UD[0] == 0){" << std::endl;
            out_file << "      context.cpuRegs.GPR.r["<< rd_index <<"].UD[0] = context.cpuRegs.GPR.r["<< rs_index <<"].UD[0];" << std::endl;
            out_file << "  }" << std::endl;
            out_file << "}" << std::endl;

            break;
        }
        case MIPS_INS_MOVN: {
            std::cerr << "  Operand 0 [rd]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [rs]: type=" << mips_details.operands[1].type << ", reg=" << mips_details.operands[1].reg << std::endl;
            std::cerr << "  Operand 2 [rt]: type=" << mips_details.operands[2].type << ", reg=" << mips_details.operands[2].reg << std::endl;
            // TODO: Implement MOVN (Move conditional on Not Zero)
            // MIPS: movn rd, rs, rt
            // C++: if (rt != 0) rd = rs
            const auto& rd_reg = mips_details.operands[0].reg;
            const auto& rt_reg = mips_details.operands[1].reg;
            const auto& rs_reg = mips_details.operands[2].reg;

            int rd_index = get_gpr_index(rd_reg);
            int rt_index = get_gpr_index(rt_reg);
            int rs_index = get_gpr_index(rs_reg);

            /*
            {
                if (context.cpuRegs.GPR.r[rt_index].UD[0] != 0){
                    context.cpuRegs.GPR.r[rd_index].UD[0] = context.cpuRegs.GPR.r[rs_index].UD[0];
                }
            }
            */
           
            out_file << "{"
            out_file << "  if (context.cpuRegs.GPR.r["<< rt_index <<"].UD[0] != 0){" << std::endl;
            out_file << "      context.cpuRegs.GPR.r["<< rd_index <<"].UD[0] = context.cpuRegs.GPR.r["<< rs_index <<"].UD[0];" << std::endl;
            out_file << "  }" << std::endl;
            out_file << "}" << std::endl;

            break;
        }
        case MIPS_INS_SYNC: {
            // TODO: Implement SYNC (Synchronize)
            // This is for memory ordering on multiprocessor systems.
            // For a single-threaded recompiler, this can often be treated as a NOP.
            break;
        }
        case MIPS_INS_MFHI: {
            std::cerr << "  Operand 0 [rd]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            // TODO: Implement MFHI (Move From HI)
            // MIPS: mfhi rd
            const auto& rd_reg = mips_details.operands[0].reg;

            int rd_index = get_gpr_index(rd_reg);

            /*
            context.cpuRegs.GPR.r[rd_index].UD[0] = context.cpuRegs.HI.UD[0];
            */
            out_file << "context.cpuRegs.GPR.r["<< rd_index <<"].UD[0] = context.cpuRegs.HI.UD[0];" << std::endl;
            break;
        }
        case MIPS_INS_MTHI: {
            std::cerr << "  Operand 0 [rs]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            // TODO: Implement MTHI (Move To HI)
            // MIPS: mthi rs
            const auto& rd_reg = mips_details.operands[0].reg;

            int rd_index = get_gpr_index(rd_reg);
            /*
            context.cpuRegs.HI.UD[0] = (u32)context.cpuRegs.GPR.[ rd_index ].UD[0];
            */

            out_file << "context.cpuRegs.HI.UD[0] = (u32)context.cpuRegs.GPR.["<< rd_index <<"].UD[0];" << std::endl;
            break;
        }
        case MIPS_INS_MFLO: {
            std::cerr << "  Operand 0 [rd]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            // TODO: Implement MFLO (Move From LO)
            // MIPS: mflo rd

            const auto& rd_reg = mips_details.operands[0].reg;

            int rd_index = get_gpr_index(rd_reg);

            /*
            context.cpuRegs.GPR.r[rd_index].UD[0] = context.cpuRegs.LO.UD[0];
            */
            out_file << "context.cpuRegs.GPR.r["<< rd_index <<"].UD[0] = context.cpuRegs.LO.UD[0];" << std::endl;
            break;
        }
        case MIPS_INS_MTLO: {
            std::cerr << "  Operand 0 [rs]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            // TODO: Implement MTLO (Move To LO)
            // MIPS: mtlo rs
            const auto& rd_reg = mips_details.operands[0].reg;

            int rd_index = get_gpr_index(rd_reg);
            /*
            context.cpuRegs.LO.UD[0] = (u32)context.cpuRegs.GPR.[ rd_index ].UD[0];
            */

            out_file << "context.cpuRegs.LO.UD[0] = (u32)context.cpuRegs.GPR.["<< rd_index <<"].UD[0];" << std::endl;
            break;
        }
        case MIPS_INS_MULTU: {
            std::cerr << "  Operand 0 [rs]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [rt]: type=" << mips_details.operands[1].type << ", reg=" << mips_details.operands[1].reg << std::endl;
            // TODO: Implement MULTU (Multiply Unsigned)
            // MIPS: multu rs, rt
            const auto& rs_reg = mips_details.operands[0].reg;
            const auto& rt_reg = mips_details.operands[1].reg;

            int rs_index = get_gpr_index(rs_reg);
            int rt_index = get_gpr_index(rt_reg);

            /*
            u32 op1 = (u32)context.cpuRegs.GPR.r[rs_index].UD[0];
            u32 op2 = (u32)context.cpuRegs.GPR.r[rt_index].UD[0];
            u64 product = (u64)op1 * op2;
            context.cpuRegs.LO.UD[0] = (u64)(u32)product;
            context.cpuRegs.HI.UD[0] = (u64)(u32)(product >> 32);
            */

            out_file << "u32 op1 = (u32)context.cpuRegs.GPR.r["<< rs_index <<"].UD[0];" << std::endl;
            out_file << "u32 op2 = (u32)context.cpuRegs.GPR.r["<< rt_index <<"].UD[0];" << std::endl;
            out_file << "u64 product = (u64)op1 * op2;" << std::endl;
            out_file << "context.cpuRegs.LO.UD[0] = (u64)(u32)product;" << std::endl;
            out_file << "context.cpuRegs.HI.UD[0] = (u64)(u32)(product >> 32);" << std::endl;

            break;
        }
        case MIPS_INS_DIVU: {
            std::cerr << "  Operand 0 [rs]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [rt]: type=" << mips_details.operands[1].type << ", reg=" << mips_details.operands[1].reg << std::endl;
            // TODO: Implement DIVU (Divide Unsigned)
            // MIPS: divu rs, rt

            const auto& rs_reg = mips_details.operands[0].reg;
            const auto& rt_reg = mips_details.operands[1].reg;

            int rs_index = get_gpr_index(rs_reg);
            int rt_index = get_gpr_index(rt_reg);

            /*
            {
                u32 num = context.cpuRegs.GPR.r[rs_index].UD[0];
                u32 den = context.cpuRegs.GPR.r[rt_index].UD[0];

                if (den != 0){
                    u32 HI_ans = num % den;
                    u32 LO_ans = num / den;
                    context.cpuRegs.LO.UD[0] = (u64)(u32)(LO_ans);
                    context.cpuRegs.HI.UD[0] = (u64)(u32)(HI_ans);
                }
            
            }
            */

            out_file << "{"
            out_file << "u32 num = context.cpuRegs.GPR.r[" << rs_index << "].UD[0];"<< std::endl;
            out_file << "u32 den = context.cpuRegs.GPR.r[" << rt_index << "].UD[0];"<< std::endl;
            out_file << "  if (den != 0){ " << std::endl;
            out_file << "      u32 HI_ans = num " << "%" << " den;" << std::endl;
            out_file << "      u32 LO_ans = num " << "/" << " den;" << std::endl;
            out_file << "      context.cpuRegs.LO.UD[0] = (u64)(u32)(LO_ans);" << std::endl;
            out_file << "      context.cpuRegs.HI.UD[0] = (u64)(u32)(HI_ans);" << std::endl;
            out_file << "   }" << std::endl;
            out_file << "}" << std::endl;
            break;
        }
        case MIPS_INS_ADD: {
            std::cerr << "  Operand 0 [rd]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [rs]: type=" << mips_details.operands[1].type << ", reg=" << mips_details.operands[1].reg << std::endl;
            std::cerr << "  Operand 2 [rt]: type=" << mips_details.operands[2].type << ", reg=" << mips_details.operands[2].reg << std::endl;
            // TODO: Implement ADD (Add with overflow trap)
            // For now, can be implemented same as ADDU.
            const auto& rd_reg = mips_details.operands[0].reg;
            const auto& rs_reg = mips_details.operands[1].reg;
            const auto& rt_reg = mips_details.operands[2].reg;

            int rd_index = get_gpr_index(rd_reg);
            int rs_index = get_gpr_index(rs_reg);
            int rt_index = get_gpr_index(rt_reg);

            /*
            s32 op1 = (s32)context.cpuRegs.GPR.r[rs_index].SD[0];
            s32 op2 = (s32)context.cpuRegs.GPR.r[rt_index].SD[0];


            s64 sum = (s64)op1 + (s64)op2;

            0000 0000 0000 0000 0000 0000 0000 0000 | 0000 0000 0000 0000 0000 0000 0000 0000

            if (sum != (s64)(s32)sum){

                handle_overflow();

            }
            else{
                context.cpuRegs.GPR.r[rd_index].SD[0] = sum;
            }

            */

            out_file << "s32 op1 = (s32)context.cpuRegs.GPR.r[" << rs_index <<"].SD[0];" << std::endl;
            out_file << "s32 op2 = (s32)context.cpuRegs.GPR.r[" << rt_index << "].SD[0];" << std::endl;
            out_file << "s64 sum = (s64)op1 + (s64)op2;" << std::endl;
            out_file << "if (sum != (s64)(s32)sum){ " << std::endl;
            out_file << "  handle_overflow();" << std::endl;
            out_file << "}" << std::endl;
            out_file << "else{"
            out_file << "  context.cpuRegs.GPR.r[" << rd_index <<"].SD[0] = sum;" << std::endl;
            out_file << "}" << std::endl;


            break;
        }
        case MIPS_INS_SUB: {
            std::cerr << "  Operand 0 [rd]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [rs]: type=" << mips_details.operands[1].type << ", reg=" << mips_details.operands[1].reg << std::endl;
            std::cerr << "  Operand 2 [rt]: type=" << mips_details.operands[2].type << ", reg=" << mips_details.operands[2].reg << std::endl;
            // TODO: Implement SUB (Subtract with overflow trap)
            // For now, can be implemented same as SUBU.
            // rd = rs - rt

            const auto& rd_reg = mips_details.operands[0].reg;
            const auto& rs_reg = mips_details.operands[1].reg;
            const auto& rt_reg = mips_details.operands[2].reg;

            int rd_index = get_gpr_index(rd_reg);
            int rs_index = get_gpr_index(rs_reg);
            int rt_index = get_gpr_index(rt_reg);

            /*
                s32 op1 = (s32)context.cpuRegs.GPR.r[rs_index].SD[0];
                s32 op2 = (s32)context.cpuRegs.GPR.r[rt_index].SD[0];

                s64 diff = (s64)op1 - (s64)op2;

                if (diff != (s64)(s32)diff){
                    handle_overflow();
                }
                else{
                    context.cpuRegs.GPR.r[rd_index].SD[0] = diff;
                }
            
            */

            out_file << "s32 op1 = (s32)context.cpuRegs.GPR.r[rs_index].SD[0];" << std::endl;
            out_file << "s32 op2 = (s32)context.cpuRegs.GPR.r[rt_index].SD[0];" << std::endl;
            out_file << "s64 diff = (s64)op1 - (s64)op2;" << std::endl;
            out_file << "if (diff != (s64)(s32)diff){ " << std::endl;
            out_file << "  handle_overflow();" << std::endl;
            out_file << "}" << std::endl;
            out_file << "else{"
            out_file << "  context.cpuRegs.GPR.r[rd_index].SD[0] = diff;" << std::endl;
            out_file << "}" << std::endl;
            break;
        }
        case MIPS_INS_AND: {
            std::cerr << "  Operand 0 [rd]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [rs]: type=" << mips_details.operands[1].type << ", reg=" << mips_details.operands[1].reg << std::endl;
            std::cerr << "  Operand 2 [rt]: type=" << mips_details.operands[2].type << ", reg=" << mips_details.operands[2].reg << std::endl;
            // TODO: Implement AND (AND)
            // MIPS: and rd, rs, rt
 
            const auto& rd_reg = mips_details.operands[0].reg;
            const auto& rs_reg = mips_details.operands[1].reg;
            const auto& rt_reg = mips_details.operands[2].reg;

            int rd_index = get_gpr_index(rd_reg);
            int rs_index = get_gpr_index(rs_reg);
            int rt_index = get_gpr_index(rt_reg);

             /*
             {
                 u64 rs = context.cpuRegs.GPR.r[rs_index].UD[0];
                 u64 rt = context.cpuRegs.GPR.r[rt_index].UD[0];
                 context.cpuRegs.GPT.r[rd_index].UD[0] = rs & rt;
             
             }
             */
            out_file << "{"
            out_file << "   u64 rs_val = context.cpuRegs.GPR.r["<< rs_index <<"].UD[0];" << std::endl;
            out_file << "   u64 rt_val = context.cpuRegs.GPR.r["<< rt_index <<"].UD[0];" << std::endl;
            out_file << "   context.cpuRegs.GPR.r[" << rd_index << "].UD[0] = rs_val & rt_val;" << std::endl;
            out_file << "}" << std::endl;
            break;
        }
        /*case MIPS_INS_MFSA: {
           // TODO: Implement MFSA (Move From SA)
           // MIPS: mfsa rd
           // C++: rd = sa
           break;
        
        case MIPS_INS_MTSA: {
           // TODO: Implement MTSA (Move To SA)
           // MIPS: mtsa rs
           // C++: sa = rs
           break;
        */
        case MIPS_INS_SLTU: {
            std::cerr << "  Operand 0 [rd]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [rs]: type=" << mips_details.operands[1].type << ", reg=" << mips_details.operands[1].reg << std::endl;
            std::cerr << "  Operand 2 [rt]: type=" << mips_details.operands[2].type << ", reg=" << mips_details.operands[2].reg << std::endl;
            // TODO: Implement SLTU (Set on Less Than Unsigned)
            // MIPS: sltu rd, rs, rt

            const auto& rd_reg = mips_details.operands[0].reg;
            const auto& rs_reg = mips_details.operands[1].reg;
            const auto& rt_reg = mips_details.operands[2].reg;

            int rd_index = get_gpr_index(rd_reg);
            int rs_index = get_gpr_index(rs_reg);
            int rt_index = get_gpr_index(rt_reg);

            out_file << "context.cpuRegs.GPR.r[" << rd_index << "].UD[0] = (u64)((u32)context.cpuRegs.GPR.r[" << rs_index << "].UD[0] < (u32)context.cpuRegs.GPR.r[" << rt_index << "].UD[0] ? 1 : 0);"<< std::endl;
            break;
        }
        case MIPS_INS_DADD: {
            std::cerr << "  Operand 0 [rd]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [rs]: type=" << mips_details.operands[1].type << ", reg=" << mips_details.operands[1].reg << std::endl;
            std::cerr << "  Operand 2 [rt]: type=" << mips_details.operands[2].type << ", reg=" << mips_details.operands[2].reg << std::endl;
            // TODO: Implement DADD (Doubleword Add with overflow trap)
            // For now, can be implemented same as DADDU.

            const auto& rd_reg = mips_details.operands[0].reg;
            const auto& rs_reg = mips_details.operands[1].reg;
            const auto& rt_reg = mips_details.operands[2].reg;

            int rd_index = get_gpr_index(rd_reg);
            int rs_index = get_gpr_index(rs_reg);
            int rt_index = get_gpr_index(rt_reg);
            /*
            s64 rs = (s64)context.cpuRegs.GPR.r[rs_index].SD[0];
            s64 rt = (s64)context.cpuRegs.GPR.r[rt_index].SD[0];


            s64 sum = (s64)rs + (s64)rt;

            0000 0000 0000 0000 0000 0000 0000 0000 | 0000 0000 0000 0000 0000 0000 0000 0000

            if (((rs > 0 && rt > 0) && sum < 0) || ((rs < 0 && rt < 0) && sum > 0)) {

                handle_overflow();

            }
            else{
                context.cpuRegs.GPR.r[rd_index].SD[0] = sum;
            }
            */

            out_file << "{"
            out_file << "    s64 rs = context.cpuRegs.GPR.r[" << rs_index << "].SD[0];" << std::endl;
            out_file << "    s64 rt = context.cpuRegs.GPR.r[" << rt_index << "].SD[0];" << std::endl;
            out_file << "    s64 sum = rs + rt;" << std::endl;
            out_file << "    if (((rs > 0 && rt > 0) && sum < 0) || ((rs < 0 && rt < 0) && sum > 0)) {" << std::endl;
            out_file << "        // TODO: Trigger Overflow Exception" << std::endl;
            out_file << "    } else {" << std::endl;
            out_file << "        context.cpuRegs.GPR.r[" << rd_index << "].SD[0] = sum;" << std::endl;
            out_file << "    }" << std::endl;
            out_file << "}" << std::endl;
            break;
        }
        case MIPS_INS_DADDU: {
            std::cerr << "  Operand 0 [rd]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [rs]: type=" << mips_details.operands[1].type << ", reg=" << mips_details.operands[1].reg << std::endl;
            std::cerr << "  Operand 2 [rt]: type=" << mips_details.operands[2].type << ", reg=" << mips_details.operands[2].reg << std::endl;
            // TODO: Implement DADDU (Doubleword Add Unsigned)
            // MIPS: daddu rd, rs, rt

            const auto& rd_reg = mips_details.operands[0].reg;
            const auto& rs_reg = mips_details.operands[1].reg;
            const auto& rt_reg = mips_details.operands[2].reg;

            int rd_index = get_gpr_index(rd_reg);
            int rs_index = get_gpr_index(rs_reg);
            int rt_index = get_gpr_index(rt_reg);
            /*
            u64 rs = (u64)context.cpuRegs.GPR.r[rs_index].UD[0];
            u64 rt = (u64)context.cpuRegs.GPR.r[rt_index].UD[0];


            u64 sum = (u64)rs + (u64)rt;

            0000 0000 0000 0000 0000 0000 0000 0000 | 0000 0000 0000 0000 0000 0000 0000 0000

            if (rs > sum || rt > sum) {

                handle_overflow();

            }
            else{
                context.cpuRegs.GPR.r[rd_index].UD[0] = sum;
            }
            */

            out_file << "{"
            out_file << "    u64 rs = context.cpuRegs.GPR.r[" << rs_index << "].UD[0];" << std::endl;
            out_file << "    u64 rt = context.cpuRegs.GPR.r[" << rt_index << "].UD[0];" << std::endl;
            out_file << "    u64 sum = rs + rt;" << std::endl;
            out_file << "    context.cpuRegs.GPR.r[" << rd_index << "].UD[0] = sum;" << std::endl;
            out_file << "}" << std::endl;
            break;
        }
        case MIPS_INS_DSUB: {
            std::cerr << "  Operand 0 [rd]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [rs]: type=" << mips_details.operands[1].type << ", reg=" << mips_details.operands[1].reg << std::endl;
            std::cerr << "  Operand 2 [rt]: type=" << mips_details.operands[2].type << ", reg=" << mips_details.operands[2].reg << std::endl;
            // TODO: Implement DSUB (Doubleword Subtract with overflow trap)
            // For now, can be implemented same as DSUBU.

            const auto& rd_reg = mips_details.operands[0].reg;
            const auto& rs_reg = mips_details.operands[1].reg;
            const auto& rt_reg = mips_details.operands[2].reg;

            int rd_index = get_gpr_index(rd_reg);
            int rs_index = get_gpr_index(rs_reg);
            int rt_index = get_gpr_index(rt_reg);
            /*
            s64 rs = (s64)context.cpuRegs.GPR.r[rs_index].SD[0];
            s64 rt = (s64)context.cpuRegs.GPR.r[rt_index].SD[0];


            s64 sum = (s64)rs + (s64)rt;

            0000 0000 0000 0000 0000 0000 0000 0000 | 0000 0000 0000 0000 0000 0000 0000 0000

            if (((rs > 0 && rt > 0) && sum < 0) || ((rs < 0 && rt < 0) && sum > 0)) {

                handle_overflow();

            }
            else{
                context.cpuRegs.GPR.r[rd_index].SD[0] = sum;
            }
            */

            out_file << "{"
            out_file << "    s64 rs = context.cpuRegs.GPR.r[" << rs_index << "].SD[0];" << std::endl;
            out_file << "    s64 rt = context.cpuRegs.GPR.r[" << rt_index << "].SD[0];" << std::endl;
            out_file << "    s64 diff = rs - rt;" << std::endl;
            out_file << "    if (((rs > 0 && rt < 0) && diff < 0) || ((rs < 0 && rt > 0) && diff > 0)) {" << std::endl;
            out_file << "        // TODO: Trigger Overflow Exception" << std::endl;
            out_file << "    } else {" << std::endl;
            out_file << "        context.cpuRegs.GPR.r[" << rd_index << "].SD[0] = diff;" << std::endl;
            out_file << "    }" << std::endl;
            out_file << "}" << std::endl;
            break;
        }
        case MIPS_INS_DSUBU: {
            std::cerr << "  Operand 0 [rd]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [rs]: type=" << mips_details.operands[1].type << ", reg=" << mips_details.operands[1].reg << std::endl;
            std::cerr << "  Operand 2 [rt]: type=" << mips_details.operands[2].type << ", reg=" << mips_details.operands[2].reg << std::endl;
            // TODO: Implement DSUBU (Doubleword Subtract Unsigned)
            // MIPS: dsubu rd, rs, rt

            const auto& rd_reg = mips_details.operands[0].reg;
            const auto& rs_reg = mips_details.operands[1].reg;
            const auto& rt_reg = mips_details.operands[2].reg;

            int rd_index = get_gpr_index(rd_reg);
            int rs_index = get_gpr_index(rs_reg);
            int rt_index = get_gpr_index(rt_reg);

            out_file << "{"
            out_file << "    u64 rs = context.cpuRegs.GPR.r[" << rs_index << "].UD[0];" << std::endl;
            out_file << "    u64 rt = context.cpuRegs.GPR.r[" << rt_index << "].UD[0];" << std::endl;
            out_file << "    u64 diff = rs - rt;" << std::endl;
            out_file << "    context.cpuRegs.GPR.r[" << rd_index << "].UD[0] = diff;" << std::endl;
            out_file << "}" << std::endl;
            break;
        }
        case MIPS_INS_TGE: {
            std::cerr << "  Operand 0 [rs]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [rt]: type=" << mips_details.operands[1].type << ", reg=" << mips_details.operands[1].reg << std::endl;
            // TODO: Implement TGE (Trap if Greater than or Equal)
            // MIPS: tge rs, rt

            const auto& rs_reg = mips_details.operands[0].reg;
            const auto& rt_reg = mips_details.operands[1].reg;

            int rs_index = get_gpr_index(rs_reg);
            int rt_index = get_gpr_index(rt_reg);

            /*
            if (context.cpuRegs.GPR.r[rs_index] >= context.cpuRegs.GPR.r[rt_index]){
                // TODO HANDLE TRAP
            }
            */

            out_file << "if (context.cpuRegs.GPR.r[" << rs_index <<"].SD[0] >= context.cpuRegs.GPR.r[" << rt_index <<"].SD[0]){ "
            out_file << "// TODO HANDLE TRAP" << std::endl;
            out_file << "}" << std::endl;

            break;
        }
        case MIPS_INS_TGEU: {
            std::cerr << "  Operand 0 [rs]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [rt]: type=" << mips_details.operands[1].type << ", reg=" << mips_details.operands[1].reg << std::endl;
            // TODO: Implement TGEU (Trap if Greater than or Equal Unsigned)
            // MIPS: tgeu rs, rt

            const auto& rs_reg = mips_details.operands[0].reg;
            const auto& rt_reg = mips_details.operands[1].reg;

            int rs_index = get_gpr_index(rs_reg);
            int rt_index = get_gpr_index(rt_reg);

            /*
            if (context.cpuRegs.GPR.r[rs_index] >= context.cpuRegs.GPR.r[rt_index]){
                // TODO HANDLE TRAP
            }
            */

            out_file << "if (context.cpuRegs.GPR.r[" << rs_index <<"].UD[0] >= context.cpuRegs.GPR.r[" << rt_index <<"].UD[0]){ "
            out_file << "// TODO HANDLE TRAP" << std::endl;
            out_file << "}" << std::endl;

            break;
        }
        case MIPS_INS_TLT: {
            std::cerr << "  Operand 0 [rs]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [rt]: type=" << mips_details.operands[1].type << ", reg=" << mips_details.operands[1].reg << std::endl;
            // TODO: Implement TLT (Trap if Less Than)
            // MIPS: tlt rs, rt

            const auto& rs_reg = mips_details.operands[0].reg;
            const auto& rt_reg = mips_details.operands[1].reg;

            int rs_index = get_gpr_index(rs_reg);
            int rt_index = get_gpr_index(rt_reg);

            /*
            if (context.cpuRegs.GPR.r[rs_index] < context.cpuRegs.GPR.r[rt_index]){
                // TODO HANDLE TRAP
            }
            */

            out_file << "if (context.cpuRegs.GPR.r[" << rs_index <<"].SD[0] < context.cpuRegs.GPR.r[" << rt_index <<"].SD[0]){ "
            out_file << "// TODO HANDLE TRAP" << std::endl;
            out_file << "}" << std::endl;

            break;
        }
        case MIPS_INS_TLTU: {
            std::cerr << "  Operand 0 [rs]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [rt]: type=" << mips_details.operands[1].type << ", reg=" << mips_details.operands[1].reg << std::endl;
            // TODO: Implement TLTU (Trap if Less Than Immediate Unsigned)
            // MIPS: tltu rs, rt

            const auto& rs_reg = mips_details.operands[0].reg;
            const auto& rt_reg = mips_details.operands[1].reg;

            int rs_index = get_gpr_index(rs_reg);
            int rt_index = get_gpr_index(rt_reg);

            /*
            if (context.cpuRegs.GPR.r[rs_index] < context.cpuRegs.GPR.r[rt_index]){
                // TODO HANDLE TRAP
            }
            */

            out_file << "if (context.cpuRegs.GPR.r[" << rs_index <<"].UD[0] < context.cpuRegs.GPR.r[" << rt_index <<"].UD[0]){ "
            out_file << "// TODO HANDLE TRAP" << std::endl;
            out_file << "}" << std::endl;


            break;
        }
        case MIPS_INS_TEQ: {
            std::cerr << "  Operand 0 [rs]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [rt]: type=" << mips_details.operands[1].type << ", reg=" << mips_details.operands[1].reg << std::endl;
            // TODO: Implement TEQ (Trap if Equal)
            // MIPS: teq rs, rt


            const auto& rs_reg = mips_details.operands[0].reg;
            const auto& rt_reg = mips_details.operands[1].reg;

            int rs_index = get_gpr_index(rs_reg);
            int rt_index = get_gpr_index(rt_reg);

            /*
            if (context.cpuRegs.GPR.r[rs_index] == context.cpuRegs.GPR.r[rt_index]){
                // TODO HANDLE TRAP
            }
            */

            out_file << "if (context.cpuRegs.GPR.r[" << rs_index <<"].SD[0] == context.cpuRegs.GPR.r[" << rt_index <<"].SD[0]){ "
            out_file << "// TODO HANDLE TRAP" << std::endl;
            out_file << "}" << std::endl;

            break;
        }
        case MIPS_INS_TNE: {
            std::cerr << "  Operand 0 [rs]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [rt]: type=" << mips_details.operands[1].type << ", reg=" << mips_details.operands[1].reg << std::endl;
            // TODO: Implement TNE (Trap if Not Equal)
            // MIPS: tne rs, rt

            const auto& rs_reg = mips_details.operands[0].reg;
            const auto& rt_reg = mips_details.operands[1].reg;

            int rs_index = get_gpr_index(rs_reg);
            int rt_index = get_gpr_index(rt_reg);

            /*
            if (context.cpuRegs.GPR.r[rs_index] >= context.cpuRegs.GPR.r[rt_index]){
                // TODO HANDLE TRAP
            }
            */

            out_file << "if (context.cpuRegs.GPR.r[" << rs_index <<"].SD[0] != context.cpuRegs.GPR.r[" << rt_index <<"].SD[0]){ "
            out_file << "// TODO HANDLE TRAP" << std::endl;
            out_file << "}" << std::endl;

            break;
        }
        case MIPS_INS_DSLL: {
            std::cerr << "  Operand 0 [rd]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [rt]: type=" << mips_details.operands[1].type << ", reg=" << mips_details.operands[1].reg << std::endl;
            std::cerr << "  Operand 2 [sa]: type=" << mips_details.operands[2].type << ", imm=" << mips_details.operands[2].imm << std::endl;
            // TODO: Implement DSLL (Doubleword Shift Left Logical)
            // MIPS: dsll rd, rt, sa

            const auto& rd_reg = mips_details.operands[0].reg;
            const auto& rt_reg = mips_details.operands[1].reg;
            const auto& sa = mips_details.operands[2].imm;

            int rd_index = get_gpr_index(rd_reg);
            int rt_index = get_gpr_index(rt_reg);

            /*
            context.cpuRegs.GPR.r[rd_index].UD[0] = (u64)(context.cpuRegs.GPR.r[rt_index].UD[0] << context.cpuRegs.GPR.sa);
            
            */

            out_file << "context.cpuRegs.GPR.r[" << rd_index << "].UD[0] = context.cpuRegs.GPR.r[" << rt_index << "].UD[0] << " << " << sa << ";" << std::endl;

            break;
        }
        case MIPS_INS_DSRL: {
            std::cerr << "  Operand 0 [rd]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [rt]: type=" << mips_details.operands[1].type << ", reg=" << mips_details.operands[1].reg << std::endl;
            std::cerr << "  Operand 2 [sa]: type=" << mips_details.operands[2].type << ", imm=" << mips_details.operands[2].imm << std::endl;
            // TODO: Implement DSRL (Doubleword Shift Right Logical)
            // MIPS: dsrl rd, rt, sa

            
            const auto& rd_reg = mips_details.operands[0].reg;
            const auto& rt_reg = mips_details.operands[1].reg;
            const auto& sa = mips_details.operands[2].imm;

            int rd_index = get_gpr_index(rd_reg);
            int rt_index = get_gpr_index(rt_reg);

            /*
            context.cpuRegs.GPR.r[rd_index].UD[0] = context.cpuRegs.GPR.r[rt_index].UD[0] >> (56 - (shift * 8));
            
            */

            out_file << "context.cpuRegs.GPR.r[" << rd_index << "].UD[0] = context.cpuRegs.GPR.r[" << rt_index << "].UD[0] >> " << sa << ";" << std::endl;

            break;
        }
        case MIPS_INS_DSRA: {
            std::cerr << "  Operand 0 [rd]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [rt]: type=" << mips_details.operands[1].type << ", reg=" << mips_details.operands[1].reg << std::endl;
            std::cerr << "  Operand 2 [sa]: type=" << mips_details.operands[2].type << ", imm=" << mips_details.operands[2].imm << std::endl;
            // TODO: Implement DSRA (Doubleword Shift Right Arithmetic)
            // MIPS: dsra rd, rt, sa

            
            const auto& rd_reg = mips_details.operands[0].reg;
            const auto& rt_reg = mips_details.operands[1].reg;
            const auto& sa = mips_details.operands[2].imm;

            int rd_index = get_gpr_index(rd_reg);
            int rt_index = get_gpr_index(rt_reg);

            /*
            context.cpuRegs.GPR.r[rd_index].UD[0] = (u64)(context.cpuRegs.GPR.r[rt_index].UD[0] >> context.cpuRegs.GPR.sa);
            
            */

            out_file << "context.cpuRegs.GPR.r[" << rd_index << "].SD[0] = context.cpuRegs.GPR.r[" << rt_index << "].SD[0] >> " << sa << ";" << std::endl;

            break;
        }
        case MIPS_INS_DSLL32: {
            std::cerr << "  Operand 0 [rd]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [rt]: type=" << mips_details.operands[1].type << ", reg=" << mips_details.operands[1].reg << std::endl;
            std::cerr << "  Operand 2 [sa]: type=" << mips_details.operands[2].type << ", imm=" << mips_details.operands[2].imm << std::endl;
            // TODO: Implement DSLL32 (Doubleword Shift Left Logical + 32)
            // MIPS: dsll32 rd, rt, sa

            const auto& rd_reg = mips_details.operands[0].reg;
            const auto& rt_reg = mips_details.operands[1].reg;
            const auto& sa = mips_details.operands[2].imm;

            int rd_index = get_gpr_index(rd_reg);
            int rt_index = get_gpr_index(rt_reg);

            /*
            context.cpuRegs.GPR.r[rd_index].UD[0] = (u64)(context.cpuRegs.GPR.r[rt_index].UD[0] << context.cpuRegs.GPR.sa);
            
            */

            out_file << "context.cpuRegs.GPR.r[" << rd_index << "].UD[0] = context.cpuRegs.GPR.r[" << rt_index << "].UD[0] << (32 + " << sa << ");" << std::endl;

            break;
        }
        case MIPS_INS_DSRL32: {
            std::cerr << "  Operand 0 [rd]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [rt]: type=" << mips_details.operands[1].type << ", reg=" << mips_details.operands[1].reg << std::endl;
            std::cerr << "  Operand 2 [sa]: type=" << mips_details.operands[2].type << ", imm=" << mips_details.operands[2].imm << std::endl;
            // TODO: Implement DSRL32 (Doubleword Shift Right Logical + 32)
            // MIPS: dsrl32 rd, rt, sa

            
            const auto& rd_reg = mips_details.operands[0].reg;
            const auto& rt_reg = mips_details.operands[1].reg;
            const auto& sa = mips_details.operands[2].imm;

            int rd_index = get_gpr_index(rd_reg);
            int rt_index = get_gpr_index(rt_reg);

            /*
            context.cpuRegs.GPR.r[rd_index].UD[0] = context.cpuRegs.GPR.r[rt_index].UD[0] >> (32 + sa);
            
            */

            out_file << "context.cpuRegs.GPR.r[" << rd_index << "].UD[0] = context.cpuRegs.GPR.r[" << rt_index << "].UD[0] >> (32 + " << sa << ");" << std::endl;

            break;
        }
        case MIPS_INS_DSRA32: {
            std::cerr << "  Operand 0 [rd]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [rt]: type=" << mips_details.operands[1].type << ", reg=" << mips_details.operands[1].reg << std::endl;
            std::cerr << "  Operand 2 [sa]: type=" << mips_details.operands[2].type << ", imm=" << mips_details.operands[2].imm << std::endl;
            // TODO: Implement DSRA32 (Doubleword Shift Right Arithmetic + 32)
            // MIPS: dsra32 rd, rt, sa

            
            const auto& rd_reg = mips_details.operands[0].reg;
            const auto& rt_reg = mips_details.operands[1].reg;
            const auto& sa = mips_details.operands[2].imm;

            int rd_index = get_gpr_index(rd_reg);
            int rt_index = get_gpr_index(rt_reg);

            /*
            context.cpuRegs.GPR.r[rd_index].UD[0] = (u64)(context.cpuRegs.GPR.r[rt_index].UD[0] >> context.cpuRegs.GPR.sa);
            
            */

            out_file << "context.cpuRegs.GPR.r[" << rd_index << "].SD[0] = context.cpuRegs.GPR.r[" << rt_index << "].SD[0] >> (32 + " << sa << ");" << std::endl;


            break;
        }

        // --- REGIMM TABLE (rt field based) ---
        case MIPS_INS_BLTZ: {
            std::cerr << "  Operand 0 [rs]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [offset]: type=" << mips_details.operands[1].type << ", imm=" << mips_details.operands[1].imm << std::endl;
            // TODO: Implement BLTZ (Branch on Less Than Zero)
            // MIPS: bltz rs, offset

            const auto& rs_reg = mips_details.operands[0].reg;
            const auto& offset = mips_details.operands[1].imm;

            int rs_index = get_gpr_index(rs_reg);
            u32 target = calculate_target(*insn);

            /*
            
            if (context.cpuRegs.GPR.r[rs_index].SD[0] < 0){
                context.cpuRegs.GPR.pc = insn->address + 4 + (offset << 2);
            }
            else{
                context.cpuRegs.GPR.pc = insn->address + 8;
            }
            */
            out_file << "    if ((s64)context.cpuRegs.GPR.r[" << rs_index << "].SD[0] < 0) {" << std::endl;
            out_file << "        func_0x" << std::hex << target << "();" << std::endl;
            out_file << "        return;" << std::endl;
            out_file << "    }" << std::endl;
            break;
        }
        case MIPS_INS_BGEZ: {
            std::cerr << "  Operand 0 [rs]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [offset]: type=" << mips_details.operands[1].type << ", imm=" << mips_details.operands[1].imm << std::endl;
            // TODO: Implement BGEZ (Branch on Greater Than or Equal to Zero)
            // MIPS: bgez rs, offset
            const auto& rs_reg = mips_details.operands[0].reg;
            const auto& offset = mips_details.operands[1].imm;

            int rs_index = get_gpr_index(rs_reg);

            u32 target = calculate_target(*insn);

            /*
            
            if (context.cpuRegs.GPR.r[rs_index].SD[0] < 0){
                context.cpuRegs.GPR.pc = insn->address + 4 + (offset << 2);
            }
            else{
                context.cpuRegs.GPR.pc = insn->address + 8;
            }
            */
            out_file << "    if ((s64)context.cpuRegs.GPR.r[" << rs_index << "].SD[0] >= 0) {" << std::endl;
            out_file << "        func_0x" << std::hex << target << "();" << std::endl;
            out_file << "        return;" << std::endl;
            out_file << "    }" << std::endl;
            break;
        }
        case MIPS_INS_TGEI: {
            std::cerr << "  Operand 0 [rs]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [imm]: type=" << mips_details.operands[1].type << ", imm=" << mips_details.operands[1].imm << std::endl;
            // TODO: Implement TGEI (Trap if Greater than or Equal Immediate)
            // MIPS: tgei rs, immediate

            const auto& rs_reg = mips_details.operands[0].reg;
            const auto& imm = mips_details.operands[1].imm;

            int rs_index = get_gpr_index(rs_reg);

            /*
            if (context.cpuRegs.GPR.r[rs_index] >= context.cpuRegs.GPR.r[rt_index]){
                // TODO HANDLE TRAP
            }
            */

            out_file << "if (context.cpuRegs.GPR.r[" << rs_index <<"].SD[0] >= " << imm << "){ "
            out_file << "// TODO HANDLE TRAP" << std::endl;
            out_file << "}" << std::endl;

            break;
        }
        case MIPS_INS_TGEIU: {
            std::cerr << "  Operand 0 [rs]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [imm]: type=" << mips_details.operands[1].type << ", imm=" << mips_details.operands[1].imm << std::endl;
            // TODO: Implement TGEIU (Trap if Greater than or Equal Immediate Unsigned)
            // MIPS: tgeiu rs, immediate

            const auto& rs_reg = mips_details.operands[0].reg;
            const auto& imm = mips_details.operands[1].imm;

            int rs_index = get_gpr_index(rs_reg);

            /*
            if (context.cpuRegs.GPR.r[rs_index] >= context.cpuRegs.GPR.r[rt_index]){
                // TODO HANDLE TRAP
            }
            */

            out_file << "if (context.cpuRegs.GPR.r[" << rs_index <<"].UD[0] >= " << imm << "){ "
            out_file << "// TODO HANDLE TRAP" << std::endl;
            out_file << "}" << std::endl;

            break;
        }
        case MIPS_INS_TLTI: {
            std::cerr << "  Operand 0 [rs]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [imm]: type=" << mips_details.operands[1].type << ", imm=" << mips_details.operands[1].imm << std::endl;
            // TODO: Implement TLTI (Trap if Less Than Immediate)
            // MIPS: tlti rs, immediate

            const auto& rs_reg = mips_details.operands[0].reg;
            const auto& imm = mips_details.operands[1].imm;

            int rs_index = get_gpr_index(rs_reg);

            /*
            if (context.cpuRegs.GPR.r[rs_index] < context.cpuRegs.GPR.r[rt_index]){
                // TODO HANDLE TRAP
            }
            */

            out_file << "if (context.cpuRegs.GPR.r[" << rs_index <<"].SD[0] < " << imm << "){ "
            out_file << "// TODO HANDLE TRAP" << std::endl;
            out_file << "}" << std::endl;

            break;
        }
        case MIPS_INS_TLTIU: {
            std::cerr << "  Operand 0 [rs]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [imm]: type=" << mips_details.operands[1].type << ", imm=" << mips_details.operands[1].imm << std::endl;
            // TODO: Implement TLTIU (Trap if Less Than Immediate Unsigned)
            // MIPS: tltiu rs, immediate

            const auto& rs_reg = mips_details.operands[0].reg;
            const auto& imm = mips_details.operands[1].imm;

            int rs_index = get_gpr_index(rs_reg);

            /*
            if (context.cpuRegs.GPR.r[rs_index] < context.cpuRegs.GPR.r[rt_index]){
                // TODO HANDLE TRAP
            }
            */

            out_file << "if (context.cpuRegs.GPR.r[" << rs_index <<"].UD[0] < " << imm << "){ "
            out_file << "// TODO HANDLE TRAP" << std::endl;
            out_file << "}" << std::endl;

            break;
        }
        case MIPS_INS_TEQI: {
            std::cerr << "  Operand 0 [rs]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [imm]: type=" << mips_details.operands[1].type << ", imm=" << mips_details.operands[1].imm << std::endl;
            // TODO: Implement TEQI (Trap if Equal Immediate)
            // MIPS: teqi rs, immediate

            const auto& rs_reg = mips_details.operands[0].reg;
            const auto& imm = mips_details.operands[1].imm;

            int rs_index = get_gpr_index(rs_reg);

            /*
            if (context.cpuRegs.GPR.r[rs_index] >= context.cpuRegs.GPR.r[rt_index]){
                // TODO HANDLE TRAP
            }
            */

            out_file << "if (context.cpuRegs.GPR.r[" << rs_index <<"].SD[0] = " << imm << "){ "
            out_file << "// TODO HANDLE TRAP" << std::endl;
            out_file << "}" << std::endl;

            break;
        }
        case MIPS_INS_TNEI: {
            std::cerr << "  Operand 0 [rs]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [imm]: type=" << mips_details.operands[1].type << ", imm=" << mips_details.operands[1].imm << std::endl;
            // TODO: Implement TNEI (Trap if Not Equal Immediate)
            // MIPS: tnei rs, immediate

            const auto& rs_reg = mips_details.operands[0].reg;
            const auto& imm = mips_details.operands[1].imm;

            int rs_index = get_gpr_index(rs_reg);

            /*
            if (context.cpuRegs.GPR.r[rs_index] >= context.cpuRegs.GPR.r[rt_index]){
                // TODO HANDLE TRAP
            }
            */

            out_file << "if (context.cpuRegs.GPR.r[" << rs_index <<"].SD[0] != " << imm << "){ "
            out_file << "// TODO HANDLE TRAP" << std::endl;
            out_file << "}" << std::endl;
            break;
        }
        /*
        case MIPS_INS_MTSAB: {
            // TODO: Implement MTSAB (Move To SA/Byte)
            // MIPS: mtsab rs, immediate
            break;
        }
        case MIPS_INS_MTSAH: {
            // TODO: Implement MTSAH (Move To SA/Halfword)
            // MIPS: mtsah rs, immediate
            break;
        }
        */

        // --- Normal Opcode Table ---
        case MIPS_INS_SLTIU: {
            std::cerr << "  Operand 0 [rt]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [rs]: type=" << mips_details.operands[1].type << ", reg=" << mips_details.operands[1].reg << std::endl;
            std::cerr << "  Operand 2 [imm]: type=" << mips_details.operands[2].type << ", imm=" << mips_details.operands[2].imm << std::endl;
            // TODO: Implement SLTIU (Set on Less Than Immediate Unsigned)
            // MIPS: sltiu rt, rs, immediate
            const auto& rt_reg = mips_details.operands[0].reg;
            const auto& rs_reg = mips_details.operands[1].reg;
            const auto& imm = mips_details.operands[2].imm;

            int rt_index = get_gpr_index(rt_reg);
            int rs_index = get_gpr_index(rs_reg);

            out_file << "context.cpuRegs.GPR.r[" << rt_index << "].UD[0] = (u64)((u32)context.cpuRegs.GPR.r[" << rs_index << "].UD[0] < (u32)" << imm << " ? 1 : 0);"<< std::endl;
            break;
        }
        case MIPS_INS_ANDI: {
            // TODO: Implement ANDI (AND Immediate)
            // MIPS: andi rt, rs, immediate

            const auto& rd_reg = mips_details.operands[0].reg;
            const auto& rs_reg = mips_details.operands[1].reg;
            const auto& imm = mips_details.operands[2].imm;

            int rd_index = get_gpr_index(rd_reg);
            int rs_index = get_gpr_index(rs_reg);

 
             /*
             {
                 u64 rs = context.cpuRegs.GPR.r[rs_index].UD[0];
                 u64 rt = context.cpuRegs.GPR.r[rt_index].UD[0];
                 context.cpuRegs.GPT.r[rd_index].UD[0] = rs & rt;
             
             }
             */
            out_file << "{"
            out_file << "   u64 rs_val = context.cpuRegs.GPR.r["<< rs_index <<"].UD[0];" << std::endl;
            out_file << "   u64 rt_val = " << imm << ";" << std::endl;
            out_file << "   context.cpuRegs.GPR.r[" << rd_index << "].UD[0] = rs_val & rt_val;" << std::endl;
            out_file << "}" << std::endl;
            break;
        }
        case MIPS_INS_XORI: {
            // TODO: Implement XORI (XOR Immediate)
            // MIPS: xori rt, rs, immediate

            const auto& rd_reg = mips_details.operands[0].reg;
            const auto& rs_reg = mips_details.operands[1].reg;
            const auto& imm = mips_details.operands[2].imm;

            int rd_index = get_gpr_index(rd_reg);
            int rs_index = get_gpr_index(rs_reg);

            /*
            {
                u64 rs = context.cpuRegs.GPR.r[rs_index].UD[0];
                u64 rt = context.cpuRegs.GPR.r[rt_index].UD[0];
                context.cpuRegs.GPT.r[rd_index].UD[0] = rs ^ rt;
            
            }
            */
           out_file << "{"
           out_file << "   u64 rs_val = context.cpuRegs.GPR.r["<< rs_index <<"].UD[0];" << std::endl;
           out_file << "   u64 rt_val = " << imm << ";" << std::endl;
           out_file << "   context.cpuRegs.GPR.r[" << rd_index << "].UD[0] = rs_val ^ rt_val;" << std::endl;
           out_file << "}" << std::endl;
           break;
        }
        case MIPS_INS_DADDI: {
            // TODO: Implement DADDI (Doubleword Add Immediate)
            // MIPS: daddi rt, rs, immediate

            const auto& rt_reg = mips_details.operands[0].reg;
            const auto& rs_reg = mips_details.operands[1].reg;
            const auto& imm = mips_details.operands[2].imm;

            int rt_index = get_gpr_index(rt_reg);
            int rs_index = get_gpr_index(rs_reg);

            /*
            u64 rs = (u64)context.cpuRegs.GPR.r[rs_index].UD[0];
            u64 rt = (u64)context.cpuRegs.GPR.r[rt_index].UD[0];


            u64 sum = (u64)rs + (u64)rt;

            0000 0000 0000 0000 0000 0000 0000 0000 | 0000 0000 0000 0000 0000 0000 0000 0000

            if (rs > sum || rt > sum) {

                handle_overflow();

            }
            else{
                context.cpuRegs.GPR.r[rd_index].UD[0] = sum;
            }
            */

            out_file << "{"
            out_file << "    s64 rs = context.cpuRegs.GPR.r[" << rs_index << "].SD[0];" << std::endl;
            out_file << "    s64 rt = " << imm << ";" << std::endl;
            out_file << "    s64 sum = rs + rt;" << std::endl;
            out_file << "    context.cpuRegs.GPR.r[" << rt_index << "].SD[0] = sum;" << std::endl;
            out_file << "}" << std::endl;
            break;
        }
        case MIPS_INS_DADDIU: {
            // TODO: Implement DADDIU (Doubleword Add Immediate Unsigned)
            // MIPS: daddiu rt, rs, immediate
            const auto& rt_reg = mips_details.operands[0].reg;
            const auto& rs_reg = mips_details.operands[1].reg;
            const auto& imm = mips_details.operands[2].imm;

            int rt_index = get_gpr_index(rt_reg);
            int rs_index = get_gpr_index(rs_reg);

            /*
            u64 rs = (u64)context.cpuRegs.GPR.r[rs_index].UD[0];
            u64 rt = (u64)context.cpuRegs.GPR.r[rt_index].UD[0];


            u64 sum = (u64)rs + (u64)rt;

            0000 0000 0000 0000 0000 0000 0000 0000 | 0000 0000 0000 0000 0000 0000 0000 0000

            if (rs > sum || rt > sum) {

                handle_overflow();

            }
            else{
                context.cpuRegs.GPR.r[rd_index].UD[0] = sum;
            }
            */

            out_file << "{"
            out_file << "    u64 rs = context.cpuRegs.GPR.r[" << rs_index << "].UD[0];" << std::endl;
            out_file << "    u64 rt = " << imm << ";" << std::endl;
            out_file << "    u64 sum = rs + rt;" << std::endl;
            out_file << "    context.cpuRegs.GPR.r[" << rt_index << "].UD[0] = sum;" << std::endl;
            out_file << "}" << std::endl;
            break;
        }
        case MIPS_INS_ADDI: {
            // TODO: Implement ADDI (Add Immediate with overflow trap)
            // For now, can be implemented same as ADDIU.
            // MIPS: addi rt, rs, immediate

            const auto& rs_reg = mips_details.operands[1].reg;
            const auto& rt_reg = mips_details.operands[0].reg;
            const auto& imm = mips_details.operands[2].imm;

            int rs_index = get_gpr_index(rs_reg);
            int rt_index = get_gpr_index(rt_reg);

            /*
            s32 op1 = (s32)context.cpuRegs.GPR.r[rs_index].SD[0];
            s32 op2 = (s32)context.cpuRegs.GPR.r[rt_index].SD[0];


            s64 sum = (s64)op1 + (s64)op2;

            0000 0000 0000 0000 0000 0000 0000 0000 | 0000 0000 0000 0000 0000 0000 0000 0000

            if (sum != (s64)(s32)sum){

                handle_overflow();

            }
            else{
                context.cpuRegs.GPR.r[rd_index].SD[0] = sum;
            }

            */

            out_file << "s32 op1 = (s32)context.cpuRegs.GPR.r[" << rs_index << "].SD[0];" << std::endl;
            out_file << "s32 op2 = (s32)" << imm << ";" << std::endl;
            out_file << "s64 sum = (s64)op1 + (s64)op2;" << std::endl;
            out_file << "if (sum != (s64)(s32)sum){ " << std::endl;
            out_file << "  handle_overflow();" << std::endl;
            out_file << "}" << std::endl;
            out_file << "else{"
            out_file << "  context.cpuRegs.GPR.r[" << rt_index << "].SD[0] = sum;" << std::endl;
            out_file << "}" << std::endl;
            break;
        }
        case MIPS_INS_LDL: {

            const auto& dest_reg_capstone = mips_details.operands[0].reg;
            int dest_index = get_gpr_index(dest_reg_capstone);

            // Operand 1 is the memory structure
            // It contains the base register 'base' and the offset 'offset'
            const auto& base_reg_capstone = mips_details.operands[1].mem.base;
            const auto& offset = mips_details.operands[1].mem.disp;
            int base_index = get_gpr_index(base_reg_capstone);
            // This is the magic merge operation, taken from PCSX2's logic.
            // It combines the shifted memory data with the existing register data using masks.
            // LDL_MASK preserves the lower bytes of the register.
            // LDL_SHIFT moves the relevant bytes from memory to the most-significant side.
            // Note: You would need to define the LDL_MASK and LDL_SHIFT arrays as seen in R5900OpcodeImpl.cpp
            // For this explanation, we'll represent it conceptually.
        
            out_file << "{"
            out_file << "    u32 addr = (u32)context.cpuRegs.GPR.r[" << base_index << "].UD[0] + " << offset << ";" << std::endl;
            out_file << "    u32 shift = addr & 7;" << std::endl;
            out_file << "    u64 mem = ReadMemory64(addr & ~7);" << std::endl;
            out_file << "    u64 mask = 0x00FFFFFFFFFFFFFF >> (shift * 8);" << std::endl;
            out_file << "    u64 data = mem << (56 - (shift * 8));" << std::endl;
            out_file << "    context.cpuRegs.GPR.r[" << dest_index << "].UD[0] = (context.cpuRegs.GPR.r[" << dest_index << "].UD[0] & mask) | data;" << std::endl;
            out_file << "}" << std::endl;
            break;
        }
        case MIPS_INS_LDR: {
            // TODO: Implement LDR (Load Doubleword Right)
            // This is for unaligned loads. Complex. Can be deferred.
            const auto& dest_reg_capstone = mips_details.operands[0].reg;
            int dest_index = get_gpr_index(dest_reg_capstone);
                    
            const auto& base_reg_capstone = mips_details.operands[1].mem.base;
            const auto& offset = mips_details.operands[1].mem.disp;
            int base_index = get_gpr_index(base_reg_capstone);
                    
            // Generate the C++ code for the unaligned load logic
            out_file << "{"
            out_file << "    u32 addr = (u32)context.cpuRegs.GPR.r[" << base_index << "].UD[0] + " << offset << ";" << std::endl;
            // 'shift' is how many bytes we are into the 8-byte aligned block
            out_file << "    u32 shift = addr & 7;" << std::endl;
            // Read the full 8-byte aligned block that contains our address
            out_file << "    u64 mem = ReadMemory64(addr & ~7);" << std::endl;
            // Create a mask to preserve the upper bytes of the destination register
            out_file << "    u64 mask = 0xFFFFFFFFFFFFFFFF << ((shift + 1) * 8);" << std::endl;
            // Shift the data from memory to align it to the right side of the register
            out_file << "    u64 data = mem >> (56 - (shift * 8));" << std::endl;
            // Merge the new data with the preserved part of the destination register
            out_file << "    context.cpuRegs.GPR.r[" << dest_index << "].UD[0] = (context.cpuRegs.GPR.r[" << dest_index << "].UD[0] & mask) | data;" << std::endl;
            out_file << "}" << std::endl;
            break;
        }

        // TODO: Implement LWL (Load Word Left)
        // This is for unaligned loads. Complex. Can be deferred.
        case MIPS_INS_LWL: {
            // TODO: Implement LWL (Load Word Left)
            // This is for unaligned loads. Complex. Can be deferred.
            const auto& dest_reg_capstone = mips_details.operands[0].reg;
            int dest_index = get_gpr_index(dest_reg_capstone);

            const auto& base_reg_capstone = mips_details.operands[1].mem.base;
            const auto& offset = mips_details.operands[1].mem.disp;
            int base_index = get_gpr_index(base_reg_capstone);

            // Generate the C++ code for the unaligned load logic
            out_file << "{"
            out_file << "    u32 addr = (u32)context.cpuRegs.GPR.r[" << base_index << "].UD[0] + " << offset << ";" << std::endl;
            // 'shift' is how many bytes we are into the 4-byte aligned block (0-3)
            out_file << "    u32 shift = addr & 3;" << std::endl;
            // Read the full 4-byte aligned word from memory
            out_file << "    u32 mem = ReadMemory32(addr & ~3);" << std::endl;
            // Create a mask to preserve the lower bytes of the destination register
            out_file << "    u32 mask = 0x00FFFFFF >> (shift * 8);" << std::endl;
            // Shift the data from memory to align it to the left side of the register
            out_file << "    u32 data = mem << (24 - (shift * 8));" << std::endl;
            // Merge the new data with the preserved part of the destination register
            // The result is then sign-extended into the 64-bit GPR.
            out_file << "    u32 result = (context.cpuRegs.GPR.r[" << dest_index << "].UL[0] & mask) | data;" << std::endl;
            out_file << "    context.cpuRegs.GPR.r[" << dest_index << "].SD[0] = (s64)(s32)result;" << std::endl;
            out_file << "}" << std::endl;
            break;
        }
        case MIPS_INS_LWR: {
            // TODO: Implement LWR (Load Word Right)
            // This is for unaligned loads. Complex. Can be deferred.
            const auto& dest_reg_capstone = mips_details.operands[0].reg;
            int dest_index = get_gpr_index(dest_reg_capstone);

            const auto& base_reg_capstone = mips_details.operands[1].mem.base;
            const auto& offset = mips_details.operands[1].mem.disp;
            int base_index = get_gpr_index(base_reg_capstone);

            // Generate the C++ code for the unaligned load logic
            out_file << "{"
            out_file << "    u32 addr = (u32)context.cpuRegs.GPR.r[" << base_index << "].UD[0] + " << offset << ";" << std::endl;
            // 'shift' is how many bytes we are into the 4-byte aligned block
            out_file << "    u32 shift = addr & 3;" << std::endl;
            // Read the full 4-byte aligned word from memory
            out_file << "    u32 mem = ReadMemory32(addr & ~3);" << std::endl;
            // Create a mask to preserve the upper bytes of the destination register
            out_file << "    u32 mask = 0xFFFFFF00 << (24 - (shift * 8));" << std::endl;
            // Shift the data from memory to align it to the right side of the register
            out_file << "    u32 data = mem >> (shift * 8);" << std::endl;
            // Merge the new data with the preserved part of the destination register
            // The result is then sign-extended into the 64-bit GPR.
            out_file << "    u32 result = (context.cpuRegs.GPR.r[" << dest_index << "].UL[0] & mask) | data;" << std::endl;
            out_file << "    context.cpuRegs.GPR.r[" << dest_index << "].SD[0] = (s64)(s32)result;" << std::endl;
            out_file << "}" << std::endl;
            break;
        }
        case MIPS_INS_LWU: {
            // TODO: Implement LWU (Load Word Unsigned)
            // MIPS: lwu rt, offset(base)
            const auto& dest_capstone = mips_details.operands[0].reg;
            const auto& base_capstone = mips_details.operands[1].mem.base;
            const auto& offset = mips_details.operands[1].mem.disp;
            int dest_index = get_gpr_index(dest_capstone);
            int base_index = get_gpr_index(base_capstone);

            out_file << "{"
            out_file << "    u32 address = context.cpuRegs.GPR.r[" << base_index << "].UD[0] + " << offset << ";" << std::endl;
            // LWU zero-extends the 32-bit memory value into the 64-bit register.
            // Casting the u32 result of ReadMemory32 to u64 achieves this.
            out_file << "    context.cpuRegs.GPR.r[" << dest_index << "].UD[0] = (u64)ReadMemory32(address);" << std::endl;
            out_file << "}" << std::endl;
            break;
        }
        case MIPS_INS_SWL: {
            // TODO: Implement SWL (Store Word Left)
            // This is for unaligned stores. Complex. Can be deferred.
            const auto& rt_capstone = mips_details.operands[0].reg;
            const auto& base_capstone = mips_details.operands[1].mem.base;
            const auto& offset = mips_details.operands[1].mem.disp;
            int rt_index = get_gpr_index(rt_capstone);
            int base_index = get_gpr_index(base_capstone);

            out_file << "{"
            out_file << "    u32 address = context.cpuRegs.GPR.r[" << base_index << "].UL[0] + " << offset << ";" << std::endl;
            out_file << "    u32 shift = address & 3;" << std::endl;
            out_file << "    u32 aligned_address = address & ~3;" << std::endl;
            out_file << "    u32 mem = ReadMemory32(aligned_address);" << std::endl;
            out_file << "    u32 reg_val = context.cpuRegs.GPR.r[" << rt_index << "].UL[0];" << std::endl;
            out_file << "    switch (shift) {" << std::endl;
            out_file << "        case 0: WriteMemory32(aligned_address, (mem & 0xFFFFFF00) | (reg_val >> 24)); break;" << std::endl;
            out_file << "        case 1: WriteMemory32(aligned_address, (mem & 0xFFFF0000) | (reg_val >> 16)); break;" << std::endl;
            out_file << "        case 2: WriteMemory32(aligned_address, (mem & 0xFF000000) | (reg_val >> 8)); break;" << std::endl;
            out_file << "        case 3: WriteMemory32(aligned_address, reg_val); break;" << std::endl;
            out_file << "    }" << std::endl;
            out_file << "}" << std::endl;
            break;
        }
        case MIPS_INS_SWR: {
            // TODO: Implement SWR (Store Word Right)
            // This is for unaligned stores. Complex. Can be deferred.

            const auto& rt_capstone = mips_details.operands[0].reg;
            const auto& base_capstone = mips_details.operands[1].mem.base;
            const auto& offset = mips_details.operands[1].mem.disp;
            int rt_index = get_gpr_index(rt_capstone);
            int base_index = get_gpr_index(base_capstone);
                    
            out_file << "{"
            out_file << "    u32 address = context.cpuRegs.GPR.r[" << base_index << "].UL[0] + " << offset << ";" << std::endl;
            out_file << "    u32 shift = address & 3;" << std::endl;
            out_file << "    u32 aligned_address = address & ~3;" << std::endl;
            out_file << "    u32 mem = ReadMemory32(aligned_address);" << std::endl;
            out_file << "    u32 reg_val = context.cpuRegs.GPR.r[" << rt_index << "].UL[0];" << std::endl;
            out_file << "    switch (shift) {" << std::endl;
            out_file << "        case 0: WriteMemory32(aligned_address, reg_val); break;" << std::endl;
            out_file << "        case 1: WriteMemory32(aligned_address, (mem & 0x000000FF) | (reg_val << 8)); break;" << std::endl;
            out_file << "        case 2: WriteMemory32(aligned_address, (mem & 0x0000FFFF) | (reg_val << 16)); break;" << std::endl;
            out_file << "        case 3: WriteMemory32(aligned_address, (mem & 0x00FFFFFF) | (reg_val << 24)); break;" << std::endl;
            out_file << "    }" << std::endl;
            out_file << "}" << std::endl;
            break;
        }
        case MIPS_INS_SDL: {
            // TODO: Implement SDL (Store Doubleword Left)
            // This is for unaligned stores. Complex. Can be deferred.
            const auto& rt_capstone = mips_details.operands[0].reg;
            const auto& base_capstone = mips_details.operands[1].mem.base;
            const auto& offset = mips_details.operands[1].mem.disp;
            int rt_index = get_gpr_index(rt_capstone);
            int base_index = get_gpr_index(base_capstone);

            out_file << "{"
            out_file << "    u64 address = context.cpuRegs.GPR.r[" << base_index << "].UD[0] + " << offset << ";" << std::endl;
            out_file << "    u64 shift = address & 7;" << std::endl;
            out_file << "    u64 aligned_address = address & ~7;" << std::endl;
            out_file << "    u64 mem = ReadMemory64(aligned_address);" << std::endl;
            out_file << "    u64 reg_val = context.cpuRegs.GPR.r[" << rt_index << "].UD[0];" << std::endl;
            out_file << "    switch (shift) {" << std::endl;
            out_file << "        case 0: WriteMemory64(aligned_address, (mem & 0xFFFFFFFFFFFFFF00ULL) | (reg_val >> 56)); break;" << std::endl;
            out_file << "        case 1: WriteMemory64(aligned_address, (mem & 0xFFFFFFFFFFFF0000ULL) | (reg_val >> 48)); break;" << std::endl;
            out_file << "        case 2: WriteMemory64(aligned_address, (mem & 0xFFFFFFFFFF000000ULL) | (reg_val >> 40)); break;" << std::endl;
            out_file << "        case 3: WriteMemory64(aligned_address, (mem & 0xFFFFFFFF00000000ULL) | (reg_val >> 32)); break;" << std::endl;
            out_file << "        case 4: WriteMemory64(aligned_address, (mem & 0xFFFFFF0000000000ULL) | (reg_val >> 24)); break;" << std::endl;
            out_file << "        case 5: WriteMemory64(aligned_address, (mem & 0xFFFF000000000000ULL) | (reg_val >> 16)); break;" << std::endl;
            out_file << "        case 6: WriteMemory64(aligned_address, (mem & 0xFF00000000000000ULL) | (reg_val >> 8)); break;" << std::endl;
            out_file << "        case 7: WriteMemory64(aligned_address, reg_val); break;" << std::endl;
            out_file << "    }" << std::endl;
            out_file << "}" << std::endl;
            break;
        }
        case MIPS_INS_SDR: {
            // TODO: Implement SDR (Store Doubleword Right)
            // This is for unaligned stores. Complex. Can be deferred.

            const auto& rt_capstone = mips_details.operands[0].reg;
            const auto& base_capstone = mips_details.operands[1].mem.base;
            const auto& offset = mips_details.operands[1].mem.disp;
            int rt_index = get_gpr_index(rt_capstone);
            int base_index = get_gpr_index(base_capstone);
                    
            out_file << "{"
            out_file << "    u64 address = context.cpuRegs.GPR.r[" << base_index << "].UD[0] + " << offset << ";" << std::endl;
            out_file << "    u64 shift = address & 7;" << std::endl;
            out_file << "    u64 aligned_address = address & ~7;" << std::endl;
            out_file << "    u64 mem = ReadMemory64(aligned_address);" << std::endl;
            out_file << "    u64 reg_val = context.cpuRegs.GPR.r[" << rt_index << "].UD[0];" << std::endl;
            out_file << "    switch (shift) {" << std::endl;
            out_file << "        case 0: WriteMemory64(aligned_address, reg_val); break;" << std::endl;
            out_file << "        case 1: WriteMemory64(aligned_address, (mem & 0xFF00000000000000ULL) | (reg_val << 8)); break;" << std::endl;
            out_file << "        case 2: WriteMemory64(aligned_address, (mem & 0xFFFF000000000000ULL) | (reg_val << 16)); break;" << std::endl;
            out_file << "        case 3: WriteMemory64(aligned_address, (mem & 0xFFFFFF0000000000ULL) | (reg_val << 24)); break;" << std::endl;
            out_file << "        case 4: WriteMemory64(aligned_address, (mem & 0xFFFFFFFF00000000ULL) | (reg_val << 32)); break;" << std::endl;
            out_file << "        case 5: WriteMemory64(aligned_address, (mem & 0xFFFF000000000000ULL) | (reg_val << 40)); break;" << std::endl;
            out_file << "        case 6: WriteMemory64(aligned_address, (mem & 0xFF00000000000000ULL) | (reg_val << 48)); break;" << std::endl;
            out_file << "        case 7: WriteMemory64(aligned_address, (mem & 0xFFFFFFFFFFFFFF00ULL) | (reg_val << 56)); break;" << std::endl;
            out_file << "    }" << std::endl;
            out_file << "}" << std::endl;
            break;
        }
        case MIPS_INS_CACHE: {
            // TODO: Implement CACHE (Cache Operation)
            // 
            out_file << "// CACHE instruction (NOP)" << std::endl;
            break;
        }
        case MIPS_INS_PREF: {
            // TODO: Implement PREF (Prefetch)
            // This is a memory hint. 

            out_file << "// PREF (Prefetch) instruction (NOP)" << std::endl;
            break;
        }
        case MIPS_INS_LD: {
            // TODO: Implement LD (Load Doubleword)
            // MIPS: ld rt, offset(base)

            const auto& rt_capstone = mips_details.operands[0].reg;
            const auto& base_capstone = mips_details.operands[1].mem.base;
            const auto& offset = mips_details.operands[1].mem.disp;
            int rt_index = get_gpr_index(rt_capstone);
            int base_index = get_gpr_index(base_capstone);

            out_file << "{"
            out_file << "    u64 address = context.cpuRegs.GPR.r[" << base_index << "].UD[0] + " << offset << ";" << std::endl;
            // LD requires the address to be 8-byte aligned.
            out_file << "    if (address & 7) {" << std::endl;
            out_file << "        std::cerr << \"FATAL ERROR: Unaligned memory access for LD at address: 0x\" << std::hex << address << std::endl;" << std::endl;
            out_file << "        exit(1);" << std::endl;
            out_file << "    }" << std::endl;
            // LD is a direct 64-bit load. No sign/zero extension is needed.
            out_file << "    context.cpuRegs.GPR.r[" << rt_index << "].UD[0] = ReadMemory64(address);" << std::endl;
            out_file << "}" << std::endl;
            break;
        }
        case MIPS_INS_SD: {
            // TODO: Implement SD (Store Doubleword)
            // MIPS: sd rt, offset(base)
            const auto& rt_capstone = mips_details.operands[0].reg;
            const auto& base_capstone = mips_details.operands[1].mem.base;
            const auto& offset = mips_details.operands[1].mem.disp;
            int rt_index = get_gpr_index(rt_capstone);
            int base_index = get_gpr_index(base_capstone);

            out_file << "{"
            out_file << "    u64 address = context.cpuRegs.GPR.r[" << base_index << "].UD[0] + " << offset << ";" << std::endl;
            // SD requires the address to be 8-byte aligned.
            out_file << "    if (address & 7) {" << std::endl;
            out_file << "        std::cerr << \"FATAL ERROR: Unaligned memory access for SD at address: 0x\" << std::hex << address << std::endl;" << std::endl;
            out_file << "        exit(1);" << std::endl;
            out_file << "    }" << std::endl;
            // SD is a direct 64-bit store. No truncation is needed.
            out_file << "    WriteMemory64(address, context.cpuRegs.GPR.r[" << rt_index << "].UD[0]);" << std::endl;
            out_file << "}" << std::endl;
            break;
        }

        default:            out_file << "// Unhandled instruction: " << insn->mnemonic << std::endl;
            break;
    }

    // Default case: we consumed one instruction.
}

/* 
Input: All machine code instructions from the PS2 file
Output: Set of blocks, each block containing all the instructions tied to that block
1. Collects all function entries
2. Each function entry is the beginning of a basic block
3. After block is finished being built insert into block_entries
*/
std::set<uint64_t> collect_function_entries(cs_insn* insns, size_t count){
        // Collect entry points
    std::set<uint64_t> entries;
    entries.emplace(insns[0].address);
    
    for(size_t i = 0; i < count; i++){
        if(is_direct_function_call(insns[i])){
            entries.emplace(insns[i].address);
        }
        if(is_return(insns[i])){
            if(i + 2 < count){
                entries.emplace(insns[i+2].address);
            }
        }
    }
    std::cout << "// Found " << entries.size() << " unique entry points." << std::endl;
    for (u64 entry : entries) {
        std::cout << "// ENTRY: " << entry << " (0x" << std::hex << entry << ")" << std::dec << std::endl;
    }
    return entries;
}

std::vector<basic_block> collect_basic_blocks(cs_insn* insns, size_t count){
    std::vector<basic_block> block_entries;

    if (count == 0){
        return block_entries;
    }

    // Collect entry points
    std::set<u64> entries = collect_function_entries(insns, count);
    std::cout << "// Found " << entries.size() << " unique entry points." << std::endl; 

    //Map entries to index
    // Build blocks

    /*
        For each of these entries start adding the instructions till the address of the instruction exists in the entries that means this is a new function
    */
    //Map entries to index
    // Build blocks

    /*
        For each of these entries start adding the instructions till the address of the instruction exists in the entries that means this is a new function
    */

    for(u64 start_addr : entries){
        basic_block current_block;
        size_t current_idx = -1;
        for(int i = 0; i < count; i++){
            if(insns[i].address == start_addr){
                current_idx = i;
                break;
            }
        }

        if (current_idx == -1){continue;}
        current_block.start_address = start_addr;


        while (current_idx < count){
            current_block.instructions.push_back(&insns[current_idx]);
            current_block.end_address = insns[current_idx].address;

            if (is_control_flow_instruction(insns[current_idx])) {
                // Include the delay slot instruction
                if (current_idx + 1 < count) {
                    current_block.instructions.push_back(&insns[current_idx + 1]);
                    current_block.end_address = insns[current_idx + 1].address;
                    current_idx++;
                }
                break; // End the block after a control flow instruction and its delay slot
            }
        
            // Check if the NEXT instruction is an entry point
            if ((current_idx + 1 < count) && entries.count(insns[current_idx + 1].address)) {
                break; // End the block before the next entry point
            }
        
            current_idx++;
        }
        block_entries.emplace_back(current_block);
    }
    std::cout << "// Successfully created " << block_entries.size() << " basic blocks." << std::endl;
    std::sort(block_entries.begin(), block_entries.end(), 
        [](const basic_block& a, const basic_block& b) {
            return a.start_address < b.start_address;
        });
    return block_entries;
}

void generate_functions_from_block(const std::vector<basic_block>& blocks, std::ofstream& out_file){
    /*
        So this would be called after the function entries are collected
        1. Create function name based on the entry address -> void function0x1234()
        2. Then we translate line by line of the current basic block instruction using -> Translate block
        3. What do we do with the return address? Do we jump into that function at that address?
    */
    for(const auto& block : blocks){

        out_file << "void func_" << std::hex << block.start_address << "(){ "<<std::endl;


        for(int i = 0; i < block.instructions.size() - 1; ++i){

            if(is_branch_likely(*block.instructions[i])){
                if(i + 1 >= block.instructions.size()){
                    out_file << " // ERROR: Branch-likely at end of block" << std::endl;
                    return; 
                }
                translate_likely_instructions(out_file, block.instructions[i], block.instructions[i+1]);
                i++;
            }
            else{
                translate_instruction_block(out_file, block.instructions[i]);
            }
        }
        out_file << "}" << std::endl << std::endl;
    }
}


u32 calculate_target(cs_insn& insn){
    u32 res = 0;
    cs_mips& mips_details = insn.detail->mips;

    switch (insn.id){
        // PC-relative branches
        case MIPS_INS_BEQ:
        case MIPS_INS_BNE:
        case MIPS_INS_BGTZ:
        case MIPS_INS_BLEZ:
        case MIPS_INS_BLTZ:
        case MIPS_INS_BGEZ:
        case MIPS_INS_BLTZAL:
        case MIPS_INS_BGEZAL:
        case MIPS_INS_BEQL:
        case MIPS_INS_BNEL:
        case MIPS_INS_BLEZL:
        case MIPS_INS_BGTZL:
            // For these, the immediate is the offset, already sign-extended by Capstone
            // and needs to be multiplied by 4 (shifted left by 2)
            res = (insn.address + 4) + (mips_details.operands[mips_details.op_count - 1].imm << 2);
            break;
        // Absolute jumps
        case MIPS_INS_J:
        case MIPS_INS_JAL:
            // Combine upper 4 bits of PC with 26-bit target (shifted by 2)
            res = (insn.address & 0xF0000000) | (mips_details.operands[0].imm << 2);
            break;
        default:
            // This case should ideally not be reached if called only for direct branches/jumps
            std::cerr << "ERROR: calculate_target called for non-direct branch/jump: " << insn.mnemonic << std::endl;
            break;
    }
    return res;
}

bool is_direct_branch(cs_insn& insn){
    switch (insn.id){
        case MIPS_INS_BEQ: {
            return true;
        }
        case MIPS_INS_BNE: {
            return true;
        }
        case MIPS_INS_BGTZ: {
            return true;
        }
        case MIPS_INS_BLEZ: {
            return true;
        }
        case MIPS_INS_BLTZ: {
            return true;
        }
        case MIPS_INS_BGEZ: {
            return true;
        }
        case MIPS_INS_BLTZAL: {
            return true;
        }
        case MIPS_INS_BGEZAL: {
            return true;
        }
        case MIPS_INS_BEQL: {
            return true;
        }
        case MIPS_INS_BNEL: {
            return true;
        }
        case MIPS_INS_BLEZL: {
            return true;
        }
        case MIPS_INS_BGTZL: {
            return true;
        }
        

    }
    return false;
}

bool is_branch_likely(cs_insn& insn) {
    switch (insn.id) {
        // Standard "Likely" Branches
        case MIPS_INS_BEQL:
        case MIPS_INS_BNEL:
        case MIPS_INS_BLEZL:
        case MIPS_INS_BGTZL:
        case MIPS_INS_BLTZL:
        case MIPS_INS_BGEZL:
        // "And Link, Likely" Branches
        case MIPS_INS_BLTZALL: // Note: The likely version is "ALL"
        case MIPS_INS_BGEZALL: // Note: The likely version is "ALL"
            return true;

        // If the instruction ID is none of the above...
        default:
            return false;
    }
}

void translate_likely_instructions(std::ofstream& out_file, cs_insn* insn, cs_insn* delay_slot_insn){
    cs_mips& mips_details = insn->detail->mips;
    switch(insn->id){
        case MIPS_INS_BEQL: {
            // TODO: Implement BEQL (Branch on Equal Likely)
            // Branch likely instructions are tricky. For now, can treat as normal branch.
    
            const auto& rs_reg = insn->detail->mips.operands[0].reg;
            const auto& rt_reg = insn->detail->mips.operands[1].reg;
            const auto& offset = mips_details.operands[2].imm;
    
            int rs_index = get_gpr_index(rs_reg);
            int rt_index = get_gpr_index(rt_reg);
            u32 target = calculate_target(*insn);
    
            /*
            
            if (context.cpuRegs.GPR.r[rs_index].SD[0] < 0){
                context.cpuRegs.GPR.pc = insn->address + 4 + (offset << 2);
            }
            else{
                context.cpuRegs.GPR.pc = insn->address + 8;
            }
            */
            out_file << "    if ((s64)context.cpuRegs.GPR.r[" << rs_index << "].SD[0] == (s64)context.cpuRegs.GPR.r[" << rt_index << "].SD[0]) {" << std::endl;
            translate_instruction_block(out_file, delay_slot_insn);
            out_file << "        func_0x" << std::hex << target << "();" << std::endl;
            out_file << "        return;" << std::endl;
            out_file << "    }" << std::endl;
            break;
        }
        case MIPS_INS_BNEL: {
            // TODO: Implement BNEL (Branch on Not Equal Likely)
            // Branch likely instructions are tricky. For now, can treat as normal branch.
    
            const auto& rs_reg = insn->detail->mips.operands[0].reg;
            const auto& rt_reg = insn->detail->mips.operands[1].reg;
            const auto& offset = mips_details.operands[2].imm;
    
            int rs_index = get_gpr_index(rs_reg);
            int rt_index = get_gpr_index(rt_reg);
            u32 target = calculate_target(*insn);
    
            /*
            
            if (context.cpuRegs.GPR.r[rs_index].SD[0] < 0){
                context.cpuRegs.GPR.pc = insn->address + 4 + (offset << 2);
            }
            else{
                context.cpuRegs.GPR.pc = insn->address + 8;
            }
            */
            out_file << "    if ((s64)context.cpuRegs.GPR.r[" << rs_index << "].SD[0] != (s64)context.cpuRegs.GPR.r[" << rt_index << "].SD[0]) {" << std::endl;
            translate_instruction_block(out_file, delay_slot_insn);
            out_file << "        func_0x" << std::hex << target << "();" << std::endl;
            out_file << "        return;" << std::endl;
            out_file << "    }" << std::endl;
            break;
        }
        case MIPS_INS_BLEZL: {
            // TODO: Implement BLEZL (Branch on Less Than or Equal to Zero Likely)
            // Branch likely instructions are tricky. For now, can treat as normal branch.
    
            const auto& rs_reg = insn->detail->mips.operands[0].reg;
            const auto& offset = mips_details.operands[1].imm;
    
            int rs_index = get_gpr_index(rs_reg);
            u32 target = calculate_target(*insn);
    
            /*
            
            if (context.cpuRegs.GPR.r[rs_index].SD[0] < 0){
                context.cpuRegs.GPR.pc = insn->address + 4 + (offset << 2);
            }
            else{
                context.cpuRegs.GPR.pc = insn->address + 8;
            }
            */
            out_file << "    if ((s64)context.cpuRegs.GPR.r[" << rs_index << "].SD[0] <= 0) {" << std::endl;
            translate_instruction_block(out_file, delay_slot_insn);
            out_file << "        func_0x" << std::hex << target << "();" << std::endl;
            out_file << "        return;" << std::endl;
            out_file << "    }" << std::endl;
            break;
        }
        case MIPS_INS_BGTZL: {
            // TODO: Implement BGTZL (Branch on Greater Than Zero Likely)
            // Branch likely instructions are tricky. For now, can treat as normal branch.
    
            const auto& rs_reg = insn->detail->mips.operands[0].reg;
    
    
            int rs_index = get_gpr_index(rs_reg);
            u32 target = calculate_target(*insn);
    
            /*
            
            if (context.cpuRegs.GPR.r[rs_index].SD[0] < 0){
                context.cpuRegs.GPR.pc = insn->address + 4 + (offset << 2);
            }
            else{
                context.cpuRegs.GPR.pc = insn->address + 8;
            }
            */
            out_file << "    if ((s64)context.cpuRegs.GPR.r[" << rs_index << "].SD[0] > 0) {" << std::endl;
            translate_instruction_block(out_file, delay_slot_insn);
            out_file << "        func_0x" << std::hex << target << "();" << std::endl;
            out_file << "        return;" << std::endl;
            out_file << "    }" << std::endl;
            break;
        }
        case MIPS_INS_BLTZL: {
            std::cerr << "  Operand 0 [rs]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [offset]: type=" << mips_details.operands[1].type << ", imm=" << mips_details.operands[1].imm << std::endl;
            // TODO: Implement BLTZL (Branch on Less Than Zero Likely)
            // Branch likely instructions are tricky. For now, can treat as normal branch.
    
            const auto& rs_reg = insn->detail->mips.operands[0].reg;
    
            int rs_index = get_gpr_index(rs_reg);
            u32 target = calculate_target(*insn);
    
            /*
            
            if (context.cpuRegs.GPR.r[rs_index].SD[0] < 0){
                context.cpuRegs.GPR.pc = insn->address + 4 + (offset << 2);
            }
            else{
                context.cpuRegs.GPR.pc = insn->address + 8;
            }
            */
            out_file << "    if ((s64)context.cpuRegs.GPR.r[" << rs_index << "].SD[0] < 0) {" << std::endl;
            translate_instruction_block(out_file, delay_slot_insn);
            out_file << "        func_0x" << std::hex << target << "();" << std::endl;
            out_file << "        return;" << std::endl;
            out_file << "    }" << std::endl;
            break;
        }
        case MIPS_INS_BGEZL: {
            std::cerr << "  Operand 0 [rs]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [offset]: type=" << mips_details.operands[1].type << ", imm=" << mips_details.operands[1].imm << std::endl;
            // TODO: Implement BGEZL (Branch on Greater Than or Equal to Zero Likely)
            // Branch likely instructions are tricky. For now, can treat as normal branch.
    
            const auto& rs_reg = insn->detail->mips.operands[0].reg;
    
            int rs_index = get_gpr_index(rs_reg);
            u32 target = calculate_target(*insn);
    
            /*
            
            if (context.cpuRegs.GPR.r[rs_index].SD[0] < 0){
                context.cpuRegs.GPR.pc = insn->address + 4 + (offset << 2);
            }
            else{
                context.cpuRegs.GPR.pc = insn->address + 8;
            }
            */
            out_file << "    if ((s64)context.cpuRegs.GPR.r[" << rs_index << "].SD[0] >= 0) {" << std::endl;
            translate_instruction_block(out_file, delay_slot_insn);
            out_file << "        func_0x" << std::hex << target << "();" << std::endl;
            out_file << "        return;" << std::endl;
            out_file << "    }" << std::endl;
            break;
        }
        case MIPS_INS_BLTZALL: {
            std::cerr << "  Operand 0 [rs]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [offset]: type=" << mips_details.operands[1].type << ", imm=" << mips_details.operands[1].imm << std::endl;
            // TODO: Implement BLTZALL (Branch on Less Than Zero and Link Likely)
            // Branch likely instructions are tricky. For now, can treat as normal branch.
    
            const auto& rs_reg = insn->detail->mips.operands[0].reg;
    
            int rs_index = get_gpr_index(rs_reg);
            u32 target = calculate_target(*insn);
            out_file << "context.cpuRegs.GPR.r[31] = "<< insn->address <<" + 8;" << std::endl;
            /*
            
            if (context.cpuRegs.GPR.r[rs_index].SD[0] < 0){
                context.cpuRegs.GPR.pc = insn->address + 4 + (offset << 2);
            }
            else{
                context.cpuRegs.GPR.pc = insn->address + 8;
            }
            */
            out_file << "    if ((s64)context.cpuRegs.GPR.r[" << rs_index << "].SD[0] < 0) {" << std::endl;
            translate_instruction_block(out_file, delay_slot_insn);
            out_file << "        func_0x" << std::hex << target << "();" << std::endl;
            out_file << "        return;" << std::endl;
            out_file << "    }" << std::endl;
            break;
        }
        case MIPS_INS_BGEZALL: {
            std::cerr << "  Operand 0 [rs]: type=" << mips_details.operands[0].type << ", reg=" << mips_details.operands[0].reg << std::endl;
            std::cerr << "  Operand 1 [offset]: type=" << mips_details.operands[1].type << ", imm=" << mips_details.operands[1].imm << std::endl;
            // TODO: Implement BGEZALL (Branch on Greater Than or Equal to Zero and Link Likely)
            // Branch likely instructions are tricky. For now, can treat as normal branch.
    
            const auto& rs_reg = insn->detail->mips.operands[0].reg;
    
            int rs_index = get_gpr_index(rs_reg);
            u32 target = calculate_target(*insn);
            out_file << "    context.cpuRegs.GPR.r[31] = "<< insn->address <<" + 8;" << std::endl;
            /*
            
            if (context.cpuRegs.GPR.r[rs_index].SD[0] < 0){
                context.cpuRegs.GPR.pc = insn->address + 4 + (offset << 2);
            }
            else{
                context.cpuRegs.GPR.pc = insn->address + 8;
            }
            */
            out_file << "    if ((s64)context.cpuRegs.GPR.r[" << rs_index << "].SD[0] >= 0) {" << std::endl;
            translate_instruction_block(out_file, delay_slot_insn);
            out_file << "        func_0x" << std::hex << target << "();" << std::endl;
            out_file << "        return;" << std::endl;
            out_file << "    }" << std::endl;
            break;
        }
    }
}


bool is_return(cs_insn& insn){
    switch (insn.id){
        case MIPS_INS_JR:{
            return true;
        }
    }
    return false;
}

bool is_function_call(cs_insn& insn){
        switch (insn.id){
        case MIPS_INS_JAL:{
            return true;
        }
        case MIPS_INS_JALR:{
            return true;
        }
    }
    return false;
}

bool is_direct_function_call(cs_insn& insn){
        switch (insn.id){
        case MIPS_INS_JAL:{
            return true;
        }
    }
    return false;
}

bool is_control_flow_instruction(const cs_insn& insn) {
    for (int i = 0; i < insn.detail->groups_count; ++i) {
        // Check if the instruction belongs to the "JUMP" group...
        if (insn.detail->groups[i] == CS_GRP_JUMP ||
            // ...or the "RELATIVE BRANCH" group.
            insn.detail->groups[i] == CS_GRP_BRANCH_RELATIVE) {
            std::cout << insn.detail->groups[i] << std::endl;
            return true; // If it's in either group, it's a control flow instruction.
        }
    }
    return false;
}