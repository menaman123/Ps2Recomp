#include <cstdint>
#include <iostream>
#include <vector>
#include <string>
#include <set>
#include <fstream> // Required for file operations
#include <set>     // Required for std::set
#include "analyze.h"
#include <algorithm> 
#include <sstream> 
#include "instructions/RabbitizerInstructionR5900.h"
#include "instructions/RabbitizerInstrDescriptor.h"
#include "rabbitizer.hpp"
#include <include/json.hpp>
#include <fstream>
#include <iostream>
#include <cassert>


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
std::map<uint32_t, Function> parse_ghidra_function_file(const std::string& file_path, const uint8_t* text_data, uint32_t text_size, uint32_t text_base, std::ofstream& log_file) {
    std::map<uint32_t, Function> functions;
    std::set<std::string> seen_names;
    
    // Open JSON file
    std::ifstream infile(file_path);
    if (!infile.is_open()) {
        std::cerr << "[-] Could not open Ghidra function file: " << file_path << std::endl;
        return functions;
    }
    
    // Parse JSON
    nlohmann::json j;
    try {
        infile >> j;
    } catch (nlohmann::json::parse_error& e) {
        std::cerr << "[-] JSON parse error: " << e.what() << std::endl;
        return functions;
    }
    
    // Check if "functions" key exists
    if (!j.contains("functions")) {
        std::cerr << "[-] JSON does not contain 'functions' key" << std::endl;
        return functions;
    }
    
    // Parse each function
    for (const auto& func_json : j["functions"]) {
        Function current_function;
        
        // Parse function name
        log_file << "Parsing function: " << func_json["name"].get<std::string>() << std::endl;

        if (func_json["name"].get<std::string>() == "entry") {
            current_function.name = func_json["name"].get<std::string>();
        } else {
            std::string addr = func_json["address"].get<std::string>();
            // Use substr(2) to get the substring starting from index 2 (the third character)
            current_function.name = "FUN_" + addr.substr(2);
        }
        
        // Skip duplicates
        if (seen_names.find(current_function.name) != seen_names.end()) {
            continue;
        }
        
        // Parse function address
        if (func_json.contains("address")) {
            std::string addr_str = func_json["address"].get<std::string>();
            log_file << "Address: " << addr_str << std::endl;
            // Remove "0x" prefix if present
            if (addr_str.substr(0, 2) == "0x") {
                addr_str = addr_str.substr(2);
            }
            current_function.base_address = std::stoul(addr_str, nullptr, 16);
        } else {
            std::cerr << "[-] Function '" << current_function.name << "' missing 'address' field, skipping" << std::endl;
            continue;
        }
        
        // Parse function size
        if (func_json.contains("size")) {
            log_file << "Size: " << func_json["size"].get<uint32_t>() << std::endl;
            current_function.size = func_json["size"].get<uint32_t>();
        } else {
            current_function.size = 0;
        }
        
        // Parse blocks
        if (func_json.contains("blocks") && func_json["blocks"].is_array()) {
            for (const auto& block_json : func_json["blocks"]) {
                Block block;
                
                // Parse block address
                if (block_json.contains("address")) {
                    std::string block_addr_str = block_json["address"].get<std::string>();
                    log_file << "Block address: " << block_addr_str << std::endl;
                    if (block_addr_str.substr(0, 2) == "0x") {
                        block_addr_str = block_addr_str.substr(2);
                    }
                    block.start_address = std::stoul(block_addr_str, nullptr, 16);
                } else {
                    continue; // Skip blocks without addresses
                }
                
                // Parse block size and calculate end address
                if (block_json.contains("size")) {
                    uint32_t block_size = block_json["size"].get<uint32_t>();
                    // End address is inclusive (last byte of block)
                    block.end_address = block.start_address + block_size;
                    log_file << "Block start address: " << block.start_address << std::endl;
                    log_file << "Block size: " << block_size << std::endl;
                    log_file << "Block end address: " << block.end_address << std::endl;
                } else {
                    block.end_address = block.start_address;
                }

                // Now add instructions 
                // Parse instructions in this block
                for (uint32_t vram = block.start_address; vram < block.end_address; vram += 4) {
                    // Calculate offset into text_data
                    uint32_t offset = vram - text_base;
                    log_file << "Current VRAM: 0x" << std::hex << vram << std::endl;
                    

                    // Bounds check
                    if (offset + 4 > text_size) {
                        std::cerr << "[-] Warning: Address 0x" << std::hex << vram 
                                  << " is outside text section bounds" << std::endl;
                        break;
                    }

                    // Read 32-bit instruction (little-endian for PS2)
                    uint32_t word = text_data[offset] |
                                   (text_data[offset + 1] << 8) |
                                   (text_data[offset + 2] << 16) |
                                   (text_data[offset + 3] << 24);

                    // Create rabbitizer instruction
                    rabbitizer::InstructionR5900 instr(word, vram);
                    std::string disasm_str = instr.disassemble(0, "");
                    log_file << "Instruction at 0x" << std::hex << vram << ": " << disasm_str << std::endl;


                    // Add to block's instruction vector
                    block.instructions.push_back(instr);
                    log_file << "IN FOR LOOP Instruction count: " << block.instructions.size() << std::endl;
                }
                log_file << "Block Instruction Count: " << block.instructions.size() << std::endl;
                // Add block to function
                uint32_t block_size = block_json["size"].get<uint32_t>();
                assert(block_size > 0 && "Block size must be greater than 0");
                assert(block.instructions.size() == block_size / 4 && 
                       "Instruction count doesn't match expected count based on block size");
                
                // Add block to function
                current_function.blocks.push_back(block);
            }
        }
        
        // Add function to map
        functions[current_function.base_address] = current_function;
        seen_names.insert(current_function.name);
        
        std::cout << "[+] Parsed function: " << current_function.name 
                  << " at 0x" << std::hex << current_function.base_address 
                  << " with " << std::dec << current_function.blocks.size() << " blocks" << std::endl;
    }
    
    std::cout << "[+] Parsed " << functions.size() << " functions with blocks from JSON file." << std::endl;
    
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