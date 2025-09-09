#include "Recompiler.h"
#include "cpu_state.h"
#include "instructions/RabbitizerInstructionR5900.h" // Header for COP1 register macros
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>

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
    write_cpp_file(cpp_file);
    cpp_file.close();
    std::cout << "Successfully generated C++ file: " << output_cpp << std::endl;

    return true;
}

void Recompiler::write_header_file(std::ofstream& file) {
    file << "#pragma once\n\n";
    file << "#include <cstdint>\n";
    file << "#include \"host_app/cpu_state.h\"\n\n";
    file << "// Forward declarations of all recompiled functions\n";
    for (const auto& func : m_functions) {
        file << "void " << func.name << "(CpuState& ctx);\n";
    }
}

void Recompiler::write_cpp_file(std::ofstream& file) {
    file << "#include \"recompiled_functions.h\"\n";
    file << "#include <stdexcept> // For std::runtime_error\n\n";
    file << "// Definitions of all recompiled functions\n\n";

    for (const auto& func : m_functions) {
        recompile_function(func, file);
    }
}

void Recompiler::recompile_function(const Function& func, std::ofstream& file) {
    file << "// Function: " << func.name << " at 0x" << std::hex << func.base_address << std::dec << "\n";
    file << "void " << func.name << "(CpuState& ctx) {\n";

    for (const auto& block : func.blocks) {
        file << "block_" << std::hex << block.start_address << ":\n";
        for (const auto& instr : block.instructions) {
            char buffer[256];
            RabbitizerInstruction_disassemble(&instr, buffer, nullptr, 0, 0);
            file << "    // 0x" << std::hex << instr.vram << ": " << buffer << std::dec << "\n";
            translate_instruction(instr, file);
        }
    }

    file << "}\n\n";
}

