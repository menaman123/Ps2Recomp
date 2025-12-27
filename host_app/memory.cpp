#include "memory.h"
#include "cpu_state.h"
#include <iostream>
#include <vector>
#include <cstring>
#include <unordered_map>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <iomanip>

// PS2 Memory Map
// Main RAM: 32MB at 0x00000000
// Scratchpad RAM: 16KB at 0x70000000
// IOP RAM: 2MB at 0x1C000000 (simplified)

// memory.h - Add these declarations
std::vector<uint8_t> main_memory(32 * 1024 * 1024);      // 32MB Main RAM
std::vector<uint8_t> scratchpad_memory(16 * 1024);       // 16KB Scratchpad
std::vector<uint8_t> iop_memory(2 * 1024 * 1024);        // 2MB IOP RAM
std::vector<uint8_t> bios_memory(4 * 1024 * 1024);       // 4MB BIOS
std::vector<uint8_t> vu0_code_memory(4 * 1024);          // 4KB VU0 code
std::vector<uint8_t> vu0_data_memory(4 * 1024);          // 4KB VU0 data
std::vector<uint8_t> vu1_code_memory(16 * 1024);         // 16KB VU1 code
std::vector<uint8_t> vu1_data_memory(16 * 1024);         // 16KB VU1 data

extern std::ofstream g_logFile;

namespace memory {
    void initialize(){
        std::fill(main_memory.begin(), main_memory.end(), 0);
        
        std::string map_path = "C:/Users/Owner/Desktop/PS2_Recomp/ghidra_functions/ram_data_map.txt";
        std::ifstream file(map_path);

        if (!file.is_open()) {
            std::cerr << "Fatal Error: Could not open " << map_path << std::endl;
            return;
        }

        std::string line;
        size_t entries_loaded = 0;

        while (std::getline(file, line)) {
            // Skip empty lines or purely comment lines
            if (line.empty() || line[0] == '#') continue;

            // Split line at the colon
            size_t colon_pos = line.find(':');
            if (colon_pos == std::string::npos) continue;

            // Extract address and the value string (before any potential comment)
            std::string addr_str = line.substr(0, colon_pos);
            std::string val_part = line.substr(colon_pos + 1);
            
            // Trim potential comments (starts with #)
            size_t hash_pos = val_part.find('#');
            std::string val_str = (hash_pos != std::string::npos) ? val_part.substr(0, hash_pos) : val_part;

            // Trim whitespace
            val_str.erase(val_str.find_last_not_of(" \n\r\t") + 1);

            try {
                // Convert hex address string to uint32
                uint32_t address = std::stoul(addr_str, nullptr, 16);
                
                // Remove "0x" from value string if present
                if (val_str.find("0x") == 0) val_str = val_str.substr(2);

                // Calculate number of bytes (2 hex chars = 1 byte)
                // Note: val_str is currently Big Endian representation of the LE bytes
                // Example: 0x002F3678 -> "002F3678"
                size_t num_bytes = val_str.length() / 2;

                for (size_t i = 0; i < num_bytes; ++i) {
                    // Extract byte-sized hex chunk (reading from the right for Little Endian)
                    // We extract 2 chars starting from the end of the string
                    std::string byte_hex = val_str.substr((num_bytes - 1 - i) * 2, 2);
                    uint8_t byte_val = static_cast<uint8_t>(std::stoul(byte_hex, nullptr, 16));

                    // Safely write to memory
                    if (address + i < main_memory.size()) {
                        main_memory[address + i] = byte_val;
                    }
                }
                entries_loaded++;
            } catch (const std::exception& e) {
                std::cerr << "Warning: Skipping malformed line: " << line << " (" << e.what() << ")" << std::endl;
            }
        }

        std::cout << "Memory initialized: Loaded " << entries_loaded << " entries from text map." << std::endl;
        file.close();

        // Sanity Check
        uint32_t check_addr = 0x002e6bc0; // Pointer address from your snippet
        uint32_t val = read<uint32_t>(check_addr);
        std::cout << "Sanity Check: Pointer at 0x" << std::hex << check_addr 
                  << " is 0x" << std::setw(8) << std::setfill('0') << val << std::dec << std::endl;
    }

