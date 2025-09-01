#include <iostream>
#include <vector>
#include <string>
#include <iomanip>
#include <elfio/elfio.hpp>

#include "analyze.h" // Main header for our new analysis pipeline
#include "Function.h"  // The Function class definition

// Renamed the original function to make its purpose clear.
// This performs a simple linear sweep disassembly of a memory buffer.
static void linear_disassemble_and_print(const uint8_t* code, size_t size, uint64_t base_address) {
    RabbitizerInstruction insn;
    char buffer[256];

    for (size_t offset = 0; offset + 4 <= size; offset += 4) {
        uint32_t raw_data = *(reinterpret_cast<const uint32_t*>(code + offset));
        uint64_t current_address = base_address + offset;

        RabbitizerInstructionR5900_init(&insn, raw_data, current_address);
        RabbitizerInstructionR5900_processUniqueId(&insn);

        if (RabbitizerInstruction_isValid(&insn)) {
            RabbitizerInstruction_disassemble(&insn, buffer, NULL, 0, 0);
            std::cout << "0x" << std::hex << current_address
                      << ":\t" << buffer << std::dec << std::endl;
        } else {
            std::cout << "0x" << std::hex << current_address
                      << ":\t.word   0x" << std::setw(8) << std::setfill('0') << raw_data
                      << "  // <invalid instruction>" << std::dec << std::endl;
        }
        RabbitizerInstruction_destroy(&insn);
    }
}


int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <path_to_elf_file>\n";
        return 1;
    }
    std::string filePath = argv[1];

    ELFIO::elfio reader;
    if (!reader.load(filePath)) {
        std::cerr << "[-] Could not load ELF file: " << filePath << "\n";
        return 1;
    }

    // Find the .text section
    const ELFIO::section* text_section = reader.sections[".text"];
    if (text_section == nullptr) {
        std::cerr << "[-] .text section not found in the ELF file.\n";
        return 1;
    }

    const uint8_t* text_section_data = reinterpret_cast<const uint8_t*>(text_section->get_data());
    uint32_t text_section_size = text_section->get_size();
    uint32_t text_section_vram = text_section->get_address();

    // --- 1. Original Linear Disassembly (as requested) ---
    std::cout << "======================================================================" << std::endl;
    std::cout << "---               FULL LINEAR DISASSEMBLY DUMP                   ---" << std::endl;
    std::cout << "======================================================================" << std::endl;
    linear_disassemble_and_print(text_section_data, text_section_size, text_section_vram);


    // --- 2. New Advanced Function Analysis ---
    std::cout << "\n\n";
    std::cout << "======================================================================" << std::endl;
    std::cout << "---             STARTING ADVANCED FUNCTION ANALYSIS              ---" << std::endl;
    std::cout << "======================================================================" << std::endl;
    
    std::vector<Function> analyzed_functions = analyze_executable(text_section_data, text_section_size, text_section_vram);


    // --- 3. Print the results of the advanced analysis ---
    std::cout << "\n\n";
    std::cout << "======================================================================" << std::endl;
    std::cout << "---                 ADVANCED ANALYSIS RESULTS                    ---" << std::endl;
    std::cout << "======================================================================" << std::endl;
    
    std::cout << "Analysis complete. Found " << analyzed_functions.size() << " functions." << std::endl << std::endl;

    for (const auto& func : analyzed_functions) {
        func.dump_to_console();
    }

    return 0;
}
