#include "DataFlowEngine.h"
#include "instructions/RabbitizerInstructionR5900.h"
#include "Function.h"
#include <cstdint>

// The main public entry point for the engine.
RegisterStateMap DataFlowEngine::analyze_block(const Block& block, const RegisterStateMap& initial_state) {
    // Start with a copy of the register states from the end of the predecessor block.
    RegisterStateMap final_state = initial_state;

    // Symbolically execute each instruction in the block.
    for (const auto& instr : block.instructions) {
        process_instruction(instr, final_state);
    }

    return final_state;
}

// This is the core of the engine. It understands the semantics of each MIPS instruction.
void DataFlowEngine::process_instruction(const RabbitizerInstruction& instr, RegisterStateMap& current_state) {
    const auto& id = instr.uniqueId;

    // Most instructions write to a destination register. We clear its old state first.
    // Note: Instructions like `beq` or `sw` don't modify a register, so they won't be affected.
    if (RabbitizerInstrDescriptor_modifiesRd(instr.descriptor)) {
        current_state[(RabbitizerRegister_GprO32)RAB_INSTR_GET_rd(&instr)] = RegisterState{StateUnknown{}};
    }
    if (RabbitizerInstrDescriptor_modifiesRt(instr.descriptor)) {
        current_state[(RabbitizerRegister_GprO32)RAB_INSTR_GET_rt(&instr)] = RegisterState{StateUnknown{}};
    }

    // --- Main Switch on Instruction ID ---
    switch (id) {
        // --- Arithmetic Instructions (Immediate) ---
        case RABBITIZER_INSTR_ID_cpu_addiu:
        case RABBITIZER_INSTR_ID_cpu_daddiu:
        {
            uint8_t rs = RAB_INSTR_GET_rs(&instr);
            uint8_t rt = RAB_INSTR_GET_rt(&instr);
            int32_t imm = RabbitizerInstruction_getProcessedImmediate(&instr);

            // If the source is a known constant, we can compute the new constant.
            auto rs_reg = static_cast<RabbitizerRegister_GprO32>(rs);
            if (std::holds_alternative<StateConstant>(current_state[rs_reg].state)) {
                uint64_t new_value = std::get<StateConstant>(current_state[rs_reg].state).value + imm;
                current_state[static_cast<RabbitizerRegister_GprO32>(rt)] = RegisterState{StateConstant{new_value}};
            } else {
                // Otherwise, it's a computed value.
                // Note: A more advanced implementation would create a COMPUTED state here.
                current_state[static_cast<RabbitizerRegister_GprO32>(rt)] = RegisterState{StateUnknown{}};
            }
            break;
        }

        // --- Logical Instructions ---
        case RABBITIZER_INSTR_ID_cpu_or:
        case RABBITIZER_INSTR_ID_cpu_nor:
        case RABBITIZER_INSTR_ID_cpu_and:
        case RABBITIZER_INSTR_ID_cpu_xor:
        {
            uint8_t rd = RAB_INSTR_GET_rd(&instr);
            uint8_t rs = RAB_INSTR_GET_rs(&instr);
            uint8_t rt = RAB_INSTR_GET_rt(&instr);

            auto rd_reg = static_cast<RabbitizerRegister_GprO32>(rd);
            auto rs_reg = static_cast<RabbitizerRegister_GprO32>(rs);
            auto rt_reg = static_cast<RabbitizerRegister_GprO32>(rt);

            // Check for the common 'move' pattern: or rd, rs, $zero
            if (id == RABBITIZER_INSTR_ID_cpu_or && rt_reg == RABBITIZER_REG_GPR_O32_zero) {
                current_state[rd_reg] = current_state[rs_reg]; // Propagate the state
            }
            // or rd, $zero, rs
            else if (id == RABBITIZER_INSTR_ID_cpu_or && rs_reg == RABBITIZER_REG_GPR_O32_zero) {
                current_state[rd_reg] = current_state[rt_reg]; // Propagate the state
            }
            else {
                // For now, any other logical operation results in an unknown state.
                // A more advanced implementation would create a COMPUTED state.
                current_state[rd_reg] = RegisterState{StateUnknown{}};
            }
            break;
        }

        // --- Load Instructions ---
        case RABBITIZER_INSTR_ID_cpu_lw: 
        case RABBITIZER_INSTR_ID_cpu_ld: 
        {
            uint8_t rt = RAB_INSTR_GET_rt(&instr);
            uint8_t base = RAB_INSTR_GET_rs(&instr);
            int32_t offset = RabbitizerInstruction_getProcessedImmediate(&instr);

            // Record that the register now holds a value from a specific memory location.
            current_state[static_cast<RabbitizerRegister_GprO32>(rt)] = 
                RegisterState{StateMemoryLoad{static_cast<RabbitizerRegister_GprO32>(base), offset}};
            break;
        }

        // --- Other instructions are not yet handled ---
        default: {
            // Any instruction we don't explicitly handle invalidates the destination
            // register's state, which we already did by default at the top.
            break;
        }
    }
}