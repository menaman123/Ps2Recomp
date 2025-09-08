#pragma once
#include <cstdint>
#include <variant>
#include <optional>
#include <unordered_map> // Required for RegisterStateMap
#include "generated/Registers_enums.h"
#include "generated/InstrId_enum.h"

// --- State Definitions (with equality operators) ---
struct StateUnknown {
    bool operator==(const StateUnknown&) const { return true; }
    bool operator!=(const StateUnknown& other) const { return !(*this == other); }
};
struct StateConstant {
    uint64_t value;
    bool operator==(const StateConstant& other) const { return value == other.value; }
    bool operator!=(const StateConstant& other) const { return !(*this == other); }
};
struct StateSymbolic {
    RabbitizerRegister_GprO32 source_register;
    bool operator==(const StateSymbolic& other) const { return source_register == other.source_register; }
    bool operator!=(const StateSymbolic& other) const { return !(*this == other); }
};
struct StateStackRelative {
    int32_t offset;
    bool operator==(const StateStackRelative& other) const { return offset == other.offset; }
    bool operator!=(const StateStackRelative& other) const { return !(*this == other); }
};
struct StateComputed {
    RabbitizerInstrId op;
    RabbitizerRegister_GprO32 rs;
    RabbitizerRegister_GprO32 rt;
    bool operator==(const StateComputed& other) const { return op == other.op && rs == other.rs && rt == other.rt; }
    bool operator!=(const StateComputed& other) const { return !(*this == other); }
};
struct StateMemoryLoad {
    RabbitizerRegister_GprO32 base_reg;
    int32_t offset;
    bool operator==(const StateMemoryLoad& other) const { return base_reg == other.base_reg && offset == other.offset; }
    bool operator!=(const StateMemoryLoad& other) const { return !(*this == other); }
};

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
    RegisterState() : state(StateUnknown{}) {}
    template<typename T>
    RegisterState(T&& s) : state(std::forward<T>(s)) {}

    bool operator==(const RegisterState& other) const {
        return state == other.state;
    }
    bool operator!=(const RegisterState& other) const {
        return state != other.state;
    }
};

// A map from a register to its current symbolic state.
using RegisterStateMap = std::unordered_map<RabbitizerRegister_GprO32, RegisterState>;

// Global operator overload to compare two RegisterStateMaps
inline bool operator!=(const RegisterStateMap& lhs, const RegisterStateMap& rhs) {
    if (lhs.size() != rhs.size()) {
        return true;
    }
    for (const auto& [reg, state] : lhs) {
        auto it = rhs.find(reg);
        if (it == rhs.end() || it->second != state) {
            return true;
        }
    }
    return false;
}
