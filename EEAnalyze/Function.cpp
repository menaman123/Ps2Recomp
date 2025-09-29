#include "Function.h"
#include "instructions/RabbitizerInstructionR5900.h"
#include <set>
#include <vector>
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <queue>

struct RegisterValue {
    enum Type { UNKNOWN, CONSTANT, BASE_PLUS_OFFSET, SCALED_INDEX } type = UNKNOWN;
    uint32_t constant = 0;
    uint32_t base_reg = 0;
    int32_t offset = 0;
    uint32_t scale = 1;
    uint32_t index_reg = 0;
};

class JumpTableAnalyzer {
private:
    const Function* func;
    std::unordered_map<uint32_t, RegisterValue> register_values;
    
    // Track register values through the function
    void track_register_value(uint32_t reg, const RegisterValue& value) {
        register_values[reg] = value;
    }
    
    RegisterValue get_register_value(uint32_t reg) const {
        auto it = register_values.find(reg);
        return (it != register_values.end()) ? it->second : RegisterValue{};
    }
    
    // Analyze an instruction and update register tracking
    void analyze_instruction(const RabbitizerInstruction& instr) {
        RegisterValue result;
        
        switch (instr.uniqueId) {
            case RABBITIZER_INSTR_ID_cpu_lui: {
                result.type = RegisterValue::CONSTANT;
                result.constant = RabbitizerInstruction_getProcessedImmediate(&instr) << 16;
                track_register_value(RAB_INSTR_GET_rt(&instr), result);
                break;
            }
            
            case RABBITIZER_INSTR_ID_cpu_addiu: {
                uint32_t rt = RAB_INSTR_GET_rt(&instr);
                uint32_t rs = RAB_INSTR_GET_rs(&instr);
                int32_t immediate = RabbitizerInstruction_getProcessedImmediate(&instr);
                
                RegisterValue rs_val = get_register_value(rs);
                if (rs_val.type == RegisterValue::CONSTANT) {
                    result.type = RegisterValue::CONSTANT;
                    result.constant = rs_val.constant + immediate;
                } else if (rs == RABBITIZER_REG_GPR_O32_zero) {
                    result.type = RegisterValue::CONSTANT;
                    result.constant = immediate;
                } else {
                    result.type = RegisterValue::BASE_PLUS_OFFSET;
                    result.base_reg = rs;
                    result.offset = immediate;
                }
                track_register_value(rt, result);
                break;
            }
            
            case RABBITIZER_INSTR_ID_cpu_addu: {
                uint32_t rd = RAB_INSTR_GET_rd(&instr);
                uint32_t rs = RAB_INSTR_GET_rs(&instr);
                uint32_t rt = RAB_INSTR_GET_rt(&instr);
                
                RegisterValue rs_val = get_register_value(rs);
                RegisterValue rt_val = get_register_value(rt);
                
                if (rs_val.type == RegisterValue::CONSTANT && rt_val.type == RegisterValue::CONSTANT) {
                    result.type = RegisterValue::CONSTANT;
                    result.constant = rs_val.constant + rt_val.constant;
                } else if (rs_val.type == RegisterValue::CONSTANT && rt_val.type == RegisterValue::BASE_PLUS_OFFSET) {
                    result.type = RegisterValue::BASE_PLUS_OFFSET;
                    result.base_reg = rt_val.base_reg;
                    result.offset = rt_val.offset + rs_val.constant;
                } else if (rt_val.type == RegisterValue::CONSTANT && rs_val.type == RegisterValue::BASE_PLUS_OFFSET) {
                    result.type = RegisterValue::BASE_PLUS_OFFSET;
                    result.base_reg = rs_val.base_reg;
                    result.offset = rs_val.offset + rt_val.constant;
                }
                track_register_value(rd, result);
                break;
            }
            
            case RABBITIZER_INSTR_ID_cpu_sll: {
                uint32_t rd = RAB_INSTR_GET_rd(&instr);
                uint32_t rt = RAB_INSTR_GET_rt(&instr);
                uint32_t sa = RAB_INSTR_GET_sa(&instr);
                
                RegisterValue rt_val = get_register_value(rt);
                if (rt_val.type == RegisterValue::CONSTANT) {
                    result.type = RegisterValue::CONSTANT;
                    result.constant = rt_val.constant << sa;
                } else if (sa == 2) { // Common for address scaling (multiply by 4)
                    result.type = RegisterValue::SCALED_INDEX;
                    result.index_reg = rt;
                    result.scale = 4;
                }
                track_register_value(rd, result);
                break;
            }
            
            // Handle other relevant instructions...
            default:
                // For instructions that modify registers we don't handle,
                // mark the destination as unknown
                if (RabbitizerInstrDescriptor_modifiesRt(instr.descriptor)) {
                    track_register_value(RAB_INSTR_GET_rt(&instr), RegisterValue{});
                }
                if (RabbitizerInstrDescriptor_modifiesRd(instr.descriptor)) {
                    track_register_value(RAB_INSTR_GET_rd(&instr), RegisterValue{});
                }
                break;
        }
    }
    
public:
    JumpTableAnalyzer(const Function* f) : func(f) {}
    
    struct JumpTableInfo {
        uint32_t table_address = 0;
        int32_t num_cases = 0;
        uint32_t switch_instruction_addr = 0;
        std::vector<uint32_t> case_targets;
        bool valid = false;
    };
    
