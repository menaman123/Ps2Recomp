#include "recompiler_v2.h"
#include "instructions/InstructionR5900.hpp"
#include "instructions/Registers.hpp"
#include "rabbitizer.hpp"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <vector>
#include <string>
#include <algorithm>
#include "EEAnalyze/Function.h"
#include "host_app/cpu_state.h"
#include "host_app/memory.h"
#include "host_app/syscalls.h"
#include "generated/Registers_enum_classes.hpp"

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

RecompilerV2::RecompilerV2(const uint8_t* elf_data, uint32_t elf_size, 
                           uint32_t text_base, uint32_t text_size, 
                           std::set<uint32_t> block_entries)
    : elf_data(elf_data), elf_size(elf_size), 
      text_base(text_base), text_size(text_size), 
      block_entries(block_entries) {
}

uint32_t RecompilerV2::read_instruction(uint32_t address) const {
    if (address < text_base || address >= text_base + text_size) {
        return 0; // NOP for out of bounds
    }
    uint32_t offset = address - text_base;
    return *reinterpret_cast<const uint32_t*>(elf_data + offset);
}

// NEED TO FIX THIS*

void RecompilerV2::analyze() {
    std::cout << "[Recompiler] Starting analysis...\n";
    
    // Step 1: Find all jump targets
    find_all_jump_targets();
    std::cout << "[Recompiler] Found " << all_jump_targets.size() << " jump targets\n";
    
    // Step 2: Create blocks
    create_blocks();
    std::cout << "[Recompiler] Created " << blocks.size() << " code blocks\n";
    
    // Step 3: Analyze each block
    for (auto& [addr, block] : blocks) {
        analyze_block(block);
    }
}

void RecompilerV2::find_all_jump_targets() {
    // Entry point is always a target
    all_jump_targets.insert(text_base);
    all_entry_points.insert(text_base);
    
    // Scan all instructions for branches and jumps
    for (uint32_t pc = text_base; pc < text_base + text_size; pc += 4) {
        uint32_t instr_word = read_instruction(pc);
        if (instr_word == 0) continue;
        
        rabbitizer::InstructionR5900 instr(instr_word, pc);
        
        if (instr.isBranch()) {
            // Get branch target
            uint32_t target = instr.getBranchVramGeneric();
            std::cout << "BRANCH TARGET: 0x" << std::hex << target << std::endl;
            
            std::cout << "BRANCH Instruction address: 0x" << std::hex << pc << std::endl; 

            if (target >= text_base && target < text_base + text_size) {
                all_jump_targets.insert(target);
            }
            // Instruction after delay slot is also a target
            all_jump_targets.insert(pc + 8);
        }
        else if (instr.isJump()) {
            if (instr.getCPtr()->uniqueId == RABBITIZER_INSTR_ID_cpu_j ||
                instr.getCPtr()->uniqueId == RABBITIZER_INSTR_ID_cpu_jal) {
                // Direct jump
                uint32_t target = (instr.Get_instr_index() << 2);
                std::cout << "DIRECT JUMP TARGET: 0x" << std::hex << target << std::endl;
                std::cout << "DIRECT JUMP Instruction address: 0x" << std::hex << pc << std::endl; 

                all_jump_targets.insert(target);
                if (instr.getCPtr()->uniqueId == RABBITIZER_INSTR_ID_cpu_jal) {
                    all_entry_points.insert(target);
                }
                
            }
            // Instruction after delay slot is a potential target
            if (instr.getCPtr()->uniqueId == RABBITIZER_INSTR_ID_cpu_jal ||
                instr.getCPtr()->uniqueId == RABBITIZER_INSTR_ID_cpu_jalr) {
                all_jump_targets.insert(pc + 8);
            }
        }
    }
}
// HEREEE
void RecompilerV2::create_blocks() {
    std::vector<uint32_t> sorted_targets(all_jump_targets.begin(), all_jump_targets.end());
    std::sort(sorted_targets.begin(), sorted_targets.end());
    
    for (size_t i = 0; i < sorted_targets.size(); ++i) {
        CodeBlock block;
        block.start_address = sorted_targets[i];
        
        // End at next target or end of text section
        if (i + 1 < sorted_targets.size()) {
            block.end_address = sorted_targets[i + 1];
        } else {
            block.end_address = text_base + text_size;
        }
        
        // Read all instructions in block
        for (uint32_t pc = block.start_address; pc < block.end_address; pc += 4) {
            uint32_t instr = read_instruction(pc);
            block.instructions.push_back(instr);
            
            // Check if this ends the block
            if (instr != 0) {
                rabbitizer::InstructionR5900 decoded(instr, pc);
                if (decoded.isBranch() || decoded.isJump()) {
                    // Block ends after delay slot
                    if (pc + 4 < block.end_address) {
                        block.instructions.push_back(read_instruction(pc + 4));
                    }
                    block.end_address = pc + 8;
                    break;
                }
            }
        }
        
        blocks[block.start_address] = block;
    }
}

