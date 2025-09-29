#include <iostream>
#include <vector>
#include <string>
#include <set>
#include <iomanip>
#include <elfio/elfio.hpp>
#include <sys/stat.h> // Required for stat
#ifdef _WIN32
    #include <direct.h> // Required for _mkdir
#endif
#include <fstream> // Required for file output                                                                                                                                                                                                  │
#include <sstream> // Required for std::stringstream  
#include "analyze.h" // Main header for our new analysis pipeline
#include "Function.h"  // The Function class definition
#include "instructions/RabbitizerInstructionR5900.h"
#include <iostream>
#include <vector>
#include <string>
#include <iomanip>
#include <elfio/elfio.hpp>
#include <fstream>
#include <sstream>
#include <algorithm> // For std::remove
#include "analyze.h"
#include "Function.h"
#include "instructions/RabbitizerInstructionR5900.h"

static std::string trim(const std::string& str) {
    const std::string whitespace = " \t";
    const auto strBegin = str.find_first_not_of(whitespace);
    if (strBegin == std::string::npos) return ""; // no content
    const auto strEnd = str.find_last_not_of(whitespace);
    const auto strRange = strEnd - strBegin + 1;
    return str.substr(strBegin, strRange);
}

void log_functions_to_file(const std::vector<Function>& functions, const std::string& output_filename) {                                                                                                                                        
    std::ofstream log_file(output_filename);                                                                                                                                                                                                    
    if (!log_file.is_open()) {                                                                                                                                                                                                                  
        std::cerr << "[-] Could not open log file for writing: " << output_filename << std::endl;                                                                                                                                               
        return;                                                                                                                                                                                                                                 
    }                                                                                                                                                                                                                                           

    for (const auto& func : functions) {                                                                                                                                                                                                        
        std::stringstream signature_stream;                                                                                                                                                                                                     
        signature_stream << "void " << func.name << "()";                                                                                                                                                                                       

        log_file << "Address: " << std::hex << std::setw(8) << std::setfill('0') << func.base_address                                                                                                                                           
                 << " | Name: " << func.name                                                                                                                                                                                                    
                 << " | Signature: " << signature_stream.str()                                                                                                                                                                                  
                 << std::dec << std::endl;                                                                                                                                                                                                      
    }                                                                                                                                                                                                                                           

    log_file << "\nTotal functions found: " << functions.size() << std::endl;                                                                                                                                                                   
    log_file.close();                                                                                                                                                                                                                           
    std::cout << "[+] Function log written to " << output_filename << std::endl;                                                                                                                                                                
}   
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

/*
int main(int argc, char** argv) {
    std::string filePath = argv[1];

    ELFIO::elfio reader;
    if (!reader.load(filePath)) {
        std::cerr << "[-] Could not load ELF file: " << filePath << "\n";
        return 1;
    }

    const ELFIO::section* text_section = reader.sections[".text"];
    if (text_section == nullptr) {
        std::cerr << "[-] .text section not found in the ELF file.\n";
        return 1;
    }

    const uint8_t* text_section_data = reinterpret_cast<const uint8_t*>(text_section->get_data());
    uint32_t text_section_size = text_section->get_size();
    uint32_t text_section_vram = text_section->get_address();
    uint32_t entry_point = reader.get_entry();

    std::cout << "--- STARTING ADVANCED FUNCTION ANALYSIS ---" << std::endl;
    std::vector<Function> analyzed_functions = analyze_executable(entry_point, text_section_data, text_section_size, text_section_vram);
    std::cout << "--- ANALYSIS COMPLETE ---" << std::endl;


    // --- Print a summary of all functions found to the console ---
    std::cout << "\n======================================================================" << std::endl;
    std::cout << "---                       FUNCTION SUMMARY                       ---" << std::endl;
    std::cout << "======================================================================" << std::endl;
        std::cout << "Found " << analyzed_functions.size() << " functions." << std::endl;
    /*
    int func_index = 0;
    for (const auto& func : analyzed_functions) {
        std::cout << "  [" << std::setw(4) << ++func_index << "] "
                  << "0x" << std::hex << func.base_address << "\t"
                  << func.name << std::dec << std::endl;
    }
    */



    // --- Write the detailed, block-by-block analysis to separate files ---
    /*
        for (const auto& func : analyzed_functions) {
        func.dump_to_console();
    }

    log_functions_to_file(analyzed_functions, "function_log.txt");

    std::cout << "\nLog files written successfully." << std::endl;

    return 0;
}
    */

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <path_to_elf_file> <path_to_ghidra_analysis.txt>" << std::endl;
        return 1;
    }
    std::string elf_path = argv[1];
    std::string ghidra_path = argv[2];
    ELFIO::elfio reader;
    if (!reader.load(elf_path)) {
        std::cerr << "[-] Could not load ELF file: " << elf_path << "\n";
        return 1;
    }
    std::cout << "--- STARTING GHIDRA FILE PARSING ---" << std::endl;
    const ELFIO::section* text_section = reader.sections[".text"];
    const uint8_t* text_section_data = reinterpret_cast<const uint8_t*>(text_section->get_data());
    uint32_t text_section_size = text_section->get_size();

      // Now call the parser with the new arguments
    std::set<uint32_t> analyzed_functions = parse_ghidra_analysis_file(ghidra_path, text_section_data, text_section_size);
    std::cout << "--- PARSING COMPLETE ---" << std::endl;
    std::cout << "\n======================================================================" << std::endl;
    std::cout << "---                       FUNCTION SUMMARY                       ---" << std::endl;
    std::cout << "======================================================================" << std::endl;
    std::cout << "Found " << analyzed_functions.size() << " functions." << std::endl;
    log_functions_to_file(analyzed_functions, "function_log.txt");
    std::cout << "\nLog file written successfully." << std::endl;
    return 0;
}