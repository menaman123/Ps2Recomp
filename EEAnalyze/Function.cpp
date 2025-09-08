#include "Function.h"
#include "instructions/RabbitizerInstructionR5900.h"
#include <set>
#include <vector>
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <queue>

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
        if (RabbitizerInstrDescriptor_isBranch(descriptor) || RabbitizerInstrDescriptor_isJump(descriptor)) {
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
    // Create the address-to-index map for performance
    std::unordered_map<uint32_t, int> address_to_block_index;
    for (int i = 0; i < this->blocks.size(); ++i) {
        address_to_block_index[this->blocks[i].start_address] = i;
    }

    // Loop through each block to set its successors
    for (int i = 0; i < this->blocks.size(); ++i) {
        Block& current_block = this->blocks[i];
        if (current_block.instructions.empty()) continue;

        const RabbitizerInstruction* control_flow_instr = nullptr;

        // Check if the second-to-last instruction is a branch/jump.
        if (current_block.instructions.size() > 1) {
            const auto& potential_branch = current_block.instructions[current_block.instructions.size() - 2];
            if (RabbitizerInstruction_hasDelaySlot(&potential_branch)) {
                control_flow_instr = &potential_branch;
            }
        }

        // If not, check if the very last instruction is a branch/jump.
        if (control_flow_instr == nullptr) {
            const auto& last_instr = current_block.instructions.back();
            // --- FIX IS HERE ---
            // Pass the pointer 'last_instr.descriptor' directly, not its address.
            if (RabbitizerInstruction_hasDelaySlot(&last_instr) || RabbitizerInstrDescriptor_isJump(last_instr.descriptor)) {
                control_flow_instr = &last_instr;
            }
        }
        
        // If we found a branch/jump, analyze it.
        if (control_flow_instr != nullptr) {
            const RabbitizerInstrDescriptor* descriptor = control_flow_instr->descriptor;

            if (RabbitizerInstrDescriptor_isBranch(descriptor) && control_flow_instr->uniqueId != RABBITIZER_INSTR_ID_cpu_b) {
                // Two successors
                uint32_t target_address = RabbitizerInstruction_getBranchVramGeneric(control_flow_instr);
                if (address_to_block_index.count(target_address)) {
                    current_block.taken_branch_successor_index = address_to_block_index.at(target_address);
                }
                uint32_t fallthrough_address = current_block.end_address;
                if (address_to_block_index.count(fallthrough_address)) {
                    current_block.fall_through_successor_index = address_to_block_index.at(fallthrough_address);
                }
            } else if (RabbitizerInstrDescriptor_isJump(descriptor) || control_flow_instr->uniqueId == RABBITIZER_INSTR_ID_cpu_b) {
                if (control_flow_instr->uniqueId == RABBITIZER_INSTR_ID_cpu_jr && RAB_INSTR_GET_rs(control_flow_instr) == RABBITIZER_REG_GPR_O32_ra) {
                    // This is a 'jr $ra', a function return, so it has no successors.
                } else {
                    // One successor
                    uint32_t target_address = 0;
                    if (RabbitizerInstrDescriptor_isJumpWithAddress(descriptor)) {
                        target_address = RabbitizerInstruction_getInstrIndexAsVram(control_flow_instr);
                    } else {
                        target_address = RabbitizerInstruction_getBranchVramGeneric(control_flow_instr);
                    }

                    if (address_to_block_index.count(target_address)) {
                        current_block.fall_through_successor_index = address_to_block_index.at(target_address);
                    }
                }
            }
        } else {
            // It's a normal block of instructions.
            uint32_t fallthrough_address = current_block.end_address;
            if (address_to_block_index.count(fallthrough_address)) {
                current_block.fall_through_successor_index = address_to_block_index.at(fallthrough_address);
            }
        }
    }
}

void Function::analyze_prologue() {
    if (this->blocks.empty()) {
        return;
    }

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
    if (this->blocks.empty()) {
        return;
    }

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

    std::vector<Block> final_blocks;
    for (const auto& block : this->blocks) {
        if (reachable_addresses.count(block.start_address)) {
            final_blocks.push_back(block);
        }
    }
    this->blocks = final_blocks;
}

void Function::run_data_flow_analysis() {
    if (blocks.empty()) return;

    std::unordered_map<uint32_t, RegisterStateMap> block_start_states;
    std::vector<uint32_t> worklist;
    std::set<uint32_t> worklist_set; 
    std::unordered_map<uint32_t, int> address_to_block_index;
    for (int i = 0; i < this->blocks.size(); ++i) {
        address_to_block_index[this->blocks[i].start_address] = i;
    }

    // Initialize with prologue state and add entry block to worklist
    block_start_states[base_address] = registerStateAfterPrologue;
    worklist.push_back(base_address);
    worklist_set.insert(base_address);

    while(!worklist.empty()) {
        uint32_t current_block_addr = worklist.back();
        worklist.pop_back();
        worklist_set.erase(current_block_addr);

        int block_index = address_to_block_index.at(current_block_addr);
        const Block& current_block = blocks[block_index];
        
        RegisterStateMap final_state = DataFlowEngine::analyze_block(current_block, block_start_states[current_block_addr]);

        block_end_states[current_block_addr] = final_state;

        auto process_successor = [&](int successor_index) {
            if (successor_index != -1) {
                const Block& successor_block = blocks[successor_index];
                // For simplicity, we just overwrite. A real implementation would merge states.
                block_start_states[successor_block.start_address] = final_state;
                if (worklist_set.find(successor_block.start_address) == worklist_set.end()) {
                    worklist.push_back(successor_block.start_address);
                    worklist_set.insert(successor_block.start_address);
                }
            }
        };

        process_successor(current_block.fall_through_successor_index);
        process_successor(current_block.taken_branch_successor_index);
    }
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