    JumpTableInfo analyze_potential_jump_table(uint32_t jr_instruction_addr, 
                                               const uint8_t* function_code, 
                                               uint32_t function_size,
                                               const uint8_t* elf_data, 
                                               uint32_t elf_size) {
        JumpTableInfo info;
        register_values.clear();
        
        std::cout << "        [Analysis] Starting detailed jump table analysis for jr at 0x" 
                  << std::hex << jr_instruction_addr << std::dec << std::endl;
        
        // Find the instruction offset within the function
        if (jr_instruction_addr < func->base_address || 
            jr_instruction_addr >= func->base_address + function_size) {
            std::cout << "        [Analysis] ERROR: jr instruction address 0x" << std::hex << jr_instruction_addr 
                      << " is outside function bounds [0x" << func->base_address 
                      << " - 0x" << (func->base_address + function_size) << "]" << std::dec << std::endl;
            return info;
        }
        
        uint32_t jr_offset = jr_instruction_addr - func->base_address;
        uint32_t jr_instruction_word = *(reinterpret_cast<const uint32_t*>(function_code + jr_offset));
        
        RabbitizerInstruction jr_instr;
        RabbitizerInstructionR5900_init(&jr_instr, jr_instruction_word, jr_instruction_addr);
        RabbitizerInstructionR5900_processUniqueId(&jr_instr);
        
        if (jr_instr.uniqueId != RABBITIZER_INSTR_ID_cpu_jr) {
            std::cout << "        [Analysis] ERROR: Instruction is not a jr (uniqueId: " 
                      << jr_instr.uniqueId << ")" << std::endl;
            RabbitizerInstruction_destroy(&jr_instr);
            return info;
        }
        
        if (RAB_INSTR_GET_rs(&jr_instr) == RABBITIZER_REG_GPR_O32_ra) {
            std::cout << "        [Analysis] SKIP: jr instruction uses $ra register (function return, not jump table)" << std::endl;
            RabbitizerInstruction_destroy(&jr_instr);
            return info;
        }
        
        uint32_t jump_reg = RAB_INSTR_GET_rs(&jr_instr);
        std::cout << "        [Analysis] Found indirect jump using register $" << jump_reg << std::endl;
        RabbitizerInstruction_destroy(&jr_instr);
        
        // Analyze all instructions up to the jump to build register state
        std::cout << "        [Analysis] Building register state by analyzing " 
                  << (jr_offset / 4) << " instructions..." << std::endl;
        int meaningful_instructions = 0;
        for (uint32_t offset = 0; offset < jr_offset; offset += 4) {
            uint32_t instruction_word = *(reinterpret_cast<const uint32_t*>(function_code + offset));
            RabbitizerInstruction instr;
            RabbitizerInstructionR5900_init(&instr, instruction_word, func->base_address + offset);
            RabbitizerInstructionR5900_processUniqueId(&instr);
            
            // Track meaningful instructions for debugging
            if (instr.uniqueId == RABBITIZER_INSTR_ID_cpu_lui || 
                instr.uniqueId == RABBITIZER_INSTR_ID_cpu_addiu || 
                instr.uniqueId == RABBITIZER_INSTR_ID_cpu_addu || 
                instr.uniqueId == RABBITIZER_INSTR_ID_cpu_sll) {
                meaningful_instructions++;
            }
            
            analyze_instruction(instr);
            RabbitizerInstruction_destroy(&instr);
        }
        std::cout << "        [Analysis] Found " << meaningful_instructions << " meaningful instructions for register tracking" << std::endl;
        
        // Now work backwards from the jump to find the load instruction
        uint32_t lw_instruction_addr = 0;
        uint32_t table_base_reg = 0;
        int32_t table_offset = 0;
        int instructions_scanned = 0;
        
        std::cout << "        [Analysis] Searching backwards for lw instruction that loads register $" << jump_reg << "..." << std::endl;
        
        // Look for the lw instruction that loads the jump register
        for (int32_t back_offset = jr_offset - 4; back_offset >= 0; back_offset -= 4) {
            instructions_scanned++;
            uint32_t instruction_word = *(reinterpret_cast<const uint32_t*>(function_code + back_offset));
            RabbitizerInstruction instr;
            RabbitizerInstructionR5900_init(&instr, instruction_word, func->base_address + back_offset);
            RabbitizerInstructionR5900_processUniqueId(&instr);
            
            if (instr.uniqueId == RABBITIZER_INSTR_ID_cpu_lw && 
                RAB_INSTR_GET_rt(&instr) == jump_reg) {
                lw_instruction_addr = func->base_address + back_offset;
                table_base_reg = RAB_INSTR_GET_rs(&instr);
                table_offset = RabbitizerInstruction_getProcessedImmediate(&instr);
                
                char buffer[256];
                RabbitizerInstruction_disassemble(&instr, buffer, nullptr, 0, 0);
                std::cout << "        [Analysis] Found target lw instruction at 0x" << std::hex << lw_instruction_addr 
                          << ": " << buffer << std::dec << std::endl;
                std::cout << "        [Analysis] Table base register: $" << table_base_reg 
                          << ", offset: " << table_offset << std::endl;
                
                RabbitizerInstruction_destroy(&instr);
                break;
            }
            RabbitizerInstruction_destroy(&instr);
        }
        
        if (lw_instruction_addr == 0) {
            std::cout << "        [Analysis] FAIL: Could not find lw instruction that loads register $" << jump_reg 
                      << " (scanned " << instructions_scanned << " instructions backwards)" << std::endl;
            return info;
        }
        
        // Get the value of the table base register
        RegisterValue base_reg_value = get_register_value(table_base_reg);
        uint32_t computed_table_address = 0;
        
        std::cout << "        [Analysis] Analyzing base register $" << table_base_reg << " state..." << std::endl;
        
        if (base_reg_value.type == RegisterValue::CONSTANT) {
            computed_table_address = base_reg_value.constant + table_offset;
            std::cout << "        [Analysis] Base register is constant: 0x" << std::hex << base_reg_value.constant 
                      << " + " << std::dec << table_offset 
                      << " = 0x" << std::hex << computed_table_address << std::dec << std::endl;
        } else if (base_reg_value.type == RegisterValue::BASE_PLUS_OFFSET) {
            // This might be a more complex addressing mode
            RegisterValue base_base_value = get_register_value(base_reg_value.base_reg);
            std::cout << "        [Analysis] Base register is base+offset: $" << base_reg_value.base_reg 
                      << " + " << base_reg_value.offset << std::endl;
            
            if (base_base_value.type == RegisterValue::CONSTANT) {
                computed_table_address = base_base_value.constant + base_reg_value.offset + table_offset;
                std::cout << "        [Analysis] Computed address: 0x" << std::hex << base_base_value.constant 
                          << " + " << std::dec << base_reg_value.offset << " + " << table_offset 
                          << " = 0x" << std::hex << computed_table_address << std::dec << std::endl;
            } else {
                std::cout << "        [Analysis] FAIL: Base-base register $" << base_reg_value.base_reg 
                          << " has unknown state (type: " << (int)base_base_value.type << ")" << std::endl;
                return info;
            }
        } else {
            std::cout << "        [Analysis] FAIL: Base register $" << table_base_reg 
                      << " has unsupported state type: " << (int)base_reg_value.type << std::endl;
            std::cout << "        [Analysis] Supported types: CONSTANT(" << (int)RegisterValue::CONSTANT 
                      << "), BASE_PLUS_OFFSET(" << (int)RegisterValue::BASE_PLUS_OFFSET << ")" << std::endl;
            return info;
        }
        
        // Look for bounds checking to determine number of cases
        int32_t num_cases = -1;
        uint32_t bounds_check_addr = 0;
        instructions_scanned = 0;
        
        std::cout << "        [Analysis] Searching for bounds checking (sltiu/slti)..." << std::endl;
        
        for (int32_t back_offset = jr_offset - 4; back_offset >= 0; back_offset -= 4) {
            instructions_scanned++;
            uint32_t instruction_word = *(reinterpret_cast<const uint32_t*>(function_code + back_offset));
            RabbitizerInstruction instr;
            RabbitizerInstructionR5900_init(&instr, instruction_word, func->base_address + back_offset);
            RabbitizerInstructionR5900_processUniqueId(&instr);
            
            if (instr.uniqueId == RABBITIZER_INSTR_ID_cpu_sltiu || 
                instr.uniqueId == RABBITIZER_INSTR_ID_cpu_slti) {
                num_cases = RabbitizerInstruction_getProcessedImmediate(&instr);
                bounds_check_addr = func->base_address + back_offset;
                
                char buffer[256];
                RabbitizerInstruction_disassemble(&instr, buffer, nullptr, 0, 0);
                std::cout << "        [Analysis] Found bounds check at 0x" << std::hex << bounds_check_addr 
                          << ": " << buffer << std::dec << " -> " << num_cases << " cases" << std::endl;
                
                RabbitizerInstruction_destroy(&instr);
                break;
            }
            RabbitizerInstruction_destroy(&instr);
        }
        
        if (num_cases == -1) {
            std::cout << "        [Analysis] WARNING: No bounds checking found (scanned " 
                      << instructions_scanned << " instructions)" << std::endl;
        }
        
        if (num_cases <= 0 || num_cases > 1000) {
            int32_t original_num_cases = num_cases;
            num_cases = 10; // Conservative estimate
            std::cout << "        [Analysis] WARNING: Invalid case count " << original_num_cases 
                      << ", using conservative estimate: " << num_cases << std::endl;
        }
        
        // Validate that the computed address is reasonable
        std::cout << "        [Analysis] Validating computed table address 0x" << std::hex << computed_table_address << std::dec << std::endl;
        
        if (computed_table_address == 0) {
            std::cout << "        [Analysis] FAIL: Computed table address is 0" << std::endl;
            return info;
        }
        
        if (computed_table_address + (num_cases * 4) > elf_size) {
            std::cout << "        [Analysis] FAIL: Table extends beyond ELF bounds - address 0x" << std::hex 
                      << computed_table_address << " + " << std::dec << (num_cases * 4) 
                      << " > ELF size " << elf_size << std::endl;
            return info;
        }
        
        // Read the jump table entries
        std::vector<uint32_t> case_targets;
        int valid_targets = 0, invalid_targets = 0;
        
        std::cout << "        [Analysis] Reading " << num_cases << " jump table entries..." << std::endl;
        
        for (int i = 0; i < num_cases; ++i) {
            uint32_t table_entry_offset = computed_table_address + (i * 4);
            if (table_entry_offset + 4 <= elf_size) {
                uint32_t case_target = *(reinterpret_cast<const uint32_t*>(elf_data + table_entry_offset));
                
                std::cout << "        [Analysis] Entry " << i << " @ offset 0x" << std::hex << table_entry_offset 
                          << " -> target 0x" << case_target << std::dec;
                
                // Validate that the target is within the function
                if (case_target >= func->base_address && 
                    case_target < func->base_address + function_size) {
                    case_targets.push_back(case_target);
                    valid_targets++;
                    std::cout << " (VALID)" << std::endl;
                } else {
                    invalid_targets++;
                    std::cout << " (INVALID - outside function bounds [0x" << std::hex 
                              << func->base_address << " - 0x" << (func->base_address + function_size) 
                              << "])" << std::dec << std::endl;
                }
            } else {
                invalid_targets++;
                std::cout << "        [Analysis] Entry " << i << " @ offset 0x" << std::hex << table_entry_offset 
                          << " extends beyond ELF bounds" << std::dec << std::endl;
            }
        }
        
        std::cout << "        [Analysis] Jump table validation results: " << valid_targets 
                  << " valid targets, " << invalid_targets << " invalid targets" << std::endl;
        
        // Only consider it valid if we found reasonable targets
        if (!case_targets.empty()) {
            info.table_address = computed_table_address;
            info.num_cases = case_targets.size();
            info.switch_instruction_addr = jr_instruction_addr;
            info.case_targets = case_targets;
            info.valid = true;
            std::cout << "        [Analysis] SUCCESS: Jump table analysis completed successfully" << std::endl;
        } else {
            std::cout << "        [Analysis] FAIL: No valid case targets found" << std::endl;
        }
        
        return info;
    }
};


