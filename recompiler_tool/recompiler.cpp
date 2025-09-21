#include "Recompiler.h"
#include "host_app/cpu_state.h"
#include "host_app/memory.h"
#include "rabbitizer.hpp"

#include <iostream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

// Helper to get a string representation of a GPR register for code generation.
static std::string get_gpr_name(uint8_t reg_num) {
    return "ctx.cpuRegs.GPR.r[" + std::to_string(reg_num) + "]";
}

// Helper for floating-point registers
static std::string get_fpr_name(uint8_t reg_num) {
    return "ctx.fpuRegs.fpr[" + std::to_string(reg_num) + "]";
}
static std::string get_vr_name(uint8_t reg_num) {
    return "ctx.vr[" + std::to_string(reg_num) + "]";
}

static std::string format_imm(uint32_t imm) {
    std::stringstream ss;
    ss << "0x" << std::hex << imm;
    return ss.str();
}

Recompiler::Recompiler(const std::map<uint32_t, Function>& functions) : m_functions(functions) {}

bool Recompiler::recompile_to_files(const std::string& output_header, const std::string& output_cpp) {
    std::ofstream header_file(output_header);
    if (!header_file.is_open()) {
        std::cerr << "Error: Could not open header file for writing: " << output_header << std::endl;
        return false;
    }
    write_header_file(header_file);
    header_file.close();
    std::cout << "Successfully generated header file: " << output_header << std::endl;

    std::ofstream cpp_file(output_cpp);
    if (!cpp_file.is_open()) {
        std::cerr << "Error: Could not open C++ file for writing: " << output_cpp << std::endl;
        return false;
    }
    
    write_cpp_file(cpp_file, std::filesystem::path(output_header).filename().string());

    cpp_file.close();
    std::cout << "Successfully generated C++ file: " << output_cpp << std::endl;

    return true;
}

void Recompiler::write_header_file(std::ofstream& file) {
    file << "#pragma once\n\n";
    file << "#include \"../host_app/cpu_state.h\"\n\n";
    file << "#include <map>\n";
    file << "#include <functional>\n\n";

    for (const auto& pair : m_functions) {
        const Function& func = pair.second; // Get the Function object from the pair
        file << "void "<< func.name << "(CpuContext& ctx);\n";
    }
}

void Recompiler::write_cpp_file(std::ofstream& file, const std::string& output_header_filename) {
    file << "#include \"" << output_header_filename << "\"\n";
    file << "#include \"../host_app/cpu_state.h\"\n";
    file << "#include \"../host_app/memory.h\"\n\n";
    file << "#include \"../host_app/syscalls.h\"\n\n";
    file << "#include <iostream>\n";
    file << "#include <iomanip>\n";

    for (const auto& pair : m_functions) {
        const Function& func = pair.second; // Get the Function object from the pair
        recompile_function(func, file);
    }
}

bool Recompiler::has_delay_slot(const rabbitizer::InstructionCpu& instr) const {
    return instr.isBranch() || instr.isJump();
}

void Recompiler::recompile_function(const Function& func, std::ofstream& file) {
    file << "// Function at 0x" << std::hex << func.base_address << "\n";
    file << "void "<<func.name << "(CpuContext& ctx) {\n";

    for (const auto& block : func.blocks) {

        std::cout << "block_" << std::hex << block.start_address << ":\n";
        file << "block_" << std::hex << block.start_address << ":\n";
        
        for (size_t i = 0; i < block.instructions.size(); ++i) {
            const auto& instr_struct = block.instructions[i];
            // FIX: The C++ wrapper is constructed from the raw instruction word and its vram.
            rabbitizer::InstructionCpu instr(instr_struct.word, instr_struct.vram);

            if (has_delay_slot(instr)) {
                if (i + 1 < block.instructions.size()) {
                    const auto& delay_slot_struct = block.instructions[i + 1];
                    // FIX: Construct the delay slot wrapper the same way.
                    rabbitizer::InstructionCpu delay_slot_instr(delay_slot_struct.word, delay_slot_struct.vram);
                    file << "    ";
                    translate_instruction(delay_slot_instr, file);
                } else {
                    file << "    // WARNING: Branch at end of block has no delay slot instruction.\n";
                }

                file << "    ";
                translate_instruction(instr, file);
                i++;
            } else {
                file << "    ";
                translate_instruction(instr, file);
            }
        }
    }
    
    file << "}\n\n";
}