void RecompilerV2::analyze_block(CodeBlock& block) {
    if (block.instructions.empty()) return;
    
    uint32_t pc = block.start_address;
    for (size_t i = 0; i < block.instructions.size(); ++i) {
        rabbitizer::InstructionR5900 instr(block.instructions[i], pc);
        
        if (instr.isBranch()) {
            block.ends_with_branch = true;
            uint32_t target = instr.getBranchVramGeneric();
            block.jump_targets.insert(target);
            block.jump_targets.insert(pc + 8); // Fall-through
        }
        else if (instr.isJump()) {
            if (instr.getCPtr()->uniqueId == RABBITIZER_INSTR_ID_cpu_jr &&
                instr.GetO32_rs() == rabbitizer::Registers::Cpu::GprO32::GPR_O32_ra) { // $ra
                block.ends_with_return = true;
            }
            else if (instr.getCPtr()->uniqueId == RABBITIZER_INSTR_ID_cpu_jr) {
                block.ends_with_indirect_jump = true;
            }
            else {
                // Direct jump
                uint32_t target = (instr.Get_instr_index() << 2);
                block.jump_targets.insert(target);
            }
        }
        
        pc += 4;
    }
}

void RecompilerV2::generate_code(const std::string& output_file) {
    std::ofstream file(output_file);
    if (!file.is_open()) {
        std::cerr << "Failed to open output file: " << output_file << "\n";
        return;
    }
    
    // Headers
    
    file << "#include <map>\n";
    file << "#include <vector>\n";
    file << "#include <functional>\n\n";
    file << "#include \"recompiled.h\"\n";
    file << "#include \"../host_app/cpu_state.h\"\n\n";
    file << "#include \"host_app\\memory.h\"\n";
    file << "#include \"host_app\\syscalls.h\"\n";
    file << "#include <cstdint>\n";
    file << "#include <fstream> \n";
    file << "#include <iomanip> \n";
    file << "#include <sstream>\n";
    file << "#include <iostream>\n\n";

    file << "extern std::ofstream g_logFile;\n\n";
    
    // Main execution function
    file << "void execute_ps2_code(CpuContext& ctx) {\n";
    file << "    uint32_t next_pc = ctx.cpuRegs.pc;\n\n";
    
    // Dispatch loop
    file << "dispatch:\n";
    file << "    switch(ctx.cpuRegs.pc) {\n";
    generate_dispatch_table(file);
    file << "        default:\n";
    file << "            std::cerr << \"Unknown PC: 0x\" << std::hex << ctx.cpuRegs.pc << \"\\n\";\n";
    file << "            return;\n";
    file << "    }\n\n";
    
    // Generate all blocks
    for (const auto& [addr, block] : blocks) {
        file << "L_" << std::hex << addr << ":\n";
        file << "    // Block at 0x" << std::hex << addr << "\n";
        generate_block_code(block, file);
        file << "\n";
    }
    
    // Return point for external calls
    file << "return_from_call:\n";
    file << "    ctx.cpuRegs.pc = next_pc;\n";
    file << "    goto dispatch;\n";
    
    file << "}\n";
    file.close();
}

void RecompilerV2::generate_dispatch_table(std::ofstream& file) {
    for (const auto& [addr, block] : blocks) {
        file << "        case 0x" << std::hex << addr 
             << ": goto L_" << addr << ";\n";
    }
}