void Recompiler::translate_instruction(const RabbitizerInstruction& instr, std::ofstream& file) {
    file << "    "; // Indentation
    const auto imm = static_cast<int16_t>(RabbitizerInstruction_getProcessedImmediate(&instr));

    switch (instr.uniqueId) {
        // --- NOP ---
        case RABBITIZER_INSTR_ID_cpu_nop:
            file << "/* NOP */ ;\n";
            break;

        // --- Arithmetic Instructions ---
        case RABBITIZER_INSTR_ID_cpu_addiu:
            file << get_gpr_name(RAB_INSTR_GET_rt(&instr)) << " = " << get_gpr_name(RAB_INSTR_GET_rs(&instr)) << " + " << imm << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_addu:
            file << get_gpr_name(RAB_INSTR_GET_rd(&instr)) << " = " << get_gpr_name(RAB_INSTR_GET_rs(&instr)) << " + " << get_gpr_name(RAB_INSTR_GET_rt(&instr)) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_subu:
            file << get_gpr_name(RAB_INSTR_GET_rd(&instr)) << " = " << get_gpr_name(RAB_INSTR_GET_rs(&instr)) << " - " << get_gpr_name(RAB_INSTR_GET_rt(&instr)) << ";\n";
            break;

        // --- Logical Instructions ---
        case RABBITIZER_INSTR_ID_cpu_or:
            file << get_gpr_name(RAB_INSTR_GET_rd(&instr)) << " = " << get_gpr_name(RAB_INSTR_GET_rs(&instr)) << " | " << get_gpr_name(RAB_INSTR_GET_rt(&instr)) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_ori:
            file << get_gpr_name(RAB_INSTR_GET_rt(&instr)) << " = " << get_gpr_name(RAB_INSTR_GET_rs(&instr)) << " | " << static_cast<uint16_t>(imm) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_and:
            file << get_gpr_name(RAB_INSTR_GET_rd(&instr)) << " = " << get_gpr_name(RAB_INSTR_GET_rs(&instr)) << " & " << get_gpr_name(RAB_INSTR_GET_rt(&instr)) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_andi:
            file << get_gpr_name(RAB_INSTR_GET_rt(&instr)) << " = " << get_gpr_name(RAB_INSTR_GET_rs(&instr)) << " & " << static_cast<uint16_t>(imm) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_xor:
            file << get_gpr_name(RAB_INSTR_GET_rd(&instr)) << " = " << get_gpr_name(RAB_INSTR_GET_rs(&instr)) << " ^ " << get_gpr_name(RAB_INSTR_GET_rt(&instr)) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_lui:
            file << get_gpr_name(RAB_INSTR_GET_rt(&instr)) << " = " << static_cast<uint16_t>(imm) << " << 16;\n";
            break;

        // --- Shift Instructions ---
        case RABBITIZER_INSTR_ID_cpu_sll:
            file << get_gpr_name(RAB_INSTR_GET_rd(&instr)) << " = static_cast<uint32_t>(" << get_gpr_name(RAB_INSTR_GET_rt(&instr)) << ") << " << RAB_INSTR_GET_sa(&instr) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_srl:
            file << get_gpr_name(RAB_INSTR_GET_rd(&instr)) << " = static_cast<uint32_t>(" << get_gpr_name(RAB_INSTR_GET_rt(&instr)) << ") >> " << RAB_INSTR_GET_sa(&instr) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_sra:
            file << get_gpr_name(RAB_INSTR_GET_rd(&instr)) << " = static_cast<int32_t>(" << get_gpr_name(RAB_INSTR_GET_rt(&instr)) << ") >> " << RAB_INSTR_GET_sa(&instr) << ";\n";
            break;

        // --- Memory Instructions ---
        case RABBITIZER_INSTR_ID_cpu_lw:
            file << get_gpr_name(RAB_INSTR_GET_rt(&instr)) << " = ctx.memory.read32(" << get_gpr_name(RAB_INSTR_GET_rs(&instr)) << " + " << imm << ");\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_sw:
            file << "ctx.memory.write32(" << get_gpr_name(RAB_INSTR_GET_rs(&instr)) << " + " << imm << ", " << get_gpr_name(RAB_INSTR_GET_rt(&instr)) << ");\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_lb:
            file << get_gpr_name(RAB_INSTR_GET_rt(&instr)) << " = static_cast<int8_t>(ctx.memory.read8(" << get_gpr_name(RAB_INSTR_GET_rs(&instr)) << " + " << imm << "));\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_lbu:
            file << get_gpr_name(RAB_INSTR_GET_rt(&instr)) << " = ctx.memory.read8(" << get_gpr_name(RAB_INSTR_GET_rs(&instr)) << " + " << imm << ");\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_sb:
            file << "ctx.memory.write8(" << get_gpr_name(RAB_INSTR_GET_rs(&instr)) << " + " << imm << ", " << get_gpr_name(RAB_INSTR_GET_rt(&instr)) << ");\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_lh:
            file << get_gpr_name(RAB_INSTR_GET_rt(&instr)) << " = static_cast<int16_t>(ctx.memory.read16(" << get_gpr_name(RAB_INSTR_GET_rs(&instr)) << " + " << imm << "));\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_lhu:
            file << get_gpr_name(RAB_INSTR_GET_rt(&instr)) << " = ctx.memory.read16(" << get_gpr_name(RAB_INSTR_GET_rs(&instr)) << " + " << imm << ");\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_sh:
            file << "ctx.memory.write16(" << get_gpr_name(RAB_INSTR_GET_rs(&instr)) << " + " << imm << ", " << get_gpr_name(RAB_INSTR_GET_rt(&instr)) << ");\n";
            break;

        // --- Branch Instructions ---
        case RABBITIZER_INSTR_ID_cpu_beq:
            file << "if (" << get_gpr_name(RAB_INSTR_GET_rs(&instr)) << " == " << get_gpr_name(RAB_INSTR_GET_rt(&instr)) << ") { goto block_" << std::hex << RabbitizerInstruction_getBranchVramGeneric(&instr) << std::dec << "; }\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_bne:
            file << "if (" << get_gpr_name(RAB_INSTR_GET_rs(&instr)) << " != " << get_gpr_name(RAB_INSTR_GET_rt(&instr)) << ") { goto block_" << std::hex << RabbitizerInstruction_getBranchVramGeneric(&instr) << std::dec << "; }\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_blez:
            file << "if (static_cast<int32_t>(" << get_gpr_name(RAB_INSTR_GET_rs(&instr)) << ") <= 0) { goto block_" << std::hex << RabbitizerInstruction_getBranchVramGeneric(&instr) << std::dec << "; }\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_bgtz:
            file << "if (static_cast<int32_t>(" << get_gpr_name(RAB_INSTR_GET_rs(&instr)) << ") > 0) { goto block_" << std::hex << RabbitizerInstruction_getBranchVramGeneric(&instr) << std::dec << "; }\n";
            break;

        // --- Jump Instructions ---
        case RABBITIZER_INSTR_ID_cpu_j:
            file << "goto block_" << std::hex << RabbitizerInstruction_getInstrIndexAsVram(&instr) << std::dec << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_jal:
            file << get_gpr_name(RABBITIZER_REG_GPR_O32_ra) << " = 0x" << std::hex << (instr.vram + 8) << std::dec << ";\n";
            file << "    func_" << std::hex << RabbitizerInstruction_getInstrIndexAsVram(&instr) << std::dec << "(ctx);\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_jr:
            if (RAB_INSTR_GET_rs(&instr) == RABBITIZER_REG_GPR_O32_ra) {
                file << "return;\n";
            } else {
                file << "// Dynamic jump not yet implemented!\n";
                file << "throw std::runtime_error(\"Dynamic jump not implemented\");\n";
            }
            break;

        // --- Set on Less Than ---
        case RABBITIZER_INSTR_ID_cpu_slt:
            file << get_gpr_name(RAB_INSTR_GET_rd(&instr)) << " = (static_cast<int32_t>(" << get_gpr_name(RAB_INSTR_GET_rs(&instr)) << ") < static_cast<int32_t>(" << get_gpr_name(RAB_INSTR_GET_rt(&instr)) << ")) ? 1 : 0;\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_slti:
            file << get_gpr_name(RAB_INSTR_GET_rt(&instr)) << " = (static_cast<int32_t>(" << get_gpr_name(RAB_INSTR_GET_rs(&instr)) << ") < " << imm << ") ? 1 : 0;\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_sltiu:
            file << get_gpr_name(RAB_INSTR_GET_rt(&instr)) << " = (" << get_gpr_name(RAB_INSTR_GET_rs(&instr)) << " < " << static_cast<uint16_t>(imm) << ") ? 1 : 0;\n";
            break;

        // --- HI/LO Registers ---
        case RABBITIZER_INSTR_ID_cpu_mfhi:
            file << get_gpr_name(RAB_INSTR_GET_rd(&instr)) << " = ctx.hi;\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_mflo:
            file << get_gpr_name(RAB_INSTR_GET_rd(&instr)) << " = ctx.lo;\n";
            break;

        // --- System Call ---
        case RABBITIZER_INSTR_ID_cpu_syscall:
            file << "ctx.handle_syscall();\n";
            break;

        // --- Coprocessor 0 (System Control) ---
        case RABBITIZER_INSTR_ID_cpu_mfc0:
            file << get_gpr_name(RAB_INSTR_GET_rt(&instr)) << " = ctx.cp0_gpr[" << std::to_string(RAB_INSTR_GET_rd(&instr)) << "];\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_mtc0:
            file << "ctx.cp0_gpr[" << std::to_string(RAB_INSTR_GET_rd(&instr)) << "] = " << get_gpr_name(RAB_INSTR_GET_rt(&instr)) << ";\n";
            break;

        // --- Coprocessor 1 (Floating-Point) ---
        case RABBITIZER_INSTR_ID_cpu_lwc1:
            file << get_fpr_name(RAB_INSTR_GET_rt(&instr)) << " = ctx.memory.read32(" << get_gpr_name(RAB_INSTR_GET_rs(&instr)) << " + " << imm << ");\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_swc1:
            file << "ctx.memory.write32(" << get_gpr_name(RAB_INSTR_GET_rs(&instr)) << " + " << imm << ", " << get_fpr_name(RAB_INSTR_GET_rt(&instr)) << ");\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_add_s:
            file << get_fpr_name(RAB_INSTR_R5900_GET_vfd(&instr)) << " = " << get_fpr_name(RAB_INSTR_R5900_GET_vfs(&instr)) << " + " << get_fpr_name(RAB_INSTR_R5900_GET_vft(&instr)) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_sub_s:
            file << get_fpr_name(RAB_INSTR_R5900_GET_vfd(&instr)) << " = " << get_fpr_name(RAB_INSTR_R5900_GET_vfs(&instr)) << " - " << get_fpr_name(RAB_INSTR_R5900_GET_vft(&instr)) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_mul_s:
            file << get_fpr_name(RAB_INSTR_R5900_GET_vfd(&instr)) << " = " << get_fpr_name(RAB_INSTR_R5900_GET_vfs(&instr)) << " * " << get_fpr_name(RAB_INSTR_R5900_GET_vft(&instr)) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_div_s:
            file << get_fpr_name(RAB_INSTR_R5900_GET_vfd(&instr)) << " = " << get_fpr_name(RAB_INSTR_R5900_GET_vfs(&instr)) << " / " << get_fpr_name(RAB_INSTR_R5900_GET_vft(&instr)) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_mtc1:
            file << get_fpr_name(RAB_INSTR_GET_rd(&instr)) << " = " << get_gpr_name(RAB_INSTR_GET_rt(&instr)) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_mfc1:
            file << get_gpr_name(RAB_INSTR_GET_rt(&instr)) << " = " << get_fpr_name(RAB_INSTR_GET_rd(&instr)) << ";\n";
            break;

        default:
            file << "// UNIMPLEMENTED INSTRUCTION: " << instr.uniqueId << "\n";
            file << "throw std::runtime_error(\"Unimplemented instruction in recompiler\");\n";
            break;
    }
}