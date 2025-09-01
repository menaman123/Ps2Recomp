#include "Function.h"
#include "instructions/RabbitizerInstructionR5900.h"
#include "RegisterState.h"
#include <set>
#include <vector>
#include <algorithm>
#include <iostream>
#include <iomanip>

void Function::find_basic_blocks(const uint8_t* code, uint32_t code_size) {
    if (code_size == 0) return;

    // --- PASS 1: Find all leader addresses (same as before) ---
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
            // The instruction AFTER the delay slot is a leader.
            if (offset + 8 <= code_size) {
                leader_addresses.insert(current_vram + 8);
            }

            // The target of the branch/jump is a leader.
            uint32_t target_vram = 0;
            if (RabbitizerInstrDescriptor_isJumpWithAddress(descriptor)) {
                target_vram = RabbitizerInstruction_getInstrIndexAsVram(&instr);
            } else {
                target_vram = RabbitizerInstruction_getBranchVramGeneric(&instr);
            }
            leader_addresses.insert(target_vram);
        }
        RabbitizerInstruction_destroy(&instr);
    }

    // --- PASS 2: Form blocks based on your model ---
    std::vector<uint32_t> sorted_leaders(leader_addresses.begin(), leader_addresses.end());
    std::sort(sorted_leaders.begin(), sorted_leaders.end());

    for (size_t i = 0; i < sorted_leaders.size(); ++i) {
        uint32_t block_start_addr = sorted_leaders[i];

        // The block ends right before the next leader starts.
        uint32_t block_end_addr_exclusive = (i + 1 < sorted_leaders.size()) ? sorted_leaders[i+1] : (this->base_address + code_size);

        Block current_block;

        /* 
            TODO: We don't want to repeat work, a function call may lie in side another function, If this is the case we grab that function and its corresponding blocks and append to the block
            If current start_address is equal to a function's start address, grab this function and its block append to this current function we are working on 
        */
        current_block.start_address = block_start_addr;
        current_block.end_address = block_end_addr_exclusive;

        // Populate the block with its instructions.
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

        // --- Corrected Logic to find the control flow instruction ---
        const RabbitizerInstruction* control_flow_instr = nullptr;

        // Check if the second-to-last instruction is a branch/jump.
        if (current_block.instructions.size() > 1) {
            const auto& potential_branch = current_block.instructions[current_block.instructions.size() - 2];
            if (RabbitizerInstruction_hasDelaySlot(&potential_branch)) {
                control_flow_instr = &potential_branch;
            }
        }

        // If not, check if the very last instruction is a branch/jump.
        // This handles blocks that are only a single branch/jump instruction.
        if (control_flow_instr == nullptr) {
            const auto& last_instr = current_block.instructions.back();
            if (RabbitizerInstruction_hasDelaySlot(&last_instr)) {
                control_flow_instr = &last_instr;
            }
        }
        // --- End of corrected logic ---


        // If we found a branch/jump, analyze it.
        if (control_flow_instr != nullptr) {
            const RabbitizerInstrDescriptor* descriptor = control_flow_instr->descriptor;

            // Case 1: Is it a true conditional branch (beq, bne, but NOT b)?
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
            }
            // Case 2: Is it any kind of unconditional transfer (j, jal, jr, or b)?
            else if (RabbitizerInstrDescriptor_isJump(descriptor) || control_flow_instr->uniqueId == RABBITIZER_INSTR_ID_cpu_b) {
                if (RabbitizerInstruction_isReturn(control_flow_instr)) {
                    // Zero successors ('jr $ra')
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
        }
        // Case 3: It's a normal block of instructions.
        else {
            uint32_t fallthrough_address = current_block.end_address;
            if (address_to_block_index.count(fallthrough_address)) {
                current_block.fall_through_successor_index = address_to_block_index.at(fallthrough_address);
            }
        }
    }
}

