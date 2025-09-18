#pragma once

#include <vector>
#include <unordered_map>
#include <cstdint>
#include <string>
#include <set>
#include "generated/Registers_enums.h"
#include "instructions/RabbitizerInstruction.h"
#include "register_state.h"
#include "DataFlowEngine.h"

// Represents a single Basic Block of MIPS instructions.
struct Block {
    uint32_t start_address = 0;
    uint32_t end_address = 0;
    std::vector<RabbitizerInstruction> instructions;
    std::vector<int> predecessors; 
    int taken_branch_successor_index = -1;
    int fall_through_successor_index = -1;
};

// Represents an external symbol or address referenced by a function.
struct ExternalReference {
    uint32_t address = 0;
    std::string section;
    std::string name; // Used if the reference is a named function/variable
};

// Represents a single function, containing all its analyzed properties and basic blocks.
class Function {
public:
    uint32_t base_address = 0;
    uint32_t size = 0;
    std::string name;
    std::vector<Block> blocks;
    std::vector<ExternalReference> external_references;
    std::unordered_map<RabbitizerRegister_GprO32, RegisterState> registerStateAfterPrologue;
    std::unordered_map<RabbitizerRegister_GprO32, int32_t> savedRegisterLocations;
    std::unordered_map<uint32_t, RegisterStateMap> block_end_states;

    Function() = default;
    Function(uint32_t address);

    // The primary analysis entry point. Uses the object's size member.
    void analyze(const uint8_t* elf_data, uint32_t elf_size);
    
    void dump_to_console() const;

private:
    void find_basic_blocks(const uint8_t* function_code, std::set<uint32_t>& leaders);
    //void analyze_and_resolve_jump_tables(const uint8_t* elf_data, uint32_t elf_size, const uint8_t* function_code, std::set<uint32_t>& leaders);
    void create_blocks_from_leaders(const uint8_t* function_code, const std::set<uint32_t>& leaders);
    void build_control_flow_graph();
    void analyze_prologue();
    void cull_unreachable_blocks();
    void run_data_flow_analysis();
};