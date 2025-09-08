#pragma once
#include <unordered_map>
// Forward declarations to break include cycles
struct Block;
#include "register_state.h"
#include "instructions/RabbitizerInstruction.h" // For RabbitizerInstruction
#include "generated/Registers_enums.h" // For RabbitizerRegister_GprO32
// A map from a register to its current symbolic state.
using RegisterStateMap = std::unordered_map<RabbitizerRegister_GprO32, RegisterState>;

class DataFlowEngine {
public:
    // --- Public API ---

    /**
     * Analyzes a single basic block and updates the register state map.
     * @param block The basic block to analyze.
     * @param initial_state The state of all registers at the beginning of the block.
     * @return The final state of all registers after executing the block.
     */
    static RegisterStateMap analyze_block(const Block& block, const RegisterStateMap& initial_state);

private:
    // --- Helper Methods for Instruction Analysis ---

    /**
     * Processes a single instruction and updates the register state map accordingly.
     * @param instr The instruction to process.
     * @param current_state The register state map to be updated by the instruction.
     */
    static void process_instruction(const RabbitizerInstruction& instr, RegisterStateMap& current_state);
};