void Function::analyze_prologue() {
    // The prologue is almost always in the function's first basic block.
    if (this->blocks.empty()) {
        return; 
    }

    const Block& entry_block = this->blocks[0];

    // Loop through the instructions in the first block.
    for (const RabbitizerInstruction& instr : entry_block.instructions) {
        const RabbitizerInstrDescriptor* descriptor = instr.descriptor;

        // --- Pattern 1: Stack Allocation ---
        // Look for: daddiu sp, sp, -<size>
        if (instr.uniqueId == RABBITIZER_INSTR_ID_cpu_addiu &&
            RabbitizerInstruction_get_rd(&instr) == RABBITIZER_REG_GPR_O32_sp &&
            RabbitizerInstruction_get_rs(&instr) == RABBITIZER_REG_GPR_O32_sp) {

            int32_t stack_adjustment = RabbitizerInstruction_getProcessedImmediate(&instr);
            if (stack_adjustment < 0) {
                // Found it. Record the new symbolic state of the stack pointer.
                this->registerStateAfterPrologue[RABBITIZER_REG_GPR_O32_sp] = RegisterState::asStackRelative(stack_adjustment);
            }
        }

        // --- Pattern 2: Saving Callee-Saved Registers ---
        // Look for: sd <s_reg_or_ra>, <offset>(sp)
        else if (instr.uniqueId == RABBITIZER_INSTR_ID_cpu_sd &&
                 RabbitizerInstruction_get_rs(&instr) == RABBITIZER_REG_GPR_O32_sp) {

            uint8_t saved_reg_num = RabbitizerInstruction_get_rt(&instr);
            // Check if it's the return address ($ra) or a saved register ($s0-$s7)
            if (saved_reg_num == RABBITIZER_REG_GPR_O32_ra || (saved_reg_num >= RABBITIZER_REG_GPR_O32_s0 && saved_reg_num <= RABBITIZER_REG_GPR_O32_s7)) {
                int32_t stack_offset = RabbitizerInstruction_getProcessedImmediate(&instr);
                // Record that this register was saved at this offset.
                this->savedRegisterLocations[(RabbitizerRegister_GprO32)saved_reg_num] = stack_offset;
            }
        }

        // --- Pattern 3: Copying an Argument to a Saved Register ---
        // Look for: move s0, a0 (often `or s0, a0, $zero`)
        else if (descriptor->maybeIsMove && RabbitizerInstrDescriptor_modifiesRd(descriptor)) {
            uint8_t dest_reg = RabbitizerInstruction_get_rd(&instr);
            uint8_t source_reg = RabbitizerInstruction_get_rs(&instr);

            // Check if it's a move from an argument register (a0-a3) to a saved register (s0-s7)
            if (dest_reg >= RABBITIZER_REG_GPR_O32_s0 && dest_reg <= RABBITIZER_REG_GPR_O32_s7 &&
                source_reg >= RABBITIZER_REG_GPR_O32_a0 && source_reg <= RABBITIZER_REG_GPR_O32_a3) {

                // Record that the 's' register is now a symbolic copy of the 'a' register.
                this->registerStateAfterPrologue[(RabbitizerRegister_GprO32)dest_reg] =
                    RegisterState::asSymbolic((RabbitizerRegister_GprO32)source_reg);
            }
        }

        // --- End Condition ---
        // If we hit a branch or jump, the prologue is over.
        else if (RabbitizerInstrDescriptor_isBranch(descriptor) || RabbitizerInstrDescriptor_isJump(descriptor)) {
            break;
        }
    }
}

void Function::analyze(const uint8_t* code, uint32_t code_size) {

    // Step 1: Discover all basic blocks from the raw code.
    // This populates the 'this->blocks' vector with instructions.
    this->find_basic_blocks(code, code_size);

    // Step 2: Connect the blocks together into a graph.
    // This populates the successor indices in each block.
    this->build_control_flow_graph();

    // Step 3: Analyze the entry block for prologue information.
    // This populates the 'registerStateAfterPrologue' and 'savedRegisterLocations' maps.
    this->analyze_prologue();
}

void Function::dump_to_console() const {
    // Print a header for the entire function
    std::cout << "=========================================================" << std::endl;
    std::cout << "Function: " << this->name << " at 0x" << std::hex << this->base_address << std::dec << std::endl;
    std::cout << "=========================================================" << std::endl;

    if (this->blocks.empty()) {
        std::cout << "  (No basic blocks found for this function)" << std::endl;
        return;
    }

    // Loop through each basic block that was found
    for (size_t i = 0; i < this->blocks.size(); ++i) {
        const Block& block = this->blocks[i];

        // Print a header for the block, including its successor information
        std::cout << "\n--- Block " << i << " at 0x" << std::hex << block.start_address
                  << " (Ends at 0x" << block.end_address << ")" << std::dec << std::endl;
        std::cout << "      Successors -> Taken: " << block.taken_branch_successor_index
                  << ", Fall-through: " << block.fall_through_successor_index << std::endl;

        // Loop through each instruction within the block
        for (const RabbitizerInstruction& instr : block.instructions) {
            char buffer[256];
            // Get the disassembled instruction as a string
            RabbitizerInstruction_disassemble(&instr, buffer, nullptr, 0, 0);

            // Print the VRAM address, the raw machine code, and the disassembled string
            std::cout << "  0x" << std::hex << instr.vram << ":  "
                      << std::setw(8) << std::setfill('0') << instr.word << "    "
                      << std::dec // Switch cout back to decimal for the next loop
                      << buffer << std::endl;
        }
    }
    std::cout << "\n================ End of Function (" << this->name << ") ================" << std::endl << std::endl;
}