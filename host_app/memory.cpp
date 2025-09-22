#include "memory.h"
#include "cpu_state.h"
#include <iostream>
#include <vector>
#include <cstring>

// Defines and allocates the main memory for the emulated PS2.
std::vector<uint8_t> main_memory(32 * 1024 * 1024);

namespace memory {
    template <typename T>
    T read(uint32_t address) {
        if (address > main_memory.size() - sizeof(T)) {
            std::cerr << "FATAL_ERROR: Out-of-bounds memory read at address 0x" 
                      << std::hex << address << std::endl;
            // You might want to add more robust error handling here
            return 0;
        }
        T value;
        std::memcpy(&value, &main_memory[address], sizeof(T));
        return value;
    }

    template <typename T>
    void write(uint32_t address, T value) {
        if (address > main_memory.size() - sizeof(T)) {
            std::cerr << "FATAL_ERROR: Out-of-bounds memory write at address 0x" 
                      << std::hex << address << std::endl;
            // You might want to add more robust error handling here
            return;
        }
        std::memcpy(&main_memory[address], &value, sizeof(T));
    }
    
    // Implementation for 128-bit read
    void read_quad(uint32_t address, QuadWord& value) {
        if (address > main_memory.size() - sizeof(QuadWord)) {
            std::cerr << "FATAL_ERROR: Out-of-bounds memory read at address 0x" 
                      << std::hex << address << std::endl;
            // You might want to add more robust error handling here
            return;
        }
        std::memcpy(&value, &main_memory[address], sizeof(QuadWord));
    }

    // Implementation for 128-bit write
    void write_quad(uint32_t address, const QuadWord& value) {
        if (address > main_memory.size() - sizeof(QuadWord)) {
            std::cerr << "FATAL_ERROR: Out-of-bounds memory write at address 0x" 
                      << std::hex << address << std::endl;
            // You might want to add more robust error handling here
            return;
        }
        std::memcpy(&main_memory[address], &value, sizeof(QuadWord));
    }

    void* get_pointer(uint32_t address) {
        if (address > main_memory.size()) {
            return nullptr;
        }
        return &main_memory[address];
    }

    // Explicit template instantiations
    template uint8_t read<uint8_t>(uint32_t address);
    template uint16_t read<uint16_t>(uint32_t address);
    template uint32_t read<uint32_t>(uint32_t address);
    template uint64_t read<uint64_t>(uint32_t address);
    template void write<uint8_t>(uint32_t address, uint8_t value);
    template void write<uint16_t>(uint32_t address, uint16_t value);
    template void write<uint32_t>(uint32_t address, uint32_t value);
    template void write<uint64_t>(uint32_t address, uint64_t value);
}