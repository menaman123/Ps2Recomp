#include "Function.h"
#include "instructions/RabbitizerInstructionR5900.h"
#include <set>
#include <vector>
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <queue>

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
    this->name = "func_" + std::to_string(address);
}

void Function::find_basic_blocks(const uint8_t* code, uint32_t code_size) {
    if (code_size == 0) return;

    std::set<uint32_t> leader_addresses;
    leader_addresses.insert(this->base_address);

    for (uint32_t offset = 0; offset + 4 <= code_size; offset += 4) {
        uint32_t current_vram = this->base_address + offset;
        uint32_t instruction_word = *(reinterpret_cast<const uint32_t*>(code + offset));

        RabbitizerInstruction instr;
        RabbitizerInstructionR5900_init(&instr, instruction_word, current_vram);
        RabbitizerInstructionR5900_processUniqueId(&instr);
        const RabbitizerInstrDescriptor* descriptor = instr.descriptor;
        if (RabbitizerInstruction_hasDelaySlot(&instr)) {
            if (offset + 8 <= code_size) {
                leader_addresses.insert(current_vram + 8);
            }
            uint32_t target_vram = 0;
            if (RabbitizerInstrDescriptor_isJumpWithAddress(descriptor)) {
                target_vram = RabbitizerInstruction_getInstrIndexAsVram(&instr);
            } else {
                target_vram = RabbitizerInstruction_getBranchVramGeneric(&instr);
            }
            if (target_vram >= this->base_address && target_vram < this->base_address + code_size) {
                leader_addresses.insert(target_vram);
            }

            offset += 4;
        }
        RabbitizerInstruction_destroy(&instr);
    }

    std::vector<uint32_t> sorted_leaders(leader_addresses.begin(), leader_addresses.end());
    std::sort(sorted_leaders.begin(), sorted_leaders.end());

    for (size_t i = 0; i < sorted_leaders.size(); ++i) {
        uint32_t block_start_addr = sorted_leaders[i];
        uint32_t block_end_addr_exclusive = (i + 1 < sorted_leaders.size()) ? sorted_leaders[i+1] : (this->base_address + code_size);

        Block current_block;
        current_block.start_address = block_start_addr;
        current_block.end_address = block_end_addr_exclusive;

        for (uint32_t addr = block_start_addr; addr < block_end_addr_exclusive; addr += 4) {
            uint32_t instruction_offset = addr - this->base_address;
            uint32_t instruction_word = *(reinterpret_cast<const uint32_t*>(code + instruction_offset));

            RabbitizerInstruction decoded_instr;
            RabbitizerInstructionR5900_init(&decoded_instr, instruction_word, addr);
            RabbitizerInstructionR5900_processUniqueId(&decoded_instr);
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

void Function::analyze(const uint8_t* code, uint32_t code_size) {
    std::cout << "    [1/5] Finding basic blocks..." << std::endl;
    this->find_basic_blocks(code, code_size);

    std::cout << "    [2/5] Building Control Flow Graph..." << std::endl;
    this->build_control_flow_graph();

    std::cout << "    [3/5] Culling unreachable blocks..." << std::endl;
    this->cull_unreachable_blocks();

    std::cout << "    [4/5] Analyzing prologue..." << std::endl;
    this->analyze_prologue();

    std::cout << "    [5/5] Running data flow analysis..." << std::endl;
    this->run_data_flow_analysis();
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