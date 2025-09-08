#pragma once
#include <cstdint>
#include <variant>
#include <optional>
#include "generated/Registers_enums.h"
#include "generated/InstrId_enum.h"

// --- State Definitions ---
// Default/unknown state
struct StateUnknown {};
// A known 64-bit integer value
struct StateConstant {
    uint64_t value;
};
// A copy of another register's value (e.g., from a 'move' instruction)
struct StateSymbolic {
    RabbitizerRegister_GprO32 source_register;
};
// An offset from the initial stack pointer
struct StateStackRelative {
    int32_t offset;
};
// The result of an arithmetic operation.
// The analysis engine will look up the states of the source registers when needed.
struct StateComputed {
    RabbitizerInstrId op;
    RabbitizerRegister_GprO32 rs;
    RabbitizerRegister_GprO32 rt;
};
// A value loaded from memory.
struct StateMemoryLoad {
    RabbitizerRegister_GprO32 base_reg;
    int32_t offset;
};
// A class to hold the details of the state using a std::variant
using RegisterStateVariant = std::variant<
    StateUnknown,
    StateConstant,
    StateSymbolic,
    StateStackRelative,
    StateComputed,
    StateMemoryLoad
>;
class RegisterState {
public:
    RegisterStateVariant state;
    // Default constructor to initialize with an unknown state
    RegisterState() : state(StateUnknown{}) {}
    // Constructor to create a state from a specific type
    template<typename T>
    RegisterState(T&& s) : state(std::forward<T>(s)) {}
};