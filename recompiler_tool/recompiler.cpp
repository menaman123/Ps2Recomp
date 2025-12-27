#include "Recompiler.h"
#include "host_app/cpu_state.h"
#include "host_app/memory.h"
#include "rabbitizer.hpp"
#include "instructions/InstructionR5900.hpp"

#include <iostream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <filesystem>

std::ofstream log_file;

// Helper to get a string representation of a GPR register for code generation.
static std::string get_gpr_name(uint8_t reg_num) {
    return "ctx.cpuRegs.GPR.r[" + std::to_string(reg_num) + "]";
}

// Helper for floating-point registers
static std::string get_fpr_name(uint8_t reg_num) {
    return "ctx.fpuRegs.fpr[" + std::to_string(reg_num) + "]";
}

static std::string get_vr_name(uint8_t reg_num) {
    return "ctx.vuRegs.regs[" + std::to_string(reg_num) + "]";
}

static std::string format_imm(int32_t imm) {
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
    file << "#include <vector>\n";
    file << "#include <functional>\n\n";

    // CHANGE: Removed default parameter - addr is now required
    for (const auto& pair : m_functions) {
        const Function& func = pair.second;
        file << "void "<< func.name << "(CpuContext& ctx);\n";
    }

    file << "\nextern std::map<uint32_t, std::function<void(CpuContext&, uint32_t)>> recompiled_functions;\n";
    file << "std::function<void(CpuContext&, uint32_t)> find_containing_function(uint32_t pc);\n";
    file << "void initialize_recompiled_functions();\n";
}

// CHANGE: Updated to simpler function generation without switch statement
void Recompiler::write_cpp_file(std::ofstream& file, const std::string& output_header_filename) {
    file << "#include \"" << output_header_filename << "\"\n";
    file << "#include \"../host_app/cpu_state.h\"\n";
    file << "#include \"../host_app/memory.h\"\n";
    file << "#include \"../host_app/syscalls.h\"\n\n";
    file << "#include <iostream>\n";
    file << "#include <iomanip>\n";
    file << "#include <cmath>\n";
    file << "#include <vector>\n";
    file << "#include <fstream>\n\n";

    file << "extern std::ofstream g_logFile;\n\n";

    file << "std::map<uint32_t, std::function<void(CpuContext&, uint32_t)>> recompiled_functions;\n";
    
    // Add function ranges for smart lookup
    file << "struct FunctionRange {\n";
    file << "    uint32_t start;\n";
    file << "    uint32_t end;\n";
    file << "    std::function<void(CpuContext&, uint32_t)> func;\n";
    file << "};\n";
    file << "std::vector<FunctionRange> function_ranges;\n\n";

    for (const auto& pair : m_functions) {
        const Function& func = pair.second;
        recompile_function(func, file);
    }

    // Add the smart lookup function
    file << "std::function<void(CpuContext&, uint32_t)> find_containing_function(uint32_t pc) {\n";
    file << "    for (const auto& range : function_ranges) {\n";
    file << "        if (pc >= range.start && pc < range.end) {\n";
    file << "            return range.func;\n";
    file << "        }\n";
    file << "    }\n";
    file << "    return nullptr;\n";
    file << "}\n\n";

    file << "void initialize_recompiled_functions() {\n";
    for (const auto& pair : m_functions) {
        const Function& func = pair.second;
        file << "    recompiled_functions[0x" << std::hex << func.base_address << "] = [](CpuContext& ctx, uint32_t addr) { " << func.name << "(ctx); };\n";
        
        uint32_t end_address = func.base_address + func.size;
        file << "    function_ranges.push_back({0x" << std::hex << func.base_address 
             << ", 0x" << std::hex << end_address << ", [](CpuContext& ctx, uint32_t addr) { " << func.name << "(ctx); }});\n";
    }
    file << "}\n";
}

bool Recompiler::has_delay_slot(const rabbitizer::InstructionR5900& instr) const {
    return instr.isBranch() || instr.isJump() && !instr.isTrap();
}