    // Translate PS2 virtual address to host memory
// Translate PS2 virtual address to host memory
    uint8_t* translate_address(uint32_t address, size_t size) {
        // Main RAM (0x00000000 - 0x01FFFFFF) - 32MB
        if (address >= 0x00000000 && address < 0x02000000) {
            if (address + size > main_memory.size()) {
                std::cerr << "ERROR: Main RAM access out of bounds at 0x" 
                        << std::hex << address << std::endl;
                return nullptr;
            }
            return &main_memory[address];
        }
        // Main RAM uncached mirror (0x20000000-0x21FFFFFF) - 32MB
        else if (address >= 0x20000000 && address < 0x22000000) {
            uint32_t offset = address - 0x20000000;
            if (offset + size > main_memory.size()) {
                std::cerr << "ERROR: Main RAM mirror (0x20000000) access out of bounds at 0x" 
                        << std::hex << address << std::endl;
                return nullptr;
            }
            return &main_memory[offset];
        }
        // Main RAM uncached accelerated mirror (0x30100000-0x31FFFFFF) - 31MB
        // NOTE: Starts at +1MB offset (0x00100000 in physical RAM)
        else if (address >= 0x30100000 && address < 0x32000000) {
            uint32_t offset = address - 0x30000000;
            if (offset + size > main_memory.size()) {
                std::cerr << "ERROR: Main RAM mirror (0x30100000) access out of bounds at 0x" 
                        << std::hex << address << std::endl;
                return nullptr;
            }
            return &main_memory[offset];
        }
        // I/O registers (0x10000000 - 0x1000FFFF) - 64KB
        else if (address >= 0x10000000 && address < 0x10010000) {
            std::cerr << "WARNING: I/O register access at 0x" 
                    << std::hex << address << " - not yet implemented" << std::endl;
            return nullptr;
        }
        // VU0 code memory (0x11000000 - 0x11000FFF) - 4KB
        else if (address >= 0x11000000 && address < 0x11001000) {
            uint32_t offset = address - 0x11000000;
            if (offset + size > vu0_code_memory.size()) {
                std::cerr << "ERROR: VU0 code memory access out of bounds at 0x" 
                        << std::hex << address << std::endl;
                return nullptr;
            }
            return &vu0_code_memory[offset];
        }
        // VU0 data memory (0x11004000 - 0x11004FFF) - 4KB
        else if (address >= 0x11004000 && address < 0x11005000) {
            uint32_t offset = address - 0x11004000;
            if (offset + size > vu0_data_memory.size()) {
                std::cerr << "ERROR: VU0 data memory access out of bounds at 0x" 
                        << std::hex << address << std::endl;
                return nullptr;
            }
            return &vu0_data_memory[offset];
        }
        // VU1 code memory (0x11008000 - 0x1100BFFF) - 16KB
        else if (address >= 0x11008000 && address < 0x1100C000) {
            uint32_t offset = address - 0x11008000;
            if (offset + size > vu1_code_memory.size()) {
                std::cerr << "ERROR: VU1 code memory access out of bounds at 0x" 
                        << std::hex << address << std::endl;
                return nullptr;
            }
            return &vu1_code_memory[offset];
        }
        // VU1 data memory (0x1100C000 - 0x1100FFFF) - 16KB
        else if (address >= 0x1100C000 && address < 0x11010000) {
            uint32_t offset = address - 0x1100C000;
            if (offset + size > vu1_data_memory.size()) {
                std::cerr << "ERROR: VU1 data memory access out of bounds at 0x" 
                        << std::hex << address << std::endl;
                return nullptr;
            }
            return &vu1_data_memory[offset];
        }
        // GS privileged registers (0x12000000 - 0x12001FFF) - 8KB
        else if (address >= 0x12000000 && address < 0x12002000) {
            std::cerr << "WARNING: GS register access at 0x" 
                    << std::hex << address << " - not yet implemented" << std::endl;
            return nullptr;
        }
        // IOP RAM (0x1C000000 - 0x1C1FFFFF) - 2MB
        else if (address >= 0x1C000000 && address < 0x1C200000) {
            uint32_t offset = address - 0x1C000000;
            if (offset + size > iop_memory.size()) {
                std::cerr << "ERROR: IOP RAM access out of bounds at 0x" 
                        << std::hex << address << std::endl;
                return nullptr;
            }
            return &iop_memory[offset];
        }
        // BIOS (0x1FC00000 - 0x1FFFFFFF) - 4MB (rom0)
        else if (address >= 0x1FC00000 && address < 0x20000000) {
            uint32_t offset = address - 0x1FC00000;
            if (offset + size > bios_memory.size()) {
                std::cerr << "ERROR: BIOS access out of bounds at 0x" 
                        << std::hex << address << std::endl;
                return nullptr;
            }
            return &bios_memory[offset];
        }


        // Scratchpad RAM (0x70000000 - 0x70003FFF) - 16KB
        else if (address >= 0x70000000 && address < 0x70004000) {
            uint32_t offset = address - 0x70000000;
            if (offset + size > scratchpad_memory.size()) {
                std::cerr << "ERROR: Scratchpad access out of bounds at 0x" 
                        << std::hex << address << std::endl;
                return nullptr;
            }
            return &scratchpad_memory[offset];
        }
        // Main RAM mirror at 0x80000000-0x81FFFFFF (KSEG0 - cached)
        else if (address >= 0x80000000 && address < 0x82000000) {
            uint32_t offset = address - 0x80000000;
            if (offset + size > main_memory.size()) {
                std::cerr << "ERROR: Main RAM mirror (KSEG0) access out of bounds at 0x" 
                        << std::hex << address << std::endl;
                return nullptr;
            }
            return &main_memory[offset];
        }
        // BIOS mirror at 0x9FC00000-0x9FFFFFFF (KSEG0 - cached - rom09)
        else if (address >= 0x9FC00000 && address < 0xA0000000) {
            uint32_t offset = address - 0x9FC00000;
            if (offset + size > bios_memory.size()) {
                std::cerr << "ERROR: BIOS mirror (KSEG0) access out of bounds at 0x" 
                        << std::hex << address << std::endl;
                return nullptr;
            }
            return &bios_memory[offset];
        }
        // Main RAM mirror at 0xA0000000-0xA1FFFFFF (KSEG1 - uncached)
        else if (address >= 0xA0000000 && address < 0xA2000000) {
            uint32_t offset = address - 0xA0000000;
            if (offset + size > main_memory.size()) {
                std::cerr << "ERROR: Main RAM mirror (KSEG1) access out of bounds at 0x" 
                        << std::hex << address << std::endl;
                return nullptr;
            }
            return &main_memory[offset];
        }
        // BIOS mirror at 0xBFC00000-0xBFFFFFFF (KSEG1 - uncached - rom0b)
        else if (address >= 0xBFC00000 && address < 0xC0000000) {
            uint32_t offset = address - 0xBFC00000;
            if (offset + size > bios_memory.size()) {
                std::cerr << "ERROR: BIOS mirror (KSEG1) access out of bounds at 0x" 
                        << std::hex << address << std::endl;
                return nullptr;
            }
            return &bios_memory[offset];
        }
        else {
            /*std::cerr << "WARNING: Unmapped memory access at 0x" 
                    << std::hex << address << " (size: " << size << ")" << std::endl;*/
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
        //g_logFile << "SUCCESS: Read memory access at 0x" << std::hex << address << std::endl;
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