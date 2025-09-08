#include <iostream>
#include <iomanip>
#include <vector>
#include <set>
#include <string>
#include <fstream>
#include <algorithm>
#include <elfio/elfio.hpp>

// Rabbitizer headers
#include "rabbitizer.h"
#include "instructions/RabbitizerInstruction.h"
#include "instructions/RabbitizerInstructionR5900.h"

// Use the macros from Rabbitizer's headers
#define GET_instr_index(instr_ptr)     RAB_INSTR_GET_instr_index(instr_ptr)
#define GET_rs(instr_ptr)              RAB_INSTR_GET_rs(instr_ptr)
#define GET_rt(instr_ptr)              RAB_INSTR_GET_rt(instr_ptr)
#define GET_immediate(instr_ptr)       RAB_INSTR_GET_immediate(instr_ptr)

using namespace ELFIO;

// Function to load Ghidra's function list
std::set<Elf64_Addr> load_ghidra_functions(const std::string& path) {
    std::set<Elf64_Addr> ghidra_functions;
    std::ifstream infile(path);
    std::string line;
    const std::string addr_prefix = "Address: ";
    while (std::getline(infile, line)) {
        size_t pos = line.find(addr_prefix);
        if (pos != std::string::npos) {
            size_t start = pos + addr_prefix.length();
            size_t end = line.find(" |", start);
            if (end != std::string::npos) {
                std::string addr_str = line.substr(start, end - start);
                try {
                    ghidra_functions.insert(std::stoull(addr_str, nullptr, 16));
                }
                catch (const std::invalid_argument& ia) {
                    // Could not parse hex string, ignore.
                }
            }
        }
    }
    return ghidra_functions;
}