// --- HELPER FUNCTIONS for Data Flow Analysis ---

// Merges two individual register states.
static RegisterState merge_individual_states(const RegisterState& state1, const RegisterState& state2) {
    if (state1 == state2) {
        return state1;
    }
    return RegisterState{StateUnknown{}};
}

// Merges the "source" state map into the "destination" state map.
static RegisterStateMap merge_register_states(const RegisterStateMap& dest, const RegisterStateMap& source) {
    RegisterStateMap merged_state = dest;
    for (const auto& [reg, src_state] : source) {
        if (merged_state.find(reg) != merged_state.end()) {
            merged_state[reg] = merge_individual_states(merged_state.at(reg), src_state);
        } else {
            merged_state[reg] = src_state;
        }
    }
    return merged_state;
}


Function::Function(uint32_t address) {
    this->base_address = address;
}
/*
void Function::analyze_and_resolve_jump_tables(const uint8_t* elf_data, uint32_t elf_size, 
                                               const uint8_t* function_code, std::set<uint32_t>& leaders) {
    JumpTableAnalyzer analyzer(this);
    
    // Scan for all indirect jumps (jr instructions that aren't returns)
    for (uint32_t offset = 0; offset + 4 <= this->size; offset += 4) {
        uint32_t current_vram = this->base_address + offset;
        uint32_t instruction_word = *(reinterpret_cast<const uint32_t*>(function_code + offset));

        RabbitizerInstruction instr;
        RabbitizerInstructionR5900_init(&instr, instruction_word, current_vram);
        RabbitizerInstructionR5900_processUniqueId(&instr);

        if (instr.uniqueId == RABBITIZER_INSTR_ID_cpu_jr && 
            RAB_INSTR_GET_rs(&instr) != RABBITIZER_REG_GPR_O32_ra) {
            
            std::cout << "      [Jump Table] Analyzing potential jump table at 0x" 
                      << std::hex << current_vram << std::dec << std::endl;
            
            // The analyzer now implicitly knows the function size via the Function object
            JumpTableAnalyzer::JumpTableInfo table_info = analyzer.analyze_potential_jump_table(
                current_vram, elf_data, elf_size, function_code);
            
            if (table_info.valid) {
                std::cout << "      [Jump Table] Found jump table with " << table_info.case_targets.size() << " cases:" << std::endl;
                for (uint32_t target : table_info.case_targets) {
                    leaders.insert(target);
                }
            } else {
                std::cout << "      [Jump Table] Could not resolve jump table structure" << std::endl;
            }
        }
        RabbitizerInstruction_destroy(&instr);
    }
}
*/


