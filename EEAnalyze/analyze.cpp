#include <iostream>
#include <vector>
#include <string>
#include <fstream> // Required for file operations
#include <set>     // Required for std::set
#include "analyze.h"
#include <algorithm> 
#include <sstream> 
#include "instructions/RabbitizerInstructionR5900.h"
#include "instructions/RabbitizerInstrDescriptor.h"

static std::string trim(const std::string& str) {
    const std::string whitespace = " \t";
    const auto strBegin = str.find_first_not_of(whitespace);
    if (strBegin == std::string::npos) return ""; // no content
    const auto strEnd = str.find_last_not_of(whitespace);
    const auto strRange = strEnd - strBegin + 1;
    return str.substr(strBegin, strRange);
}

// Helper function to load a set of addresses from a text file (one hex address per line)
static std::set<uint32_t> load_addresses_from_file(const std::string& path) {
    std::set<uint32_t> addresses;
    std::ifstream infile(path);
    std::string line;
    if (!infile.is_open()) {
        std::cerr << "[ERROR] Could not open address file: " << path << std::endl;
        return addresses;
    }

    while (std::getline(infile, line)) {
        if (line.empty()) continue;
        try {
            // std::stoul is more appropriate for uint32_t
            addresses.insert(static_cast<uint32_t>(std::stoul(line, nullptr, 16)));
        }
        catch (const std::invalid_argument& ia) {
            // Ignore lines that are not valid hex numbers
        }
    }
    std::cout << "[+] Loaded " << addresses.size() << " addresses from " << path << std::endl;
    return addresses;
}

// New function to parse the Ghidra analysis file
std::vector<Function> parse_ghidra_analysis_file(const std::string& file_path, const uint8_t* text_section_data, uint32_t text_section_size) {
    std::vector<Function> functions;
    std::ifstream infile(file_path);
    if (!infile.is_open()) {
        std::cerr << "[-] Could not open Ghidra analysis file: " << file_path << std::endl;
        return functions;
    }
    std::string line;
    Function current_function;
    bool in_function_block = false;
    bool in_references_block = false;
    while (std::getline(infile, line)) {
        if (line.empty() || line.find("##") == 0) {
            continue; // Skip empty lines and comments
        }
        if (line.find("Function:") != std::string::npos) {
            if (in_function_block) {
                // Analyze the fully parsed function before adding it
                std::cout << "[+] Analyzing parsed function: " << current_function.name << std::endl;
                current_function.analyze(text_section_data, text_section_size);
                functions.push_back(current_function);
            }
            current_function = Function();
            in_function_block = true;
            in_references_block = false;
            current_function.name = trim(line.substr(line.find(":") + 1));
        } else if (in_function_block) {
            if (line.find("Address:") != std::string::npos && !in_references_block) {
                std::string addr_str = trim(line.substr(line.find(":") + 1));
                current_function.base_address = std::stoul(addr_str, nullptr, 16);
            } else if (line.find("Size:") != std::string::npos) {
                std::string size_str = trim(line.substr(line.find(":") + 1));
                size_str = size_str.substr(0, size_str.find(" bytes"));
                current_function.size = std::stoul(size_str);
            } else if (line.find("External References:") != std::string::npos) {
                in_references_block = true;
            } else if (in_references_block && line.find("- Address:") != std::string::npos) {
                ExternalReference ref;
                std::stringstream ss(line);
                std::string token;
                ss >> token >> token; // Skip "- Address:"
                std::string addr_str = trim(token);
                addr_str.erase(std::remove(addr_str.begin(), addr_str.end(), ','), addr_str.end());
                if (addr_str.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos) {
                    ref.address = std::stoul(addr_str, nullptr, 16);
                } else {
                    ref.name = addr_str;
                    ref.address = 0;
                }
                ss >> token >> token; // Skip "Section:"
                ref.section = trim(token);
                current_function.external_references.push_back(ref);
            }
        }
    }
    // Add the very last function in the file
    if (in_function_block) {
        std::cout << "[+] Analyzing parsed function: " << current_function.name << std::endl;
        current_function.analyze(text_section_data, text_section_size);
        functions.push_back(current_function);
    }
    std::cout << "[+] Parsed and analyzed " << functions.size() << " functions from Ghidra analysis file." << std::endl;
    return functions;
}