// CHANGE: Complete rewrite - no switch statement, simple labels
void Recompiler::recompile_function(const Function& func, std::ofstream& file) {
    std::cout << "Recompiling function: " << func.name << std::endl;
    std::cout << "Base address: 0x" << std::hex << func.base_address << std::endl;
    std::cout << "Size: " << std::dec << func.size << std::endl;
    std::cout << "Blocks: " << std::dec << func.blocks.size() << std::endl;

    file << "// Function: " << func.name << " at 0x" << std::hex << func.base_address << "\n";
    file << "void " << func.name << "(CpuContext& ctx) {\n";
    if(func.name == "entry"){
        file << "    initialize_recompiled_functions();" << std::endl;
    }
    
    // Generate each basic block with simple labels
    for (size_t block_idx = 0; block_idx < func.blocks.size(); ++block_idx) {
        std::cout << "Recompiling block: " << block_idx << std::endl;
        const auto& block = func.blocks[block_idx];
        
        // CHANGE: Simple label format Label_0000 instead of block_0
        file << "Label_" << std::setw(4) << std::setfill('0') << block_idx << ": // 0x" << std::hex << block.start_address << "\n";
        
        generate_block_code(func, block, block_idx, file);
        
        file << "\n";
    }
    
    file << "}\n\n";
}

// CHANGE: New helper function to generate block code
void Recompiler::generate_block_code(const Function& func, const Block& block, size_t block_idx, std::ofstream& file) {
    bool has_terminator = false;
    
    // Process each instruction in the block
    log_file <<  "[PROCESSING FUNCTION BLOCK] Function: " << func.name << std::endl;
    log_file <<  "[PROCESSING FUNCTION BLOCK] Block Instruction Count: " <<  std::dec << block.instructions.size() << std::endl;

    /*
    
    
    */
    for (int i = 0; i < block.instructions.size(); ++i) {

        log_file << "FUNCTION: " << func.name << " BLOCK INDEX: " << block_idx << " BLOCK INSTRUCTION INDEX: " << i << "\n \n" << std::endl;

        const auto& instr_struct = block.instructions[i];
        const auto& instr_word = instr_struct.getCPtr()->word;
        const auto& instr_vram = instr_struct.getCPtr()->vram;
        
        rabbitizer::InstructionR5900 instr(instr_word, instr_vram);
        std::string disasm_string; 
        // Check if this is a branch/jump instruction

        if (has_delay_slot(instr)) {

            bool is_likely_branch = (static_cast<int>(instr.getUniqueId()) == RABBITIZER_INSTR_ID_cpu_beql ||
                                    static_cast<int>(instr.getUniqueId()) == RABBITIZER_INSTR_ID_cpu_bnel ||
                                    static_cast<int>(instr.getUniqueId()) == RABBITIZER_INSTR_ID_cpu_blezl ||
                                    static_cast<int>(instr.getUniqueId()) == RABBITIZER_INSTR_ID_cpu_bgtzl ||
                                    static_cast<int>(instr.getUniqueId()) == RABBITIZER_INSTR_ID_cpu_bltzl ||
                                    static_cast<int>(instr.getUniqueId()) == RABBITIZER_INSTR_ID_cpu_bgezl);
            // Execute delay slot first (if it exists and is next)
            if (!is_likely_branch){
                // if beq find all the registers it is using current in this instruction then do the rest
                // after in translate_control_flow we use the variable
                if (static_cast<int>(instr.getUniqueId()) == RABBITIZER_INSTR_ID_cpu_beq){
                    // get registers being used
                    file << "    bool branch_taken_" << std::hex << instr.getVram() << " = (" 
                    << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] == " 
                    << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0]);\n";
                
                    
                }
                if (i + 1 < block.instructions.size()) {
                    const auto& delay_slot_struct = block.instructions[i + 1];
                    const auto& delay_word = delay_slot_struct.getCPtr()->word;
                    const auto& delay_vram = delay_slot_struct.getCPtr()->vram;
                    rabbitizer::InstructionR5900 delay_slot_instr(delay_word, delay_vram);
                    disasm_string = delay_slot_instr.disassemble(0, "");
                    
                    log_file << "[PROCESSING INSTRUCTION] " << disasm_string<< std::endl;
                    file << "    ";
                    translate_instruction(delay_slot_instr, file);
                }
            }
            // Handle the branch/jump - CHANGE: New function for control flow
            file << "    ";
            disasm_string = instr.disassemble(0, "");
            
            log_file << "[PROCESSING INSTRUCTION THAT CAUSES DELAY SLOT] " << disasm_string << std::endl;
            translate_control_flow(instr, func, block, block_idx, i, file);
            has_terminator = true;
             // No more instructions after branch
             i++;
        } else {
            // Regular instruction
            file << "    ";
            
            disasm_string = instr.disassemble(0, "");
            log_file << "[PROCESSING INSTRUCTION] " << disasm_string<< std::endl;
            translate_instruction(instr, file);
        }
    }
    
    // Handle fall-through if no explicit branch
    if (!has_terminator) {
        uint32_t fall_through_addr = block.end_address;
        file << "// This BLOCK HAS NO BRANCH SO GO TO NEXT BLOCK AFTER THIS, CURRENT BLOCK ENDS AT: 0x" << std::hex << block.end_address << "\n";
        file << "// Fall through to 0x" << std::hex << fall_through_addr << "\n"; 

        bool found_next_block = false;
        for (size_t i = 0; i < func.blocks.size(); ++i) {
            if (func.blocks[i].start_address == fall_through_addr) {
                file << "    goto Label_" << std::setw(4) << std::setfill('0') << i << "; // Fall through\n";
                found_next_block = true;
                break;
            }
        }
        
        if (!found_next_block) {
            // CHANGE: Call external function or set PC
            file << "    // Fall through to 0x" << std::hex << fall_through_addr << "\n";
            file << "    if (recompiled_functions.count(0x" << std::hex << fall_through_addr << ")) {\n";
            file << "        recompiled_functions[0x" << std::hex << fall_through_addr << "](ctx, 0x" << std::hex << fall_through_addr << ");\n";
            file << "        return;\n";
            file << "    } else {\n";
            file << "        ctx.cpuRegs.pc = 0x" << std::hex << fall_through_addr << ";\n";
            file << "        return;\n";
            file << "    }\n";
        }
    }
}

