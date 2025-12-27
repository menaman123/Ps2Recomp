#pragma once

#include "cpu_state.h"
#include <vector>
#include <iostream>

// Declare a global variable for the PS2's main memory.
extern std::vector<uint8_t> main_memory;

// Define a 128-bit data type for quadword operations.
struct alignas(16) QuadWord {
    uint8_t m2[16];
};


namespace memory {
    uint8_t* translate_address(uint32_t address, size_t size);
    void initialize();
    // Templated read and write functions
    template <typename T>
    T read(uint32_t address);

    template <typename T>
    void write(uint32_t address, T value);

    // Functions for reading and writing 128-bit quadwords
    void read_quad(uint32_t address, QuadWord& value);
    void write_quad(uint32_t address, const QuadWord& value);

    // Function to get a direct pointer to memory
    void* get_pointer(uint32_t address);
}