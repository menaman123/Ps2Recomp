
#include <iostream>
#include <string>
#include <vector>
#include <elfio/elfio.hpp>

#include "EEAnalyze/analyze.h"      // Your analysis engine
#include "Recompiler.h"      // Your recompiler engine

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: recompiler_tool <path_to_elf_file> <path_to_ghidra_analysis.txt>" << std::endl;
        return 1;
    }

    std::string elf_path = argv[1];
    std::cout << "[+] Loading ELF file: " << elf_path << std::endl;

    // --- Step 1: Load the ELF file using ELFIO ---
    ELFIO::elfio reader;
    if (!reader.load(elf_path)) {
        std::cerr << "[-] Failed to load ELF file." << std::endl;
        return 1;
    }

    // --- Step 2: Load the Ghidra text file
    std::string ghidra_path = argv[2];
    std::cout << "[+] Loading Ghidra analysis file: " << ghidra_path << std::endl;
    



    const ELFIO::section* text_section = reader.sections[".text"];
    if (text_section == nullptr) {
        std::cerr << "[-] .text section not found in ELF file." << std::endl;
        return 1;
    }

    const uint8_t* text_data = reinterpret_cast<const uint8_t*>(text_section->get_data());
    uint32_t text_size = text_section->get_size();
    uint32_t text_vram = text_section->get_address();
    uint32_t entry_point = reader.get_entry();

    std::cout << "[+] .text section loaded. VRAM: 0x" << std::hex << text_vram 
              << ", Size: " << std::dec << text_size << " bytes." << std::endl;

    // --- Step 2: Analyze the executable to find all functions ---
    std::cout << "[+] Starting analysis phase..." << std::endl;
    /*
    std::vector<Function> functions = analyze_executable(entry_point, text_data, text_size, text_vram);
    
    if (functions.empty()) {
        std::cerr << "[-] Analysis failed or no functions were found." << std::endl;
        return 1;
    }
    std::cout << "[+] Analysis complete. Found " << functions.size() << " functions." << std::endl;
    */
    std::vector<Function> analyzed_functions = parse_ghidra_analysis_file(ghidra_path, text_data, text_size);

    if (analyzed_functions.empty()) {
        std::cerr << "[-] Analysis failed or no functions were found." << std::endl;
        return 1;
    }
    std::cout << "[+] Analysis complete. Found " << analyzed_functions.size() << " functions." << std::endl;

    // --- NEW: Convert the vector to a map ---
    std::map<uint32_t, Function> functions_map;
    for (const auto& func : analyzed_functions) {
        functions_map[func.base_address] = func;
    }

    // --- Step 3: Feed the function list into the recompiler ---
    std::cout << "[+] Starting recompilation phase..." << std::endl;
    Recompiler recompiler(functions_map);

    // --- Step 4: Generate the final C++ files ---
    bool success = recompiler.recompile_to_files("recompiled_functions.h", "recompiled_functions.cpp");

    if (success) {
        std::cout << "[+] Recompilation successful! Output files are recompiled_functions.h and recompiled_functions.cpp" << std::endl;
    } else {
        std::cerr << "[-] Recompilation failed." << std::endl;
        return 1;
    }

    return 0;
}