// CHANGE: New function for control flow with function calls
void Recompiler::translate_control_flow(const rabbitizer::InstructionR5900& instr, 
                                        const Function& func,
                                        const Block& block,
                                        size_t current_block_idx,
                                        size_t current_instr_idx, 
                                        std::ofstream& file) {
    
    uint32_t current_pc = instr.getVram();
    
    switch (instr.getUniqueId()) {
        // Conditional branches
        case RABBITIZER_INSTR_ID_cpu_beq: {
            uint32_t target_addr = current_pc + 4 + (static_cast<int16_t>(instr.Get_immediate()) << 2);
            uint32_t fall_through_addr = current_pc + 8;
            
            file << "    if (branch_taken_" << std::hex << current_pc << ") {\n";
            emit_branch_target(target_addr, func, file, "        ");
            file << "    } else {\n";
            emit_branch_target(fall_through_addr, func, file, "        ");
            file << "    }\n";
            break;
        }
        case RABBITIZER_INSTR_ID_cpu_beql: {
            uint32_t target_addr = current_pc + 4 + (static_cast<int16_t>(instr.Get_immediate()) << 2);
            uint32_t fall_through_addr = current_pc + 8;
            
            file << "if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] == " 
                << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0]) {\n";
            
            // Execute delay slot ONLY when branch is taken
            if (current_instr_idx + 1 < block.instructions.size()) {
                const auto& delay_slot_struct = block.instructions[current_instr_idx + 1];
                const auto& delay_word = delay_slot_struct.getCPtr()->word;
                const auto& delay_vram = delay_slot_struct.getCPtr()->vram;
                rabbitizer::InstructionR5900 delay_slot_instr(delay_word, delay_vram);
                file << "        ";
                translate_instruction(delay_slot_instr, file);
            }
            
            emit_branch_target(target_addr, func, file, "        ");
            file << "    } else {\n";
            emit_branch_target(fall_through_addr, func, file, "        ");
            file << "    }\n";
            break;
        }

        case RABBITIZER_INSTR_ID_cpu_bnel: {
            uint32_t target_addr = current_pc + 4 + (static_cast<int16_t>(instr.Get_immediate()) << 2);
            uint32_t fall_through_addr = current_pc + 8;
            
            file << "if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] != " 
                << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0]) {\n";
            
            // Execute delay slot ONLY when branch is taken
            if (current_instr_idx + 1 < block.instructions.size()) {
                const auto& delay_slot_struct = block.instructions[current_instr_idx + 1];
                const auto& delay_word = delay_slot_struct.getCPtr()->word;
                const auto& delay_vram = delay_slot_struct.getCPtr()->vram;
                rabbitizer::InstructionR5900 delay_slot_instr(delay_word, delay_vram);
                file << "        ";
                translate_instruction(delay_slot_instr, file);
            }
            
            emit_branch_target(target_addr, func, file, "        ");
            file << "    } else {\n";
            emit_branch_target(fall_through_addr, func, file, "        ");
            file << "    }\n";
            break;
        }

        case RABBITIZER_INSTR_ID_cpu_blezl: {
            uint32_t target_addr = current_pc + 4 + (static_cast<int16_t>(instr.Get_immediate()) << 2);
            uint32_t fall_through_addr = current_pc + 8;
            
            file << "if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".SL[0] <= 0) {\n";
            
            // Execute delay slot ONLY when branch is taken
            if (current_instr_idx + 1 < block.instructions.size()) {
                const auto& delay_slot_struct = block.instructions[current_instr_idx + 1];
                const auto& delay_word = delay_slot_struct.getCPtr()->word;
                const auto& delay_vram = delay_slot_struct.getCPtr()->vram;
                rabbitizer::InstructionR5900 delay_slot_instr(delay_word, delay_vram);
                file << "        ";
                translate_instruction(delay_slot_instr, file);
            }
            
            emit_branch_target(target_addr, func, file, "        ");
            file << "    } else {\n";
            emit_branch_target(fall_through_addr, func, file, "        ");
            file << "    }\n";
            break;
        }

        case RABBITIZER_INSTR_ID_cpu_bgtzl: {
            uint32_t target_addr = current_pc + 4 + (static_cast<int16_t>(instr.Get_immediate()) << 2);
            uint32_t fall_through_addr = current_pc + 8;
            
            file << "if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".SL[0] > 0) {\n";
            
            // Execute delay slot ONLY when branch is taken
            if (current_instr_idx + 1 < block.instructions.size()) {
                const auto& delay_slot_struct = block.instructions[current_instr_idx + 1];
                const auto& delay_word = delay_slot_struct.getCPtr()->word;
                const auto& delay_vram = delay_slot_struct.getCPtr()->vram;
                rabbitizer::InstructionR5900 delay_slot_instr(delay_word, delay_vram);
                file << "        ";
                translate_instruction(delay_slot_instr, file);
            }
            
            emit_branch_target(target_addr, func, file, "        ");
            file << "    } else {\n";
            emit_branch_target(fall_through_addr, func, file, "        ");
            file << "    }\n";
            break;
        }

        case RABBITIZER_INSTR_ID_cpu_bltzl: {
            uint32_t target_addr = current_pc + 4 + (static_cast<int16_t>(instr.Get_immediate()) << 2);
            uint32_t fall_through_addr = current_pc + 8;
            
            file << "if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".SL[0] < 0) {\n";
            
            // Execute delay slot ONLY when branch is taken
            if (current_instr_idx + 1 < block.instructions.size()) {
                const auto& delay_slot_struct = block.instructions[current_instr_idx + 1];
                const auto& delay_word = delay_slot_struct.getCPtr()->word;
                const auto& delay_vram = delay_slot_struct.getCPtr()->vram;
                rabbitizer::InstructionR5900 delay_slot_instr(delay_word, delay_vram);
                file << "        ";
                translate_instruction(delay_slot_instr, file);
            }
            
            emit_branch_target(target_addr, func, file, "        ");
            file << "    } else {\n";
            emit_branch_target(fall_through_addr, func, file, "        ");
            file << "    }\n";
            break;
        }

        case RABBITIZER_INSTR_ID_cpu_bgezl: {
            uint32_t target_addr = current_pc + 4 + (static_cast<int16_t>(instr.Get_immediate()) << 2);
            uint32_t fall_through_addr = current_pc + 8;
            
            file << "if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".SL[0] >= 0) {\n";
            
            // Execute delay slot ONLY when branch is taken
            if (current_instr_idx + 1 < block.instructions.size()) {
                const auto& delay_slot_struct = block.instructions[current_instr_idx + 1];
                const auto& delay_word = delay_slot_struct.getCPtr()->word;
                const auto& delay_vram = delay_slot_struct.getCPtr()->vram;
                rabbitizer::InstructionR5900 delay_slot_instr(delay_word, delay_vram);
                file << "        ";
                translate_instruction(delay_slot_instr, file);
            }
            
            emit_branch_target(target_addr, func, file, "        ");
            file << "    } else {\n";
            emit_branch_target(fall_through_addr, func, file, "        ");
            file << "    }\n";
            break;
        }

        case RABBITIZER_INSTR_ID_cpu_bne: {
            uint32_t target_addr = current_pc + 4 + (static_cast<int16_t>(instr.Get_immediate()) << 2);
            uint32_t fall_through_addr = current_pc + 8;
            
            file << "if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] != " 
                 << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0]) {\n";
            emit_branch_target(target_addr, func, file, "        ");
            file << "    } else {\n";
            emit_branch_target(fall_through_addr, func, file, "        ");
            file << "    }\n";
            break;
        }
        
        case RABBITIZER_INSTR_ID_cpu_beqz: {
            uint32_t target_addr = current_pc + 4 + (static_cast<int16_t>(instr.Get_immediate()) << 2);
            uint32_t fall_through_addr = current_pc + 8;
            
            file << "if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] == 0) {\n";
            emit_branch_target(target_addr, func, file, "        ");
            file << "    } else {\n";
            emit_branch_target(fall_through_addr, func, file, "        ");
            file << "    }\n";
            break;
        }
        
        case RABBITIZER_INSTR_ID_cpu_bnez: {
            uint32_t target_addr = current_pc + 4 + (static_cast<int16_t>(instr.Get_immediate()) << 2);
            uint32_t fall_through_addr = current_pc + 8;
            
            file << "if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] != 0) {\n";
            emit_branch_target(target_addr, func, file, "        ");
            file << "    } else {\n";
            emit_branch_target(fall_through_addr, func, file, "        ");
            file << "    }\n";
            break;
        }
        
        case RABBITIZER_INSTR_ID_cpu_bgtz: {
            uint32_t target_addr = current_pc + 4 + (static_cast<int16_t>(instr.Get_immediate()) << 2);
            uint32_t fall_through_addr = current_pc + 8;
            
            file << "if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".SL[0] > 0) {\n";
            emit_branch_target(target_addr, func, file, "        ");
            file << "    } else {\n";
            emit_branch_target(fall_through_addr, func, file, "        ");
            file << "    }\n";
            break;
        }
        
        case RABBITIZER_INSTR_ID_cpu_blez: {
            uint32_t target_addr = current_pc + 4 + (static_cast<int16_t>(instr.Get_immediate()) << 2);
            uint32_t fall_through_addr = current_pc + 8;
            
            file << "if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".SL[0] <= 0) {\n";
            emit_branch_target(target_addr, func, file, "        ");
            file << "    } else {\n";
            emit_branch_target(fall_through_addr, func, file, "        ");
            file << "    }\n";
            break;
        }
        
        case RABBITIZER_INSTR_ID_cpu_bltz: {
            uint32_t target_addr = current_pc + 4 + (static_cast<int16_t>(instr.Get_immediate()) << 2);
            uint32_t fall_through_addr = current_pc + 8;
            
            file << "if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".SL[0] < 0) {\n";
            emit_branch_target(target_addr, func, file, "        ");
            file << "    } else {\n";
            emit_branch_target(fall_through_addr, func, file, "        ");
            file << "    }\n";
            break;
        }
        
        case RABBITIZER_INSTR_ID_cpu_bgez: {
            uint32_t target_addr = current_pc + 4 + (static_cast<int16_t>(instr.Get_immediate()) << 2);
            uint32_t fall_through_addr = current_pc + 8;
            
            file << "if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".SL[0] >= 0) {\n";
            emit_branch_target(target_addr, func, file, "        ");
            file << "    } else {\n";
            emit_branch_target(fall_through_addr, func, file, "        ");
            file << "    }\n";
            break;
        }
        
        // Unconditional jump
        case RABBITIZER_INSTR_ID_cpu_j: {
            uint32_t target_addr = (current_pc & 0xF0000000) | (instr.Get_instr_index() << 2);
            emit_branch_target(target_addr, func, file, "");
            break;
        }
        
        case RABBITIZER_INSTR_ID_cpu_b: {
            uint32_t target_addr = current_pc + 4 + (static_cast<int16_t>(instr.Get_immediate()) << 2);
            emit_branch_target(target_addr, func, file, "");
            break;
        }
        
        // CHANGE: Function calls with proper return handling
        case RABBITIZER_INSTR_ID_cpu_jal: {
            uint32_t target_addr = (current_pc & 0xF0000000) | (instr.Get_instr_index() << 2);
            uint32_t return_addr = current_pc + 8;
            file << "   // JAL was called \n";
            file << "   // The address after JAL is: 0x" << std::hex << return_addr << "\n";
            if (func.blocks.size() > current_block_idx + 1){
                file << "   // The next block should be: " << func.blocks[current_block_idx + 1].start_address << std::endl;
            }
            else{
                file << "   // THIS IS THE END OF THE BLOCK: 0x" << std::hex << block.end_address << std::endl;
            }
            file << get_gpr_name(31) << ".UL[0] = 0x" << std::hex << return_addr << ";\n";
            file << "    if (recompiled_functions.count(0x" << std::hex << target_addr << ")) {\n";
            file << "        recompiled_functions[0x" << std::hex << target_addr << "](ctx, 0x" << std::hex << target_addr << ");\n";
            
            // After return, check if we continue in this function
            bool found_return = false;
            for (size_t i = 0; i < func.blocks.size(); ++i) {
            if (func.blocks[i].start_address == return_addr) {
                    file << "        goto Label_" << std::setw(4) << std::setfill('0') << i << ";\n";
                    found_return = true;
                    break;
                }
            }
            if (!found_return) {
                file << "\n";
            }
            
            file << "    } else {\n";
            file << "        ctx.cpuRegs.pc = 0x" << std::hex << target_addr << ";\n";
            file << "    }\n";
            break;
        }
        
        // Return from function
        case RABBITIZER_INSTR_ID_cpu_jr: {
            if (static_cast<uint8_t>(instr.GetO32_rs()) == 31) {
                file << "return; // Return from function\n";
            } else {
                // CHANGE: Indirect jump with function lookup
                file << "{\n";
                file << "        uint32_t target = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0];\n";
                file << "        auto func_ptr = find_containing_function(target);\n";
                file << "        if (func_ptr != nullptr) {\n";
                file << "            func_ptr(ctx, target);\n";
                file << "            return;\n";
                file << "        } else {\n";
                file << "            ctx.cpuRegs.pc = target;\n";
                file << "            return;\n";
                file << "        }\n";
                file << "    }\n";
            }
            break;
        }
        
        case RABBITIZER_INSTR_ID_cpu_jalr: {
            uint8_t rd = static_cast<uint8_t>(instr.GetO32_rd());
            uint8_t rs = static_cast<uint8_t>(instr.GetO32_rs());
            uint32_t return_addr = current_pc + 8;
            
            file << get_gpr_name(rd) << ".UL[0] = 0x" << std::hex << return_addr << ";\n";
            file << "    {\n";
            file << "        uint32_t target = " << get_gpr_name(rs) << ".UL[0];\n";
            file << "        auto func_ptr = find_containing_function(target);\n";
            file << "        if (func_ptr != nullptr) {\n";
            file << "            func_ptr(ctx, target);\n";
            
            // Continue after return
            bool found_return = false;
            for (size_t i = 0; i < func.blocks.size(); ++i) {
                if (func.blocks[i].start_address == return_addr) {
                    file << "            goto Label_" << std::setw(4) << std::setfill('0') << i << ";\n";
                    found_return = true;
                    break;
                }
            }
            if (!found_return) {
                file << "            return;\n";
            }
            
            file << "        } else {\n";
            file << "            ctx.cpuRegs.pc = target;\n";
            file << "            return;\n";
            file << "        }\n";
            file << "    }\n";
            break;
        }
        
        default:
            file << "// Unhandled control flow: " << instr.getOpcodeName() << "\n";
            file << "    return;\n";
            break;
    }
}