void Function::find_basic_blocks(const uint8_t* code, std::set<uint32_t>& leaders) {
    if (this->size == 0) return;

    leaders.insert(this->base_address);

    for (uint32_t offset = 0; offset + 4 <= this->size; offset += 4) {
        uint32_t current_vram = this->base_address + offset;
        uint32_t instruction_word = *(reinterpret_cast<const uint32_t*>(code + offset));

        RabbitizerInstruction instr;
        RabbitizerInstructionR5900_init(&instr, instruction_word, current_vram);
        RabbitizerInstructionR5900_processUniqueId(&instr);
        
        if (RabbitizerInstruction_hasDelaySlot(&instr)) {
            if (offset + 8 <= this->size) {
                leaders.insert(current_vram + 8);
            }
            
            uint32_t target_vram = RabbitizerInstruction_getBranchVramGeneric(&instr);

            if (target_vram >= this->base_address && target_vram < this->base_address + this->size) {
                leaders.insert(target_vram);
            }
        }
        RabbitizerInstruction_destroy(&instr);
    }
}

void Function::create_blocks_from_leaders(const uint8_t* code, const std::set<uint32_t>& leaders) {
    this->blocks.clear();
    std::vector<uint32_t> sorted_leaders(leaders.begin(), leaders.end());
    std::sort(sorted_leaders.begin(), sorted_leaders.end());
    std::cout << "-----------------Begin basic blocks from leaders------------------" << std::endl;
    std::cout << "Function size: " << this->size << std::endl;
    std::cout << "Number of Instructions" << this->size / 4 << std::endl;


    for (uint32_t leader : sorted_leaders) {
        std::cout << "Leader Address: 0x" << std::hex << leader << std::dec << std::endl;
    }
    std::cout << "-----------------End basic blocks from leaders------------------" << std::endl;

    for (size_t i = 0; i < sorted_leaders.size(); ++i) {
        
        
        uint32_t block_start_addr = sorted_leaders[i];
        uint32_t block_end_addr_exclusive = (i + 1 < sorted_leaders.size()) ? sorted_leaders[i+1] : (this->base_address + this->size);

        // Ensure the block is within the function's boundaries
        /*
                if (block_start_addr < this->base_address || block_start_addr >= this->base_address + this->size) continue;
        if (block_end_addr_exclusive > this->base_address + this->size) {
            block_end_addr_exclusive = this->base_address + this->size;
        }
        
        */

        bool is_last_block = false;
        if (i + 1 < sorted_leaders.size()){
            uint32_t next_leader_addr = sorted_leaders[i + 1];
            if (block_start_addr + 4 == next_leader_addr){
                is_last_block = true;

            }
        }
        bool is_single_instruction_block = (block_end_addr_exclusive - block_start_addr) == 4;

        if (is_last_block && is_single_instruction_block) {
            std::cout << "CHECKING... trailing NOP block at: 0x" << std::hex << block_start_addr << std::dec << std::endl; 
            // Read the single instruction word
            uint32_t instruction_offset = block_start_addr - this->base_address;
            uint32_t instruction_word = *(reinterpret_cast<const uint32_t*>(code + instruction_offset));

            // Use Rabbitizer to check if it's a NOP
            RabbitizerInstruction nop_check_instr;
            RabbitizerInstructionR5900_init(&nop_check_instr, instruction_word, block_start_addr);
            RabbitizerInstructionR5900_processUniqueId(&nop_check_instr);

            char instr_buffer[256]; // 256 chars is plenty for one instruction

            // 2. Call the disassemble function to fill the buffer.
            //    The arguments are: instruction, destination buffer, immediate override, override length, extra justification.
            //    We can pass NULL and 0 for the simple case.
            RabbitizerInstruction_disassemble(&nop_check_instr, instr_buffer, NULL, 0, 0);
            std::cout << "CHECKING FOR NOP @ Address: 0x"<< std::hex << block_start_addr << " | Decoded instruction: " << instr_buffer << std::endl;

            if (RabbitizerInstruction_isNop(&nop_check_instr)) {
                std::cout << "Skipping trailing NOP block at 0x" << std::hex << block_start_addr << std::dec << std::endl;
                continue; // Skip creating this block and move to the next leader
            }
        }

        Block current_block;
        current_block.start_address = block_start_addr;
        current_block.end_address = block_end_addr_exclusive;
        std::cout << "Leader Address: 0x" << std::hex << block_start_addr << std::dec << std::endl;
        std::cout << "End Address: 0x" << std::hex << block_end_addr_exclusive << std::dec << std::endl;
        // If block is the last block end_addr-4 -> end_addr, contains only one instruction and the one instruction is a NOP instruction then we continue

        for (uint32_t addr = block_start_addr; addr < block_end_addr_exclusive; addr += 4) {
            uint32_t instruction_offset = addr - this->base_address;
            uint32_t instruction_word = *(reinterpret_cast<const uint32_t*>(code + instruction_offset));

            RabbitizerInstruction decoded_instr;
            RabbitizerInstructionR5900_init(&decoded_instr, instruction_word, addr);
            RabbitizerInstructionR5900_processUniqueId(&decoded_instr);
            char instr_buffer[256]; // 256 chars is plenty for one instruction

            // 2. Call the disassemble function to fill the buffer.
            //    The arguments are: instruction, destination buffer, immediate override, override length, extra justification.
            //    We can pass NULL and 0 for the simple case.
            RabbitizerInstruction_disassemble(&decoded_instr, instr_buffer, NULL, 0, 0);
            std::cout << "Address:" << std::endl;
            std::cout << "Address: 0x"<< std::hex << addr << " | Decoded instruction: " << instr_buffer << std::endl;
            current_block.instructions.push_back(decoded_instr);
        }

        if (!current_block.instructions.empty()) {
            this->blocks.push_back(current_block);
        }
    }
}

