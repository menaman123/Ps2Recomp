#include "memory.h"
#include "cpu_state.h"
#include <iostream>
#include <vector>
#include <cstring>
#include <unordered_map>

// PS2 Memory Map
// Main RAM: 32MB at 0x00000000
// Scratchpad RAM: 16KB at 0x70000000
// IOP RAM: 2MB at 0x1C000000 (simplified)

std::vector<uint8_t> main_memory(32 * 1024 * 1024);      // 32MB Main RAM
std::vector<uint8_t> scratchpad_memory(16 * 1024);       // 16KB Scratchpad
std::vector<uint8_t> iop_memory(2 * 1024 * 1024);        // 2MB IOP RAM

namespace memory {
    // Translate PS2 virtual address to host memory
    uint8_t* translate_address(uint32_t address, size_t size) {
        // Main RAM (0x00000000 - 0x01FFFFFF)
        if (address < 0x02000000) {
            if (address + size > main_memory.size()) {
                std::cerr << "ERROR: Main RAM access out of bounds at 0x" 
                          << std::hex << address << std::endl;
                return nullptr;
            }
            return &main_memory[address];
        }
        // Scratchpad RAM (0x70000000 - 0x70003FFF)
        else if (address >= 0x70000000 && address < 0x70004000) {
            uint32_t offset = address - 0x70000000;
            if (offset + size > scratchpad_memory.size()) {
                std::cerr << "ERROR: Scratchpad access out of bounds at 0x" 
                          << std::hex << address << std::endl;
                return nullptr;
            }
            return &scratchpad_memory[offset];
        }
        // IOP RAM (0x1C000000 - 0x1C1FFFFF) - simplified
        else if (address >= 0x1C000000 && address < 0x1C200000) {
            uint32_t offset = address - 0x1C000000;
            if (offset + size > iop_memory.size()) {
                std::cerr << "ERROR: IOP RAM access out of bounds at 0x" 
                          << std::hex << address << std::endl;
                return nullptr;
            }
            return &iop_memory[offset];
        }
        // Mirror regions - PS2 has several memory mirrors
        // Main RAM mirror at 0x80000000-0x81FFFFFF (cached)
        else if (address >= 0x80000000 && address < 0x82000000) {
            return translate_address(address - 0x80000000, size);
        }
        // Main RAM mirror at 0xA0000000-0xA1FFFFFF (uncached)
        else if (address >= 0xA0000000 && address < 0xA2000000) {
            return translate_address(address - 0xA0000000, size);
        }
        else {
            std::cerr << "WARNING: Unmapped memory access at 0x" 
                      << std::hex << address << " (size: " << size << ")" << std::endl;
            // Return nullptr for unmapped regions
            return nullptr;
        }
    }

    template <typename T>
    T read(uint32_t address) {
        uint8_t* ptr = translate_address(address, sizeof(T));
        if (!ptr) {
            // For unmapped reads, return 0 (common behavior)
            return 0;
        }
        T value;
        std::memcpy(&value, ptr, sizeof(T));
        return value;
    }

    template <typename T>
    void write(uint32_t address, T value) {
        uint8_t* ptr = translate_address(address, sizeof(T));
        if (!ptr) {
            // For unmapped writes, just ignore (or you could trigger an exception)
            return;
        }
        std::memcpy(ptr, &value, sizeof(T));
    }
    
    // Implementation for 128-bit read
    void read_quad(uint32_t address, QuadWord& value) {
        uint8_t* ptr = translate_address(address, sizeof(QuadWord));
        if (!ptr) {
            std::memset(&value, 0, sizeof(QuadWord));
            return;
        }
        std::memcpy(&value, ptr, sizeof(QuadWord));
    }

    // Implementation for 128-bit write
    void write_quad(uint32_t address, const QuadWord& value) {
        uint8_t* ptr = translate_address(address, sizeof(QuadWord));
        if (!ptr) {
            return;
        }
        std::memcpy(ptr, &value, sizeof(QuadWord));
    }

    void* get_pointer(uint32_t address) {
        return translate_address(address, 1);
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