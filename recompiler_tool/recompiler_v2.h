#pragma once

#include <map>
#include <set>
#include <vector>
#include <string>
#include <fstream>
#include <cstdint>
#include "EEAnalyze/Function.h"
#include "instructions/InstructionR5900.hpp"
struct CodeBlock {
    uint32_t start_address;
    uint32_t end_address;
    
    std::vector<uint32_t> instructions;
    std::set<uint32_t> jump_targets;     // Where this block can jump to
    bool ends_with_branch = false;
    bool ends_with_return = false;
    bool ends_with_indirect_jump = false;
};

class RecompilerV2 {
private:
    std::map<uint32_t, CodeBlock> blocks;
    std::set<uint32_t> all_entry_points;
    std::set<uint32_t> all_jump_targets;
    std::set<uint32_t> block_entries;
    
    const uint8_t* elf_data;
    uint32_t elf_size;
    uint32_t text_base;
    uint32_t text_size;
    
    // Helper functions
    uint32_t read_instruction(uint32_t address) const;
    bool is_jump_target(uint32_t address) const;
    void find_all_jump_targets();
    void create_blocks();
    void analyze_block(CodeBlock& block);
    
    void generate_dispatch_table(std::ofstream& file);
    void generate_block_code(const CodeBlock& block, std::ofstream& file);
    void translate_instruction(uint32_t instruction, uint32_t pc, std::ofstream& file);
    void generate_branch(uint32_t instruction, uint32_t pc, std::ofstream& file);
    void generate_jump(uint32_t instruction, uint32_t pc, std::ofstream& file);
    
public:
    RecompilerV2(const uint8_t* elf_data, uint32_t elf_size, 
                 uint32_t text_base, uint32_t text_size, std::set<uint32_t> block_entries);
    
    void analyze();
    void generate_code(const std::string& output_file);
    void generate_header(const std::string& header_file);
};