void Function::build_control_flow_graph() {
    std::unordered_map<uint32_t, int> address_to_block_index;
    for (int i = 0; i < this->blocks.size(); ++i) {
        address_to_block_index[this->blocks[i].start_address] = i;
    }

    for (int i = 0; i < this->blocks.size(); ++i) {
        Block& current_block = this->blocks[i];
        if (current_block.instructions.empty()) continue;

        const RabbitizerInstruction& last_instr = current_block.instructions.back();
        const RabbitizerInstruction* control_flow_instr = nullptr;

        // In a correctly formed block, the control flow instruction is the one
        // BEFORE the delay slot instruction.
        if (current_block.instructions.size() > 1 && RabbitizerInstruction_hasDelaySlot(&current_block.instructions[current_block.instructions.size() - 2])) {
            control_flow_instr = &current_block.instructions[current_block.instructions.size() - 2];
        } else if (RabbitizerInstrDescriptor_isJump(last_instr.descriptor)) {
            // This handles rare cases of jumps without a delay slot.
            control_flow_instr = &last_instr;
        }

        if (control_flow_instr != nullptr) {
            const RabbitizerInstrDescriptor* descriptor = control_flow_instr->descriptor;

            if (RabbitizerInstrDescriptor_isBranch(descriptor) && control_flow_instr->uniqueId != RABBITIZER_INSTR_ID_cpu_b) { // Conditional branch
                uint32_t target_address = RabbitizerInstruction_getBranchVramGeneric(control_flow_instr);
                if (address_to_block_index.count(target_address)) {
                    current_block.taken_branch_successor_index = address_to_block_index.at(target_address);
                    blocks[address_to_block_index.at(target_address)].predecessors.push_back(i);
                }
                
                uint32_t fallthrough_address = control_flow_instr->vram + 8;
                if (address_to_block_index.count(fallthrough_address)) {
                    current_block.fall_through_successor_index = address_to_block_index.at(fallthrough_address);
                     blocks[address_to_block_index.at(fallthrough_address)].predecessors.push_back(i);
                }
            } else if (RabbitizerInstrDescriptor_isJump(descriptor) || control_flow_instr->uniqueId == RABBITIZER_INSTR_ID_cpu_b) { // Unconditional jump or branch
                if (control_flow_instr->uniqueId == RABBITIZER_INSTR_ID_cpu_jr && RAB_INSTR_GET_rs(control_flow_instr) == RABBITIZER_REG_GPR_O32_ra) {
                    // This is a return instruction, so it has no successor in the graph.
                } else {
                    uint32_t target_address = 0;
                    if (RabbitizerInstrDescriptor_isJumpWithAddress(descriptor)) {
                        target_address = RabbitizerInstruction_getInstrIndexAsVram(control_flow_instr);
                    } else {
                        target_address = RabbitizerInstruction_getBranchVramGeneric(control_flow_instr);
                    }
                    if (address_to_block_index.count(target_address)) {
                        current_block.fall_through_successor_index = address_to_block_index.at(target_address);
                        blocks[address_to_block_index.at(target_address)].predecessors.push_back(i);
                    }
                }
            }
        } else { // No branch/jump at the end of the block
            uint32_t fallthrough_address = current_block.end_address;
            if (address_to_block_index.count(fallthrough_address)) {
                current_block.fall_through_successor_index = address_to_block_index.at(fallthrough_address);
                blocks[address_to_block_index.at(fallthrough_address)].predecessors.push_back(i);
            }
        }
    }
}