void Recompiler::translate_instruction(const rabbitizer::InstructionCpu& instr, std::ofstream& file) {
    switch (instr.getUniqueId()) {
        //
        // MIPS I - Arithmetic instructions
        //
        case RABBITIZER_INSTR_ID_cpu_add:
        case RABBITIZER_INSTR_ID_cpu_addu:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".SL[0] = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".SL[0] + " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".SL[0];\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_sub:
        case RABBITIZER_INSTR_ID_cpu_subu:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".SL[0] = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".SL[0] - " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".SL[0];\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_addi:
        case RABBITIZER_INSTR_ID_cpu_addiu:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".SL[0] = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".SL[0] + " << format_imm(static_cast<int16_t>(instr.Get_immediate())) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_multu:
            file << "    {\n";
            file << "        int64_t result = static_cast<int64_t>(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0]) * static_cast<int64_t>(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0]);\n";
            file << "        ctx.cpuRegs.LO.UL[0] = static_cast<uint32_t>(result);\n";
            file << "        ctx.cpuRegs.HI.UL[0] = static_cast<uint32_t>(result >> 32);\n";
            file << "    }\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_div:
        case RABBITIZER_INSTR_ID_cpu_divu:
            file << "    if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".SL[0] != 0) {\n";
            file << "        ctx.cpuRegs.LO.SL[0] = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".SL[0] / " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".SL[0];\n";
            file << "        ctx.cpuRegs.HI.SL[0] = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".SL[0] % " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".SL[0];\n";
            file << "    }\n";
            break;
        //
        // MIPS I - Logical instructions
        //
        case RABBITIZER_INSTR_ID_cpu_and:
             file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UL[0] = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] & " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0];\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_or:
             file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UL[0] = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] | " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0];\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_xor:
             file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UL[0] = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] ^ " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0];\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_nor:
             file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UL[0] = ~(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] | " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0]);\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_andi:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0] = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] & " << format_imm(instr.Get_immediate()) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_ori:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0] = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] | " << format_imm(instr.Get_immediate()) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_xori:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0] = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] ^ " << format_imm(instr.Get_immediate()) << ";\n";
            break;
        //
        // MIPS I - Shift instructions
        //
        case RABBITIZER_INSTR_ID_cpu_sll:
             file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UL[0] = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0] << " << instr.Get_sa() << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_srl:
             file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UL[0] = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0] >> " << instr.Get_sa() << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_sra:
             file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".SL[0] = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".SL[0] >> " << instr.Get_sa() << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_sllv:
             file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UL[0] = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0] << (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] & 0x1F);\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_srlv:
             file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UL[0] = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0] >> (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] & 0x1F);\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_srav:
             file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".SL[0] = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".SL[0] >> (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] & 0x1F);\n";
            break;
        //
        // MIPS I - Branch instructions
        //
        case RABBITIZER_INSTR_ID_cpu_beq:
            file << "    if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] == " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0]) ctx.cpuRegs.pc += " << format_imm(static_cast<int16_t>(instr.Get_immediate()) << 2) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_bne:
            file << "    if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] != " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0]) ctx.cpuRegs.pc += " << format_imm(static_cast<int16_t>(instr.Get_immediate()) << 2) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_blez:
            file << "    if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".SL[0] <= 0) ctx.cpuRegs.pc += " << format_imm(static_cast<int16_t>(instr.Get_immediate()) << 2) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_bgtz:
            file << "    if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".SL[0] > 0) ctx.cpuRegs.pc += " << format_imm(static_cast<int16_t>(instr.Get_immediate()) << 2) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_bltz:
            file << "    if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".SL[0] < 0) ctx.cpuRegs.pc += " << format_imm(static_cast<int16_t>(instr.Get_immediate()) << 2) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_bgez:
            file << "    if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".SL[0] >= 0) ctx.cpuRegs.pc += " << format_imm(static_cast<int16_t>(instr.Get_immediate()) << 2) << ";\n";
            break;
        //
        // MIPS I - Jump instructions
        //
        case RABBITIZER_INSTR_ID_cpu_j:
            file << "    ctx.cpuRegs.pc = (ctx.cpuRegs.pc & 0xF0000000) | " << format_imm(instr.Get_instr_index() << 2) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_jal:
            file << "    " << get_gpr_name(31) << ".UL[0] = ctx.cpuRegs.pc + 8;\n";
            file << "    ctx.cpuRegs.pc = (ctx.cpuRegs.pc & 0xF0000000) | " << format_imm(instr.Get_instr_index() << 2) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_jr:
            file << "    ctx.cpuRegs.pc = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0];\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_jalr:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UL[0] = ctx.cpuRegs.pc + 8;\n";
            file << "    ctx.cpuRegs.pc = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0];\n";
            break;
        //
        // MIPS I - Load/Store instructions
        //
        case RABBITIZER_INSTR_ID_cpu_lb:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".SL[0] = static_cast<int32_t>(static_cast<int8_t>(memory::read<uint8_t>(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] + " << format_imm(static_cast<int16_t>(instr.Get_immediate())) << ")));\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_lbu:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0] = static_cast<uint32_t>(memory::read<uint8_t>(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] + " << format_imm(static_cast<int16_t>(instr.Get_immediate())) << "));\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_lh:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".SL[0] = static_cast<int32_t>(static_cast<int16_t>(memory::read<uint16_t>(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] + " << format_imm(static_cast<int16_t>(instr.Get_immediate())) << ")));\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_lhu:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0] = static_cast<uint32_t>(memory::read<uint16_t>(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] + " << format_imm(static_cast<int16_t>(instr.Get_immediate())) << "));\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_lw:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0] = memory::read<uint32_t>(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] + " << format_imm(static_cast<int16_t>(instr.Get_immediate())) << ");\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_sb:
            file << "    memory::write<uint8_t>(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] + " << format_imm(static_cast<int16_t>(instr.Get_immediate())) << ", " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0]);\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_sh:
            file << "    memory::write<uint16_t>(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] + " << format_imm(static_cast<int16_t>(instr.Get_immediate())) << ", " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0]);\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_sw:
            file << "    memory::write<uint32_t>(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] + " << format_imm(static_cast<int16_t>(instr.Get_immediate())) << ", " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0]);\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_lui:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0] = " << format_imm(instr.Get_immediate() << 16) << ";\n";
            break;
        //
        // FPU Instructions
        //
        case RABBITIZER_INSTR_ID_cpu_add_s:
            file << "    " << get_fpr_name(static_cast<uint8_t>(instr.GetO32_fd())) << ".f = " << get_fpr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ".f + " << get_fpr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".f;\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_sub_s:
            file << "    " << get_fpr_name(static_cast<uint8_t>(instr.GetO32_fd())) << ".f = " << get_fpr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ".f - " << get_fpr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".f;\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_mul_s:
            file << "    " << get_fpr_name(static_cast<uint8_t>(instr.GetO32_fd())) << ".f = " << get_fpr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ".f * " << get_fpr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".f;\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_div_s:
            file << "    " << get_fpr_name(static_cast<uint8_t>(instr.GetO32_fd())) << ".f = " << get_fpr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ".f / " << get_fpr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".f;\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_mtc1:
            file << "    " << get_fpr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ".UL = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0];\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_mfc1:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0] = " << get_fpr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ".UL;\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_bnez:
            file << "    if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] != 0) ctx.cpuRegs.pc += " << format_imm(static_cast<int16_t>(instr.Get_immediate()) << 2) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_bnel:
            file << "    if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] != " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0]) { ctx.cpuRegs.pc += " << format_imm(static_cast<int16_t>(instr.Get_immediate()) << 2) << "; } else { /* Branch likely, nullify delay slot */ }\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_beql:
             file << "    if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] == " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0]) { ctx.cpuRegs.pc += " << format_imm(static_cast<int16_t>(instr.Get_immediate()) << 2) << "; } else { /* Branch likely, nullify delay slot */ }\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_blezl:
            file << "    if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".SL[0] <= 0) { ctx.cpuRegs.pc += " << format_imm(static_cast<int16_t>(instr.Get_immediate()) << 2) << "; } else { /* Branch likely, nullify delay slot */ }\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_bltzl:
            file << "    if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".SL[0] < 0) { ctx.cpuRegs.pc += " << format_imm(static_cast<int16_t>(instr.Get_immediate()) << 2) << "; } else { /* Branch likely, nullify delay slot */ }\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_bgezl:
            file << "    if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".SL[0] >= 0) { ctx.cpuRegs.pc += " << format_imm(static_cast<int16_t>(instr.Get_immediate()) << 2) << "; } else { /* Branch likely, nullify delay slot */ }\n";
            break;

        // Doubleword and Logical Instructions
        case RABBITIZER_INSTR_ID_cpu_daddiu:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".SD[0] = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".SD[0] + " << static_cast<int16_t>(instr.Get_immediate()) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_daddu:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".SD[0] = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".SD[0] + " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".SD[0];\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_ddivu:
            file << "    if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UD[0] != 0) {\n";
            file << "        ctx.cpuRegs.LO.UD[0] = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UD[0] / " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UD[0];\n";
            file << "        ctx.cpuRegs.HI.UD[0] = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UD[0] % " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UD[0];\n";
            file << "    }\n";
            break;

        case RABBITIZER_INSTR_ID_cpu_sltiu:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0] = (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] < " << static_cast<uint16_t>(instr.Get_immediate()) << ") ? 1 : 0;\n";
            break;

        case RABBITIZER_INSTR_ID_cpu_sltu:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UL[0] = (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] < " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0]) ? 1 : 0;\n";
            break;

        // Load/Store and Move Instructions
        case RABBITIZER_INSTR_ID_r5900_lq:
            file << "    // lq instruction - 128-bit load\n";
            file << "    memory::read_quad(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] + " << static_cast<int16_t>(instr.Get_immediate()) << ", " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ");\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_lwl:
            file << "    // lwl instruction\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_lwr:
            file << "    // lwr instruction\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_movz:
            file << "    if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0] == 0) " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UL[0] = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0];\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_mult:
            file << "    {\n";
            file << "        int64_t result = static_cast<int64_t>(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".SL[0]) * static_cast<int64_t>(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".SL[0]);\n";
            file << "        ctx.cpuRegs.LO.UL[0] = static_cast<uint32_t>(result);\n";
            file << "        ctx.cpuRegs.HI.UL[0] = static_cast<uint32_t>(result >> 32);\n";
            file << "    }\n";
            break;
        case RABBITIZER_INSTR_ID_r5900_sq:
            file << "    // sq instruction - 128-bit store\n";
            file << "    memory::write_quad(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] + " << static_cast<int16_t>(instr.Get_immediate()) << ", " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ");\n";
            break;
        
        // System and MMI Instructions
        case RABBITIZER_INSTR_ID_r5900_pextlw:
            file << "    // pextlw instruction\n";
            break;
        case RABBITIZER_INSTR_ID_r5900_pextuw:
            file << "    // pextuw instruction\n";
            break;
        case RABBITIZER_INSTR_ID_r5900_ppach:
            file << "    // ppach instruction\n";
            break;
        case RABBITIZER_INSTR_ID_r5900_ppacw:
            file << "    // ppacw instruction\n";
            break;
        case RABBITIZER_INSTR_ID_r5900_psraw:
            file << "    // psraw instruction\n";
            break;
        case RABBITIZER_INSTR_ID_r5900_psrlw:
            file << "    // psrlw instruction\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_sync:
            file << "    // sync instruction - memory barrier\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_syscall:
            file << "    runtime_syscall_dispatcher(" << get_gpr_name(3) << ".UL[0], ctx);\n";
            break;

        // Vector Unit (VU) Instructions
        case RABBITIZER_INSTR_ID_r5900_vaddx:
            file << "    " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << ".x = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ".x + " << get_vr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".x;\n";
            break;
        case RABBITIZER_INSTR_ID_r5900_vadd:
            file << "    " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << " = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << " + " << get_vr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_r5900_vsub:
            file << "    " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << " = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << " - " << get_vr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_r5900_vmul:
            file << "    " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << " = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << " * " << get_vr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_r5900_vdiv:
            file << "    " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << ".x = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ".x / " << get_vr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".x;\n";
            file << "    " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << ".y = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ".y / " << get_vr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".y;\n";
            file << "    " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << ".z = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ".z / " << get_vr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".z;\n";
            file << "    " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << ".w = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ".w / " << get_vr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".w;\n";
            break;
        case RABBITIZER_INSTR_ID_r5900_vsqrt:
            file << "    " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << ".x = sqrt(" << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ".x);\n";
            file << "    " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << ".y = sqrt(" << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ".y);\n";
            file << "    " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << ".z = sqrt(" << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ".z);\n";
            file << "    " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << ".w = sqrt(" << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ".w);\n";
            break;
        case RABBITIZER_INSTR_ID_r5900_vabs:
            file << "    " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << ".x = abs(" << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ".x);\n";
            file << "    " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << ".y = abs(" << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ".y);\n";
            file << "    " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << ".z = abs(" << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ".z);\n";
            file << "    " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << ".w = abs(" << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ".w);\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_sd:
            file << "    memory::write<uint64_t>(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] + " << format_imm(static_cast<int16_t>(instr.Get_immediate())) << ", " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UD[0]);\n";
            break;

        default:
            file << "    // Unhandled instruction: " << instr.getOpcodeName() << "\n";
            break;
    }
}