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

// Generic function to load a set of addresses from a text file (one hex address per line)
std::set<Elf64_Addr> load_addresses_from_file(const std::string& path) {
    std::set<Elf64_Addr> addresses;
    std::ifstream infile(path);
    std::string line;
    while (std::getline(infile, line)) {
        // Remove potential prefixes and suffixes, just in case
        size_t first = line.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) continue;
        size_t last = line.find_last_not_of(" \t\r\n");
        line = line.substr(first, (last - first + 1));

        try {
            addresses.insert(std::stoull(line, nullptr, 16));
        }
        catch (const std::invalid_argument& ia) {
            // Ignore lines that are not valid hex numbers
        }
    }
    return addresses;
}

// --- Sweep 1: Find all JAL targets ---
std::set<Elf64_Addr> find_jal_targets(const char* text_data, Elf_Xword text_size, Elf64_Addr text_addr) {
    std::set<Elf64_Addr> jal_targets;
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
    return jal_targets;
}

// --- Sweep 2: Check for remaining functions proceeding jr instructions but not called by JAL ---
std::set<Elf64_Addr> find_fallthrough_prologues(const char* text_data, Elf_Xword text_size, Elf64_Addr text_addr, const std::set<Elf64_Addr>& jal_targets) {
    std::set<Elf64_Addr> fallthrough_prologue_targets;
    for (unsigned int i = 0; i < text_size; i += 4) {
        uint32_t instr_word = *(reinterpret_cast<const uint32_t*>(text_data + i));
        RabbitizerInstruction instr;
        RabbitizerInstruction_init(&instr, instr_word, text_addr + i);
        RabbitizerInstructionR5900_processUniqueId(&instr);

        if (instr.uniqueId == RABBITIZER_INSTR_ID_cpu_jr && GET_rs(&instr) == 31) { // jr ra
            unsigned int current_offset = i + 8; // Start after the delay slot
            while (current_offset < text_size) {
                uint32_t word = *(reinterpret_cast<const uint32_t*>(text_data + current_offset));
                RabbitizerInstruction padding_check_instr;
                RabbitizerInstruction_init(&padding_check_instr, word, text_addr + current_offset);
                RabbitizerInstructionR5900_processUniqueId(&padding_check_instr);
                
                if (padding_check_instr.uniqueId != RABBITIZER_INSTR_ID_cpu_nop && padding_check_instr.uniqueId != RABBITIZER_INSTR_ID_cpu_INVALID) {
                    RabbitizerInstruction_destroy(&padding_check_instr);
                    break;
                }
                
                RabbitizerInstruction_destroy(&padding_check_instr);
                current_offset += 4;
            }

            if (current_offset < text_size) {
                Elf64_Addr potential_func_start = text_addr + current_offset;
                for (int j = 0; j < 10; ++j) {
                    unsigned int lookahead_offset = current_offset + (j * 4);
                    if (lookahead_offset >= text_size) break;

                    uint32_t next_instr_word = *(reinterpret_cast<const uint32_t*>(text_data + lookahead_offset));
                    RabbitizerInstruction next_instr;
                    RabbitizerInstruction_init(&next_instr, next_instr_word, text_addr + lookahead_offset);
                    RabbitizerInstructionR5900_processUniqueId(&next_instr);

                    bool is_prologue = false;
                    if (next_instr.uniqueId == RABBITIZER_INSTR_ID_cpu_addiu && GET_rs(&next_instr) == 29 && GET_rt(&next_instr) == 29) {
                        if (static_cast<int16_t>(GET_immediate(&next_instr)) < 0) is_prologue = true;
                    }
                    else if ((next_instr.uniqueId == RABBITIZER_INSTR_ID_cpu_sw || next_instr.uniqueId == RABBITIZER_INSTR_ID_cpu_sd) && GET_rs(&next_instr) == 29 && GET_rt(&next_instr) == 31) {
                        is_prologue = true;
                    }

                    if (is_prologue) {
                        if (jal_targets.find(potential_func_start) == jal_targets.end()) {
                            fallthrough_prologue_targets.insert(potential_func_start);
                        }
                        RabbitizerInstruction_destroy(&next_instr);
                        break;
                    }
                    
                    RabbitizerInstruction_destroy(&next_instr);
                }
            }
        }
        RabbitizerInstruction_destroy(&instr);
    }
    return fallthrough_prologue_targets;
}

// --- Sweep 4: Find functions from symbol table ---
std::set<Elf64_Addr> find_symbol_table_functions(elfio& reader) {
    std::set<Elf64_Addr> symbol_targets;
    symbol_targets.insert(reader.get_entry()); // Add ELF entry point
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
    return symbol_targets;
}