void Function::analyze_prologue() {
    if (this->blocks.empty()) return;

    const Block& entry_block = this->blocks[0];

    for (const RabbitizerInstruction& instr : entry_block.instructions) {
        const RabbitizerInstrDescriptor* descriptor = instr.descriptor;

        if (instr.uniqueId == RABBITIZER_INSTR_ID_cpu_addiu &&
            RAB_INSTR_GET_rd(&instr) == RABBITIZER_REG_GPR_O32_sp &&
            RAB_INSTR_GET_rs(&instr) == RABBITIZER_REG_GPR_O32_sp) {
            int32_t stack_adjustment = RabbitizerInstruction_getProcessedImmediate(&instr);
            if (stack_adjustment < 0) {
                this->registerStateAfterPrologue[RABBITIZER_REG_GPR_O32_sp] = StateStackRelative{stack_adjustment};
            }
        } else if (instr.uniqueId == RABBITIZER_INSTR_ID_cpu_sd &&
                   RAB_INSTR_GET_rs(&instr) == RABBITIZER_REG_GPR_O32_sp) {
            uint8_t saved_reg_num = RAB_INSTR_GET_rt(&instr);
            if (saved_reg_num == RABBITIZER_REG_GPR_O32_ra || (saved_reg_num >= RABBITIZER_REG_GPR_O32_s0 && saved_reg_num <= RABBITIZER_REG_GPR_O32_s7)) {
                int32_t stack_offset = RabbitizerInstruction_getProcessedImmediate(&instr);
                this->savedRegisterLocations[(RabbitizerRegister_GprO32)saved_reg_num] = stack_offset;
            }
        } else if (descriptor->maybeIsMove && RabbitizerInstrDescriptor_modifiesRd(descriptor)) {
            uint8_t dest_reg = RAB_INSTR_GET_rd(&instr);
            uint8_t source_reg = RAB_INSTR_GET_rs(&instr);
            if (dest_reg >= RABBITIZER_REG_GPR_O32_s0 && dest_reg <= RABBITIZER_REG_GPR_O32_s7 &&
                source_reg >= RABBITIZER_REG_GPR_O32_a0 && source_reg <= RABBITIZER_REG_GPR_O32_a3) {
                this->registerStateAfterPrologue[(RabbitizerRegister_GprO32)dest_reg] =
                    StateSymbolic{(RabbitizerRegister_GprO32)source_reg};
            }
        } else if (RabbitizerInstrDescriptor_isBranch(descriptor) || RabbitizerInstrDescriptor_isJump(descriptor)) {
            break;
        }
    }
}

