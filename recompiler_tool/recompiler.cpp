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
    return "ctx.gpr[" + std::to_string(reg_num) + "]";
}

// Helper for floating-point registers
static std::string get_fpr_name(uint8_t reg_num) {
    return "ctx.fpr[" + std::to_string(reg_num) + "]";
}

Recompiler::Recompiler(const std::vector<Function>& functions) : m_functions(functions) {}

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
    file << "#include \"host_app/cpu_state.h\"\n\n";

    for (const auto& func : m_functions) {
        file << "void func_" << std::hex << func.base_address << "(CpuContext& ctx);\n";
    }
}

void Recompiler::write_cpp_file(std::ofstream& file, const std::string& output_header_filename) {
    file << "#include \"" << output_header_filename << "\"\n";
    file << "#include \"host_app/memory.h\"\n\n";
    file << "extern Memory memory; // Assume a global memory object for now\n\n";

    for (const auto& func : m_functions) {
        recompile_function(func, file);
    }
}

bool Recompiler::has_delay_slot(const rabbitizer::InstructionCpu& instr) const {
    return instr.isBranch() || instr.isJump();
}

void Recompiler::recompile_function(const Function& func, std::ofstream& file) {
    file << "// Function at 0x" << std::hex << func.base_address << "\n";
    file << "void func_" << std::hex << func.base_address << "(CpuContext& ctx) {\n";

    for (const auto& block : func.blocks) {
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
    file << "    // " << instr.disassemble(0) << "\n";
    
    const auto imm = static_cast<int16_t>(instr.getProcessedImmediate());

    // FIX: Cast all register enum returns to uint8_t before passing to helper functions.
    switch (instr.getUniqueId()) {
        case RABBITIZER_INSTR_ID_cpu_nop:
            file << "    /* NOP */ ;\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_addiu:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << " = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << " + " << imm << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_addu:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << " = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << " + " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_subu:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << " = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << " - " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_or:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << " = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << " | " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_ori:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << " = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << " | " << static_cast<uint16_t>(imm) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_and:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << " = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << " & " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_andi:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << " = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << " & " << static_cast<uint16_t>(imm) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_xor:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << " = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << " ^ " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_lui:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << " = " << static_cast<uint16_t>(imm) << " << 16;\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_sll:
             if (instr.getRaw() == 0) {
                 file << "    /* NOP */ ;\n";
             } else {
                file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << " = static_cast<uint32_t>(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ") << " << instr.Get_sa() << ";\n";
             }
            break;
        case RABBITIZER_INSTR_ID_cpu_srl:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << " = static_cast<uint32_t>(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ") >> " << instr.Get_sa() << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_sra:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << " = static_cast<int32_t>(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ") >> " << instr.Get_sa() << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_lw:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << " = memory.read_u32(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << " + " << imm << ");\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_sw:
            file << "    memory.write_u32(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << " + " << imm << ", " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ");\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_lb:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << " = static_cast<int8_t>(memory.read_u8(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << " + " << imm << "));\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_lbu:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << " = memory.read_u8(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << " + " << imm << ");\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_sb:
            file << "    memory.write_u8(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << " + " << imm << ", " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ");\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_lh:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << " = static_cast<int16_t>(memory.read_u16(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << " + " << imm << "));\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_lhu:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << " = memory.read_u16(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << " + " << imm << ");\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_sh:
            file << "    memory.write_u16(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << " + " << imm << ", " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ");\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_beq:
            file << "    if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << " == " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ") { goto block_" << std::hex << instr.getBranchVramGeneric() << std::dec << "; }\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_bne:
            file << "    if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << " != " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ") { goto block_" << std::hex << instr.getBranchVramGeneric() << std::dec << "; }\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_blez:
            file << "    if (static_cast<int32_t>(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ") <= 0) { goto block_" << std::hex << instr.getBranchVramGeneric() << std::dec << "; }\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_bgtz:
            file << "    if (static_cast<int32_t>(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ") > 0) { goto block_" << std::hex << instr.getBranchVramGeneric() << std::dec << "; }\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_j:
            file << "    goto block_" << std::hex << instr.getBranchVramGeneric() << std::dec << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_jal:
            file << "    " << get_gpr_name(RABBITIZER_REG_GPR_O32_ra) << " = 0x" << std::hex << (instr.getVram() + 8) << std::dec << "; // Set return address\n";
            file << "    func_" << std::hex << instr.getBranchVramGeneric() << std::dec << "(ctx);\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_jr:
            // FIX: Cast the enum to its underlying type for comparison.
            if (static_cast<uint8_t>(instr.GetO32_rs()) == RABBITIZER_REG_GPR_O32_ra) {
                file << "    return;\n";
            } else {
                file << "    // Dynamic jump not yet implemented!\n";
                file << "    throw std::runtime_error(\"Dynamic jump not implemented\");\n";
            }
            break;
        case RABBITIZER_INSTR_ID_cpu_slt:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << " = (static_cast<int32_t>(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ") < static_cast<int32_t>(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ")) ? 1 : 0;\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_slti:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << " = (static_cast<int32_t>(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ") < " << imm << ") ? 1 : 0;\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_sltiu:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << " = (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << " < " << static_cast<uint16_t>(imm) << ") ? 1 : 0;\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_mfhi:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << " = ctx.hi;\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_mflo:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << " = ctx.lo;\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_syscall:
            file << "    ctx.handle_syscall();\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_mfc0:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << " = ctx.cp0_gpr[" << std::to_string(static_cast<uint8_t>(instr.Get_cop0d())) << "];\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_mtc0:
            file << "    ctx.cp0_gpr[" << std::to_string(static_cast<uint8_t>(instr.Get_cop0d())) << "] = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_lwc1:
            file << "    " << get_fpr_name(static_cast<uint8_t>(instr.GetO32_ft())) << " = memory.read_u32(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << " + " << imm << ");\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_swc1:
            file << "    memory.write_u32(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << " + " << imm << ", " << get_fpr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ");\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_add_s:
            file << "    " << get_fpr_name(static_cast<uint8_t>(instr.GetO32_fd())) << " = " << get_fpr_name(static_cast<uint8_t>(instr.GetO32_fs())) << " + " << get_fpr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_sub_s:
            file << "    " << get_fpr_name(static_cast<uint8_t>(instr.GetO32_fd())) << " = " << get_fpr_name(static_cast<uint8_t>(instr.GetO32_fs())) << " - " << get_fpr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_mul_s:
            file << "    " << get_fpr_name(static_cast<uint8_t>(instr.GetO32_fd())) << " = " << get_fpr_name(static_cast<uint8_t>(instr.GetO32_fs())) << " * " << get_fpr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_div_s:
            file << "    " << get_fpr_name(static_cast<uint8_t>(instr.GetO32_fd())) << " = " << get_fpr_name(static_cast<uint8_t>(instr.GetO32_fs())) << " / " << get_fpr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_mtc1:
            file << "    " << get_fpr_name(static_cast<uint8_t>(instr.GetO32_fs())) << " = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_mfc1:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << " = " << get_fpr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ";\n";
            break;
        default:
            file << "    // UNIMPLEMENTED INSTRUCTION: " << static_cast<int>(instr.getUniqueId()) << "\n";
            file << "    throw std::runtime_error(\"Unimplemented instruction in recompiler\");\n";
            break;
    }
}