// --- Sweep 5: Programmatically scan for function pointers in data sections ---
std::set<Elf64_Addr> find_pointers_in_data_sections(elfio& reader, Elf64_Addr text_addr, Elf_Xword text_size) {
    std::set<Elf64_Addr> pointer_targets;
    const std::set<std::string> sections_to_scan = { ".data", ".rodata", ".sdata" };

    for (const auto& sec_name : sections_to_scan) {
        section* psec = reader.sections[sec_name];
        if (psec) {
            const char* sec_data = psec->get_data();
            Elf_Xword sec_size = psec->get_size();

            for (Elf_Xword j = 0; j < sec_size; j += 4) {
                if (j + 4 > sec_size) continue;

                uint32_t potential_ptr = *(reinterpret_cast<const uint32_t*>(sec_data + j));

                if (potential_ptr >= text_addr && potential_ptr < (text_addr + text_size)) {
                    if ((potential_ptr % 4) == 0) {
                        pointer_targets.insert(potential_ptr);
                    }
                }
            }
        }
    }
    return pointer_targets;
}


int main(int argc, const char* argv[]) {
    if (argc != 4) {
        std::cout << "Usage: function_finder <elf_file> <ghidra_function_list.txt> <ghidra_data_addresses.txt>" << std::endl;
        return 1;
    }

    elfio reader;
    if (!reader.load(argv[1])) {
        std::cout << "Can't find or process ELF file " << argv[1] << std::endl;
        return 2;
    }

    section* text_sec = reader.sections[ ".text" ];
    if (!text_sec) {
        std::cout << "'.text' section not found." << std::endl;
        return 3;
    }

    const char* text_data = text_sec->get_data();
    const Elf_Xword text_size = text_sec->get_size();
    const Elf64_Addr text_addr = text_sec->get_address();

    // --- Run analysis sweeps ---
    std::set<Elf64_Addr> jal_targets = find_jal_targets(text_data, text_size, text_addr);
    std::set<Elf64_Addr> fallthrough_prologue_targets = find_fallthrough_prologues(text_data, text_size, text_addr, jal_targets);
    std::set<Elf64_Addr> symbol_targets = find_symbol_table_functions(reader);
    
    // --- This is the original programmatic scan, preserved as requested ---
    std::set<Elf64_Addr> programmatically_found_pointers = find_pointers_in_data_sections(reader, text_addr, text_size);

    // --- Load addresses from Ghidra-generated files ---
    std::set<Elf64_Addr> ghidra_functions = load_addresses_from_file(argv[2]);
    // --- This is the new functionality: load data addresses from a Ghidra file ---
    std::set<Elf64_Addr> ghidra_data_pointers = load_addresses_from_file(argv[3]);


    // --- Combine results ---
    // We now use the Ghidra-provided data pointers as the main source for this category
    std::set<Elf64_Addr> all_found_functions = jal_targets;
    all_found_functions.insert(fallthrough_prologue_targets.begin(), fallthrough_prologue_targets.end());
    all_found_functions.insert(symbol_targets.begin(), symbol_targets.end());
    all_found_functions.insert(ghidra_data_pointers.begin(), ghidra_data_pointers.end());


    // --- Comparison Logic (using Ghidra function list as ground truth) ---
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

    // --- Report Results ---
    std::cout << std::dec << std::endl;
    std::cout << "--- Analysis Report ---" << std::endl;
    std::cout << "Ghidra Ground Truth Functions: " << ghidra_functions.size() << std::endl;
    std::cout << "Total Functions Found (using Ghidra data pointers): " << all_found_functions.size() << std::endl;
    std::cout << "  - Via JAL: " << jal_targets.size() << std::endl;
    std::cout << "  - Via Fallthrough Prologue: " << fallthrough_prologue_targets.size() << std::endl;
    std::cout << "  - Via Symbols: " << symbol_targets.size() << std::endl;
    std::cout << "  - Via Ghidra Data Pointers File: " << ghidra_data_pointers.size() << std::endl;
    std::cout << "  (For reference, programmatic pointer scan found: " << programmatically_found_pointers.size() << ")" << std::endl;
    
    std::cout << "\n--- Comparison vs. Ghidra Function List ---" << std::endl;
    std::cout << "Correctly Identified: " << correct_functions.size() << std::endl;
    std::cout << "Incorrectly Identified (False Positives): " << incorrect_functions.size() << std::endl;
    std::cout << "Missed Functions (False Negatives): " << missed_functions.size() << std::endl;

    return 0;
}