void RecompilerV2::generate_block_code(const CodeBlock& block, std::ofstream& file) {
    uint32_t pc = block.start_address;
    
    for (size_t i = 0; i < block.instructions.size(); ++i) {
        if (block.instructions[i] == 0) {
            file << "    // NOP\n";
            pc += 4;
            continue;
        }
        
        rabbitizer::InstructionR5900 instr(block.instructions[i], pc);
        
        // Handle delay slots
        if (instr.hasDelaySlot() && i + 1 < block.instructions.size()) {
            // Execute delay slot first
            rabbitizer::InstructionR5900 delay(block.instructions[i + 1], pc + 4);
            file << "    // Delay slot\n";
            translate_instruction(block.instructions[i + 1], pc, file);
            
            // Then execute branch/jump
            if (instr.isBranch()) {
                generate_branch(block.instructions[i], pc, file);
            } else {
                generate_jump(block.instructions[i], pc, file);
            }
            
            return; // Block ends here
        } else {
            translate_instruction(block.instructions[i], pc, file);
        }
        
        pc += 4;
    }
    
    // Fall through to next block
    if (!block.ends_with_branch && !block.ends_with_return && !block.ends_with_indirect_jump) {
        file << "    goto L_" << std::hex << block.end_address << ";\n";
    }
}

void RecompilerV2::generate_branch(uint32_t instruction, uint32_t pc, std::ofstream& file) {
    rabbitizer::InstructionR5900 instr(instruction, pc);
    uint32_t target = instr.getBranchVramGeneric();
    uint32_t fallthrough = pc + 8;
    
    switch (instr.getUniqueId()) {
        case RABBITIZER_INSTR_ID_cpu_beq:
            file << "    if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] == " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0]) {\n";
            file << "        goto L_" << std::hex << target << ";\n";
            file << "    } else {\n";
            file << "        goto L_" << std::hex << fallthrough << ";\n";
            file << "    }\n";
            break;
            
        case RABBITIZER_INSTR_ID_cpu_bne:
            file << "    if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] != " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0]) {\n";
            file << "        goto L_" << std::hex << target << ";\n";
            file << "    } else {\n";
            file << "        goto L_" << std::hex << fallthrough << ";\n";
            file << "    }\n";
            break;
            
        // Add other branch types...
        default:
            file << "    // Unhandled branch type\n";
            file << "    goto L_" << std::hex << fallthrough << ";\n";
    }
}

void RecompilerV2::generate_jump(uint32_t instruction, uint32_t pc, std::ofstream& file) {
    rabbitizer::InstructionR5900 instr(instruction, pc);
    
    switch (instr.getUniqueId()) {
        case RABBITIZER_INSTR_ID_cpu_j:
            {
                file << "// (J) WHAT ABOUT THIS: " << std::hex << (instr.Get_instr_index() << 2) << std::endl;
                file << "//JUMPING TO: " << std::hex << (instr.Get_instr_index() << 2) <<  std::endl;
                uint32_t target = (instr.Get_instr_index() << 2);
                file << "    goto L_" << std::hex << target << ";\n";
            }
            break;
            
        case RABBITIZER_INSTR_ID_cpu_jal:
            {

                file << " // (JAL) WHAT ABOUT THIS: " << std::hex << (instr.Get_instr_index() << 2) << std::endl;

                uint32_t target = (instr.Get_instr_index() << 2);
                file << "    ctx.cpuRegs.GPR.r[31].UL[0] = 0x" << std::hex << (pc + 8) << ";\n";
                file << "    goto L_" << std::hex << target << ";\n";
            }
            break;
            
        case RABBITIZER_INSTR_ID_cpu_jr:
            if (instr.GetO32_rs() == rabbitizer::Registers::Cpu::GprO32::GPR_O32_ra) {
                file << "    next_pc = ctx.cpuRegs.GPR.r[31].UL[0];\n";
                file << "    goto return_from_call;\n";
            } else {
                file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0];\n";
                file << "    goto dispatch;\n";
            }
            break;
            
        case RABBITIZER_INSTR_ID_cpu_jalr:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UL[0] = 0x" 
                 << std::hex << (pc + 8) << ";\n";
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0];\n";
            file << "    goto dispatch;\n";
            break;
    }
}

