#pragma once

#include <vector>
#include <unordered_map>
#include <cstdint>
#include <string>
#include "generated/Registers_enums.h"
#include "instructions/RabbitizerInstruction.h"
#include "register_state.h"
#include "DataFlowEngine.h"

// Represents a single Basic Block of MIPS instructions.
struct Block {
    uint32_t start_address = 0;
    uint32_t end_address = 0;

    std::vector<RabbitizerInstruction> instructions;

    // --- ADDED THIS LINE ---
    std::vector<int> predecessors; // Indices of blocks that can jump to this one.

    // Index into the Function's 'blocks' vector. -1 means no successor.
    int taken_branch_successor_index = -1;
    int fall_through_successor_index = -1;
};


// Represents a single function, containing all its analyzed properties and basic blocks.
class Function {
public:
    uint32_t base_address;
    std::string name;
    std::vector<Block> blocks;
    std::unordered_map<RabbitizerRegister_GprO32, RegisterState> registerStateAfterPrologue;
    std::unordered_map<RabbitizerRegister_GprO32, int32_t> savedRegisterLocations;
    std::unordered_map<uint32_t, RegisterStateMap> block_end_states;

    Function(uint32_t address);

    void analyze(const uint8_t* code, uint32_t code_size);
    void dump_to_console() const;

private:
    void find_basic_blocks(const uint8_t* code, uint32_t code_size);
    void build_control_flow_graph();
    void analyze_prologue();
    void cull_unreachable_blocks();
    void run_data_flow_analysis();
};