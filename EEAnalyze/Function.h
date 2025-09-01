#pragma once

#include <vector>
#include <unordered_map>
#include <cstdint>
#include <string>
#include "generated/Registers_enums.h"
#include "instructions/RabbitizerInstruction.h"
#include "register_state.h"

// Represents a single Basic Block of MIPS instructions.
struct Block {
    uint32_t start_address = 0;
    uint32_t end_address = 0;

    std::vector<RabbitizerInstruction> instructions;

    // Index into the Function's 'blocks' vector. -1 means no successor.
    int taken_branch_successor_index = -1;   // For conditional branches
    int fall_through_successor_index = -1; // For fall-through or unconditional jumps
};


// Represents a single function, containing all its analyzed properties and basic blocks.
class Function {
public:
    // --- Data Members ---
    uint32_t base_address;
    std::string name;
    std::vector<Block> blocks;
    std::unordered_map<RabbitizerRegister_GprO32, RegisterState> registerStateAfterPrologue;

    // --- Constructor ---
    // Initializes the function with its known starting address.
    Function(uint32_t address);

    // --- Main Analysis Method ---
    // This is the main method that runs all the analysis steps for this single function.
    void analyze(const uint8_t* code, uint32_t code_size);

private:
    // --- Helper Methods ---
    // These are the individual analysis steps, now private to the class.
    void find_basic_blocks(const uint8_t* code, uint32_t code_size);

    // Builds the control flow within the function
    // Based on the last instruction, it fills in the taken_branch_successor_index and fall_through_successor_index for each block. After this method runs, the simple list of blocks has been transformed into a complete Control Flow Graph (CFG) that represents every possible path the original code could take.
    void build_control_flow_graph();

    // It populates the this->registerStateAfterPrologue map with its findings. This gives you a snapshot of the register states right after the function's setup code has run, which is invaluable for the final code translation.
    void analyze_prologue();

    void dump_to_console() const;
};