// CHANGE: New helper function to emit branch target code
void Recompiler::emit_branch_target(uint32_t target_addr, const Function& func, std::ofstream& file, const std::string& indent) {
    // Check if target is within current function
    bool found_internal = false;
    for (size_t i = 0; i < func.blocks.size(); ++i) {
        if (func.blocks[i].start_address == target_addr) {
            file << indent << "goto Label_" << std::setw(4) << std::setfill('0') << i << ";\n";
            found_internal = true;
            break;
        }
    }
    
    if (!found_internal) {
        // CHANGE: Target is outside current function - call it
        file << indent << "// THIS WAS GENERATED BY EMIT BRANCH TARGET" << std::endl;
        file << indent << "if (recompiled_functions.count(0x" << std::hex << target_addr << ")) {\n";
        file << indent << "    recompiled_functions[0x" << std::hex << target_addr << "](ctx, 0x" << std::hex << target_addr << ");\n";
        file << indent << "    return;\n";
        file << indent << "} else {\n";
        file << indent << "    ctx.cpuRegs.pc = 0x" << std::hex << target_addr << ";\n";
        file << indent << "    return;\n";
        file << indent << "}\n";
    }
}

// ALL INSTRUCTION TRANSLATIONS BELOW ARE UNCHANGED - PRESERVED EXACTLY
void Recompiler::translate_instruction(const rabbitizer::InstructionR5900& instr, std::ofstream& file) {
    switch (instr.getUniqueId()) {
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
        case RABBITIZER_INSTR_ID_cpu_sll:
             file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UL[0] = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0] << " << std::to_string(instr.Get_sa()) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_srl:
             file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UL[0] = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0] >> " << std::to_string(instr.Get_sa()) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_sra:
             file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".SL[0] = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".SL[0] >> " << std::to_string(instr.Get_sa()) << ";\n";
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
        case RABBITIZER_INSTR_ID_cpu_beq:
            file << "    if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] == " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0]) ctx.cpuRegs.pc += " << format_imm(static_cast<int32_t>(static_cast<int16_t>(instr.Get_immediate() << 2))) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_bne:
            file << "    if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] != " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0]) ctx.cpuRegs.pc += " << format_imm(static_cast<int32_t>(static_cast<int16_t>(instr.Get_immediate() << 2))) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_blez:
            file << "    if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".SL[0] <= 0) ctx.cpuRegs.pc += " << format_imm(static_cast<int32_t>(static_cast<int16_t>(instr.Get_immediate() << 2))) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_bgtz:
            file << "    if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".SL[0] > 0) ctx.cpuRegs.pc += " << format_imm(static_cast<int32_t>(static_cast<int16_t>(instr.Get_immediate() << 2))) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_bltz:
            file << "    if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".SL[0] < 0) ctx.cpuRegs.pc += " << format_imm(static_cast<int32_t>(static_cast<int16_t>(instr.Get_immediate() << 2))) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_bgez:
            file << "    if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".SL[0] >= 0) ctx.cpuRegs.pc += " << format_imm(static_cast<int32_t>(static_cast<int16_t>(instr.Get_immediate() << 2))) << ";\n";
            break;
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
        case RABBITIZER_INSTR_ID_cpu_ld:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UD[0] = memory::read<uint64_t>(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] + " << format_imm(static_cast<int16_t>(instr.Get_immediate())) << ");\n";
            break;
        case RABBITIZER_INSTR_ID_r5900_sq:
            file << "    // sq instruction - 128-bit store\n";
            file << "    memory::write_quad(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] + " << format_imm(static_cast<int16_t>(instr.Get_immediate())) << ", *reinterpret_cast<const QuadWord*>(&" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << "));\n";
            break;
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
            file << "    if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] != 0) ctx.cpuRegs.pc += " << format_imm(static_cast<int32_t>(static_cast<int16_t>(instr.Get_immediate() << 2))) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_bnel:
            file << "    if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] != " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0]) { ctx.cpuRegs.pc += " << format_imm(static_cast<int32_t>(static_cast<int16_t>(instr.Get_immediate() << 2))) << "; } else { /* Branch likely, nullify delay slot */ }\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_beql:
             file << " /* Some reason beql is being executed here. Check to see if beql is being executed by translate_control_flow. Branch likely, nullify delay slot */ }\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_blezl:
            file << "    if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".SL[0] <= 0) { ctx.cpuRegs.pc += " << format_imm(static_cast<int32_t>(static_cast<int16_t>(instr.Get_immediate() << 2))) << "; } else { /* Branch likely, nullify delay slot */ }\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_bltzl:
            file << "    if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".SL[0] < 0) { ctx.cpuRegs.pc += " << format_imm(static_cast<int32_t>(static_cast<int16_t>(instr.Get_immediate() << 2))) << "; } else { /* Branch likely, nullify delay slot */ }\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_bgezl:
            file << "    if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".SL[0] >= 0) { ctx.cpuRegs.pc += " << format_imm(static_cast<int32_t>(static_cast<int16_t>(instr.Get_immediate() << 2))) << "; } else { /* Branch likely, nullify delay slot */ }\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_daddiu:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".SD[0] = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".SD[0] + " << format_imm(static_cast<int16_t>(instr.Get_immediate())) << ";\n";
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
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0] = (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] < " << format_imm(static_cast<uint16_t>(instr.Get_immediate())) << ") ? 1 : 0;\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_sltu:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UL[0] = (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] < " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0]) ? 1 : 0;\n";
            break;
        case RABBITIZER_INSTR_ID_r5900_lq:
            file << "    // lq instruction - 128-bit load\n";
            file << "    memory::read_quad(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] + " << format_imm(static_cast<int16_t>(instr.Get_immediate())) << ", *reinterpret_cast<QuadWord*>(&" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << "));\n";
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
        case RABBITIZER_INSTR_ID_cpu_syscall:
            file << "    runtime_syscall_dispatcher(" << get_gpr_name(3) << ".UL[0], ctx);\n";
            break;
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
        case RABBITIZER_INSTR_ID_r5900_pcpyld:
            // Parallel Copy Lower Doubleword
            file << "    // pcpyld - Parallel Copy Lower Doubleword\n";
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UD[1] = " 
                << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UD[0];\n";
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UD[0] = " 
                << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UD[0];\n";
            break;

        case RABBITIZER_INSTR_ID_r5900_pcpyh:
            // Parallel Copy Halfword - broadcast lowest halfword to all 8 positions
            file << "    // pcpyh - Parallel Copy Halfword\n";
            file << "    {\n";
            file << "        uint16_t hw = static_cast<uint16_t>(" 
                << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0] & 0xFFFF);\n";
            file << "        for (int i = 0; i < 8; i++) {\n";
            file << "            " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".US[i] = hw;\n";
            file << "        }\n";
            file << "    }\n";
            break;

        case RABBITIZER_INSTR_ID_cpu_dsll:
            // Doubleword Shift Left Logical
            file << "    // dsll - Doubleword Shift Left Logical\n";
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UD[0] = " 
                << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UD[0] << " 
                << std::to_string(instr.Get_sa()) << ";\n";
            break;

        case RABBITIZER_INSTR_ID_cpu_dsll32:
            // Doubleword Shift Left Logical + 32
            file << "    // dsll32 - Doubleword Shift Left Logical + 32\n";
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UD[0] = " 
                << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UD[0] << " 
                << std::to_string(instr.Get_sa() + 32) << ";\n";
            break;

        case RABBITIZER_INSTR_ID_cpu_dsllv:
            // Doubleword Shift Left Logical Variable
            file << "    // dsllv - Doubleword Shift Left Logical Variable\n";
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UD[0] = " 
                << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UD[0] << (" 
                << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] & 0x3F);\n";
            break;

        case RABBITIZER_INSTR_ID_cpu_dsrl:
            // Doubleword Shift Right Logical
            file << "    // dsrl - Doubleword Shift Right Logical\n";
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UD[0] = " 
                << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UD[0] >> " 
                << std::to_string(instr.Get_sa()) << ";\n";
            break;

        case RABBITIZER_INSTR_ID_cpu_dsra:
            // Doubleword Shift Right Arithmetic
            file << "    // dsra - Doubleword Shift Right Arithmetic\n";
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".SD[0] = " 
                << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".SD[0] >> " 
                << std::to_string(instr.Get_sa()) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_nop:
            file << "   //nop \n";
            break;
        case RABBITIZER_INSTR_ID_r5900_ei:
            file << "    ctx.cop0.n.Status |= 0x1;\n";
            break;
        default:
            file << "    // ----------------------------------------------------------------\n";
            file << "    // UNHANDLED INSTRUCTION: " << instr.getOpcodeName() << "\n";
            file << "    // Opcode: 0x" << std::hex << static_cast<int>(instr.Get_opcode()) << "\n";
            file << "    // Function: 0x" << std::hex << static_cast<int>(instr.Get_function()) << "\n";
            file << "    // Immediate: 0x" << std::hex << instr.Get_immediate() << "\n";
            file << "    // Address: 0x" << std::hex << instr.getVram() << "\n";
            file << "    // ----------------------------------------------------------------\n";
            file << "      g_logFile << \"Unhandled OP Code: 0x\" << std::hex << 0x" << std::hex << instr.getVram() << " << \" Instruction: \" << \"" << instr.getOpcodeName() << "\";\n";
            file << "    exit(1);\n";

            std::cout << "    // ----------------------------------------------------------------\n";
            std::cout << "    // UNHANDLED INSTRUCTION: " << instr.getOpcodeName() << "\n";
            std::cout << "    // Opcode: 0x" << std::hex << static_cast<int>(instr.Get_opcode()) << "\n";
            std::cout << "    // Function: 0x" << std::hex << static_cast<int>(instr.Get_function()) << "\n";
            std::cout << "    // Immediate: 0x" << std::hex << instr.Get_immediate() << "\n";
            std::cout << "    // Address: 0x" << std::hex << instr.getVram() << "\n";
            std::cout << "    // ----------------------------------------------------------------\n";
            std::cout << "      g_logFile << \"Unhandled OP Code: 0x\" << std::hex << 0x" << std::hex << instr.getVram() << " << \" Instruction: \" << \"" << instr.getOpcodeName() << "\";\n";
            std::cout << "    exit(1);\n";
    }
}