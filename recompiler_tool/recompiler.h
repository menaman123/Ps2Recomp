#pragma once

#include <string>
#include <vector>
#include <fstream>
#include <filesystem>
#include "EEAnalyze/Function.h" // Include your Function analysis class
#include "map"
#include "instructions/InstructionR5900.hpp"
#include <fstream>
// Forward-declare the rabbitizer C++ instruction class to avoid including the full header.
// This is a best practice for header files to improve compilation times.

extern std::ofstream log_file;
namespace rabbitizer {
    class InstructionR5900;
}

class Recompiler {
public:
    Recompiler(const std::map<uint32_t, Function>& functions);
    bool recompile_to_files(const std::string& output_header, const std::string& output_cpp);

private:
    std::map<uint32_t, Function> m_functions;

    void write_header_file(std::ofstream& file);
    void write_cpp_file(std::ofstream& file, const std::string& output_header_filename);
    void recompile_function(const Function& func, std::ofstream& file);
    void translate_instruction(const rabbitizer::InstructionR5900& instr, std::ofstream& file);
    void translate_branch_or_jump(const rabbitizer::InstructionR5900& instr, std::ofstream& file);
    void Recompiler::translate_branch_with_goto(const rabbitizer::InstructionR5900& instr, 
                                           const Function& func,
                                           size_t current_block_idx,
                                           std::ofstream& file);

    // Helper function to identify instructions that have a delay slot.
    bool has_delay_slot(const rabbitizer::InstructionR5900& instr) const;
    void generate_block_code(const Function& func, const Block& block, size_t block_idx, std::ofstream& file);
    void translate_control_flow(const rabbitizer::InstructionR5900& instr, const Function& func, const Block& block, size_t current_block_idx, size_t current_instr_idx, std::ofstream& file);
    void emit_branch_target(uint32_t target_addr, const Function& func, std::ofstream& file, const std::string& indent);
};