// The main entry point for the analysis phase.
/*
std::vector<Function> analyze_executable(
    uint32_t entry_point, 
    const uint8_t* text_buffer, 
    uint32_t text_size, 
    uint32_t text_vram_start,
    const std::string& ghidra_data_addresses_path
) {
    // --- Step 1: Global Analysis --- 
    // Find function starts from JAL instructions
    std::set<uint32_t> function_starts_set = find_function_starts(text_buffer, text_size, text_vram_start);
    function_starts_set.insert(entry_point);

    // --- NEW: Load addresses from the Ghidra data file and merge them ---
    std::set<uint32_t> ghidra_data_addresses = load_addresses_from_file(ghidra_data_addresses_path);
    function_starts_set.insert(ghidra_data_addresses.begin(), ghidra_data_addresses.end());

    std::vector<uint32_t> sorted_starts(function_starts_set.begin(), function_starts_set.end());
    std::sort(sorted_starts.begin(), sorted_starts.end());

    std::vector<Function> all_functions;

    // --- Step 2: Per-Function Analysis ---
    for (size_t i = 0; i < sorted_starts.size(); ++i) {
        uint32_t func_start_vram = sorted_starts[i];
        uint32_t next_func_start_vram = (i + 1 < sorted_starts.size())
                                      ? sorted_starts[i+1]
                                      : (text_vram_start + text_size);
        uint32_t func_size = next_func_start_vram - func_start_vram;
        uint32_t func_offset_in_buffer = func_start_vram - text_vram_start;
        const uint8_t* func_code_ptr = text_buffer + func_offset_in_buffer;

        std::cout << "[+] Analyzing function #" << (i + 1) << " at VRAM 0x" << std::hex << func_start_vram << " (Size: " << std::dec << func_size << " bytes)" << std::endl;

        Function func(func_start_vram);

        func.analyze(text_buffer, text_size, func_code_ptr, func_size);

        all_functions.push_back(func);
        std::cout << "     -> Successfully analyzed and stored function 0x" << std::hex << func_start_vram << std::dec << std::endl;
    }

    return all_functions;
}

*/

/**
 * Scans the entire .text section to find the starting address of every function.
 * It does this by finding all instructions that are targets of a JAL instruction.
 * @param code A pointer to the raw bytes of the .text section.
 * @param code_size The size of the .text section in bytes.
 * @param text_vram_start The virtual memory address where the .text section starts.
 * @return A set of unique addresses, each being the start of a function.
 */
std::set<uint32_t> find_function_starts(const uint8_t* code, uint32_t code_size, uint32_t text_vram_start) {
    std::set<uint32_t> function_starts;

    // Note: A more advanced version of this function would also parse the ELF's
    // symbol table (.symtab) to find named functions, which is another
    // excellent source for function entry points.

    // Loop through the entire .text section to find all 'jal' targets.
    for (uint32_t offset = 0; offset + 4 <= code_size; offset += 4) {
        uint32_t current_vram = text_vram_start + offset;
        uint32_t instruction_word = *(reinterpret_cast<const uint32_t*>(code + offset));

        RabbitizerInstruction instr;
        RabbitizerInstructionR5900_init(&instr, instruction_word, current_vram);
        RabbitizerInstructionR5900_processUniqueId(&instr);

        const RabbitizerInstrDescriptor* descriptor = instr.descriptor;

        // The `doesLink` property is true for instructions that save a return address, like 'jal'.
        // `isJumpWithAddress` distinguishes `jal` from `jalr`.
        if (descriptor->doesLink && descriptor->isJumpWithAddress) {
            uint32_t target_vram = RabbitizerInstruction_getInstrIndexAsVram(&instr);
            std::cout << "Found JAL at 0x" << std::hex << current_vram
                      << "  ->  Target: 0x" << target_vram << std::dec << std::endl;

            function_starts.insert(target_vram);
        }

        RabbitizerInstruction_destroy(&instr);
    }

    return function_starts;
}