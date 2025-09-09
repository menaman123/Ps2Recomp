#pragma once

#include <string>
#include <vector>
#include <fstream>
#include "Function.h"

class Recompiler {
public:
    Recompiler(const std::vector<Function>& functions);
    bool recompile_to_files(const std::string& output_header, const std::string& output_cpp);

private:
    const std::vector<Function>& m_functions;

    void write_header_file(std::ofstream& file);
    void write_cpp_file(std::ofstream& file);
    void recompile_function(const Function& func, std::ofstream& file);
    void translate_instruction(const RabbitizerInstruction& instr, std::ofstream& file);
};