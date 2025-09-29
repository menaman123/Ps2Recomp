#pragma once

#include <vector>
#include <set>
#include <cstdint>
#include <string> 
#include "Function.h" 
#include <elfio/elfio.hpp>

// Loads a set of addresses from a text file (one hex address per line).
std::set<uint32_t> load_addresses_from_file(const std::string& path);
std::set<uint32_t> parse_ghidra_analysis_file(const std::string& file_path, const uint8_t* text_section_data, uint32_t text_section_size);

/**
 * Scans the entire .text section to find the starting address of every function
 * by looking for targets of JAL instructions.
 */
std::set<uint32_t> find_function_starts(const uint8_t* code, uint32_t code_size, uint32_t text_vram_start);

/**
 * The main entry point for the analysis phase. It finds all function starts
 * and then analyzes each function to determine its basic blocks.
 */
/*
std::vector<Function> analyze_executable(
    uint32_t entry_point, 
    const uint8_t* text_buffer, 
    uint32_t text_size, 
    uint32_t text_vram_start,
    const std::string& ghidra_data_addresses_path
);
*/



