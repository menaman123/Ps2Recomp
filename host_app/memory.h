#pragma once

#include "cpu_state.h"
#include <vector>
#include <iostream>

// Declare a global variable for the PS2's main memory.
extern std::vector<uint8_t> main_memory;

namespace memory {
    // Templated read and write functions
    template <typename T>
    T read(uint32_t address);

    template <typename T>
    void write(uint32_t address, T value);

    // Function to get a direct pointer to memory
    void* get_pointer(uint32_t address);
}