#include "Recompiler.h"
#include "EEAnalyze/analyze.h"
#include <elfio/elfio.hpp>
#include <iostream>
#include <string>
#include <vector>
#include <map>

int main(int argc, char* argv[]) {
    std::string elf_path;
    std::string ghidra_path;
    std::string output_header_path = "recompiled_functions.h";
    std::string output_cpp_path = "recompiled_functions.cpp";

    // --- 1. Parse Command-Line Arguments ---
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--input" && i + 1 < argc) {
            elf_path = argv[++i];
        } else if (arg == "--ghidra-input" && i + 1 < argc) {
            ghidra_path = argv[++i];
        } else if (arg == "--output-header" && i + 1 < argc) {
            output_header_path = argv[++i];
        } else if (arg == "--output-cpp" && i + 1 < argc) {
            output_cpp_path = argv[++i];
        }
    }

    if (elf_path.empty() || ghidra_path.empty()) {
        std::cerr << "Usage: " << argv[0]
                  << " --input <elf_file>"
                  << " --ghidra-input <ghidra_txt_file>"
                  << " [--output-header <header_file>]"
                  << " [--output-cpp <cpp_file>]" << std::endl;
        return 1;
    }

    // --- 2. Read ELF File ---
    ELFIO::elfio reader;
    if (!reader.load(elf_path)) {
        std::cerr << "[-] Failed to load ELF file: " << elf_path << std::endl;
        return 1;
    }

    const ELFIO::section* text_section = reader.sections[".text"];
    if (text_section == nullptr) {
        std::cerr << "[-] .text section not found in ELF file." << std::endl;
        return 1;
    }

    const uint8_t* text_data = reinterpret_cast<const uint8_t*>(text_section->get_data());
    uint32_t text_size = text_section->get_size();

    // --- 3. Analyze Functions ---
    // Corrected function call
    std::vector<Function> analyzed_functions = parse_ghidra_analysis_file(ghidra_path, text_data, text_size);

    if (analyzed_functions.empty()) {
        std::cerr << "[-] Analysis failed or no functions were found from the Ghidra file." << std::endl;
        return 1;
    }
    std::cout << "[+] Analysis complete. Found " << analyzed_functions.size() << " functions." << std::endl;

    // --- 4. Convert Vector to Map for the Recompiler ---
    std::map<uint32_t, Function> functions_map;
    for (const auto& func : analyzed_functions) {
        functions_map[func.base_address] = func;
    }

    // --- 5. Recompile ---
    Recompiler recompiler(functions_map);
    if (!recompiler.recompile_to_files(output_header_path, output_cpp_path)) {
        std::cerr << "Recompilation failed." << std::endl;
        return 1;
    }

    return 0;
}