int main(int argc, const char* argv[]) {
    if (argc != 3) {
        std::cout << "Usage: function_finder <elf_file> <ghidra_function_list.txt>" << std::endl;
        return 1;
    }

    elfio reader;
    if (!reader.load(argv[1])) {
        std::cout << "Can't find or process ELF file " << argv[1] << std::endl;
        return 2;
    }



    section* text_sec = reader.sections[".text"];
    if (!text_sec) {
        std::cout << "'.text' section not found." << std::endl;
        return 3;
    }

    std::cout << "--- Section List ---\n";
    for (int i = 0; i < reader.sections.size(); ++i) {
        section* psec = reader.sections[i];
        std::cout << "Section " << i << ": " << psec->get_name()
                  << " | Addr: 0x" << std::hex << psec->get_address()
                  << " | Size: 0x" << psec->get_size()
                  << " | Flags: " << psec->get_flags() << std::dec << std::endl;
    }

    const char* text_data = text_sec->get_data();
    Elf_Xword text_size = text_sec->get_size();
    Elf64_Addr text_addr = text_sec->get_address();

    std::set<Elf64_Addr> jal_targets;
    std::set<Elf64_Addr> fallthrough_prologue_targets;
    std::set<Elf64_Addr> symbol_targets;

    // --- Add ELF entry point ---
    symbol_targets.insert(reader.get_entry());

    // --- Sweep 1: Find all JAL targets ---
    for (unsigned int i = 0; i < text_size; i += 4) {
        uint32_t instr_word = *(reinterpret_cast<const uint32_t*>(text_data + i));
        RabbitizerInstruction instr;
        RabbitizerInstruction_init(&instr, instr_word, text_addr + i);
        RabbitizerInstructionR5900_processUniqueId(&instr);

        if (instr.uniqueId == RABBITIZER_INSTR_ID_cpu_jal) {
            uint32_t target_val = GET_instr_index(&instr);
            Elf64_Addr target_addr = (instr.vram & 0xF0000000) | (target_val << 2);
            jal_targets.insert(target_addr);
        }

        RabbitizerInstruction_destroy(&instr);
    }

    // --- Sweep 2: Check for remaining functions proceeding jr instructions but not called by JAL ---
    for (unsigned int i = 0; i < text_size; i += 4) {
        uint32_t instr_word = *(reinterpret_cast<const uint32_t*>(text_data + i));
        RabbitizerInstruction instr;
        RabbitizerInstruction_init(&instr, instr_word, text_addr + i);
        RabbitizerInstructionR5900_processUniqueId(&instr);

        if (instr.uniqueId == RABBITIZER_INSTR_ID_cpu_jr && GET_rs(&instr) == 31) { // jr ra
            // --- Skip Padding after jr ra ---
            unsigned int current_offset = i + 8; // Start after the delay slot
            while (current_offset < text_size) {
                uint32_t word = *(reinterpret_cast<const uint32_t*>(text_data + current_offset));
                RabbitizerInstruction padding_check_instr;
                RabbitizerInstruction_init(&padding_check_instr, word, text_addr + current_offset);
                RabbitizerInstructionR5900_processUniqueId(&padding_check_instr);
                
                // A real instruction is not a NOP and is not invalid.
                if (padding_check_instr.uniqueId != RABBITIZER_INSTR_ID_cpu_nop && padding_check_instr.uniqueId != RABBITIZER_INSTR_ID_cpu_INVALID) {
                    RabbitizerInstruction_destroy(&padding_check_instr);
                    break; // Found a real instruction
                }
                
                RabbitizerInstruction_destroy(&padding_check_instr);
                current_offset += 4;
            }

            if (current_offset < text_size) {
                Elf64_Addr potential_func_start = text_addr + current_offset;

                // --- Lookahead for Prologue ---
                for (int j = 0; j < 10; ++j) { // Look ahead 10 instructions
                    unsigned int lookahead_offset = current_offset + (j * 4);
                    if (lookahead_offset >= text_size) break;

                    uint32_t next_instr_word = *(reinterpret_cast<const uint32_t*>(text_data + lookahead_offset));
                    RabbitizerInstruction next_instr;
                    RabbitizerInstruction_init(&next_instr, next_instr_word, text_addr + lookahead_offset);
                    RabbitizerInstructionR5900_processUniqueId(&next_instr);

                    bool is_prologue = false;
                    // Check for addiu sp, sp, -N
                    if (next_instr.uniqueId == RABBITIZER_INSTR_ID_cpu_addiu &&
                        GET_rs(&next_instr) == 29 && GET_rt(&next_instr) == 29) {
                        int16_t immediate = GET_immediate(&next_instr);
                        if (immediate < 0) {
                            is_prologue = true;
                        }
                    }
                    // Check for sw ra, offset(sp) or sd ra, offset(sp)
                    else if ((next_instr.uniqueId == RABBITIZER_INSTR_ID_cpu_sw || next_instr.uniqueId == RABBITIZER_INSTR_ID_cpu_sd) &&
                             GET_rs(&next_instr) == 29 && GET_rt(&next_instr) == 31) {
                        is_prologue = true;
                    }

                    if (is_prologue) {
                        // Check if the address is already a known JAL target
                        if (jal_targets.find(potential_func_start) == jal_targets.end()) {
                            fallthrough_prologue_targets.insert(potential_func_start);
                        }
                        RabbitizerInstruction_destroy(&next_instr);
                        break; // Found prologue, stop lookahead
                    }
                    
                    RabbitizerInstruction_destroy(&next_instr);
                }
            }
        }

        RabbitizerInstruction_destroy(&instr);
    }

    // --- Sweep 4: Find functions from symbol table ---
    for (int i = 0; i < reader.sections.size(); ++i) {
        section* psec = reader.sections[i];
        if (psec->get_type() == SHT_SYMTAB) {
            const symbol_section_accessor symbols(reader, psec);
            for (unsigned int j = 0; j < symbols.get_symbols_num(); ++j) {
                std::string name;
                Elf64_Addr value;
                Elf_Xword size;
                unsigned char bind;
                unsigned char type;
                Elf_Half section_index;
                unsigned char other;
                symbols.get_symbol(j, name, value, size, bind, type, section_index, other);
                if (type == STT_FUNC) {
                    symbol_targets.insert(value);
                }
            }
        }
    }

    // --- Sweep 5: Scan for function pointers in data sections ---
    std::cout << "--- ELF Properties ---\n";
    std::cout << "Class: " << (reader.get_class() == ELFCLASS32 ? "32-bit" : "64-bit") << std::endl;
    std::cout << "Encoding: " << (reader.get_encoding() == ELFDATA2LSB ? "Little Endian" : "Big Endian") << std::endl;

    std::set<Elf64_Addr> pointer_targets;
    const std::set<std::string> sections_to_scan = {".data", ".rodata", ".sdata"};

    for (const auto& sec_name : sections_to_scan) {
        section* psec = reader.sections[sec_name];
        if (psec) {
            std::cout << "Scanning section: " << sec_name << std::endl;
            const char* sec_data = psec->get_data();
            Elf_Xword sec_size = psec->get_size();

            for (Elf_Xword j = 0; j < sec_size; j += 4) {
                if (j + 4 > sec_size) continue;

                uint32_t potential_ptr = (reinterpret_cast<const uint32_t>(sec_data + j));

                // --- START OF NEW DEBUG CODE ---
                if (sec_name == ".rodata") {
                    std::cout << "  .rodata offset 0x" << std::hex << j << ": 0x" << potential_ptr << std::dec << std::endl;
                }
                // --- END OF NEW DEBUG CODE ---

                // Check if the pointer is within the .text section range
                if (potential_ptr >= text_addr && potential_ptr < (text_addr + text_size)) {
                    // Check if the address is 4-byte aligned
                    if ((potential_ptr % 4) == 0) {
                        pointer_targets.insert(potential_ptr);
                    }
                }
            }
        }
    }

    // --- Combine results ---
    std::set<Elf64_Addr> all_found_functions = jal_targets;
    all_found_functions.insert(fallthrough_prologue_targets.begin(), fallthrough_prologue_targets.end());
    all_found_functions.insert(symbol_targets.begin(), symbol_targets.end());
    all_found_functions.insert(pointer_targets.begin(), pointer_targets.end());

    // --- Load Ghidra results and compare ---
    std::set<Elf64_Addr> ghidra_functions = load_ghidra_functions(argv[2]);

    std::set<Elf64_Addr> correct_functions;
    std::set_intersection(all_found_functions.begin(), all_found_functions.end(),
                          ghidra_functions.begin(), ghidra_functions.end(),
                          std::inserter(correct_functions, correct_functions.begin()));

    std::set<Elf64_Addr> missed_functions;
    std::set_difference(ghidra_functions.begin(), ghidra_functions.end(),
                        all_found_functions.begin(), all_found_functions.end(),
                        std::inserter(missed_functions, missed_functions.begin()));

    std::set<Elf64_Addr> incorrect_functions;
    std::set_difference(all_found_functions.begin(), all_found_functions.end(),
                        ghidra_functions.begin(), ghidra_functions.end(),
                        std::inserter(incorrect_functions, incorrect_functions.begin()));

    std::cout << "\n--- INCORRECT Functions ---\n";
    /*
    for (const auto& addr : incorrect_functions) {
        std::cout << "0x" << std::hex << addr << std::endl;
    }
    */


    std::cout << std::dec;

    // --- Report Results ---
    std::cout << "Analysis Report" << std::endl;
    std::cout << "Ghidra Functions: " << ghidra_functions.size() << std::endl;
    std::cout << "Functions Found (Total): " << jal_targets.size() + fallthrough_prologue_targets.size() << std::endl;
    std::cout << "  - Via JAL: " << jal_targets.size() << std::endl;
    std::cout << "  - Via Fallthrough Prologue: " << fallthrough_prologue_targets.size() << std::endl;
    std::cout << "  - Via Symbols: " << symbol_targets.size() << std::endl;
    std::cout << "  - Via Pointers: " << pointer_targets.size() << std::endl;
    std::cout << "Comparison" << std::endl;
    std::cout << "Correctly Identified: " << correct_functions.size() << std::endl;
    std::cout << "Incorrectly Identified (False Positives): " << incorrect_functions.size() << std::endl;
    std::cout << "Missed Functions (False Negatives): " << missed_functions.size() << std::endl;

    // --- Detailed Lists ---
    /*
        std::cout << "\n--- Missed Functions ---\n";
        for (const auto& addr : missed_functions) {
            std::cout << "0x" << std::hex << addr << std::endl;
        }
    */


    std::set<Elf64_Addr> non_jal_functions;
    non_jal_functions.insert(fallthrough_prologue_targets.begin(), fallthrough_prologue_targets.end());
    non_jal_functions.insert(symbol_targets.begin(), symbol_targets.end());

    std::set<Elf64_Addr> correct_non_jal_functions;
    std::set_intersection(non_jal_functions.begin(), non_jal_functions.end(),
                          correct_functions.begin(), correct_functions.end(),
                          std::inserter(correct_non_jal_functions, correct_non_jal_functions.begin()));

    std::set<Elf64_Addr> final_non_jal_list;
    std::set_difference(correct_non_jal_functions.begin(), correct_non_jal_functions.end(),
                        jal_targets.begin(), jal_targets.end(),
                        std::inserter(final_non_jal_list, final_non_jal_list.begin()));

    std::cout << "\n--- Correctly Identified Functions Not Found Via JAL ---\n";
    std::cout << "Count: " << final_non_jal_list.size() << std::endl;
    /*
        for (const auto& addr : final_non_jal_list) {
        std::cout << "0x" << std::hex << addr << std::endl;
    }
    */


    return 0;
}