void Function::cull_unreachable_blocks() {
    if (this->blocks.empty()) return;

    std::set<uint32_t> reachable_addresses;
    std::vector<uint32_t> worklist;
    std::unordered_map<uint32_t, int> address_to_block_index;

    for (int i = 0; i < this->blocks.size(); ++i) {
        address_to_block_index[this->blocks[i].start_address] = i;
    }

    worklist.push_back(this->blocks[0].start_address);
    reachable_addresses.insert(this->blocks[0].start_address);

    while (!worklist.empty()) {
        uint32_t current_addr = worklist.back();
        worklist.pop_back();

        if (address_to_block_index.count(current_addr) == 0) continue;

        const Block& current_block = this->blocks[address_to_block_index.at(current_addr)];

        if (current_block.fall_through_successor_index != -1) {
            const Block& successor = this->blocks[current_block.fall_through_successor_index];
            if (reachable_addresses.find(successor.start_address) == reachable_addresses.end()) {
                reachable_addresses.insert(successor.start_address);
                worklist.push_back(successor.start_address);
            }
        }
        if (current_block.taken_branch_successor_index != -1) {
            const Block& successor = this->blocks[current_block.taken_branch_successor_index];
            if (reachable_addresses.find(successor.start_address) == reachable_addresses.end()) {
                reachable_addresses.insert(successor.start_address);
                worklist.push_back(successor.start_address);
            }
        }
    }

    // Create new vector with only reachable blocks
    std::vector<Block> final_blocks;
    std::unordered_map<uint32_t, int> new_address_to_index;
    
    for (const auto& block : this->blocks) {
        if (reachable_addresses.count(block.start_address)) {
            new_address_to_index[block.start_address] = final_blocks.size();
            final_blocks.push_back(block);
        }
    }

    // **CRITICAL FIX**: Update all successor indices and predecessors to match new positions
    for (auto& block : final_blocks) {
        // Update fall-through successor index
        if (block.fall_through_successor_index != -1) {
            uint32_t successor_addr = this->blocks[block.fall_through_successor_index].start_address;
            if (new_address_to_index.count(successor_addr)) {
                block.fall_through_successor_index = new_address_to_index[successor_addr];
            } else {
                block.fall_through_successor_index = -1; // Block was culled
            }
        }

        // Update taken branch successor index
        if (block.taken_branch_successor_index != -1) {
            uint32_t successor_addr = this->blocks[block.taken_branch_successor_index].start_address;
            if (new_address_to_index.count(successor_addr)) {
                block.taken_branch_successor_index = new_address_to_index[successor_addr];
            } else {
                block.taken_branch_successor_index = -1; // Block was culled
            }
        }

        // Clear and rebuild predecessors list with new indices
        block.predecessors.clear();
    }

    // Rebuild predecessor relationships with new indices
    for (int i = 0; i < final_blocks.size(); ++i) {
        const Block& current_block = final_blocks[i];
        
        if (current_block.fall_through_successor_index != -1) {
            final_blocks[current_block.fall_through_successor_index].predecessors.push_back(i);
        }
        if (current_block.taken_branch_successor_index != -1) {
            final_blocks[current_block.taken_branch_successor_index].predecessors.push_back(i);
        }
    }

    this->blocks = final_blocks;
}