void RecompilerV2::translate_instruction(uint32_t instruction, uint32_t pc, std::ofstream& file) {
    rabbitizer::InstructionR5900 instr(instruction, pc);
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
        //
        // MIPS I - Branch instructions
        //
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
        case RABBITIZER_INSTR_ID_cpu_ld:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UD[0] = memory::read<uint64_t>(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] + " << format_imm(static_cast<int16_t>(instr.Get_immediate())) << ");\n";
            break;
        case RABBITIZER_INSTR_ID_r5900_sq:
            file << "    // sq instruction - 128-bit store\n";
            // Cast the GPR_reg to a const QuadWord pointer, then dereference it
            file << "    memory::write_quad(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] + " << format_imm(static_cast<int16_t>(instr.Get_immediate())) << ", *reinterpret_cast<const QuadWord*>(&" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << "));\n";
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
            file << "    if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] != 0) ctx.cpuRegs.pc += " << format_imm(static_cast<int32_t>(static_cast<int16_t>(instr.Get_immediate() << 2))) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_bnel:
            file << "    if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] != " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0]) { ctx.cpuRegs.pc += " << format_imm(static_cast<int32_t>(static_cast<int16_t>(instr.Get_immediate() << 2))) << "; } else { /* Branch likely, nullify delay slot */ }\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_beql:
             file << "    if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] == " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0]) { ctx.cpuRegs.pc += " << format_imm(static_cast<int32_t>(static_cast<int16_t>(instr.Get_immediate() << 2))) << "; } else { /* Branch likely, nullify delay slot */ }\n";
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

        // Doubleword and Logical Instructions
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

        // Load/Store and Move Instructions
        case RABBITIZER_INSTR_ID_r5900_lq:
            file << "    // lq instruction - 128-bit load\n";
            // Cast the GPR_reg to a QuadWord pointer, then dereference it
            file << "    memory::read_quad(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] + " << format_imm(static_cast<int16_t>(instr.Get_immediate())) << ", *reinterpret_cast<QuadWord*>(&" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << "));\n";
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
        // System and MMI Instructions
        case RABBITIZER_INSTR_ID_r5900_pextlw:
            file << "    g_logFile << \"Unhandled OP Code: 0x\" << std::hex << 0x" << std::hex << instr.getVram() << " << \" Instruction: \" << \"" << instr.getOpcodeName() << "\";// pextlw instruction\n";
            file << "    exit(1);\n";
            break;
        case RABBITIZER_INSTR_ID_r5900_pextuw:
            file << "    g_logFile << \"Unhandled OP Code: 0x\" << std::hex << 0x" << std::hex << instr.getVram() << " << \" Instruction: \" << \"" << instr.getOpcodeName() << "\";// pextuw instruction\n";
            file << "    exit(1);\n";
            break;
        case RABBITIZER_INSTR_ID_r5900_ppach:
            file << "    g_logFile << \"Unhandled OP Code: 0x\" << std::hex << 0x" << std::hex << instr.getVram() << " << \" Instruction: \" << \"" << instr.getOpcodeName() << "\";// ppach instruction\n";
            file << "    exit(1);\n";
            break;
        case RABBITIZER_INSTR_ID_r5900_ppacw:
            file << "    g_logFile << \"Unhandled OP Code: 0x\" << std::hex << 0x" << std::hex << instr.getVram() << " << \" Instruction: \" << \"" << instr.getOpcodeName() << "\";// ppacw instruction\n";
            file << "    exit(1);\n";
            break;
        case RABBITIZER_INSTR_ID_r5900_psraw:
            file << "    g_logFile << \"Unhandled OP Code: 0x\" << std::hex << 0x" << std::hex << instr.getVram() << " << \" Instruction: \" << \"" << instr.getOpcodeName() << "\";// psraw instruction\n";
            file << "    exit(1);\n";
            break;
        case RABBITIZER_INSTR_ID_r5900_psrlw:
            file << "    g_logFile << \"Unhandled OP Code: 0x\" << std::hex << 0x" << std::hex << instr.getVram() << " << \" Instruction: \" << \"" << instr.getOpcodeName() << "\";// psrlw instruction\n";
            file << "    exit(1);\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_sync:
            file << "    g_logFile << \"Unhandled OP Code: 0x\" << std::hex << 0x" << std::hex << instr.getVram() << " << \" Instruction: \" << \"" << instr.getOpcodeName() << "\";// sync instruction - memory barrier\n";
            file << "    exit(1);\n";
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
        case RABBITIZER_INSTR_ID_cpu_nop:
            file << "   //nop \n";
            break;
        case RABBITIZER_INSTR_ID_r5900_ei:
            // Enable Interrupts by setting bit 0 of the COP0 Status Register.
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
void RecompilerV2::generate_header(const std::string& header_file) {
    std::ofstream file(header_file);
    if (!file.is_open()) {
        std::cerr << "Failed to open header file: " << header_file << "\n";
        return;
    }
    
    file << "#pragma once\n\n";
    file << "#include \"host_app\\cpu_state.h\"\n\n";
    file << "void execute_ps2_code(CpuContext& ctx);\n";
    
    file.close();
}