void Function::run_data_flow_analysis() {
    if (blocks.empty()) return;
    std::cout << "      [DFA] Starting data flow analysis for " << name << std::endl;

    std::unordered_map<uint32_t, RegisterStateMap> block_start_states;
    std::queue<int> worklist;
    std::unordered_map<uint32_t, int> address_to_block_index;
    
    for (int i = 0; i < this->blocks.size(); ++i) {
        address_to_block_index[this->blocks[i].start_address] = i;
    }

    block_start_states[base_address] = this->registerStateAfterPrologue;
    worklist.push(0); 

    int iterations = 0;

    while (!worklist.empty()) {
        iterations++;
        if (iterations > blocks.size() * 100) { 
            std::cout << "      [DFA] WARNING: Exceeded max iterations (" << iterations << "), breaking." << std::endl;
            break;
        }

        int current_block_index = worklist.front();
        worklist.pop();

        // **DEFENSIVE CHECK**: Ensure block index is valid
        if (current_block_index < 0 || current_block_index >= static_cast<int>(blocks.size())) {
            std::cout << "      [DFA] ERROR: Invalid block index " << current_block_index 
                      << " (valid range: 0 to " << (blocks.size() - 1) << ")" << std::endl;
            continue;
        }

        Block& current_block = blocks[current_block_index];
        std::cout << "      [DFA] Iteration " << iterations << ": Processing Block " << current_block_index 
                  << " at 0x" << std::hex << current_block.start_address << std::dec 
                  << ". Worklist size: " << worklist.size() << std::endl;

        RegisterStateMap merged_start_state;
        if (current_block_index != 0) {
            for (int pred_index : current_block.predecessors) {
                // **DEFENSIVE CHECK**: Ensure predecessor index is valid
                if (pred_index < 0 || pred_index >= static_cast<int>(blocks.size())) {
                    std::cout << "      [DFA] WARNING: Invalid predecessor index " << pred_index << std::endl;
                    continue;
                }
                
                const Block& pred_block = blocks[pred_index];
                if (block_end_states.count(pred_block.start_address)) {
                    merged_start_state = merge_register_states(merged_start_state, block_end_states.at(pred_block.start_address));
                }
            }
        } else {
            merged_start_state = block_start_states.at(current_block.start_address);
        }
        
        block_start_states[current_block.start_address] = merged_start_state;
        
        RegisterStateMap final_state = DataFlowEngine::analyze_block(current_block, merged_start_state);

        bool state_has_changed = false;
        if (block_end_states.count(current_block.start_address) == 0 || block_end_states.at(current_block.start_address) != final_state) {
            state_has_changed = true;
        }
        
        if (state_has_changed) {
            std::cout << "        -> State has CHANGED. Pushing successors to worklist." << std::endl;
            block_end_states[current_block.start_address] = final_state;
            
            // **DEFENSIVE CHECKS**: Ensure successor indices are valid before pushing
            if (current_block.fall_through_successor_index != -1) {
                if (current_block.fall_through_successor_index >= 0 && 
                    current_block.fall_through_successor_index < static_cast<int>(blocks.size())) {
                    worklist.push(current_block.fall_through_successor_index);
                } else {
                    std::cout << "      [DFA] WARNING: Invalid fall-through successor index " 
                              << current_block.fall_through_successor_index << std::endl;
                }
            }
            if (current_block.taken_branch_successor_index != -1) {
                if (current_block.taken_branch_successor_index >= 0 && 
                    current_block.taken_branch_successor_index < static_cast<int>(blocks.size())) {
                    worklist.push(current_block.taken_branch_successor_index);
                } else {
                    std::cout << "      [DFA] WARNING: Invalid taken branch successor index " 
                              << current_block.taken_branch_successor_index << std::endl;
                }
            }
        } else {
            std::cout << "        -> State is STABLE. No changes." << std::endl;
        }
    }
    std::cout << "      [DFA] Analysis for " << name << " finished after " << iterations << " iterations." << std::endl;
}

std::set<uint32_t> Function::analyze(const uint8_t* elf_data, uint32_t elf_size) {
    if (this->size == 0) {
        std::cout << "    [!] Skipping analysis for " << name << " because size is 0." << std::endl;
        return std::set<uint32_t>();
    }

    // NOTE: This calculation assumes the ELF is loaded at address 0 in memory.
    // A more robust solution would find which section the function is in and use the section's VRAM start address.
    const uint8_t* function_code = elf_data + (this->base_address - 0x100000); // Simplified assumption

    std::cout << "    [1/7] Finding initial basic block leaders..." << std::endl;
    std::set<uint32_t> leaders;
    find_basic_blocks(function_code, leaders);

    // std::cout << "    [2/7] Analyzing for jump tables..." << std::endl;
    // analyze_and_resolve_jump_tables(elf_data, elf_size, function_code, leaders);

    std::cout << "    [3/7] Creating final basic blocks..." << std::endl;
    create_blocks_from_leaders(function_code, leaders);
    
    std::cout << "    [4/7] Building Control Flow Graph..." << std::endl;
    build_control_flow_graph();

    //std::cout << "    [5/7] Culling unreachable blocks..." << std::endl;
    //cull_unreachable_blocks();

    std::cout << "    [6/7] Analyzing prologue..." << std::endl;
    analyze_prologue();

    std::cout << "    [7/7] Running data flow analysis..." << std::endl;
    run_data_flow_analysis();

    return leaders;
}

void Function::dump_to_console() const {
    std::cout << "=========================================================" << std::endl;
    std::cout << "Function: " << this->name << " at 0x" << std::hex << this->base_address << std::dec << std::endl;
    std::cout << "=========================================================" << std::endl;

    if (this->blocks.empty()) {
        std::cout << "  (No basic blocks found for this function)" << std::endl;
        return;
    }

    for (size_t i = 0; i < this->blocks.size(); ++i) {
        const Block& block = this->blocks[i];
        std::cout << "\n--- Block " << i << " at 0x" << std::hex << block.start_address
                  << " (Ends at 0x" << block.end_address << ")" << std::dec << std::endl;
        std::cout << "      Successors -> Taken: " << block.taken_branch_successor_index
                  << ", Fall-through: " << block.fall_through_successor_index << std::endl;
        for (const RabbitizerInstruction& instr : block.instructions) {
            char buffer[256];
            RabbitizerInstruction_disassemble(&instr, buffer, nullptr, 0, 0);
            std::cout << "  0x" << std::hex << instr.vram << ":  "
                      << std::setw(8) << std::setfill('0') << instr.word << "    "
                      << std::dec
                      << buffer << std::endl;
        }
    }
    std::cout << "\n================ End of Function (" << this->name << ") ================" << std::endl << std::endl;
}