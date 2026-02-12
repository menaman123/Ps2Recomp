#include "Recompiler.h"
#include "host_app/cpu_state.h"
#include "host_app/memory.h"
#include "rabbitizer.hpp"
#include "instructions/InstructionR5900.hpp"

#include <iostream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <filesystem>

std::ofstream log_file;

// Helper to get a string representation of a GPR register for code generation.
static std::string get_gpr_name(uint8_t reg_num) {
    return "ctx.cpuRegs.GPR.r[" + std::to_string(reg_num) + "]";
}

// Helper for floating-point registers
static std::string get_fpr_name(uint8_t reg_num) {
    return "ctx.fpuRegs.fpr[" + std::to_string(reg_num) + "]";
}

static std::string get_vr_name(uint8_t reg_num) {
    return "ctx.vuRegs.VF[" + std::to_string(reg_num) + "]";
}

static std::string format_imm(int32_t imm) {
    std::stringstream ss;
    ss << "0x" << std::hex << imm;
    return ss.str();
}



Recompiler::Recompiler(const std::map<uint32_t, Function>& functions) : m_functions(functions) {}

bool Recompiler::recompile_to_files(const std::string& output_header, const std::string& output_cpp) {
    std::ofstream header_file(output_header);
    if (!header_file.is_open()) {
        std::cerr << "Error: Could not open header file for writing: " << output_header << std::endl;
        return false;
    }
    write_header_file(header_file);
    header_file.close();
    std::cout << "Successfully generated header file: " << output_header << std::endl;

    std::ofstream cpp_file(output_cpp);
    if (!cpp_file.is_open()) {
        std::cerr << "Error: Could not open C++ file for writing: " << output_cpp << std::endl;
        return false;
    }
    
    write_cpp_file(cpp_file, std::filesystem::path(output_header).filename().string());

    cpp_file.close();
    std::cout << "Successfully generated C++ file: " << output_cpp << std::endl;

    return true;
}

void Recompiler::write_header_file(std::ofstream& file) {
    file << "#pragma once\n\n";
    file << "#include \"../host_app/cpu_state.h\"\n\n";
    file << "#include \"../host_app/sif_hle.h\"\n\n";
    file << "#include \"../host_app/hle_heap.h\"\n\n";
    file << "#include <map>\n";
    file << "#include <vector>\n";
    file << "#include <cmath>\n";
    file << "#include <algorithm>\n";
    file << "#include <functional>\n\n";

    // CHANGE: Removed default parameter - addr is now required
    for (const auto& pair : m_functions) {
        const Function& func = pair.second;
        file << "void "<< func.name << "(CpuContext& ctx);\n";
    }

    file << "\nextern std::map<uint32_t, std::function<void(CpuContext&, uint32_t)>> recompiled_functions;\n";
    file << "std::function<void(CpuContext&, uint32_t)> find_containing_function(uint32_t pc);\n";
    file << "void initialize_recompiled_functions();\n";
}

// CHANGE: Updated to simpler function generation without switch statement
void Recompiler::write_cpp_file(std::ofstream& file, const std::string& output_header_filename) {
    file << "#include \"" << output_header_filename << "\"\n";
    file << "#include \"../host_app/cpu_state.h\"\n";
    file << "#include \"../host_app/memory.h\"\n";
    file << "#include \"../host_app/syscalls.h\"\n\n";
    file << "#include \"../host_app/sif_hle.h\"\n\n";
    file << "#include \"../host_app/gs_hle.h\"\n\n";
    file << "#include \"../host_app/hle_heap.h\"\n\n";
    file << "#include <iostream>\n";
    file << "#include <iomanip>\n";
    file << "#include <cmath>\n";
    file << "#include <vector>\n";
    file << "#include <fstream>\n\n";

    file << "extern std::ofstream g_logFile;\n\n";

    file << "std::map<uint32_t, std::function<void(CpuContext&, uint32_t)>> recompiled_functions;\n";
    
    // Add function ranges for smart lookup
    file << "struct FunctionRange {\n";
    file << "    uint32_t start;\n";
    file << "    uint32_t end;\n";
    file << "    std::function<void(CpuContext&, uint32_t)> func;\n";
    file << "};\n";
    file << "std::vector<FunctionRange> function_ranges;\n\n";

    for (const auto& pair : m_functions) {
        const Function& func = pair.second;
        recompile_function(func, file);
    }

    // Add the smart lookup function
    file << "std::function<void(CpuContext&, uint32_t)> find_containing_function(uint32_t pc) {\n";
    file << "    for (const auto& range : function_ranges) {\n";
    file << "        if (pc >= range.start && pc < range.end) {\n";
    file << "            return range.func;\n";
    file << "        }\n";
    file << "    }\n";
    file << "            g_logFile << \"find_containing_function: No function found for PC 0x\" << std::hex << pc << std::endl;\n";
    file << "            exit(0);\n";
    file << "    return nullptr;\n";
    file << "}\n\n";

    file << "void initialize_recompiled_functions() {\n";
    for (const auto& pair : m_functions) {
        const Function& func = pair.second;
        
        // Check for sceSifInitRpc address
        if (func.base_address == 0x002d1a50) {
            file << "    // HLE Hook for sceSifInitRpc\n";
            file << "    recompiled_functions[0x2d1a50] = [](CpuContext& ctx, uint32_t addr) {\n";
            file << "        hle_sceSifInitRpc(ctx);\n";
            file << "        // HLE return mechanism (simulate jr $ra)\n";
            file << "        ctx.cpuRegs.pc = ctx.cpuRegs.GPR.r[31].UL[0];\n";
            file << "    };\n";
            
            // Still register the address range so lookups don't fail, 
            // though the PC will never actually be inside this range during HLE execution.
            uint32_t end_address = func.base_address + func.size;
            file << "    function_ranges.push_back({0x" << std::hex << func.base_address 
                 << ", 0x" << std::hex << end_address << ", recompiled_functions[0x2d1a50]});\n";
        } 


        else if (func.base_address == 0x001815c0) {
            file << "    // HLE Hook for Heap\n";
            file << "    recompiled_functions[0x1815c0] = [](CpuContext& ctx, uint32_t addr) {\n";
            file << "        HLE_001815c0(ctx);\n";
            file << "        // HLE return mechanism (simulate jr $ra)\n";
            file << "        ctx.cpuRegs.pc = ctx.cpuRegs.GPR.r[31].UL[0];\n";
            file << "    };\n";
            
            // Still register the address range so lookups don't fail, 
            // though the PC will never actually be inside this range during HLE execution.
            uint32_t end_address = func.base_address + func.size;
            file << "    function_ranges.push_back({0x" << std::hex << func.base_address 
                 << ", 0x" << std::hex << end_address << ", recompiled_functions[0x1815c0]});\n";
        } 
        else if (func.base_address == 0x002c6ce0) {
            file << "    // HLE Hook for memcpy\n";
            file << "    recompiled_functions[0x2c6ce0] = [](CpuContext& ctx, uint32_t addr) {\n";
            file << "        HLE_002c6ce0(ctx);\n";
            file << "        // HLE return mechanism (simulate jr $ra)\n";
            file << "        ctx.cpuRegs.pc = ctx.cpuRegs.GPR.r[31].UL[0];\n";
            file << "    };\n";
            
            // Still register the address range so lookups don't fail, 
            // though the PC will never actually be inside this range during HLE execution.
            uint32_t end_address = func.base_address + func.size;
            file << "    function_ranges.push_back({0x" << std::hex << func.base_address 
                 << ", 0x" << std::hex << end_address << ", recompiled_functions[0x2c6ce0]});\n";
        } 



        else if (func.base_address == 0x001815f0) {
            file << "    // HLE Hook for sceSifInitRpc\n";
            file << "    recompiled_functions[0x1815f0] = [](CpuContext& ctx, uint32_t addr) {\n";
            file << "        HLE_001815f0(ctx);\n";
            file << "        // HLE return mechanism (simulate jr $ra)\n";
            file << "        ctx.cpuRegs.pc = ctx.cpuRegs.GPR.r[31].UL[0];\n";
            file << "    };\n";
            
            // Still register the address range so lookups don't fail, 
            // though the PC will never actually be inside this range during HLE execution.
            uint32_t end_address = func.base_address + func.size;
            file << "    function_ranges.push_back({0x" << std::hex << func.base_address 
                 << ", 0x" << std::hex << end_address << ", recompiled_functions[0x1815f0]});\n";
        } 

        /*
                else if (func.base_address == 0x002cf930) {
            file << "    // HLE Hook for SysAlloc\n";
            file << "    recompiled_functions[0x2cf930] = [](CpuContext& ctx, uint32_t addr) {\n";
            file << "        HLE_002cf930(ctx);\n";
            file << "        // HLE return mechanism (simulate jr $ra)\n";
            file << "        ctx.cpuRegs.pc = ctx.cpuRegs.GPR.r[31].UL[0];\n";
            file << "    };\n";
            
            // Still register the address range so lookups don't fail, 
            // though the PC will never actually be inside this range during HLE execution.
            uint32_t end_address = func.base_address + func.size;
            file << "    function_ranges.push_back({0x" << std::hex << func.base_address 
                 << ", 0x" << std::hex << end_address << ", recompiled_functions[0x2cf930]});\n";
        } 
        */




        /*
        else if (func.base_address == 0x002cf770) {
            file << "    // HLE Hook for WaitForVblank\n";
            file << "    recompiled_functions[0x2cf770] = [](CpuContext& ctx, uint32_t addr) {\n";
            file << "        hle_WaitForVblank(ctx);\n";
            file << "        // Simulate 'jr $ra' return\n";
            file << "        ctx.cpuRegs.pc = ctx.cpuRegs.GPR.r[31].UL[0];\n";
            file << "    };\n";
            
            // Register range so lookups don't fail
            uint32_t end_address = func.base_address + func.size;
            file << "    function_ranges.push_back({0x" << std::hex << func.base_address 
                << ", 0x" << std::hex << end_address << ", recompiled_functions[0x2cf770]});\n";
        }        
        
        */

        

        else if (func.base_address == 0x002d6618) {
            file << "    // HLE Hook for InitTLB\n";
            file << "    recompiled_functions[0x2d6618] = [](CpuContext& ctx, uint32_t addr) {\n";
            file << "        hle_InitTLB(ctx);\n";
            file << "        // Simulate 'jr $ra' return\n";
            file << "        ctx.cpuRegs.pc = ctx.cpuRegs.GPR.r[31].UL[0];\n";
            file << "    };\n";
        
            // Register range so lookups don't fail
            uint32_t end_address = func.base_address + func.size;
            file << "    function_ranges.push_back({0x" << std::hex << func.base_address 
                << ", 0x" << std::hex << end_address << ", recompiled_functions[0x2d6618]});\n";
        }

        /*
        else if (func.base_address == 0x0019fb18) {
            file << "    // HLE Hook for Intitialize Graphics\n";
            file << "    recompiled_functions[0x19fb18] = [](CpuContext& ctx, uint32_t addr) {\n";
            file << "        hle_InitGraphics(ctx);\n";
            file << "        // Simulate 'jr $ra' return\n";
            file << "        ctx.cpuRegs.pc = ctx.cpuRegs.GPR.r[31].UL[0];\n";
            file << "    };\n";
        
            // Register range so lookups don't fail
            uint32_t end_address = func.base_address + func.size;
            file << "    function_ranges.push_back({0x" << std::hex << func.base_address 
                << ", 0x" << std::hex << end_address << ", recompiled_functions[0x19fb1]});\n";
        }
        */

        else if (func.base_address == 0x002d6880){
            file << "    // HLE Hook for Intitialize Graphics\n";
            file << "    recompiled_functions[0x2d6880] = [](CpuContext& ctx, uint32_t addr) {\n";
            file << "        hle_ContextRestore(ctx);\n";
            file << "        // Simulate 'jr $ra' return\n";
            file << "        ctx.cpuRegs.pc = ctx.cpuRegs.GPR.r[31].UL[0];\n";
            file << "    };\n";
        
            // Register range so lookups don't fail
            uint32_t end_address = func.base_address + func.size;
            file << "    function_ranges.push_back({0x" << std::hex << func.base_address 
                << ", 0x" << std::hex << end_address << ", recompiled_functions[0x2d6880]});\n";
        }
        else if (func.base_address == 0x002d69a0) { // Check this address matches your Ghidra export
            file << "    // HLE Hook for Global Constructors (__do_global_ctors)\n";
            file << "    recompiled_functions[0x2d69a0] = [](CpuContext& ctx, uint32_t addr) {\n";
            file << "        hle_DoGlobalConstructors(ctx);\n";
            file << "        // Return mechanism handled inside the HLE function via PC update\n";
            file << "    };\n";

            // Register range so lookups don't fail
            uint32_t end_address = func.base_address + func.size;
            file << "    function_ranges.push_back({0x" << std::hex << func.base_address 
                << ", 0x" << std::hex << end_address << ", recompiled_functions[0x2d69a0]});\n";
        }
        else if (func.base_address == 0x002d27c8) { // Check this address matches your Ghidra export
            file << "    // HLE Hook for hle_sceSifBindRpc\n";
            file << "    recompiled_functions[0x2d27c8] = [](CpuContext& ctx, uint32_t addr) {\n";
            file << "        hle_sceSifBindRpc(ctx);\n";
            file << "        // Return mechanism handled inside the HLE function via PC update\n";
            file << "    };\n";

            // Register range so lookups don't fail
            uint32_t end_address = func.base_address + func.size;
            file << "    function_ranges.push_back({0x" << std::hex << func.base_address 
                << ", 0x" << std::hex << end_address << ", recompiled_functions[0x2d27c8]});\n";
        }
        /*
        else if (func.base_address == 0x002aa8d0) { // Check this address matches your Ghidra export
            file << "    // HLE Hook for hle_sceSifBindRpc\n";
            file << "    recompiled_functions[0x2aa8d0] = [](CpuContext& ctx, uint32_t addr) {\n";
            file << "        hle_RenderLoop(ctx);\n";
            file << "        // Return mechanism handled inside the HLE function via PC update\n";
            file << "    };\n";

            // Register range so lookups don't fail
            uint32_t end_address = func.base_address + func.size;
            file << "    function_ranges.push_back({0x" << std::hex << func.base_address 
                << ", 0x" << std::hex << end_address << ", recompiled_functions[0x2aa8d0]});\n";
        }
        */

        else if (func.base_address == 0x00181490) { // Check this address matches your Ghidra export
            file << "    // HLE Hook for Piracy Skip\n";
            file << "    recompiled_functions[0x181490] = [](CpuContext& ctx, uint32_t addr) {\n";
            file << "        FUN_00181490(ctx);\n";
            file << "        // Return mechanism handled inside the HLE function via PC update\n";
            file << "    };\n";

            // Register range so lookups don't fail
            uint32_t end_address = func.base_address + func.size;
            file << "    function_ranges.push_back({0x" << std::hex << func.base_address 
                << ", 0x" << std::hex << end_address << ", recompiled_functions[0x181490]});\n";
        }

        else {
            // Standard generation for all other functions
            file << "    recompiled_functions[0x" << std::hex << func.base_address << "] = [](CpuContext& ctx, uint32_t addr) { " << func.name << "(ctx); };\n";
            
            uint32_t end_address = func.base_address + func.size;
            file << "    function_ranges.push_back({0x" << std::hex << func.base_address 
                 << ", 0x" << std::hex << end_address << ", [](CpuContext& ctx, uint32_t addr) { " << func.name << "(ctx); }});\n";
        }
    }
    file << "}\n";
}

bool Recompiler::has_delay_slot(const rabbitizer::InstructionR5900& instr) const {
    if (static_cast<int>(instr.getUniqueId()) == RABBITIZER_INSTR_ID_cpu_eret) return false;
    return instr.isBranch() || instr.isJump() && !instr.isTrap();
}

// CHANGE: Complete rewrite - no switch statement, simple labels
void Recompiler::recompile_function(const Function& func, std::ofstream& file) {


        if (func.base_address == 0x001e0fa0) {
        // PART 1: Prologue and switch dispatch
        file << R"code(void FUN_001e0fa0(CpuContext& ctx) {
        // Prologue - save registers
        ctx.cpuRegs.GPR.r[29].SL[0] = ctx.cpuRegs.GPR.r[29].SL[0] - 0x80;
        memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x50, ctx.cpuRegs.GPR.r[16].UD[0]);
        memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x58, ctx.cpuRegs.GPR.r[17].UD[0]);
        ctx.cpuRegs.GPR.r[16].UD[0] = ctx.cpuRegs.GPR.r[4].UD[0];
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x70, ctx.fpuRegs.fpr[20].UL);
        ctx.cpuRegs.GPR.r[17].UD[0] = ctx.cpuRegs.GPR.r[5].UD[0];
        memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x60, ctx.cpuRegs.GPR.r[31].UD[0]);
        ctx.fpuRegs.fpr[20].UL = ctx.fpuRegs.fpr[12].UL;
        
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x0);
        ctx.cpuRegs.GPR.r[2].UL[0] = ctx.cpuRegs.GPR.r[2].UL[0] >> 1;
        ctx.cpuRegs.GPR.r[4].UL[0] = ctx.cpuRegs.GPR.r[2].UL[0] & 0x1F;
        ctx.cpuRegs.GPR.r[3].UL[0] = (ctx.cpuRegs.GPR.r[4].UL[0] < 10) ? 1 : 0;
        
        if (ctx.cpuRegs.GPR.r[3].UL[0] == 0) {
            goto Label_caseD_0;
        }
        
        switch (ctx.cpuRegs.GPR.r[4].UL[0]) {
            case 0: case 1: case 3: case 7: case 9: goto Label_caseD_0;
            case 2: goto Label_caseD_2;
            case 4: goto Label_caseD_4;
            case 5: goto Label_caseD_5;
            case 6: goto Label_caseD_6;
            case 8: goto Label_caseD_8;
            default: goto Label_caseD_0;
        }
    )code";

        // PART 2: Case 2
        file << R"code(
    Label_caseD_2:
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x30);
        ctx.cpuRegs.GPR.r[5].UD[0] = ctx.cpuRegs.GPR.r[29].UD[0];
        ctx.cpuRegs.GPR.r[4].UL[0] = memory::read<uint8_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0]) + 0x0);
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1e1000;
        if (recompiled_functions.count(0x1e3308)) {
            recompiled_functions[0x1e3308](ctx, 0x1e3308);
        } else {
            ctx.cpuRegs.pc = 0x1e3308;
        }
        
        ctx.cpuRegs.GPR.r[3].UL[0] = memory::read<uint8_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x12);
        ctx.cpuRegs.GPR.r[4].UL[0] = 2;
        if (ctx.cpuRegs.GPR.r[3].UL[0] == 0) {
            goto Label_1044;
        }
        if (ctx.cpuRegs.GPR.r[17].UL[0] != 0) {
            ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x0);
            goto Label_1048;
        }
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x0);
        ctx.cpuRegs.GPR.r[2].UL[0] = ctx.cpuRegs.GPR.r[2].UL[0] & 0x200;
        if (ctx.cpuRegs.GPR.r[2].UL[0] == 0) {
            ctx.cpuRegs.GPR.r[4].UL[0] = 3;
            goto Label_1044;
        }
        ctx.fpuRegs.fpr[12].UL = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x8);
        ctx.fpuRegs.fpr[13].UL = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0xc);
        ctx.fpuRegs.fpr[14].UL = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x4);
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[16].UD[0];
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1e1030;
        if (recompiled_functions.count(0x1e6830)) {
            recompiled_functions[0x1e6830](ctx, 0x1e6830);
        } else {
            ctx.cpuRegs.pc = 0x1e6830;
        }
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x0);
        ctx.cpuRegs.GPR.r[4].UL[0] = ctx.cpuRegs.GPR.r[2].UL[0] >> 1;
        ctx.cpuRegs.GPR.r[4].UL[0] = ctx.cpuRegs.GPR.r[4].UL[0] & 0x1F;
        goto Label_1048;

    Label_1044:
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x0);

    Label_1048:
        ctx.cpuRegs.GPR.r[3].UL[0] = ctx.cpuRegs.GPR.r[4].UL[0] & 0x1F;
        ctx.cpuRegs.GPR.r[3].UL[0] = ctx.cpuRegs.GPR.r[3].UL[0] << 1;
        ctx.cpuRegs.GPR.r[2].UL[0] = ctx.cpuRegs.GPR.r[2].UL[0] & 0xFFFFFFC1;
        ctx.cpuRegs.GPR.r[2].UL[0] = ctx.cpuRegs.GPR.r[2].UL[0] | ctx.cpuRegs.GPR.r[3].UL[0];
        goto Label_11b8;
    )code";

        // PART 3: Case 4 and Case 6
        file << R"code(
    Label_caseD_4:
        ctx.fpuRegs.fpr[12].UL = ctx.fpuRegs.fpr[20].UL;
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[16].UD[0];
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1e106c;
        if (recompiled_functions.count(0x1e0840)) {
            recompiled_functions[0x1e0840](ctx, 0x1e0840);
        } else {
            ctx.cpuRegs.pc = 0x1e0840;
        }
        ctx.cpuRegs.GPR.r[4].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x0);
        ctx.cpuRegs.GPR.r[3].UL[0] = 4;
        ctx.cpuRegs.GPR.r[5].UL[0] = 5;
        if (ctx.cpuRegs.GPR.r[2].UL[0] != 0) {
            ctx.cpuRegs.GPR.r[3].UL[0] = ctx.cpuRegs.GPR.r[5].UL[0];
        }
        ctx.cpuRegs.GPR.r[4].UL[0] = ctx.cpuRegs.GPR.r[4].UL[0] & 0xFFFFFFC1;
        ctx.cpuRegs.GPR.r[3].UL[0] = ctx.cpuRegs.GPR.r[3].UL[0] << 1;
        ctx.cpuRegs.GPR.r[4].UL[0] = ctx.cpuRegs.GPR.r[4].UL[0] | ctx.cpuRegs.GPR.r[3].UL[0];
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x0, ctx.cpuRegs.GPR.r[4].UL[0]);
        goto Label_caseD_0;

    Label_caseD_6:
        ctx.fpuRegs.fpr[12].UL = ctx.fpuRegs.fpr[20].UL;
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[16].UD[0];
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1e10a0;
        if (recompiled_functions.count(0x1e0840)) {
            recompiled_functions[0x1e0840](ctx, 0x1e0840);
        } else {
            ctx.cpuRegs.pc = 0x1e0840;
        }
        ctx.cpuRegs.GPR.r[4].UL[0] = 8;
        if (ctx.cpuRegs.GPR.r[2].UL[0] == 0) {
            ctx.cpuRegs.GPR.r[4].UL[0] = 6;
            goto Label_10c0;
        }
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x0);
        ctx.cpuRegs.GPR.r[3].UL[0] = 7;
        ctx.cpuRegs.GPR.r[2].UL[0] = ctx.cpuRegs.GPR.r[2].UL[0] & 0x400;
        if (ctx.cpuRegs.GPR.r[2].UL[0] == 0) {
            ctx.cpuRegs.GPR.r[4].UL[0] = ctx.cpuRegs.GPR.r[3].UL[0];
        }

    Label_10c0:
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x0);
        ctx.cpuRegs.GPR.r[4].UL[0] = ctx.cpuRegs.GPR.r[4].UL[0] << 1;
        ctx.cpuRegs.GPR.r[2].UL[0] = ctx.cpuRegs.GPR.r[2].UL[0] & 0xFFFFFFC1;
        ctx.cpuRegs.GPR.r[2].UL[0] = ctx.cpuRegs.GPR.r[2].UL[0] | ctx.cpuRegs.GPR.r[4].UL[0];
        goto Label_11b8;
    )code";

        // PART 4: Case 5
        file << R"code(
    Label_caseD_5:
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x30);
        ctx.cpuRegs.GPR.r[5].UD[0] = ctx.cpuRegs.GPR.r[29].UD[0];
        ctx.cpuRegs.GPR.r[4].UL[0] = memory::read<uint8_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0]) + 0x0);
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1e10e8;
        if (recompiled_functions.count(0x1e3308)) {
            recompiled_functions[0x1e3308](ctx, 0x1e3308);
        } else {
            ctx.cpuRegs.pc = 0x1e3308;
        }
        ctx.cpuRegs.GPR.r[3].UL[0] = memory::read<uint8_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x11);
        if (ctx.cpuRegs.GPR.r[3].UL[0] == 0) {
            ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x0);
            ctx.cpuRegs.GPR.r[2].UL[0] = ctx.cpuRegs.GPR.r[2].UL[0] & 0xFFFFFFC1;
            ctx.cpuRegs.GPR.r[2].UL[0] = ctx.cpuRegs.GPR.r[2].UL[0] | 0x10;
            goto Label_11b8;
        }
        
        ctx.fpuRegs.fpr[1].UL = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x18);
        ctx.fpuRegs.fpr[0].UL = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x8);
        {
            float f1_val = ctx.fpuRegs.fpr[1].f;
            float f0_val = ctx.fpuRegs.fpr[0].f;
            if (f1_val != f0_val) {
                ctx.fpuRegs.fpr[12].UL = ctx.fpuRegs.fpr[20].UL;
                goto Label_1138;
            }
        }
        ctx.fpuRegs.fpr[1].UL = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x1c);
        ctx.fpuRegs.fpr[0].UL = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0xc);
        {
            float f1_val = ctx.fpuRegs.fpr[1].f;
            float f0_val = ctx.fpuRegs.fpr[0].f;
            if (f1_val == f0_val) {
                ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x0);
                goto Label_11c0;
            }
        }
        ctx.fpuRegs.fpr[12].UL = ctx.fpuRegs.fpr[20].UL;

    Label_1138:
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[16].UD[0];
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1e1140;
        if (recompiled_functions.count(0x1e0840)) {
            recompiled_functions[0x1e0840](ctx, 0x1e0840);
        } else {
            ctx.cpuRegs.pc = 0x1e0840;
        }
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x0);
        goto Label_11c0;
    )code";

        // PART 5: Case 8
        file << R"code(
    Label_caseD_8:
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x0);
        ctx.cpuRegs.GPR.r[6].UL[0] = 1;
        ctx.cpuRegs.GPR.r[3].UL[0] = 2;
        ctx.cpuRegs.GPR.r[2].UL[0] = ctx.cpuRegs.GPR.r[2].UL[0] & 0x1;
        if (ctx.cpuRegs.GPR.r[2].UL[0] != 0) {
            ctx.cpuRegs.GPR.r[6].UL[0] = ctx.cpuRegs.GPR.r[3].UL[0];
        }
        ctx.cpuRegs.GPR.r[5].UL[0] = 0;
        if (ctx.cpuRegs.GPR.r[6].UL[0] == 0) {
            goto Label_1194;
        }
        ctx.cpuRegs.GPR.r[4].UL[0] = ctx.cpuRegs.GPR.r[16].UL[0] + 0x28;
        ctx.cpuRegs.GPR.r[7].UL[0] = 0xFFFFFFDF;

    Label_1170:
        ctx.cpuRegs.GPR.r[3].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[4].UL[0]) + 0x0);
        ctx.cpuRegs.GPR.r[5].UL[0] = ctx.cpuRegs.GPR.r[5].UL[0] + 1;
        if (ctx.cpuRegs.GPR.r[3].UL[0] == 0) {
            goto Label_1188;
        }
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x0);
        ctx.cpuRegs.GPR.r[2].UL[0] = ctx.cpuRegs.GPR.r[2].UL[0] & ctx.cpuRegs.GPR.r[7].UL[0];
        memory::write<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x0, ctx.cpuRegs.GPR.r[2].UL[0]);

    Label_1188:
        ctx.cpuRegs.GPR.r[2].UL[0] = (ctx.cpuRegs.GPR.r[5].UL[0] < ctx.cpuRegs.GPR.r[6].UL[0]) ? 1 : 0;
        ctx.cpuRegs.GPR.r[4].UL[0] = ctx.cpuRegs.GPR.r[4].UL[0] + 4;
        if (ctx.cpuRegs.GPR.r[2].UL[0] != 0) {
            goto Label_1170;
        }

    Label_1194:
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x30);
        if (ctx.cpuRegs.GPR.r[2].UL[0] == 0) {
            ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x0);
            goto Label_11ac;
        }
        ctx.cpuRegs.GPR.r[4].UL[0] = memory::read<uint8_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0]) + 0x0);
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1e11a8;
        if (recompiled_functions.count(0x1e6c28)) {
            recompiled_functions[0x1e6c28](ctx, 0x1e6c28);
        } else {
            ctx.cpuRegs.pc = 0x1e6c28;
        }
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x0);

    Label_11ac:
        ctx.cpuRegs.GPR.r[2].UL[0] = ctx.cpuRegs.GPR.r[2].UL[0] & 0xFFFFFFC1;
        ctx.cpuRegs.GPR.r[2].UL[0] = ctx.cpuRegs.GPR.r[2].UL[0] | 0x2;
    )code";

        // PART 6: Common exit paths and epilogue
        file << R"code(
    Label_11b8:
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x0, ctx.cpuRegs.GPR.r[2].UL[0]);

    Label_caseD_0:
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x0);

    Label_11c0:
        ctx.cpuRegs.GPR.r[16].UD[0] = memory::read<uint64_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x50);
        ctx.cpuRegs.GPR.r[2].UL[0] = ctx.cpuRegs.GPR.r[2].UL[0] >> 1;
        ctx.cpuRegs.GPR.r[17].UD[0] = memory::read<uint64_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x58);
        ctx.cpuRegs.GPR.r[2].UL[0] = ctx.cpuRegs.GPR.r[2].UL[0] & 0x1F;
        ctx.cpuRegs.GPR.r[31].UD[0] = memory::read<uint64_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x60);
        ctx.cpuRegs.GPR.r[2].UL[0] = ctx.cpuRegs.GPR.r[2].UL[0] ^ 0x1;
        ctx.fpuRegs.fpr[20].UL = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x70);
        ctx.cpuRegs.GPR.r[2].UL[0] = (0 < ctx.cpuRegs.GPR.r[2].UL[0]) ? 1 : 0;
        ctx.cpuRegs.GPR.r[29].SL[0] = ctx.cpuRegs.GPR.r[29].SL[0] + 0x80;
        return;
    }
    )code" << std::endl;
        return;
    }

    if (func.base_address == 0x002a6b68) {
        // FUN_002a6b68 - Command State Machine Dispatcher
        file << R"code(void FUN_002a6b68(CpuContext& ctx) {
        // Prologue
        ctx.cpuRegs.GPR.r[29].SL[0] = ctx.cpuRegs.GPR.r[29].SL[0] - 0x30;
        ctx.cpuRegs.GPR.r[2].SL[0] = -1;
        memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x10, ctx.cpuRegs.GPR.r[16].UD[0]);
        memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x18, ctx.cpuRegs.GPR.r[17].UD[0]);
        ctx.cpuRegs.GPR.r[16].UD[0] = ctx.cpuRegs.GPR.r[4].UD[0];  // s0 = a0
        memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x20, ctx.cpuRegs.GPR.r[18].UD[0]);
        ctx.cpuRegs.GPR.r[17].UD[0] = ctx.cpuRegs.GPR.r[5].UD[0];  // s1 = a1
        ctx.cpuRegs.GPR.r[18].UD[0] = ctx.cpuRegs.GPR.r[6].UD[0];  // s2 = a2
        memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x28, ctx.cpuRegs.GPR.r[31].UD[0]);
        
        // Initialize local_2c and local_30 on stack
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x4, 0xFFFFFFFF);  // local_2c = -1
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x0, 0xFFFFFFFF);  // local_30 = -1
        
        // Call FUN_002c5820(1, &local_30, &local_2c)
        ctx.cpuRegs.GPR.r[4].UL[0] = 1;
        ctx.cpuRegs.GPR.r[5].UD[0] = ctx.cpuRegs.GPR.r[29].UD[0];              // a1 = sp (&local_30)
        ctx.cpuRegs.GPR.r[6].UL[0] = ctx.cpuRegs.GPR.r[29].UL[0] | 0x4;        // a2 = sp | 4 (&local_2c)
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x2a6ba4;
        if (recompiled_functions.count(0x2c5820)) {
            recompiled_functions[0x2c5820](ctx, 0x2c5820);
        } else {
            g_logFile << "Failed: FUN_002c5820" << std::endl;
            ctx.cpuRegs.pc = 0x2c5820;
            return;
        }
        
        // a2 = call result
        ctx.cpuRegs.GPR.r[6].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0];
        ctx.cpuRegs.GPR.r[2].UL[0] = 1;
        
        // Load state from gp-0x7538
        uint32_t gp_val = ctx.cpuRegs.GPR.r[28].UL[0];
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(gp_val - 0x7538);
        
        // if (call_result == 1) { gp[-0x7528] = local_2c }
        if (ctx.cpuRegs.GPR.r[6].UL[0] == 1) {
            ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x4);
            memory::write<uint32_t>(gp_val - 0x7528, ctx.cpuRegs.GPR.r[2].UL[0]);
            ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(gp_val - 0x7538);
        }

    Label_6bc0:
        // a1 = state for switch
        ctx.cpuRegs.GPR.r[5].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0];
        
        // Bounds check: if (state >= 12) goto default
        if (ctx.cpuRegs.GPR.r[5].UL[0] >= 12) {
            goto Label_caseD_c;
        }
        
        // Switch dispatch
        switch (ctx.cpuRegs.GPR.r[5].UL[0]) {
            case 0: goto Label_caseD_0;
            case 1: goto Label_caseD_1;
            case 2: goto Label_caseD_2;
            case 3: goto Label_caseD_3;
            case 4: goto Label_caseD_4;
            case 5: goto Label_caseD_5;
            case 6: goto Label_caseD_6;
            case 7: goto Label_caseD_7;
            case 8: goto Label_caseD_8;
            case 9: goto Label_caseD_9;
            case 10: goto Label_caseD_a;
            case 11: goto Label_caseD_b;
            default: goto Label_caseD_c;
        }

    Label_caseD_0:  // 0x2a6d04
        ctx.cpuRegs.GPR.r[3].UL[0] = memory::read<uint32_t>(gp_val - 0x7530);
        ctx.cpuRegs.GPR.r[2].SL[0] = -1;
        if (ctx.cpuRegs.GPR.r[3].UL[0] != 0xFFFFFFFF) {
            goto Label_caseD_c;
        }
        // Call FUN_002c5940(s0, s1, 0x2e7d68, 0x2e7d6c, 0x2e7d70)
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[16].UD[0];
        ctx.cpuRegs.GPR.r[5].UD[0] = ctx.cpuRegs.GPR.r[17].UD[0];
        ctx.cpuRegs.GPR.r[6].UL[0] = 0x2e7d68;
        ctx.cpuRegs.GPR.r[7].UL[0] = 0x2e7d6c;
        ctx.cpuRegs.GPR.r[8].UL[0] = 0x2e7d70;
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x2a6d2c;
        if (recompiled_functions.count(0x2c5940)) {
            recompiled_functions[0x2c5940](ctx, 0x2c5940);
        } else {
            g_logFile << "Failed: FUN_002c5940" << std::endl;
            ctx.cpuRegs.pc = 0x2c5940;
            return;
        }
        goto Label_caseD_c;

    Label_caseD_1:  // 0x2a6be8
        ctx.cpuRegs.GPR.r[2].SL[0] = -1;
        if (ctx.cpuRegs.GPR.r[6].UL[0] != 0xFFFFFFFF) {
            goto Label_caseD_c;
        }
        ctx.cpuRegs.GPR.r[4].UL[0] = 2;
        ctx.cpuRegs.GPR.r[3].UL[0] = memory::read<uint32_t>(0x2e7d68);
        if (ctx.cpuRegs.GPR.r[3].UL[0] == ctx.cpuRegs.GPR.r[4].UL[0]) {
            goto Label_6cf4;
        }
        
        // Memory copy: 0x3064e0 -> 0x3c61a0
        {
            ctx.cpuRegs.GPR.r[4].UL[0] = 0x3c61a0;
            ctx.cpuRegs.GPR.r[3].UL[0] = 0x3064e0;
            
            // Save state variables
            memory::write<uint32_t>(gp_val - 0x7534, ctx.cpuRegs.GPR.r[5].UL[0]);
            memory::write<uint32_t>(gp_val - 0x7530, ctx.cpuRegs.GPR.r[6].UL[0]);
            memory::write<uint32_t>(gp_val - 0x7538, 0);
            memory::write<uint32_t>(gp_val - 0x752c, 0);
            
            // Check alignment
            ctx.cpuRegs.GPR.r[2].UL[0] = (ctx.cpuRegs.GPR.r[3].UL[0] | ctx.cpuRegs.GPR.r[4].UL[0]) & 0x7;
            
            uint32_t src = 0x3064e0;
            uint32_t dst = 0x3c61a0;
            uint32_t end = src + 0x80;
            
            // Copy 0x80 bytes (main loop)
            while (src != end) {
                uint64_t v0 = memory::read<uint64_t>(src + 0x00);
                uint64_t v1 = memory::read<uint64_t>(src + 0x08);
                uint64_t v2 = memory::read<uint64_t>(src + 0x10);
                uint64_t v3 = memory::read<uint64_t>(src + 0x18);
                memory::write<uint64_t>(dst + 0x00, v0);
                memory::write<uint64_t>(dst + 0x08, v1);
                memory::write<uint64_t>(dst + 0x10, v2);
                memory::write<uint64_t>(dst + 0x18, v3);
                src += 0x20;
                dst += 0x20;
            }
            
            // Final partial copy (0x1c bytes at 0x306560 -> 0x3c61c0)
            {
                uint64_t v0 = memory::read<uint64_t>(src + 0x00);
                uint64_t v1 = memory::read<uint64_t>(src + 0x08);
                uint64_t v2 = memory::read<uint64_t>(src + 0x10);
                uint32_t v3 = memory::read<uint32_t>(src + 0x18);
                memory::write<uint64_t>(dst + 0x00, v0);
                memory::write<uint64_t>(dst + 0x08, v1);
                memory::write<uint64_t>(dst + 0x10, v2);
                memory::write<uint32_t>(dst + 0x18, v3);
            }
        }
        ctx.cpuRegs.GPR.r[2].SL[0] = -1;
        goto Label_epilogue;

    Label_6cf4:
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(gp_val - 0x7530);
        memory::write<uint32_t>(gp_val - 0x7530, ctx.cpuRegs.GPR.r[6].UL[0]);
        memory::write<uint32_t>(gp_val - 0x7538, ctx.cpuRegs.GPR.r[2].UL[0]);
        goto Label_caseD_c;

    Label_caseD_2:  // 0x2a6d34
        ctx.cpuRegs.GPR.r[5].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x4);
        ctx.cpuRegs.GPR.r[4].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x0);
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x2a6d40;
        if (recompiled_functions.count(0x2a2970)) {
            recompiled_functions[0x2a2970](ctx, 0x2a2970);
        } else {
            g_logFile << "Failed: FUN_002a2970" << std::endl;
            ctx.cpuRegs.pc = 0x2a2970;
            return;
        }
        goto Label_caseD_c;

    Label_caseD_3:  // 0x2a6d70
        ctx.cpuRegs.GPR.r[5].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x4);
        ctx.cpuRegs.GPR.r[4].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x0);
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x2a6d7c;
        if (recompiled_functions.count(0x2a3488)) {
            recompiled_functions[0x2a3488](ctx, 0x2a3488);
        } else {
            g_logFile << "Failed: FUN_002a3488" << std::endl;
            ctx.cpuRegs.pc = 0x2a3488;
            return;
        }
        goto Label_caseD_c;

    Label_caseD_4:  // 0x2a6d5c
        ctx.cpuRegs.GPR.r[5].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x4);
        ctx.cpuRegs.GPR.r[4].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x0);
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x2a6d68;
        if (recompiled_functions.count(0x2a3078)) {
            recompiled_functions[0x2a3078](ctx, 0x2a3078);
        } else {
            g_logFile << "Failed: FUN_002a3078" << std::endl;
            ctx.cpuRegs.pc = 0x2a3078;
            return;
        }
        goto Label_caseD_c;

    Label_caseD_5:  // 0x2a6d48
        ctx.cpuRegs.GPR.r[5].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x4);
        ctx.cpuRegs.GPR.r[4].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x0);
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x2a6d54;
        if (recompiled_functions.count(0x2a2d50)) {
            recompiled_functions[0x2a2d50](ctx, 0x2a2d50);
        } else {
            g_logFile << "Failed: FUN_002a2d50" << std::endl;
            ctx.cpuRegs.pc = 0x2a2d50;
            return;
        }
        goto Label_caseD_c;

    Label_caseD_6:  // 0x2a6d84
        ctx.cpuRegs.GPR.r[5].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x4);
        ctx.cpuRegs.GPR.r[4].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x0);
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x2a6d90;
        if (recompiled_functions.count(0x2a3c60)) {
            recompiled_functions[0x2a3c60](ctx, 0x2a3c60);
        } else {
            g_logFile << "Failed: FUN_002a3c60" << std::endl;
            ctx.cpuRegs.pc = 0x2a3c60;
            return;
        }
        goto Label_caseD_c;

    Label_caseD_7:  // 0x2a6d98
        ctx.cpuRegs.GPR.r[5].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x4);
        ctx.cpuRegs.GPR.r[4].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x0);
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x2a6da4;
        if (recompiled_functions.count(0x2a37a8)) {
            recompiled_functions[0x2a37a8](ctx, 0x2a37a8);
        } else {
            g_logFile << "Failed: FUN_002a37a8" << std::endl;
            ctx.cpuRegs.pc = 0x2a37a8;
            return;
        }
        goto Label_caseD_c;

    Label_caseD_8:  // 0x2a6dac
        ctx.cpuRegs.GPR.r[5].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x4);
        ctx.cpuRegs.GPR.r[4].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x0);
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x2a6db8;
        if (recompiled_functions.count(0x2a44d0)) {
            recompiled_functions[0x2a44d0](ctx, 0x2a44d0);
        } else {
            g_logFile << "Failed: FUN_002a44d0" << std::endl;
            ctx.cpuRegs.pc = 0x2a44d0;
            return;
        }
        goto Label_caseD_c;

    Label_caseD_9:  // 0x2a6dc0
        ctx.cpuRegs.GPR.r[5].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x4);
        ctx.cpuRegs.GPR.r[4].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x0);
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x2a6dcc;
        if (recompiled_functions.count(0x2a65d8)) {
            recompiled_functions[0x2a65d8](ctx, 0x2a65d8);
        } else {
            g_logFile << "Failed: FUN_002a65d8" << std::endl;
            ctx.cpuRegs.pc = 0x2a65d8;
            return;
        }
        goto Label_caseD_c;

    Label_caseD_a:  // 0x2a6dd4
        ctx.cpuRegs.GPR.r[5].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x4);
        ctx.cpuRegs.GPR.r[4].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x0);
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x2a6de0;
        if (recompiled_functions.count(0x2a5ef0)) {
            recompiled_functions[0x2a5ef0](ctx, 0x2a5ef0);
        } else {
            g_logFile << "Failed: FUN_002a5ef0" << std::endl;
            ctx.cpuRegs.pc = 0x2a5ef0;
            return;
        }
        goto Label_caseD_c;

    Label_caseD_b:  // 0x2a6de8
        ctx.cpuRegs.GPR.r[5].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x4);
        ctx.cpuRegs.GPR.r[4].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x0);
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x2a6df4;
        if (recompiled_functions.count(0x2a4b30)) {
            recompiled_functions[0x2a4b30](ctx, 0x2a4b30);
        } else {
            g_logFile << "Failed: FUN_002a4b30" << std::endl;
            ctx.cpuRegs.pc = 0x2a4b30;
            return;
        }
        // Fall through to default

    Label_caseD_c:  // 0x2a6df4 - Default case / common exit
        // if (s2 != 0) copy 12 bytes from 0x2e7d68 to s2
        if (ctx.cpuRegs.GPR.r[18].UL[0] != 0) {
            ctx.cpuRegs.GPR.r[6].UL[0] = 0x2e7d68;
            uint64_t v0 = memory::read<uint64_t>(0x2e7d68);
            uint32_t v1 = memory::read<uint32_t>(0x2e7d70);
            memory::write<uint64_t>(ctx.cpuRegs.GPR.r[18].UL[0], v0);
            memory::write<uint32_t>(ctx.cpuRegs.GPR.r[18].UL[0] + 0x8, v1);
        }
        
        // Return state
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(gp_val - 0x7538);

    Label_epilogue:
        // Restore registers and return
        ctx.cpuRegs.GPR.r[16].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x10);
        ctx.cpuRegs.GPR.r[17].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x18);
        ctx.cpuRegs.GPR.r[18].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x20);
        ctx.cpuRegs.GPR.r[31].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x28);
        ctx.cpuRegs.GPR.r[29].SL[0] = ctx.cpuRegs.GPR.r[29].SL[0] + 0x30;
        return;
    }
    )code" << std::endl;
        return;
    }


    if (func.base_address == 0x00177168) {
        // FUN_00177168 - Main Dispatcher
        file << R"code(void FUN_00177168(CpuContext& ctx) {
        // Prologue
        ctx.cpuRegs.GPR.r[29].SL[0] = ctx.cpuRegs.GPR.r[29].SL[0] - 0x60;
        memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x28, ctx.cpuRegs.GPR.r[17].UD[0]);
        ctx.cpuRegs.GPR.r[17].UD[0] = ctx.cpuRegs.GPR.r[4].UD[0]; // s1 = param_1
        memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x48, ctx.cpuRegs.GPR.r[21].UD[0]);
        ctx.cpuRegs.GPR.r[21].UL[0] = 0x323530;                   // s5 = 0x323530
        memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x50, ctx.cpuRegs.GPR.r[22].UD[0]);
        ctx.cpuRegs.GPR.r[22].UD[0] = ctx.cpuRegs.GPR.r[5].UD[0]; // s6 = param_2
        memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x20, ctx.cpuRegs.GPR.r[16].UD[0]);
        memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x30, ctx.cpuRegs.GPR.r[18].UD[0]);
        memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x38, ctx.cpuRegs.GPR.r[19].UD[0]);
        memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x40, ctx.cpuRegs.GPR.r[20].UD[0]);
        memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x58, ctx.cpuRegs.GPR.r[31].UD[0]);

        // Call FUN_002b3548(s1, 0x16, 1)
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[17].UD[0];
        ctx.cpuRegs.GPR.r[5].UL[0] = 0x16;
        ctx.cpuRegs.GPR.r[6].UL[0] = 1;
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1771a8;
        if (recompiled_functions.count(0x2b3548)) {
            recompiled_functions[0x2b3548](ctx, 0x2b3548);
        } else {
            ctx.cpuRegs.pc = 0x2b3548;
            return;
        }

        // Logic Block 1
        ctx.cpuRegs.GPR.r[4].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x34);
        ctx.cpuRegs.GPR.r[4].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0);
        ctx.cpuRegs.GPR.r[2].UL[0] = ctx.cpuRegs.GPR.r[2].UL[0] & 0x1;
        ctx.cpuRegs.GPR.r[5].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x10);
        
        {
            uint64_t mask = 0x17fffffffULL; 
            ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[4].UD[0] & mask;
            uint64_t v0_shifted = (uint64_t)ctx.cpuRegs.GPR.r[2].UL[0] << 31;
            ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[4].UD[0] | v0_shifted;
        }
        
        ctx.cpuRegs.GPR.r[5].UL[0] = ctx.cpuRegs.GPR.r[5].UL[0] + 1;
        memory::write<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0, ctx.cpuRegs.GPR.r[4].UD[0]);
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x10, ctx.cpuRegs.GPR.r[5].UL[0]);

        // Call FUN_00173238(s1, s5)
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[17].UD[0];
        ctx.cpuRegs.GPR.r[5].UD[0] = ctx.cpuRegs.GPR.r[21].UD[0]; // Delay slot move
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1771e8;
        if (recompiled_functions.count(0x173238)) {
            recompiled_functions[0x173238](ctx, 0x173238);
        } else {
            ctx.cpuRegs.pc = 0x173238;
            return;
        }

        // Logic Block 2
        ctx.cpuRegs.GPR.r[4].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0);
        {
            uint64_t v0 = 0xfc00ULL << 40; // dsll32 8 (32+8)
            uint64_t a2 = 0xc000ULL << 39; // dsll32 7 (32+7)
            ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[4].UD[0] & v0;
            
            if (ctx.cpuRegs.GPR.r[2].UD[0] == a2) {
                // Delay slot executed before jump
                ctx.cpuRegs.GPR.r[2].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0);
                goto Label_17729c;
            }
        }

        ctx.cpuRegs.GPR.r[2].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0);
        ctx.cpuRegs.GPR.r[3].UD[0] = ctx.cpuRegs.GPR.r[4].UD[0] >> 44; // dsrl32 12
        
        {
            uint64_t mask_upper = 0x103ffffffffULL;
            ctx.cpuRegs.GPR.r[3].UL[0] = ctx.cpuRegs.GPR.r[3].UL[0] & 0x3f;
            ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[4].UD[0] & mask_upper;
            
            uint64_t v1_shifted = (uint64_t)ctx.cpuRegs.GPR.r[3].UL[0] << 38; // dsll32 6
            
            uint64_t param_mask = 0xfffcULL << 52; // dsll 20 + 32? No, chain of ors
            // Manual reconstruction of large constant generation:
            // 0xfffc << 20 = 0xfffc00000 
            // | 0xffff << 16
            // | 0xffff << 12
            // | 0xfff
            // Approximating based on disassembly intent (clearing bits): 
            // 0xfffcffffffffffff is likely
            uint64_t mask_accum = 0xfffc00000ULL; 
            mask_accum |= 0xffffULL; 
            mask_accum <<= 16;
            mask_accum |= 0xffffULL;
            mask_accum <<= 12;
            mask_accum |= 0xfffULL;
            
            ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] | v1_shifted;
            
            // Param 2 mask generation
            uint64_t mask_p2 = 0xff03ULL << 48; // chained shifts
            mask_p2 |= 0xffffULL; mask_p2 <<= 16;
            mask_p2 |= 0xffffULL; mask_p2 <<= 16;
            mask_p2 |= 0xffffULL;
            
            uint64_t v1_extracted = ctx.cpuRegs.GPR.r[2].UD[0] >> 50; // dsrl32 18
            v1_extracted &= 0x3f;
            
            ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] & mask_accum;
            v1_extracted <<= 44; // dsll32 12
            
            ctx.cpuRegs.GPR.r[4].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x10);
            
            ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] | v1_extracted;
            ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] & mask_p2;
            
            memory::write<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x18, ctx.cpuRegs.GPR.r[4].UL[0]);
            
            uint64_t c000_shift = 0xc000ULL << 39; // approx from block 1
            ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] | c000_shift;
            
            memory::write<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0, ctx.cpuRegs.GPR.r[2].UD[0]);
        }
        
        ctx.cpuRegs.GPR.r[3].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[21].UL[0] + 0x4);
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x1c, ctx.cpuRegs.GPR.r[3].UL[0]);
        ctx.cpuRegs.GPR.r[2].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0);

    Label_17729c:
        ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] >> 44; // dsrl32 12
        ctx.cpuRegs.GPR.r[4].UL[0] = ctx.cpuRegs.GPR.r[2].UL[0] & 0x3f;
        
        // Bounds check
        if (ctx.cpuRegs.GPR.r[4].UL[0] >= 0x17) {
            goto Label_caseD_17;
        }

        // Switch Dispatch
        switch (ctx.cpuRegs.GPR.r[4].UL[0]) {
            case 0: goto Label_caseD_0;
            case 1: goto Label_caseD_1;
            case 2: goto Label_caseD_2;
            case 3: goto Label_caseD_3;
            case 4: goto Label_caseD_4;
            case 5: goto Label_caseD_5;
            case 6: goto Label_caseD_6;
            case 7: goto Label_caseD_7;
            case 8: goto Label_caseD_8;
            case 9: goto Label_caseD_9;
            case 10: goto Label_caseD_a;
            case 11: goto Label_caseD_b;
            case 12: goto Label_caseD_c;
            case 13: goto Label_caseD_d;
            case 14: goto Label_caseD_e;
            case 15: goto Label_caseD_f;
            case 16: goto Label_caseD_10;
            default: goto Label_caseD_17;
        }

    Label_caseD_0:
        // Load byte from 0x309738 + 3
        ctx.cpuRegs.GPR.r[3].UL[0] = memory::read<uint8_t>(0x309738 + 3);
        ctx.cpuRegs.GPR.r[3].UL[0] = ctx.cpuRegs.GPR.r[3].UL[0] & 0xf;
        
        if (ctx.cpuRegs.GPR.r[3].UL[0] == 1) {
            goto Label_177708;
        }
        
        ctx.cpuRegs.GPR.r[18].UL[0] = 0xb;
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1772e8;
        if (recompiled_functions.count(0x1f0e68)) {
            recompiled_functions[0x1f0e68](ctx, 0x1f0e68);
        } else {
            ctx.cpuRegs.pc = 0x1f0e68;
            return;
        }
        
        ctx.cpuRegs.GPR.r[20].UL[0] = ctx.cpuRegs.GPR.r[17].UL[0] + 0x5d0;
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1772f0;
        if (recompiled_functions.count(0x1f0e68)) {
            recompiled_functions[0x1f0e68](ctx, 0x1f0e68);
        } else {
            ctx.cpuRegs.pc = 0x1f0e68;
            return;
        }
        
        ctx.cpuRegs.GPR.r[19].UL[0] = ctx.cpuRegs.GPR.r[17].UL[0] + 0x4f0;
        ctx.cpuRegs.GPR.r[16].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0] + 0x0); // v0 from prev call
        
        if (ctx.cpuRegs.GPR.r[16].UL[0] == 0) {
            goto Label_177324;
        }
        
        ctx.cpuRegs.GPR.r[4].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x154);

    Label_177308:
        if (ctx.cpuRegs.GPR.r[4].UL[0] == 0) {
            goto Label_17731c;
        }
        ctx.cpuRegs.GPR.r[16].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x148);
        
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x177314;
        if (recompiled_functions.count(0x1918a8)) {
            recompiled_functions[0x1918a8](ctx, 0x1918a8);
        } else {
            ctx.cpuRegs.pc = 0x1918a8;
            return;
        }
        
        ctx.cpuRegs.GPR.r[16].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x148);

    Label_17731c:
        if (ctx.cpuRegs.GPR.r[16].UL[0] != 0) {
            ctx.cpuRegs.GPR.r[4].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x154);
            goto Label_177308;
        }

    Label_177324:
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x177328;
        if (recompiled_functions.count(0x181400)) {
            recompiled_functions[0x181400](ctx, 0x181400);
        } else {
            ctx.cpuRegs.pc = 0x181400;
            return;
        }
        ctx.cpuRegs.GPR.r[18].UL[0] = 1;
        goto Label_177714;

    Label_caseD_1:
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[17].UD[0];
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x17733c;
        if (recompiled_functions.count(0x173ab0)) {
            recompiled_functions[0x173ab0](ctx, 0x173ab0);
        } else {
            ctx.cpuRegs.pc = 0x173ab0;
            return;
        }
        ctx.cpuRegs.GPR.r[5].UD[0] = ctx.cpuRegs.GPR.r[21].UD[0]; // Delay slot
        ctx.cpuRegs.GPR.r[20].UL[0] = ctx.cpuRegs.GPR.r[17].UL[0] + 0x5d0;
        ctx.cpuRegs.GPR.r[18].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0]; // s2 = v0
        
        if (ctx.cpuRegs.GPR.r[18].UL[0] == 0x18) {
            goto Label_17774c;
        }
        
        ctx.cpuRegs.GPR.r[19].UL[0] = ctx.cpuRegs.GPR.r[17].UL[0] + 0x4f0;
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x177358;
        if (recompiled_functions.count(0x16d368)) {
            recompiled_functions[0x16d368](ctx, 0x16d368);
        } else {
            ctx.cpuRegs.pc = 0x16d368;
            return;
        }
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[20].UD[0];
        goto Label_177714;

    Label_caseD_2:
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[17].UD[0];
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x17736c;
        if (recompiled_functions.count(0x173ba0)) {
            recompiled_functions[0x173ba0](ctx, 0x173ba0);
        } else {
            ctx.cpuRegs.pc = 0x173ba0;
            return;
        }
        ctx.cpuRegs.GPR.r[5].UD[0] = ctx.cpuRegs.GPR.r[21].UD[0];
        ctx.cpuRegs.GPR.r[20].UL[0] = ctx.cpuRegs.GPR.r[17].UL[0] + 0x5d0;
        goto Label_17770c;

    Label_caseD_3:
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[17].UD[0];
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x177384;
        if (recompiled_functions.count(0x173c58)) {
            recompiled_functions[0x173c58](ctx, 0x173c58);
        } else {
            ctx.cpuRegs.pc = 0x173c58;
            return;
        }
        ctx.cpuRegs.GPR.r[5].UD[0] = ctx.cpuRegs.GPR.r[21].UD[0];
        ctx.cpuRegs.GPR.r[20].UL[0] = ctx.cpuRegs.GPR.r[17].UL[0] + 0x5d0;
        goto Label_17770c;

    Label_caseD_4:
        ctx.fpuRegs.fpr[1].UL = 0x3f000000;
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[17].UD[0];
        ctx.fpuRegs.fpr[0].UL = memory::read<uint32_t>(0x3097dc);
        ctx.cpuRegs.GPR.r[5].UD[0] = ctx.cpuRegs.GPR.r[21].UD[0];
        ctx.cpuRegs.GPR.r[7].UL[0] = 0x14;
        ctx.cpuRegs.GPR.r[8].UL[0] = 1;
        ctx.fpuRegs.fpr[0].f = ctx.fpuRegs.fpr[0].f * ctx.fpuRegs.fpr[1].f;
        ctx.cpuRegs.GPR.r[9].UL[0] = 0;
        ctx.cpuRegs.GPR.r[18].UL[0] = 0x18;
        ctx.cpuRegs.GPR.r[20].UL[0] = ctx.cpuRegs.GPR.r[17].UL[0] + 0x5d0;
        
        ctx.fpuRegs.fpr[1].f = ctx.fpuRegs.fpr[0].f; // cvt.w.s (simulated cast)
        ctx.fpuRegs.fpr[1].SL = (int32_t)ctx.fpuRegs.fpr[1].f;
        ctx.cpuRegs.GPR.r[6].SL[0] = ctx.fpuRegs.fpr[1].SL;
        
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1773d0;
        if (recompiled_functions.count(0x172b60)) {
            recompiled_functions[0x172b60](ctx, 0x172b60);
        } else {
            ctx.cpuRegs.pc = 0x172b60;
            return;
        }
        ctx.cpuRegs.GPR.r[19].UL[0] = ctx.cpuRegs.GPR.r[17].UL[0] + 0x4f0;
        {
            uint64_t v1 = 5;
            if (ctx.cpuRegs.GPR.r[2].UL[0] != 0) {
                ctx.cpuRegs.GPR.r[18].UD[0] = v1;
            }
        }
        goto Label_177710;

    Label_caseD_5:
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[17].UD[0];
        ctx.cpuRegs.GPR.r[5].UD[0] = ctx.cpuRegs.GPR.r[21].UD[0];
        ctx.cpuRegs.GPR.r[6].UL[0] = 0;
        ctx.cpuRegs.GPR.r[7].UL[0] = 0;
        ctx.cpuRegs.GPR.r[8].UL[0] = 1;
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1773f8;
        if (recompiled_functions.count(0x172b60)) {
            recompiled_functions[0x172b60](ctx, 0x172b60);
        } else {
            ctx.cpuRegs.pc = 0x172b60;
            return;
        }
        ctx.cpuRegs.GPR.r[9].UL[0] = 0;
        
        if (ctx.cpuRegs.GPR.r[2].UL[0] == 0) {
            goto Label_177708;
        }
        
        ctx.cpuRegs.GPR.r[18].UL[0] = 0x18;
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x504);
        
        if ((int32_t)ctx.cpuRegs.GPR.r[2].UL[0] <= 0) {
            goto Label_17744c;
        }
        
        ctx.cpuRegs.GPR.r[19].UL[0] = ctx.cpuRegs.GPR.r[17].UL[0] + 0x4f0;
        ctx.cpuRegs.GPR.r[2].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0);
        
        {
            uint64_t mask = 0xf87f0000ULL;
            mask |= 0xffffULL;
            ctx.cpuRegs.GPR.r[5].UL[0] = 0x1000000; // 0x100 << 16
            ctx.cpuRegs.GPR.r[6].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x28);
            
            ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] & mask;
            ctx.cpuRegs.GPR.r[4].UL[0] = ctx.cpuRegs.GPR.r[17].UL[0] + 0x594;
            ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] | ctx.cpuRegs.GPR.r[5].UL[0];
            ctx.cpuRegs.GPR.r[7].UL[0] = 0;
            ctx.cpuRegs.GPR.r[5].UD[0] = ctx.cpuRegs.GPR.r[19].UD[0]; // param_2 = s3
            
            ctx.cpuRegs.GPR.r[31].UL[0] = 0x17743c;
            if (recompiled_functions.count(0x168810)) {
                recompiled_functions[0x168810](ctx, 0x168810);
            } else {
                ctx.cpuRegs.pc = 0x168810;
                return;
            }
            memory::write<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0, ctx.cpuRegs.GPR.r[2].UD[0]);
            ctx.cpuRegs.GPR.r[18].UL[0] = 9;
            goto Label_177710;
        }

    Label_17744c:
        ctx.cpuRegs.GPR.r[18].UL[0] = 6;
        goto Label_177710;

    Label_caseD_6:
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[17].UD[0];
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x177460;
        if (recompiled_functions.count(0x173ce8)) {
            recompiled_functions[0x173ce8](ctx, 0x173ce8);
        } else {
            ctx.cpuRegs.pc = 0x173ce8;
            return;
        }
        ctx.cpuRegs.GPR.r[5].UD[0] = ctx.cpuRegs.GPR.r[21].UD[0];
        ctx.cpuRegs.GPR.r[20].UL[0] = ctx.cpuRegs.GPR.r[17].UL[0] + 0x5d0;
        goto Label_17770c;
    )code" << std::endl;
    file << R"code(
    Label_caseD_7:
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[17].UD[0];
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x177478;
        if (recompiled_functions.count(0x173f38)) {
            recompiled_functions[0x173f38](ctx, 0x173f38);
        } else {
            ctx.cpuRegs.pc = 0x173f38;
            return;
        }
        ctx.cpuRegs.GPR.r[5].UD[0] = ctx.cpuRegs.GPR.r[21].UD[0];
        ctx.cpuRegs.GPR.r[20].UL[0] = ctx.cpuRegs.GPR.r[17].UL[0] + 0x5d0;
        goto Label_17770c;

    Label_caseD_8:
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[17].UD[0];
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x177490;
        if (recompiled_functions.count(0x174108)) {
            recompiled_functions[0x174108](ctx, 0x174108);
        } else {
            ctx.cpuRegs.pc = 0x174108;
            return;
        }
        ctx.cpuRegs.GPR.r[5].UD[0] = ctx.cpuRegs.GPR.r[21].UD[0];
        ctx.cpuRegs.GPR.r[20].UL[0] = ctx.cpuRegs.GPR.r[17].UL[0] + 0x5d0;
        goto Label_17770c;

    Label_caseD_9:
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[17].UD[0];
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1774a8;
        if (recompiled_functions.count(0x174240)) {
            recompiled_functions[0x174240](ctx, 0x174240);
        } else {
            ctx.cpuRegs.pc = 0x174240;
            return;
        }
        ctx.cpuRegs.GPR.r[5].UD[0] = ctx.cpuRegs.GPR.r[21].UD[0];
        ctx.cpuRegs.GPR.r[20].UL[0] = ctx.cpuRegs.GPR.r[17].UL[0] + 0x5d0;
        goto Label_17770c;

    Label_caseD_a:
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[17].UD[0];
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1774c0;
        if (recompiled_functions.count(0x174478)) {
            recompiled_functions[0x174478](ctx, 0x174478);
        } else {
            ctx.cpuRegs.pc = 0x174478;
            return;
        }
        ctx.cpuRegs.GPR.r[5].UD[0] = ctx.cpuRegs.GPR.r[21].UD[0];
        ctx.cpuRegs.GPR.r[20].UL[0] = ctx.cpuRegs.GPR.r[17].UL[0] + 0x5d0;
        goto Label_17770c;

    Label_caseD_b:
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[17].UD[0];
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1774d8;
        if (recompiled_functions.count(0x1745d0)) {
            recompiled_functions[0x1745d0](ctx, 0x1745d0);
        } else {
            ctx.cpuRegs.pc = 0x1745d0;
            return;
        }
        ctx.cpuRegs.GPR.r[5].UD[0] = ctx.cpuRegs.GPR.r[21].UD[0];
        ctx.cpuRegs.GPR.r[20].UL[0] = ctx.cpuRegs.GPR.r[17].UL[0] + 0x5d0;
        goto Label_17770c;

    Label_caseD_c:
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[17].UD[0];
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1774f0;
        if (recompiled_functions.count(0x174a70)) {
            recompiled_functions[0x174a70](ctx, 0x174a70);
        } else {
            ctx.cpuRegs.pc = 0x174a70;
            return;
        }
        ctx.cpuRegs.GPR.r[5].UD[0] = ctx.cpuRegs.GPR.r[21].UD[0];
        ctx.cpuRegs.GPR.r[20].UL[0] = ctx.cpuRegs.GPR.r[17].UL[0] + 0x5d0;
        goto Label_17770c;

    Label_caseD_d:
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[17].UD[0];
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x177508;
        if (recompiled_functions.count(0x174f38)) {
            recompiled_functions[0x174f38](ctx, 0x174f38);
        } else {
            ctx.cpuRegs.pc = 0x174f38;
            return;
        }
        ctx.cpuRegs.GPR.r[5].UD[0] = ctx.cpuRegs.GPR.r[21].UD[0];
        ctx.cpuRegs.GPR.r[20].UL[0] = ctx.cpuRegs.GPR.r[17].UL[0] + 0x5d0;
        goto Label_17770c;

    Label_caseD_e:
        ctx.cpuRegs.GPR.r[3].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x18);
        ctx.cpuRegs.GPR.r[20].UL[0] = ctx.cpuRegs.GPR.r[17].UL[0] + 0x5d0;
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x10);
        
        if (ctx.cpuRegs.GPR.r[3].UL[0] != ctx.cpuRegs.GPR.r[2].UL[0]) {
            goto Label_177574;
        }
        
        ctx.cpuRegs.GPR.r[19].UL[0] = ctx.cpuRegs.GPR.r[17].UL[0] + 0x4f0;
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x177530;
        if (recompiled_functions.count(0x1f0e68)) {
            recompiled_functions[0x1f0e68](ctx, 0x1f0e68);
        } else {
            ctx.cpuRegs.pc = 0x1f0e68;
            return;
        }
        
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x177538;
        if (recompiled_functions.count(0x1f0e68)) {
            recompiled_functions[0x1f0e68](ctx, 0x1f0e68);
        } else {
            ctx.cpuRegs.pc = 0x1f0e68;
            return;
        }
        
        ctx.cpuRegs.GPR.r[16].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0] + 0x0);
        
        if (ctx.cpuRegs.GPR.r[16].UL[0] == 0) {
            goto Label_17756c;
        }
        
        ctx.cpuRegs.GPR.r[4].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x154);

    Label_177550:
        if (ctx.cpuRegs.GPR.r[4].UL[0] == 0) {
            goto Label_177564;
        }
        ctx.cpuRegs.GPR.r[16].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x148);
        
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x17755c;
        if (recompiled_functions.count(0x1918a8)) {
            recompiled_functions[0x1918a8](ctx, 0x1918a8);
        } else {
            ctx.cpuRegs.pc = 0x1918a8;
            return;
        }
        
        ctx.cpuRegs.GPR.r[16].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x148);

    Label_177564:
        if (ctx.cpuRegs.GPR.r[16].UL[0] != 0) {
            ctx.cpuRegs.GPR.r[4].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x154);
            goto Label_177550;
        }

    Label_17756c:
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x177570;
        if (recompiled_functions.count(0x181400)) {
            recompiled_functions[0x181400](ctx, 0x181400);
        } else {
            ctx.cpuRegs.pc = 0x181400;
            return;
        }

    Label_177574:
        ctx.cpuRegs.GPR.r[6].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x54);
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[17].UD[0];
        ctx.cpuRegs.GPR.r[7].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x58);
        ctx.cpuRegs.GPR.r[5].UD[0] = ctx.cpuRegs.GPR.r[21].UD[0];
        ctx.cpuRegs.GPR.r[8].UL[0] = 1;
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x17758c;
        if (recompiled_functions.count(0x172b60)) {
            recompiled_functions[0x172b60](ctx, 0x172b60);
        } else {
            ctx.cpuRegs.pc = 0x172b60;
            return;
        }
        ctx.cpuRegs.GPR.r[9].UD[0] = ctx.cpuRegs.GPR.r[29].UD[0];
        
        if (ctx.cpuRegs.GPR.r[2].UL[0] == 0) {
            goto Label_1775b0;
        }
        
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[17].UD[0];
        ctx.cpuRegs.GPR.r[5].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x54);
        ctx.cpuRegs.GPR.r[6].UL[0] = memory::read<uint8_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x0); // local_60
        
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1775a4;
        if (recompiled_functions.count(0x17b980)) {
            recompiled_functions[0x17b980](ctx, 0x17b980);
        } else {
            ctx.cpuRegs.pc = 0x17b980;
            return;
        }
        ctx.cpuRegs.GPR.r[18].UL[0] = 0x18;
        goto Label_177714;

    Label_1775b0:
        goto Label_177710;

    Label_caseD_f:
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[17].UD[0];
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1775c0;
        if (recompiled_functions.count(0x174c70)) {
            recompiled_functions[0x174c70](ctx, 0x174c70);
        } else {
            ctx.cpuRegs.pc = 0x174c70;
            return;
        }
        ctx.cpuRegs.GPR.r[5].UD[0] = ctx.cpuRegs.GPR.r[21].UD[0];
        ctx.cpuRegs.GPR.r[20].UL[0] = ctx.cpuRegs.GPR.r[17].UL[0] + 0x5d0;
        goto Label_17770c;

    Label_caseD_10:
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[17].UD[0];
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1775d8;
        if (recompiled_functions.count(0x174d90)) {
            recompiled_functions[0x174d90](ctx, 0x174d90);
        } else {
            ctx.cpuRegs.pc = 0x174d90;
            return;
        }
        ctx.cpuRegs.GPR.r[5].UD[0] = ctx.cpuRegs.GPR.r[21].UD[0];
        ctx.cpuRegs.GPR.r[20].UL[0] = ctx.cpuRegs.GPR.r[17].UL[0] + 0x5d0;
        goto Label_17770c;

    Label_caseD_11:
        ctx.fpuRegs.fpr[1].UL = 0x3e800000;
        ctx.fpuRegs.fpr[0].UL = memory::read<uint32_t>(0x3097dc);
        ctx.cpuRegs.GPR.r[3].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x1c);
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[21].UL[0] + 0x4);
        
        ctx.fpuRegs.fpr[0].f = ctx.fpuRegs.fpr[0].f * ctx.fpuRegs.fpr[1].f;
        ctx.cpuRegs.GPR.r[2].UL[0] = ctx.cpuRegs.GPR.r[2].UL[0] - ctx.cpuRegs.GPR.r[3].UL[0];
        ctx.fpuRegs.fpr[1].f = ctx.fpuRegs.fpr[0].f; // cvt.w.s
        ctx.fpuRegs.fpr[1].SL = (int32_t)ctx.fpuRegs.fpr[1].f;
        ctx.cpuRegs.GPR.r[4].SL[0] = ctx.fpuRegs.fpr[1].SL;
        
        ctx.cpuRegs.GPR.r[2].UL[0] = (ctx.cpuRegs.GPR.r[2].UL[0] < ctx.cpuRegs.GPR.r[4].UL[0]) ? 1 : 0;
        ctx.cpuRegs.GPR.r[2].UL[0] = ctx.cpuRegs.GPR.r[2].UL[0] ^ 1;
        
        if (ctx.cpuRegs.GPR.r[2].UL[0] == 0) {
            goto Label_caseD_17;
        }
        
        ctx.cpuRegs.GPR.r[20].UL[0] = ctx.cpuRegs.GPR.r[17].UL[0] + 0x5d0;
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x4);
        ctx.cpuRegs.GPR.r[19].UL[0] = ctx.cpuRegs.GPR.r[17].UL[0] + 0x4f0;
        
        // dsll32 0, dsra32 0 -> effectively 32-bit sign extension
        // Assuming v0 was 32-bit, this is fine.
        goto Label_177710;

    Label_caseD_12:
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[17].UD[0];
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x177640;
        if (recompiled_functions.count(0x175008)) {
            recompiled_functions[0x175008](ctx, 0x175008);
        } else {
            ctx.cpuRegs.pc = 0x175008;
            return;
        }
        ctx.cpuRegs.GPR.r[5].UD[0] = ctx.cpuRegs.GPR.r[21].UD[0];
        ctx.cpuRegs.GPR.r[20].UL[0] = ctx.cpuRegs.GPR.r[17].UL[0] + 0x5d0;
        goto Label_17770c;

    Label_caseD_13:
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[17].UD[0];
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x177658;
        if (recompiled_functions.count(0x175160)) {
            recompiled_functions[0x175160](ctx, 0x175160);
        } else {
            ctx.cpuRegs.pc = 0x175160;
            return;
        }
        ctx.cpuRegs.GPR.r[5].UD[0] = ctx.cpuRegs.GPR.r[21].UD[0];
        ctx.cpuRegs.GPR.r[20].UL[0] = ctx.cpuRegs.GPR.r[17].UL[0] + 0x5d0;
        goto Label_17770c;

    Label_caseD_14:
        ctx.fpuRegs.fpr[1].UL = 0x3e800000;
        ctx.cpuRegs.GPR.r[2].UL[0] = ctx.cpuRegs.GPR.r[17].UL[0] + 0x5d0;
        ctx.fpuRegs.fpr[0].UL = memory::read<uint32_t>(0x3097dc);
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0];
        ctx.cpuRegs.GPR.r[20].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0];
        ctx.cpuRegs.GPR.r[5].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x728);
        ctx.fpuRegs.fpr[0].f = ctx.fpuRegs.fpr[0].f * ctx.fpuRegs.fpr[1].f;
        ctx.cpuRegs.GPR.r[7].UL[0] = 0;
        ctx.cpuRegs.GPR.r[18].UL[0] = 6;
        ctx.fpuRegs.fpr[1].f = ctx.fpuRegs.fpr[0].f; // cvt.w.s
        ctx.fpuRegs.fpr[1].SL = (int32_t)ctx.fpuRegs.fpr[1].f;
        ctx.cpuRegs.GPR.r[6].SL[0] = ctx.fpuRegs.fpr[1].SL;
        
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1776a0;
        if (recompiled_functions.count(0x259bc0)) {
            recompiled_functions[0x259bc0](ctx, 0x259bc0);
        } else {
            ctx.cpuRegs.pc = 0x259bc0;
            return;
        }
        ctx.cpuRegs.GPR.r[19].UL[0] = ctx.cpuRegs.GPR.r[17].UL[0] + 0x4f0;
        ctx.cpuRegs.GPR.r[4].UL[0] = ctx.cpuRegs.GPR.r[29].UL[0] + 0x10;
        
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1776ac;
        if (recompiled_functions.count(0x11bf10)) {
            recompiled_functions[0x11bf10](ctx, 0x11bf10);
        } else {
            ctx.cpuRegs.pc = 0x11bf10;
            return;
        }
        ctx.cpuRegs.GPR.r[5].UL[0] = 0;
        ctx.cpuRegs.GPR.r[16].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x10); // local_50
        ctx.cpuRegs.GPR.r[4].UL[0] = ctx.cpuRegs.GPR.r[29].UL[0] + 0x14;
        
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1776bc;
        if (recompiled_functions.count(0x11bf10)) {
            recompiled_functions[0x11bf10](ctx, 0x11bf10);
        } else {
            ctx.cpuRegs.pc = 0x11bf10;
            return;
        }
        ctx.cpuRegs.GPR.r[5].UL[0] = 8;
        ctx.cpuRegs.GPR.r[3].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x14); // local_4c
        ctx.cpuRegs.GPR.r[2].UL[0] = ctx.cpuRegs.GPR.r[17].UL[0] + 0x19c0;
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0] + 0x8, ctx.cpuRegs.GPR.r[16].UL[0]);
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0] + 0xc, ctx.cpuRegs.GPR.r[3].UL[0]); // Delay slot
        goto Label_177710;

    Label_caseD_15:
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[17].UD[0];
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1776dc;
        if (recompiled_functions.count(0x1754b8)) {
            recompiled_functions[0x1754b8](ctx, 0x1754b8);
        } else {
            ctx.cpuRegs.pc = 0x1754b8;
            return;
        }
        ctx.cpuRegs.GPR.r[5].UD[0] = ctx.cpuRegs.GPR.r[21].UD[0];
        ctx.cpuRegs.GPR.r[20].UL[0] = ctx.cpuRegs.GPR.r[17].UL[0] + 0x5d0;
        goto Label_17770c;

    Label_caseD_16:
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[17].UD[0];
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1776f4;
        if (recompiled_functions.count(0x175618)) {
            recompiled_functions[0x175618](ctx, 0x175618);
        } else {
            ctx.cpuRegs.pc = 0x175618;
            return;
        }
        ctx.cpuRegs.GPR.r[5].UD[0] = ctx.cpuRegs.GPR.r[21].UD[0];
        ctx.cpuRegs.GPR.r[20].UL[0] = ctx.cpuRegs.GPR.r[17].UL[0] + 0x5d0;
        goto Label_17770c;

    Label_caseD_17:
        ctx.cpuRegs.GPR.r[18].UL[0] = 0x18;

    Label_177708:
        ctx.cpuRegs.GPR.r[20].UL[0] = ctx.cpuRegs.GPR.r[17].UL[0] + 0x5d0;

    Label_17770c:
        ctx.cpuRegs.GPR.r[19].UL[0] = ctx.cpuRegs.GPR.r[17].UL[0] + 0x4f0;

    Label_177710:
        ctx.cpuRegs.GPR.r[2].UL[0] = 0x18;

    Label_177714:
        if (ctx.cpuRegs.GPR.r[18].UL[0] == ctx.cpuRegs.GPR.r[2].UL[0]) {
            goto Label_17774c;
        }
        
        ctx.cpuRegs.GPR.r[3].UL[0] = ctx.cpuRegs.GPR.r[18].UL[0] & 0x3f;
        ctx.cpuRegs.GPR.r[2].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0);
        
        {
            uint64_t mask = 0xff03ULL << 48; // chained shifts
            mask |= 0xffffULL; mask <<= 16;
            mask |= 0xffffULL; mask <<= 16;
            mask |= 0xffffULL;
            
            uint64_t v1_shift = (uint64_t)ctx.cpuRegs.GPR.r[3].UL[0] << 50; // dsll32 18
            
            ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] & mask;
            ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] | v1_shift;
            
            memory::write<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0, ctx.cpuRegs.GPR.r[2].UD[0]);
        }
    
    )code" << std::endl;
    file << R"code(
    Label_17774c:
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[19].UL[0] + 0x4);
        ctx.cpuRegs.GPR.r[3].UL[0] = 6;
        ctx.cpuRegs.GPR.r[2].UL[0] = ctx.cpuRegs.GPR.r[2].UL[0] >> 8;
        ctx.cpuRegs.GPR.r[2].UL[0] = ctx.cpuRegs.GPR.r[2].UL[0] & 0xf;
        
        if (ctx.cpuRegs.GPR.r[2].UL[0] == ctx.cpuRegs.GPR.r[3].UL[0]) {
            goto Label_177780;
        }
        
        ctx.cpuRegs.GPR.r[2].UL[0] = ctx.cpuRegs.GPR.r[2].UL[0] << 2;
        ctx.cpuRegs.GPR.r[2].UL[0] = ctx.cpuRegs.GPR.r[2].UL[0] + ctx.cpuRegs.GPR.r[19].UL[0];
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0] + 0x80);
        
        if (ctx.cpuRegs.GPR.r[2].UL[0] == 0) {
            goto Label_177778;
        }
        ctx.cpuRegs.GPR.r[4].UL[0] = 0;
        ctx.cpuRegs.GPR.r[4].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0] + 0x0);

    Label_177778:
        goto Label_177784;
        ctx.cpuRegs.GPR.r[16].UD[0] = ctx.cpuRegs.GPR.r[4].UD[0];

    Label_177780:
        ctx.cpuRegs.GPR.r[16].UL[0] = 0;

    Label_177784:
        if (ctx.cpuRegs.GPR.r[16].UL[0] == 0) {
            goto Label_17779c;
        }
        ctx.cpuRegs.GPR.r[4].UL[0] = ctx.cpuRegs.GPR.r[16].UL[0] + 0xd4;
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x177790;
        if (recompiled_functions.count(0x192118)) {
            recompiled_functions[0x192118](ctx, 0x192118);
        } else {
            ctx.cpuRegs.pc = 0x192118;
            return;
        }
        ctx.cpuRegs.GPR.r[5].UL[0] = 0xc;
        goto Label_1777a4;
        ctx.cpuRegs.GPR.r[18].UL[0] = 0;

    Label_17779c:
        ctx.cpuRegs.GPR.r[2].UL[0] = 0;
        ctx.cpuRegs.GPR.r[18].UL[0] = 0;

    Label_1777a4:
        if (ctx.cpuRegs.GPR.r[2].UL[0] != 0) {
            ctx.cpuRegs.GPR.r[18].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0] + 0x18);
        }

    Label_1777ac:
        ctx.cpuRegs.GPR.r[2].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0);
        {
            uint64_t v1 = 0xfc00ULL << 34; // dsll32 2
            uint64_t a0 = 0xc000ULL << 32; // dsll32 0
            ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] & v1;
            ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] ^ a0;
            ctx.cpuRegs.GPR.r[2].UL[0] = (ctx.cpuRegs.GPR.r[2].UL[0] < 1) ? 1 : 0;
            
            if (ctx.cpuRegs.GPR.r[2].UL[0] == 0) {
                goto Label_177850;
            }
        }
        
        ctx.cpuRegs.GPR.r[6].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x34);
        ctx.cpuRegs.GPR.r[3].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[19].UL[0] + 0x4);
        ctx.cpuRegs.GPR.r[5].UL[0] = ctx.cpuRegs.GPR.r[3].UL[0] & 0xf;
        
        if ((ctx.cpuRegs.GPR.r[5].UL[0] ^ 2) == 0) {
            goto Label_1777f8;
        }
        
        ctx.cpuRegs.GPR.r[4].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[28].UL[0] - 0x7dc4);
        ctx.cpuRegs.GPR.r[3].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[4].UL[0] + 0x1c);
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x56c);
        ctx.cpuRegs.GPR.r[2].UL[0] = ctx.cpuRegs.GPR.r[2].UL[0] - ctx.cpuRegs.GPR.r[3].UL[0];
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x56c, ctx.cpuRegs.GPR.r[2].UL[0]);

            )code" << std::endl;
    file << R"code(
    Label_1777f8:
        if (ctx.cpuRegs.GPR.r[5].UL[0] != 0) {
            goto Label_177850;
        }
        
        ctx.cpuRegs.GPR.r[6].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x34);
        ctx.cpuRegs.GPR.r[4].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x34);
        ctx.cpuRegs.GPR.r[5].UL[0] = 4;
        
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x17780c;
        if (recompiled_functions.count(0x2b3548)) {
            recompiled_functions[0x2b3548](ctx, 0x2b3548);
        } else {
            ctx.cpuRegs.pc = 0x2b3548;
            return;
        }
        ctx.cpuRegs.GPR.r[6].UL[0] = 0;
        
        if (ctx.cpuRegs.GPR.r[2].UL[0] == 0) {
            goto Label_17784c;
        }
        
        ctx.fpuRegs.fpr[0].UL = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[28].UL[0] - 0x7d14);
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[20].UD[0];
        ctx.fpuRegs.fpr[1].UL = 0x3e800000;
        ctx.fpuRegs.fpr[2].UL = 0x40400000;
        
        ctx.fpuRegs.fpr[1].f = ctx.fpuRegs.fpr[0].f * ctx.fpuRegs.fpr[1].f;
        ctx.fpuRegs.fpr[0].f = ctx.fpuRegs.fpr[0].f * ctx.fpuRegs.fpr[2].f;
        
        ctx.fpuRegs.fpr[2].f = ctx.fpuRegs.fpr[1].f; // cvt.w.s
        ctx.fpuRegs.fpr[2].SL = (int32_t)ctx.fpuRegs.fpr[2].f;
        ctx.cpuRegs.GPR.r[6].SL[0] = ctx.fpuRegs.fpr[2].SL;
        
        ctx.fpuRegs.fpr[1].f = ctx.fpuRegs.fpr[0].f; // cvt.w.s
        ctx.fpuRegs.fpr[1].SL = (int32_t)ctx.fpuRegs.fpr[1].f;
        ctx.cpuRegs.GPR.r[7].SL[0] = ctx.fpuRegs.fpr[1].SL;
        
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x177848;
        if (recompiled_functions.count(0x259b20)) {
            recompiled_functions[0x259b20](ctx, 0x259b20);
        } else {
            ctx.cpuRegs.pc = 0x259b20;
            return;
        }
        ctx.cpuRegs.GPR.r[5].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x6d0);

    Label_17784c:
        ctx.cpuRegs.GPR.r[6].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x34);

    Label_177850:
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[20].UD[0];
        ctx.cpuRegs.GPR.r[7].UD[0] = ctx.cpuRegs.GPR.r[18].UD[0];
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x17785c;
        if (recompiled_functions.count(0x1714a8)) {
            recompiled_functions[0x1714a8](ctx, 0x1714a8);
        } else {
            ctx.cpuRegs.pc = 0x1714a8;
            return;
        }
        ctx.cpuRegs.GPR.r[5].UD[0] = ctx.cpuRegs.GPR.r[21].UD[0];
        
        ctx.cpuRegs.GPR.r[4].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0);
        {
            uint64_t v1 = 0xfc00ULL << 34; // dsll32 2
            uint64_t a0 = 0xc000ULL << 32; // dsll32 0
            ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[4].UD[0] & v1;
            ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[4].UD[0] ^ a0;
            
            if (ctx.cpuRegs.GPR.r[2].UL[0] < 1) {
                goto Label_177898;
            }
            
            uint64_t d000 = 0xd000ULL << 32;
            if (ctx.cpuRegs.GPR.r[4].UD[0] != d000) {
                goto Label_1778dc;
            }
            ctx.cpuRegs.GPR.r[2].UL[0] = 0;
        }
    )code" << std::endl;
    file << R"code(
    Label_177898:
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x17789c;
        if (recompiled_functions.count(0x11e4b8)) {
            recompiled_functions[0x11e4b8](ctx, 0x11e4b8);
        } else {
            ctx.cpuRegs.pc = 0x11e4b8;
            return;
        }
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[21].UD[0];
        
        if (ctx.cpuRegs.GPR.r[16].UL[0] == 0) {
            goto Label_1778c8;
        }
        
        ctx.cpuRegs.GPR.r[4].UL[0] = ctx.cpuRegs.GPR.r[16].UL[0] + 0xd4;
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1778ac;
        if (recompiled_functions.count(0x192118)) {
            recompiled_functions[0x192118](ctx, 0x192118);
        } else {
            ctx.cpuRegs.pc = 0x192118;
            return;
        }
        ctx.cpuRegs.GPR.r[5].UL[0] = 0x16;
        
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x20);
        
        if (ctx.cpuRegs.GPR.r[2].UL[0] == 0) {
            goto Label_1778c0;
        }
        ctx.cpuRegs.GPR.r[4].UL[0] = 0;
        ctx.cpuRegs.GPR.r[4].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0] + 0x0);

    Label_1778c0:
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1778c4;
        if (recompiled_functions.count(0x1e5818)) {
            recompiled_functions[0x1e5818](ctx, 0x1e5818);
        } else {
            ctx.cpuRegs.pc = 0x1e5818;
            return;
        }

    Label_1778c8:
        if (ctx.cpuRegs.GPR.r[22].UL[0] != 0) {
            goto Label_1778dc;
        }
        ctx.cpuRegs.GPR.r[2].UL[0] = 1;
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1778d4;
        if (recompiled_functions.count(0x15bcc0)) {
            recompiled_functions[0x15bcc0](ctx, 0x15bcc0);
        } else {
            ctx.cpuRegs.pc = 0x15bcc0;
            return;
        }
        ctx.cpuRegs.GPR.r[2].UL[0] = 1;

    Label_1778dc:
        ctx.cpuRegs.GPR.r[16].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x20);
        ctx.cpuRegs.GPR.r[17].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x28);
        ctx.cpuRegs.GPR.r[18].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x30);
        ctx.cpuRegs.GPR.r[19].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x38);
        ctx.cpuRegs.GPR.r[20].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x40);
        ctx.cpuRegs.GPR.r[21].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x48);
        ctx.cpuRegs.GPR.r[22].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x50);
        ctx.cpuRegs.GPR.r[31].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x58);
        ctx.cpuRegs.GPR.r[29].SL[0] = ctx.cpuRegs.GPR.r[29].SL[0] + 0x60;
        return;
    }
    )code" << std::endl;
        return;
    }
    if (func.base_address == 0x00173238) {
        file << R"code(void FUN_00173238(CpuContext& ctx) {
        // Prologue
        ctx.cpuRegs.GPR.r[29].SL[0] = ctx.cpuRegs.GPR.r[29].SL[0] - 0x40;
        ctx.cpuRegs.GPR.r[3].UL[0] = 1;
        memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x08, ctx.cpuRegs.GPR.r[17].UD[0]);
        memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x18, ctx.cpuRegs.GPR.r[19].UD[0]);
        ctx.cpuRegs.GPR.r[17].UD[0] = ctx.cpuRegs.GPR.r[4].UD[0];  // s1 = param_1
        memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x00, ctx.cpuRegs.GPR.r[16].UD[0]);
        memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x10, ctx.cpuRegs.GPR.r[18].UD[0]);
        memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x20, ctx.cpuRegs.GPR.r[31].UD[0]);
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x30, ctx.fpuRegs.fpr[20].UL);
        
        // Load param_1[0] and extract bits [59:56]
        ctx.cpuRegs.GPR.r[2].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0);
        ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] >> 59;  // dsrl32 by 27
        ctx.cpuRegs.GPR.r[4].UL[0] = ctx.cpuRegs.GPR.r[2].UL[0] & 0xF;
        
        // s3 = param_2
        ctx.cpuRegs.GPR.r[19].UD[0] = ctx.cpuRegs.GPR.r[5].UD[0];
        
        // if (extracted_bits != 1) goto LAB_001732f8
        if (ctx.cpuRegs.GPR.r[4].UL[0] != ctx.cpuRegs.GPR.r[3].UL[0]) {
            goto Label_1732f8;
        }
        
        // Special case when bits == 1
        ctx.cpuRegs.GPR.r[5].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x20);
        ctx.fpuRegs.fpr[0].UL = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[28].UL[0] - 0x7d14);
        
        if (ctx.cpuRegs.GPR.r[5].UL[0] != 0) {
            goto Label_173288;
        }
        
        // param_1[0x20] = param_2[0x4]
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[19].UL[0] + 0x4);
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x20, ctx.cpuRegs.GPR.r[2].UL[0]);
        goto Label_caseD_0;

    Label_173288:
        {
            uint32_t imm_3e80 = 0x3e800000;
            ctx.fpuRegs.fpr[1].UL = imm_3e80;
        }
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[19].UL[0] + 0x4);
        ctx.fpuRegs.fpr[0].f = ctx.fpuRegs.fpr[0].f * ctx.fpuRegs.fpr[1].f;
        ctx.cpuRegs.GPR.r[2].UL[0] = ctx.cpuRegs.GPR.r[2].UL[0] - ctx.cpuRegs.GPR.r[5].UL[0];
        ctx.fpuRegs.fpr[1].f = truncf(ctx.fpuRegs.fpr[0].f);
        ctx.fpuRegs.fpr[1].SL = (int32_t)ctx.fpuRegs.fpr[1].f;
        ctx.cpuRegs.GPR.r[3].SL[0] = ctx.fpuRegs.fpr[1].SL;
        ctx.cpuRegs.GPR.r[2].UL[0] = (ctx.cpuRegs.GPR.r[2].UL[0] < (uint32_t)ctx.cpuRegs.GPR.r[3].UL[0]) ? 1 : 0;
        ctx.cpuRegs.GPR.r[2].UL[0] = ctx.cpuRegs.GPR.r[2].UL[0] ^ 1;
        
        ctx.cpuRegs.GPR.r[4].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[28].UL[0] - 0x7dcc);
        
        if (ctx.cpuRegs.GPR.r[2].UL[0] == 0) {
            goto Label_caseD_0;
        }
        
        // Call FUN_002a11d0(gp[-0x7dcc], 2, 0)
        ctx.cpuRegs.GPR.r[5].UL[0] = 2;
        ctx.cpuRegs.GPR.r[6].UD[0] = 0;
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1732c0;
        if (recompiled_functions.count(0x2a11d0)) {
            recompiled_functions[0x2a11d0](ctx, 0x2a11d0);
        } else {
            ctx.cpuRegs.pc = 0x2a11d0;
            return;
        }
        
        // Build mask 0x87ff_ffff_ffff_ffff
        ctx.cpuRegs.GPR.r[2].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0);
        {
            uint64_t mask = 0x87ffffffffffffffULL;
            uint64_t set_bit = 0x8000ULL << 45;  // 0x8000 dsll32 13
            ctx.cpuRegs.GPR.r[2].UD[0] = (ctx.cpuRegs.GPR.r[2].UD[0] & mask) | set_bit;
        }
        memory::write<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0, ctx.cpuRegs.GPR.r[2].UD[0]);
        goto Label_caseD_0;

        )code" << std::endl;


        file << R"code(



    Label_1732f8:
        // s2 = DAT_00309724
        ctx.cpuRegs.GPR.r[18].UL[0] = memory::read<uint32_t>(0x309724);
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[18].UL[0] + 0x0);
        ctx.cpuRegs.GPR.r[2].UL[0] = ctx.cpuRegs.GPR.r[2].UL[0] & 0xF;
        
        if (ctx.cpuRegs.GPR.r[2].UL[0] != 0) {
            goto Label_caseD_0;
        }
        
        // Bounds check for switch
        ctx.cpuRegs.GPR.r[2].UL[0] = (ctx.cpuRegs.GPR.r[4].UL[0] < 7) ? 1 : 0;
        if (ctx.cpuRegs.GPR.r[2].UL[0] == 0) {
            goto Label_caseD_1;
        }
        
        // Switch dispatch
        switch (ctx.cpuRegs.GPR.r[4].UL[0]) {
            case 0: goto Label_caseD_0;
            case 1: goto Label_caseD_1;
            case 2: goto Label_caseD_2;
            case 3: goto Label_caseD_3;
            case 4: goto Label_caseD_4;
            case 5: goto Label_caseD_5;
            case 6: goto Label_caseD_6;
            default: goto Label_caseD_1;
        }

    Label_caseD_0:  // 0x173310
        ctx.cpuRegs.GPR.r[2].UL[0] = 0;
        goto Label_epilogue;

    Label_caseD_1:  // 0x173a88
        ctx.cpuRegs.GPR.r[2].UL[0] = 1;
        goto Label_epilogue;

    Label_caseD_2:  // 0x173338
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[18].UL[0] + 0x4);
        ctx.cpuRegs.GPR.r[2].UL[0] = (ctx.cpuRegs.GPR.r[2].UL[0] >> 4) & 1;
        
        if (ctx.cpuRegs.GPR.r[2].UL[0] != 0) {
            ctx.cpuRegs.GPR.r[3].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0);
            goto Label_1734bc;
        }
        
        {
            ctx.cpuRegs.GPR.r[2].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0);
            uint64_t set_bit = 0x8000ULL << 41;   // 0x8000 dsll32 9
            uint64_t mask_fdff = 0xfdffffffffffffffULL;
            uint64_t mask_fbff = 0xfbffffffffffffffULL;
            
            ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] | set_bit;
            ctx.cpuRegs.GPR.r[6].UL[0] = memory::read<uint32_t>(0x309724);
            ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] & mask_fdff;
            ctx.cpuRegs.GPR.r[3].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] & mask_fbff;
        }
        goto Label_1733a4;

    Label_1733a4:
        memory::write<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0, ctx.cpuRegs.GPR.r[3].UD[0]);
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[6].UL[0] + 0x0);
        ctx.cpuRegs.GPR.r[2].UL[0] = (ctx.cpuRegs.GPR.r[2].UL[0] >> 29) & 1;
        
        if (ctx.cpuRegs.GPR.r[2].UL[0] == 0) {
            goto Label_173554;
        }
        
        {
            uint64_t mask_87ff = 0x87ffffffffffffffULL;
            ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[3].UD[0] & mask_87ff;
            memory::write<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0, ctx.cpuRegs.GPR.r[2].UD[0]);
            ctx.cpuRegs.GPR.r[3].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[6].UL[0] + 0x0);
            ctx.cpuRegs.GPR.r[3].UL[0] = ctx.cpuRegs.GPR.r[3].UL[0] & 0xDFFFFFFF;
            memory::write<uint32_t>(ctx.cpuRegs.GPR.r[6].UL[0] + 0x0, ctx.cpuRegs.GPR.r[3].UL[0]);
        }
        goto Label_caseD_1;

    Label_173554:
        {
            uint64_t mask_87ff = 0x87ffffffffffffffULL;
            ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[3].UD[0] & mask_87ff;
            memory::write<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0, ctx.cpuRegs.GPR.r[2].UD[0]);
        }
        goto Label_caseD_1;

    Label_1734bc:
        {
            uint64_t mask_87ff = 0x87ffffffffffffffULL;
            uint64_t set_bit = 0x8000ULL << 44;  // 0x8000 dsll32 12
            ctx.cpuRegs.GPR.r[2].UL[0] = 0;
            ctx.cpuRegs.GPR.r[3].UD[0] = (ctx.cpuRegs.GPR.r[3].UD[0] & mask_87ff) | set_bit;
            memory::write<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0, ctx.cpuRegs.GPR.r[3].UD[0]);
            ctx.cpuRegs.GPR.r[4].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[19].UL[0] + 0x4);
            memory::write<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x20, ctx.cpuRegs.GPR.r[4].UL[0]);
        }
        goto Label_epilogue;

    Label_caseD_3:  // 0x1733f8
        ctx.fpuRegs.fpr[0].UL = memory::read<uint32_t>(0x3097dc);
        ctx.cpuRegs.GPR.r[16].UL[0] = ctx.cpuRegs.GPR.r[17].UL[0] + 0x5d0;  // s0 = s1 + 0x5d0
        ctx.fpuRegs.fpr[20].UL = 0x3e800000;  // f20 = 0.25f
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[16].UD[0];
        ctx.cpuRegs.GPR.r[5].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x6c8);
        ctx.fpuRegs.fpr[0].f = ctx.fpuRegs.fpr[0].f * ctx.fpuRegs.fpr[20].f;
        ctx.fpuRegs.fpr[1].f = truncf(ctx.fpuRegs.fpr[0].f);
        ctx.fpuRegs.fpr[1].SL = (int32_t)ctx.fpuRegs.fpr[1].f;
        ctx.cpuRegs.GPR.r[6].SL[0] = ctx.fpuRegs.fpr[1].SL;
        ctx.cpuRegs.GPR.r[7].UD[0] = 0;
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x173428;
        if (recompiled_functions.count(0x259bc0)) {
            recompiled_functions[0x259bc0](ctx, 0x259bc0);
        } else {
            ctx.cpuRegs.pc = 0x259bc0;
            return;
        }
        
        ctx.cpuRegs.GPR.r[3].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x320);
        ctx.cpuRegs.GPR.r[2].UL[0] = 1;
        
        if (ctx.cpuRegs.GPR.r[3].UL[0] != ctx.cpuRegs.GPR.r[2].UL[0]) {
            ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[18].UL[0] + 0x4);
            goto Label_1734a8;
        }
        
        // Additional calls when s0[0x320] == 1
        ctx.fpuRegs.fpr[0].UL = memory::read<uint32_t>(0x3097dc);
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[16].UD[0];
        ctx.cpuRegs.GPR.r[5].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x5f0);
        ctx.fpuRegs.fpr[0].f = ctx.fpuRegs.fpr[0].f * ctx.fpuRegs.fpr[20].f;
        ctx.fpuRegs.fpr[1].f = truncf(ctx.fpuRegs.fpr[0].f);
        ctx.fpuRegs.fpr[1].SL = (int32_t)ctx.fpuRegs.fpr[1].f;
        ctx.cpuRegs.GPR.r[6].SL[0] = ctx.fpuRegs.fpr[1].SL;
        ctx.cpuRegs.GPR.r[7].UD[0] = 0;
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x17345c;
        if (recompiled_functions.count(0x259bc0)) {
            recompiled_functions[0x259bc0](ctx, 0x259bc0);
        } else {
            ctx.cpuRegs.pc = 0x259bc0;
            return;
        }
        
        // Call FUN_001f0e68 twice
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x173464;
        if (recompiled_functions.count(0x1f0e68)) {
            recompiled_functions[0x1f0e68](ctx, 0x1f0e68);
        } else {
            ctx.cpuRegs.pc = 0x1f0e68;
            return;
        }
        
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x17346c;
        if (recompiled_functions.count(0x1f0e68)) {
            recompiled_functions[0x1f0e68](ctx, 0x1f0e68);
        } else {
            ctx.cpuRegs.pc = 0x1f0e68;
            return;
        }
        
        ctx.cpuRegs.GPR.r[16].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0] + 0x0);
        
        if (ctx.cpuRegs.GPR.r[16].UL[0] == 0) {
            goto Label_17349c;
        }
        
        ctx.cpuRegs.GPR.r[4].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x154);


        )code" << std::endl;

        file << R"code(

    Label_173480:
        if (ctx.cpuRegs.GPR.r[4].UL[0] == 0) {
            ctx.cpuRegs.GPR.r[16].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x148);
            goto Label_173494;
        }
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x173490;
        if (recompiled_functions.count(0x1917e0)) {
            recompiled_functions[0x1917e0](ctx, 0x1917e0);
        } else {
            ctx.cpuRegs.pc = 0x1917e0;
            return;
        }
        ctx.cpuRegs.GPR.r[16].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x148);

    Label_173494:
        if (ctx.cpuRegs.GPR.r[16].UL[0] != 0) {
            ctx.cpuRegs.GPR.r[4].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x154);
            goto Label_173480;
        }

    Label_17349c:
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1734a4;
        if (recompiled_functions.count(0x181438)) {
            recompiled_functions[0x181438](ctx, 0x181438);
        } else {
            ctx.cpuRegs.pc = 0x181438;
            return;
        }
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[18].UL[0] + 0x4);

    Label_1734a8:
        ctx.cpuRegs.GPR.r[2].UL[0] = (ctx.cpuRegs.GPR.r[2].UL[0] >> 4) & 1;
        
        if (ctx.cpuRegs.GPR.r[2].UL[0] != 0) {
            ctx.cpuRegs.GPR.r[3].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0);
            goto Label_1734bc;
        }
        
        ctx.cpuRegs.GPR.r[2].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0);
        {
            uint64_t mask_feff = 0xfeffffffffffffffULL;
            uint64_t mask_fdff = 0xfdffffffffffffffULL;
            uint64_t set_bit = 0x8000ULL << 43;  // 0x8000 dsll32 11
            ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] & mask_feff;
            ctx.cpuRegs.GPR.r[6].UL[0] = memory::read<uint32_t>(0x309724);
            ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] & mask_fdff;
            ctx.cpuRegs.GPR.r[3].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] | set_bit;
        }
        goto Label_1733a4;

    Label_caseD_4:  // 0x17357c
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[18].UL[0] + 0x4);
        ctx.cpuRegs.GPR.r[2].UL[0] = (ctx.cpuRegs.GPR.r[2].UL[0] >> 4) & 1;
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[17].UD[0];
        
        if (ctx.cpuRegs.GPR.r[2].UL[0] == 0) {
            goto Label_1735f4;
        }
        
        {
            ctx.cpuRegs.GPR.r[2].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0);
            uint64_t mask_feff = 0xfeffffffffffffffULL;
            uint64_t set_bit = 0x8000ULL << 42;  // 0x8000 dsll32 10
            uint64_t mask_fbff = 0xfbffffffffffffffULL;
            ctx.cpuRegs.GPR.r[2].UD[0] = (ctx.cpuRegs.GPR.r[2].UD[0] & mask_feff) | set_bit;
            ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] & mask_fbff;
            memory::write<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0, ctx.cpuRegs.GPR.r[2].UD[0]);
        }
        ctx.cpuRegs.GPR.r[5].UL[0] = 1;
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1735ec;
        if (recompiled_functions.count(0x176090)) {
            recompiled_functions[0x176090](ctx, 0x176090);
        } else {
            ctx.cpuRegs.pc = 0x176090;
            return;
        }
        ctx.cpuRegs.GPR.r[2].UL[0] = 0;
        goto Label_epilogue;

    Label_1735f4:
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[18].UL[0] + 0x0);
        ctx.cpuRegs.GPR.r[2].UL[0] = (ctx.cpuRegs.GPR.r[2].UL[0] >> 29) & 1;
        ctx.cpuRegs.GPR.r[6].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[28].UL[0] - 0x7dcc);
        
        if (ctx.cpuRegs.GPR.r[2].UL[0] == 0) {
            goto Label_1736d0;
        }
        
        {
            ctx.cpuRegs.GPR.r[2].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0);
            uint64_t set_bit = 0x8000ULL << 41;   // 0x8000 dsll32 9
            uint64_t mask_fdff = 0xfdffffffffffffffULL;
            uint64_t mask_fbff = 0xfbffffffffffffffULL;
            ctx.cpuRegs.GPR.r[2].UD[0] = (ctx.cpuRegs.GPR.r[2].UD[0] | set_bit) & mask_fdff;
            ctx.cpuRegs.GPR.r[3].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] & mask_fbff;
            memory::write<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0, ctx.cpuRegs.GPR.r[3].UD[0]);
        }
        
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[6].UL[0] + 0x0);
        ctx.cpuRegs.GPR.r[2].UL[0] = (ctx.cpuRegs.GPR.r[2].UL[0] >> 29) & 1;
        
        if (ctx.cpuRegs.GPR.r[2].UL[0] == 0) {
            goto Label_1736ac;
        }
        
        {
            uint64_t mask_87ff = 0x87ffffffffffffffULL;
            ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[3].UD[0] & mask_87ff;
            memory::write<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0, ctx.cpuRegs.GPR.r[2].UD[0]);
            ctx.cpuRegs.GPR.r[3].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[6].UL[0] + 0x0);
            ctx.cpuRegs.GPR.r[3].UL[0] = ctx.cpuRegs.GPR.r[3].UL[0] & 0xDFFFFFFF;
            memory::write<uint32_t>(ctx.cpuRegs.GPR.r[6].UL[0] + 0x0, ctx.cpuRegs.GPR.r[3].UL[0]);
        }
        goto Label_1736f8;
        )code" << std::endl;

        file << R"code(

    Label_1736ac:
        {
            uint64_t mask_87ff = 0x87ffffffffffffffULL;
            ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[3].UD[0] & mask_87ff;
        }
        goto Label_1736f4;

    Label_1736d0:
        {
            ctx.cpuRegs.GPR.r[2].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0);
            uint64_t mask_87ff = 0x87ffffffffffffffULL;
            ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] & mask_87ff;
        }

    Label_1736f4:
        memory::write<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0, ctx.cpuRegs.GPR.r[2].UD[0]);

    Label_1736f8:
        ctx.cpuRegs.GPR.r[2].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0);
        {
            uint64_t mask = 0xfc00ULL << 34;  // 0xfc00 dsll32 2
            uint64_t check = 0x8000ULL << 32; // 0x8000 dsll32 0
            ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] & mask;
            ctx.cpuRegs.GPR.r[16].UL[0] = ctx.cpuRegs.GPR.r[17].UL[0] + 0x5d0;
            if (ctx.cpuRegs.GPR.r[2].UD[0] != check) {
                goto Label_173a2c;
            }
        }
        
        // Special case when check passes
        ctx.fpuRegs.fpr[20].UL = 0x3e800000;
        ctx.fpuRegs.fpr[0].UL = memory::read<uint32_t>(0x3097dc);
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[16].UD[0];
        ctx.cpuRegs.GPR.r[5].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x728);
        ctx.fpuRegs.fpr[0].f = ctx.fpuRegs.fpr[0].f * ctx.fpuRegs.fpr[20].f;
        ctx.fpuRegs.fpr[1].f = truncf(ctx.fpuRegs.fpr[0].f);
        ctx.fpuRegs.fpr[1].SL = (int32_t)ctx.fpuRegs.fpr[1].f;
        ctx.cpuRegs.GPR.r[6].SL[0] = ctx.fpuRegs.fpr[1].SL;
        ctx.cpuRegs.GPR.r[7].UD[0] = 0;
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x173744;
        if (recompiled_functions.count(0x259bc0)) {
            recompiled_functions[0x259bc0](ctx, 0x259bc0);
        } else {
            ctx.cpuRegs.pc = 0x259bc0;
            return;
        }
        
        ctx.fpuRegs.fpr[0].UL = memory::read<uint32_t>(0x3097dc);
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[16].UD[0];
        ctx.cpuRegs.GPR.r[5].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x630);
        goto Label_173a6c;

    Label_caseD_5:  // 0x173758
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[18].UL[0] + 0x4);
        ctx.cpuRegs.GPR.r[2].UL[0] = (ctx.cpuRegs.GPR.r[2].UL[0] >> 4) & 1;
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[17].UD[0];
        
        if (ctx.cpuRegs.GPR.r[2].UL[0] == 0) {
            goto Label_17377c;
        }
        
        ctx.cpuRegs.GPR.r[5].UL[0] = 1;
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x173774;
        if (recompiled_functions.count(0x175e20)) {
            recompiled_functions[0x175e20](ctx, 0x175e20);
        } else {
            ctx.cpuRegs.pc = 0x175e20;
            return;
        }
        ctx.cpuRegs.GPR.r[2].UL[0] = 0;
        goto Label_epilogue;

    Label_17377c:
        ctx.fpuRegs.fpr[20].UL = 0x3e800000;
        ctx.cpuRegs.GPR.r[16].UL[0] = ctx.cpuRegs.GPR.r[17].UL[0] + 0x5d0;
        ctx.fpuRegs.fpr[0].UL = memory::read<uint32_t>(0x3097dc);
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[16].UD[0];
        ctx.cpuRegs.GPR.r[5].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x728);
        ctx.fpuRegs.fpr[0].f = ctx.fpuRegs.fpr[0].f * ctx.fpuRegs.fpr[20].f;
        ctx.fpuRegs.fpr[1].f = truncf(ctx.fpuRegs.fpr[0].f);
        ctx.fpuRegs.fpr[1].SL = (int32_t)ctx.fpuRegs.fpr[1].f;
        ctx.cpuRegs.GPR.r[6].SL[0] = ctx.fpuRegs.fpr[1].SL;
        ctx.cpuRegs.GPR.r[7].UD[0] = 0;
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1737ac;
        if (recompiled_functions.count(0x259bc0)) {
            recompiled_functions[0x259bc0](ctx, 0x259bc0);
        } else {
            ctx.cpuRegs.pc = 0x259bc0;
            return;
        }
        
        ctx.fpuRegs.fpr[0].UL = memory::read<uint32_t>(0x3097dc);
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[16].UD[0];
        ctx.cpuRegs.GPR.r[5].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x630);
        ctx.fpuRegs.fpr[0].f = ctx.fpuRegs.fpr[0].f * ctx.fpuRegs.fpr[20].f;
        ctx.fpuRegs.fpr[1].f = truncf(ctx.fpuRegs.fpr[0].f);
        ctx.fpuRegs.fpr[1].SL = (int32_t)ctx.fpuRegs.fpr[1].f;
        ctx.cpuRegs.GPR.r[6].SL[0] = ctx.fpuRegs.fpr[1].SL;
        ctx.cpuRegs.GPR.r[7].UD[0] = 0;
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1737d0;
        if (recompiled_functions.count(0x259b20)) {
            recompiled_functions[0x259b20](ctx, 0x259b20);
        } else {
            ctx.cpuRegs.pc = 0x259b20;
            return;
        }
        
        ctx.cpuRegs.GPR.r[5].UL[0] = memory::read<uint32_t>(0x309724);
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[5].UL[0] + 0x0);
        ctx.cpuRegs.GPR.r[2].UL[0] = (ctx.cpuRegs.GPR.r[2].UL[0] >> 29) & 1;
        
        if (ctx.cpuRegs.GPR.r[2].UL[0] == 0) {
            ctx.cpuRegs.GPR.r[2].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0);
            goto Label_17382c;
        }
        
        {
            ctx.cpuRegs.GPR.r[3].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0);
            uint64_t mask_87ff = 0x87ffffffffffffffULL;
            ctx.cpuRegs.GPR.r[3].UD[0] = ctx.cpuRegs.GPR.r[3].UD[0] & mask_87ff;
            memory::write<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0, ctx.cpuRegs.GPR.r[3].UD[0]);
            ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[5].UL[0] + 0x0);
            ctx.cpuRegs.GPR.r[2].UL[0] = ctx.cpuRegs.GPR.r[2].UL[0] & 0xDFFFFFFF;
            memory::write<uint32_t>(ctx.cpuRegs.GPR.r[5].UL[0] + 0x0, ctx.cpuRegs.GPR.r[2].UL[0]);
        }
        goto Label_caseD_0;

    Label_17382c:
        {
            uint64_t mask_87ff = 0x87ffffffffffffffULL;
            ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] & mask_87ff;
            memory::write<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0, ctx.cpuRegs.GPR.r[2].UD[0]);
        }
        goto Label_caseD_0;

    Label_caseD_6:  // 0x173854
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[18].UL[0] + 0x4);
        ctx.cpuRegs.GPR.r[2].UL[0] = (ctx.cpuRegs.GPR.r[2].UL[0] >> 4) & 1;
        ctx.cpuRegs.GPR.r[6].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[28].UL[0] - 0x7dcc);
        
        if (ctx.cpuRegs.GPR.r[2].UL[0] == 0) {
            goto Label_173930;
        }
        
        {
            ctx.cpuRegs.GPR.r[2].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0);
            uint64_t mask_feff = 0xfeffffffffffffffULL;
            uint64_t set_bit = 0x8000ULL << 42;  // 0x8000 dsll32 10
            uint64_t mask_fbff = 0xfbffffffffffffffULL;
            ctx.cpuRegs.GPR.r[2].UD[0] = (ctx.cpuRegs.GPR.r[2].UD[0] & mask_feff) | set_bit;
            ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] & mask_fbff;
            memory::write<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0, ctx.cpuRegs.GPR.r[4].UD[0]);
        }
        
        ctx.cpuRegs.GPR.r[3].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[6].UL[0] + 0x0);
        ctx.cpuRegs.GPR.r[2].UL[0] = ctx.cpuRegs.GPR.r[3].UL[0] & 0xF;
        
        if (ctx.cpuRegs.GPR.r[2].UL[0] != 0) {
            goto Label_173a2c;
        }
        
        ctx.cpuRegs.GPR.r[2].UL[0] = (ctx.cpuRegs.GPR.r[3].UL[0] >> 28) & 1;
        
        if (ctx.cpuRegs.GPR.r[2].UL[0] == 0) {
            goto Label_173920;
        }
        
        {
            uint64_t mask_87ff = 0x87ffffffffffffffULL;
            uint64_t set_bit = 0x8000ULL << 44;  // 0x8000 dsll32 12
            ctx.cpuRegs.GPR.r[3].UD[0] = (ctx.cpuRegs.GPR.r[4].UD[0] & mask_87ff) | set_bit;
            memory::write<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0, ctx.cpuRegs.GPR.r[3].UD[0]);
            ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[6].UL[0] + 0x0);
            ctx.cpuRegs.GPR.r[2].UL[0] = ctx.cpuRegs.GPR.r[2].UL[0] | 0x20000000;
            memory::write<uint32_t>(ctx.cpuRegs.GPR.r[6].UL[0] + 0x0, ctx.cpuRegs.GPR.r[2].UL[0]);
            memory::write<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x20, 0);
        }
        goto Label_173a2c;

    Label_173920:
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[17].UD[0];
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x173928;
        if (recompiled_functions.count(0x17b520)) {
            recompiled_functions[0x17b520](ctx, 0x17b520);
        } else {
            ctx.cpuRegs.pc = 0x17b520;
            return;
        }
        goto Label_173a2c;

    Label_173930:
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[18].UL[0] + 0x0);
        ctx.cpuRegs.GPR.r[2].UL[0] = (ctx.cpuRegs.GPR.r[2].UL[0] >> 29) & 1;
        ctx.cpuRegs.GPR.r[2].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0);
        
        if (ctx.cpuRegs.GPR.r[2].UL[0] == 0) {
            goto Label_173a08;
        }
        
        {
            uint64_t set_bit = 0x8000ULL << 41;   // 0x8000 dsll32 9
            uint64_t mask_fdff = 0xfdffffffffffffffULL;
            uint64_t mask_fbff = 0xfbffffffffffffffULL;
            ctx.cpuRegs.GPR.r[2].UD[0] = (ctx.cpuRegs.GPR.r[2].UD[0] | set_bit) & mask_fdff;
            ctx.cpuRegs.GPR.r[3].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] & mask_fbff;
            memory::write<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0, ctx.cpuRegs.GPR.r[3].UD[0]);
        }
        
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[6].UL[0] + 0x0);
        ctx.cpuRegs.GPR.r[2].UL[0] = (ctx.cpuRegs.GPR.r[2].UL[0] >> 29) & 1;
        
        if (ctx.cpuRegs.GPR.r[2].UL[0] == 0) {
            goto Label_1739e4;
        }
        
        {
            uint64_t mask_87ff = 0x87ffffffffffffffULL;
            ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[3].UD[0] & mask_87ff;
            memory::write<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0, ctx.cpuRegs.GPR.r[2].UD[0]);
            ctx.cpuRegs.GPR.r[3].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[6].UL[0] + 0x0);
            ctx.cpuRegs.GPR.r[3].UL[0] = ctx.cpuRegs.GPR.r[3].UL[0] & 0xDFFFFFFF;
            memory::write<uint32_t>(ctx.cpuRegs.GPR.r[6].UL[0] + 0x0, ctx.cpuRegs.GPR.r[3].UL[0]);
        }
        goto Label_173a2c;

    Label_1739e4:
        {
            uint64_t mask_87ff = 0x87ffffffffffffffULL;
            ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[3].UD[0] & mask_87ff;
        }
        goto Label_173a28;

    Label_173a08:
        {
            uint64_t mask_87ff = 0x87ffffffffffffffULL;
            ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] & mask_87ff;
        }

    Label_173a28:
        memory::write<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0, ctx.cpuRegs.GPR.r[2].UD[0]);

    Label_173a2c:
        ctx.fpuRegs.fpr[20].UL = 0x3e800000;
        ctx.cpuRegs.GPR.r[16].UL[0] = ctx.cpuRegs.GPR.r[17].UL[0] + 0x5d0;
        ctx.fpuRegs.fpr[0].UL = memory::read<uint32_t>(0x3097dc);
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[16].UD[0];
        ctx.cpuRegs.GPR.r[5].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x728);
        ctx.fpuRegs.fpr[0].f = ctx.fpuRegs.fpr[0].f * ctx.fpuRegs.fpr[20].f;
        ctx.fpuRegs.fpr[1].f = truncf(ctx.fpuRegs.fpr[0].f);
        ctx.fpuRegs.fpr[1].SL = (int32_t)ctx.fpuRegs.fpr[1].f;
        ctx.cpuRegs.GPR.r[6].SL[0] = ctx.fpuRegs.fpr[1].SL;
        ctx.cpuRegs.GPR.r[7].UD[0] = 0;
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x173a5c;
        if (recompiled_functions.count(0x259bc0)) {
            recompiled_functions[0x259bc0](ctx, 0x259bc0);
        } else {
            ctx.cpuRegs.pc = 0x259bc0;
            return;
        }
        
        ctx.fpuRegs.fpr[0].UL = memory::read<uint32_t>(0x3097dc);
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[16].UD[0];
        ctx.cpuRegs.GPR.r[5].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x638);

    Label_173a6c:
        ctx.fpuRegs.fpr[0].f = ctx.fpuRegs.fpr[0].f * ctx.fpuRegs.fpr[20].f;
        ctx.fpuRegs.fpr[1].f = truncf(ctx.fpuRegs.fpr[0].f);
        ctx.fpuRegs.fpr[1].SL = (int32_t)ctx.fpuRegs.fpr[1].f;
        ctx.cpuRegs.GPR.r[6].SL[0] = ctx.fpuRegs.fpr[1].SL;
        ctx.cpuRegs.GPR.r[7].UD[0] = 0;
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x173a80;
        if (recompiled_functions.count(0x259b20)) {
            recompiled_functions[0x259b20](ctx, 0x259b20);
        } else {
            ctx.cpuRegs.pc = 0x259b20;
            return;
        }
        ctx.cpuRegs.GPR.r[2].UL[0] = 0;
        goto Label_epilogue;

    Label_epilogue:  // 0x173a8c
        ctx.cpuRegs.GPR.r[16].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x00);
        ctx.cpuRegs.GPR.r[17].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x08);
        ctx.cpuRegs.GPR.r[18].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x10);
        ctx.cpuRegs.GPR.r[19].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x18);
        ctx.cpuRegs.GPR.r[31].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x20);
        ctx.fpuRegs.fpr[20].UL = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x30);
        ctx.cpuRegs.GPR.r[29].SL[0] = ctx.cpuRegs.GPR.r[29].SL[0] + 0x40;
        return;
    }
    )code" << std::endl;
        return;
    }
    /* 
        if (func.base_address == 0x001815c0) {
        std::cout << "Skipping recompile for 0x1815c0 (Hooked via HLE)" << std::endl;
        file << "// Function 0x1815c0 skipped - replaced by HLE hook\n";
        return;
    }
    */


    /*
        if (func.base_address == 0x001815f0) {
        std::cout << "Skipping recompile for 0x1815c0 (Hooked via HLE)" << std::endl;
        file << "// Function 0x1815f0 skipped - replaced by HLE hook\n";
        return;
    }
    */

    if (func.base_address == 0x002d1a50) {
        std::cout << "Skipping recompile for 0x2d1a50 (Hooked via HLE)" << std::endl;
        file << "// Function 0x2d1a50 skipped - replaced by HLE hook\n";
        return;
    }
    /*
    if (func.base_address == 0x002cf770) {
        std::cout << "Skipping recompile for 0x2cf770 (Hooked via HLE)" << std::endl;
        file << "// Function 0x2cf770 skipped - replaced by HLE hook\n";
        return;
    }    
    */


    /*
    if (func.base_address == 0x0019fb18) {
        std::cout << "Skipping recompile for 0x0019fb18 (Hooked via HLE)" << std::endl;
        file << "// Function 0x19fb18 skipped - replaced by HLE hook\n";
        return;
    }
    */

    if (func.base_address == 0x002d6880) {
        std::cout << "Skipping recompile for 0x002d6880 (Hooked via HLE)" << std::endl;
        file << "// Function 0x2d6880 skipped - replaced by HLE hook\n";
        return;
    }
    if (func.base_address == 0x002c6ce0) {
        std::cout << "Skipping recompile for 0x002c6ce0 (Hooked via HLE)" << std::endl;
        file << "// Function 0x2c6ce0 skipped - replaced by HLE hook\n";
        return;
    }
    if (func.base_address == 0x002d69a0) {
        std::cout << "Skipping recompile for 0x002d69a0 (Hooked via HLE)" << std::endl;
        file << "// Function 0x2d69a0 skipped - replaced by HLE hook\n";
        return;
    }
    if (func.base_address == 0x002d27c8) {
        std::cout << "Skipping recompile for 0x002d27c8 (Hooked via HLE)" << std::endl;
        file << "// Function 0x2d27c8 skipped - replaced by HLE hook\n";
        return;
    }
    if (func.base_address == 0x002d27c8) {
        std::cout << "Skipping recompile for 0x002d27c8 (Hooked via HLE)" << std::endl;
        file << "// Function 0x2d27c8 skipped - replaced by HLE hook\n";
        return;
    }
    if (func.base_address == 0x00181490) {
        std::cout << "Skipping recompile for 0x00181490 (Hooked via HLE)" << std::endl;
        file << "// Function 0x181490 skipped - replaced by HLE hook\n";
        return;
    }
    if (func.base_address == 0x001815c0) {
        std::cout << "Skipping recompile for 0x001815c0 (Hooked via HLE)" << std::endl;
        file << "// Function 0x1815c0 skipped - replaced by HLE hook\n";
        return;
    }
    /*
        if (func.base_address == 0x002cf930) {
        std::cout << "Skipping recompile for 0x002cf930 (Hooked via HLE)" << std::endl;
        file << "// Function 0x2cf930 skipped - replaced by HLE hook\n";
        return;
    }
    */


    /*
    if (func.base_address == 0x00100360) {
        // PART 1: Setup and Label_0000
        file << R"code(void FUN_00100360(CpuContext& ctx) {
    Label_0000: // 0x100360
        uint64_t actual_pCtx = ctx.cpuRegs.GPR.r[4].UL[0]; 
        ctx.cpuRegs.GPR.r[17].UL[0] = actual_pCtx; // Sync to r[17] for now

        g_logFile << "[DEBUG] 100360 START. Input pCtx: 0x" << std::hex << actual_pCtx << std::endl;
        ctx.cpuRegs.GPR.r[29].SL[0] = ctx.cpuRegs.GPR.r[29].SL[0] + 0xffffffa0;
        memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x18, ctx.cpuRegs.GPR.r[17].UD[0]);
        ctx.cpuRegs.GPR.r[17].SD[0] = ctx.cpuRegs.GPR.r[4].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
        memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x10, ctx.cpuRegs.GPR.r[16].UD[0]);
        memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x20, ctx.cpuRegs.GPR.r[18].UD[0]);
        memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x28, ctx.cpuRegs.GPR.r[19].UD[0]);
        ctx.cpuRegs.GPR.r[16].SL[0] = ctx.cpuRegs.GPR.r[0].SL[0] + 0xa;
        memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x30, ctx.cpuRegs.GPR.r[20].UD[0]);
        memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x38, ctx.cpuRegs.GPR.r[21].UD[0]);
        memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x40, ctx.cpuRegs.GPR.r[22].UD[0]);
        memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x48, ctx.cpuRegs.GPR.r[23].UD[0]);
        memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x50, ctx.cpuRegs.GPR.r[30].UD[0]);
        memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x58, ctx.cpuRegs.GPR.r[31].UD[0]);
        ctx.cpuRegs.GPR.r[18].SL[0] = ctx.cpuRegs.GPR.r[17].SL[0] + 0x34;
        // JAL was called 
        // The address after JAL is: 0x10039c
        // The next block should be: 1003e0
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x10039c;
        if (recompiled_functions.count(0x180fe8)) {
            recompiled_functions[0x180fe8](ctx, 0x180fe8);
            ctx.cpuRegs.GPR.r[17].UL[0] = actual_pCtx; // FORCE RESTORE after call
        } else {
            ctx.cpuRegs.pc = 0x180fe8;
        }
        ctx.cpuRegs.GPR.r[3].UL[0] = 0x2f0000;
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x28, ctx.cpuRegs.GPR.r[0].UL[0]);
        ctx.cpuRegs.GPR.r[3].SL[0] = ctx.cpuRegs.GPR.r[3].SL[0] + 0xffffc190;
        ctx.cpuRegs.GPR.r[2].SL[0] = ctx.cpuRegs.GPR.r[17].SL[0] + 0x28;
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x18, ctx.cpuRegs.GPR.r[3].UL[0]);
        ctx.cpuRegs.GPR.r[4].SL[0] = ctx.cpuRegs.GPR.r[0].SL[0] + 0x88;
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0] + 0x8, ctx.cpuRegs.GPR.r[0].UL[0]);
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0] + 0x4, ctx.cpuRegs.GPR.r[0].UL[0]);
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[18].UL[0] + 0x4, ctx.cpuRegs.GPR.r[0].UL[0]);
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[18].UL[0] + 0x8, ctx.cpuRegs.GPR.r[16].UL[0]);
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[18].UL[0] + 0xc, ctx.cpuRegs.GPR.r[16].UL[0]);
        // JAL was called 
        // The address after JAL is: 0x1003cc
        // The next block should be: 1003e0
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1003cc;
        if (recompiled_functions.count(0x1815c0)) {
            recompiled_functions[0x1815c0](ctx, 0x1815c0);
        } else {
            ctx.cpuRegs.pc = 0x1815c0;
        }
        ctx.cpuRegs.GPR.r[4].SL[0] = ctx.cpuRegs.GPR.r[2].SL[0] + 0x10;
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0] + 0x0, ctx.cpuRegs.GPR.r[16].UL[0]);
        ctx.cpuRegs.GPR.r[5].SL[0] = ctx.cpuRegs.GPR.r[0].SL[0] + 0xffffffff;
        ctx.cpuRegs.GPR.r[2].SD[0] = ctx.cpuRegs.GPR.r[4].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
        ctx.cpuRegs.GPR.r[3].SL[0] = ctx.cpuRegs.GPR.r[0].SL[0] + 0x9;

    Label_0001: // 0x1003e0
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0] + 0x0, ctx.cpuRegs.GPR.r[0].UL[0]);
        ctx.cpuRegs.GPR.r[3].SL[0] = ctx.cpuRegs.GPR.r[3].SL[0] + 0xffffffff;
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0] + 0x4, ctx.cpuRegs.GPR.r[0].UL[0]);
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0] + 0x8, ctx.cpuRegs.GPR.r[0].UL[0]);
        bool branch_taken_1003f0 = (ctx.cpuRegs.GPR.r[3].UL[0] != ctx.cpuRegs.GPR.r[5].UL[0]);
        ctx.cpuRegs.GPR.r[2].SL[0] = ctx.cpuRegs.GPR.r[2].SL[0] + 0xc;
        if (branch_taken_1003f0) {
            goto Label_0001;
        } else {
            goto Label_0002;
        }
    )code";
        file << R"code(
    Label_0002: // 0x1003f8
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[18].UL[0] + 0x0, ctx.cpuRegs.GPR.r[4].UL[0]);
        ctx.cpuRegs.GPR.r[2].SL[0] = ctx.cpuRegs.GPR.r[17].SL[0] + 0x48;
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x48, ctx.cpuRegs.GPR.r[0].UL[0]);
        ctx.cpuRegs.GPR.r[16].SL[0] = ctx.cpuRegs.GPR.r[17].SL[0] + 0x54;
        ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0] + 0x8, ctx.cpuRegs.GPR.r[0].UL[0]);
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0] + 0x4, ctx.cpuRegs.GPR.r[0].UL[0]);
        ctx.cpuRegs.GPR.r[30].SL[0] = ctx.cpuRegs.GPR.r[17].SL[0] + 0x98;
        // JAL was called 
        // The address after JAL is: 0x10041c
        // The next block should be: 1004dc
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x10041c;
        if (recompiled_functions.count(0x17a558)) {
            recompiled_functions[0x17a558](ctx, 0x17a558);
        } else {
            ctx.cpuRegs.pc = 0x17a558;
        }
        ctx.cpuRegs.GPR.r[18].SL[0] = ctx.cpuRegs.GPR.r[17].SL[0] + 0xa0;
        ctx.cpuRegs.GPR.r[2].SL[0] = ctx.cpuRegs.GPR.r[17].SL[0] + 0x9c;
        ctx.cpuRegs.GPR.r[4].UL[0] = 0x300000;
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x0, ctx.cpuRegs.GPR.r[2].UL[0]);
        ctx.cpuRegs.GPR.r[4].SL[0] = ctx.cpuRegs.GPR.r[4].SL[0] + 0x3278;
        ctx.cpuRegs.GPR.r[2].UL[0] = 0x300000;
        ctx.cpuRegs.GPR.r[3].UL[0] = 0x2f0000;
        ctx.cpuRegs.GPR.r[6].UL[0] = 0x2f0000;
        ctx.cpuRegs.GPR.r[7].UL[0] = 0x2f0000;
        ctx.cpuRegs.GPR.r[8].UL[0] = 0x2f0000;
        ctx.cpuRegs.GPR.r[9].UL[0] = 0x300000;
        ctx.cpuRegs.GPR.r[3].SL[0] = ctx.cpuRegs.GPR.r[3].SL[0] + 0xfffffee0;
        ctx.cpuRegs.GPR.r[2].SL[0] = ctx.cpuRegs.GPR.r[2].SL[0] + 0xffffcf40;
        ctx.cpuRegs.GPR.r[6].SL[0] = ctx.cpuRegs.GPR.r[6].SL[0] + 0xffffe018;
        ctx.cpuRegs.GPR.r[7].SL[0] = ctx.cpuRegs.GPR.r[7].SL[0] + 0x5940;
        ctx.cpuRegs.GPR.r[8].SL[0] = ctx.cpuRegs.GPR.r[8].SL[0] + 0x5ee8;
        ctx.cpuRegs.GPR.r[9].SL[0] = ctx.cpuRegs.GPR.r[9].SL[0] + 0xffffc110;
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x98, ctx.cpuRegs.GPR.r[4].UL[0]);
        ctx.cpuRegs.GPR.r[5].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0xa0, ctx.cpuRegs.GPR.r[3].UL[0]);
        ctx.cpuRegs.GPR.r[4].SL[0] = ctx.cpuRegs.GPR.r[17].SL[0] + 0xb4;
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x9c, ctx.cpuRegs.GPR.r[2].UL[0]);
        ctx.cpuRegs.GPR.r[19].SL[0] = ctx.cpuRegs.GPR.r[17].SL[0] + 0xa4;
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0xa4, ctx.cpuRegs.GPR.r[6].UL[0]);
        ctx.cpuRegs.GPR.r[21].SL[0] = ctx.cpuRegs.GPR.r[17].SL[0] + 0xa8;
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0xa8, ctx.cpuRegs.GPR.r[7].UL[0]);
        ctx.cpuRegs.GPR.r[23].SL[0] = ctx.cpuRegs.GPR.r[17].SL[0] + 0xac;
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0xac, ctx.cpuRegs.GPR.r[8].UL[0]);
        ctx.cpuRegs.GPR.r[20].SL[0] = ctx.cpuRegs.GPR.r[17].SL[0] + 0xdc;
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0xb0, ctx.cpuRegs.GPR.r[9].UL[0]);
        ctx.cpuRegs.GPR.r[22].SL[0] = ctx.cpuRegs.GPR.r[17].SL[0] + 0xe0;
        // JAL was called 
        // The address after JAL is: 0x10049c
        // The next block should be: 1004dc
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x10049c;
        if (recompiled_functions.count(0x13f2a8)) {
            recompiled_functions[0x13f2a8](ctx, 0x13f2a8);
        } else {
            ctx.cpuRegs.pc = 0x13f2a8;
        }
        ctx.cpuRegs.GPR.r[2].SL[0] = ctx.cpuRegs.GPR.r[17].SL[0] + 0xb0;
        ctx.cpuRegs.GPR.r[5].UL[0] = 0x310000;
        ctx.cpuRegs.GPR.r[5].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[5].UL[0] + 0xffffa718);
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x4, ctx.cpuRegs.GPR.r[2].UL[0]);
        ctx.cpuRegs.GPR.r[3].UL[0] = 0x300000;
        ctx.cpuRegs.GPR.r[2].UL[0] = 0x300000;
        ctx.cpuRegs.GPR.r[4].UL[0] = 0x300000;
        ctx.cpuRegs.GPR.r[2].SL[0] = ctx.cpuRegs.GPR.r[2].SL[0] + 0x4d78;
        ctx.cpuRegs.GPR.r[3].SL[0] = ctx.cpuRegs.GPR.r[3].SL[0] + 0xffffb1d8;
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0xdc, ctx.cpuRegs.GPR.r[2].UL[0]);
        ctx.cpuRegs.GPR.r[4].SL[0] = ctx.cpuRegs.GPR.r[4].SL[0] + 0x6620;
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0xe0, ctx.cpuRegs.GPR.r[3].UL[0]);
        ctx.cpuRegs.GPR.r[2].SL[0] = ctx.cpuRegs.GPR.r[17].SL[0] + 0xe4;
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0xe4, ctx.cpuRegs.GPR.r[4].UL[0]);
        bool branch_taken_1004d4 = (ctx.cpuRegs.GPR.r[5].UL[0] != 0);
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x8, ctx.cpuRegs.GPR.r[2].UL[0]);
        if (branch_taken_1004d4) {
            goto Label_0004;
        } else {
            goto Label_0003;
        }
    )code";

        file << R"code(
    Label_0003: // 0x1004dc
        ctx.cpuRegs.GPR.r[4].SL[0] = ctx.cpuRegs.GPR.r[0].SL[0] + 0x10;
        // JAL was called 
        // The address after JAL is: 0x1004e4
        // The next block should be: 1004f4
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1004e4;
        if (recompiled_functions.count(0x181560)) {
            recompiled_functions[0x181560](ctx, 0x181560);
        } else {
            ctx.cpuRegs.pc = 0x181560;
        }
        ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[2].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
        // JAL was called 
        // The address after JAL is: 0x1004ec
        // The next block should be: 1004f4
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1004ec;
        if (recompiled_functions.count(0x1014b8)) {
            recompiled_functions[0x1014b8](ctx, 0x1014b8);
        } else {
            ctx.cpuRegs.pc = 0x1014b8;
        }
        ctx.cpuRegs.GPR.r[1].UL[0] = 0x310000;
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[1].UL[0] + 0xffffa718, ctx.cpuRegs.GPR.r[2].UL[0]);

    Label_0004: // 0x1004f4
        ctx.cpuRegs.GPR.r[6].SL[0] = ctx.cpuRegs.GPR.r[0].SL[0] + 0x4;
        ctx.cpuRegs.GPR.r[16].UL[0] = 0x310000;
        ctx.cpuRegs.GPR.r[16].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0xffffa718);
        ctx.cpuRegs.GPR.r[4].SL[0] = ctx.cpuRegs.GPR.r[17].SL[0] + 0x1c;
        ctx.cpuRegs.GPR.r[5].SD[0] = ctx.cpuRegs.GPR.r[0].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
        // JAL was called 
        // The address after JAL is: 0x10050c
        // THIS IS THE END OF THE BLOCK: 0x100600
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x10050c;
        if (recompiled_functions.count(0x2c6e88)) {
            recompiled_functions[0x2c6e88](ctx, 0x2c6e88);
        } else {
            ctx.cpuRegs.pc = 0x2c6e88;
        }
        ctx.cpuRegs.GPR.r[3].SL[0] = ctx.cpuRegs.GPR.r[0].SL[0] + 0x64;
        ctx.cpuRegs.GPR.r[2].SL[0] = ctx.cpuRegs.GPR.r[0].SL[0] + 0xa;
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0xec, ctx.cpuRegs.GPR.r[0].UL[0]);
        ctx.cpuRegs.GPR.r[5].SD[0] = ctx.cpuRegs.GPR.r[30].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0xe8, ctx.cpuRegs.GPR.r[0].UL[0]);
        ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0xf4, ctx.cpuRegs.GPR.r[0].UL[0]);
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0xf8, ctx.cpuRegs.GPR.r[0].UL[0]);
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x44, ctx.cpuRegs.GPR.r[2].UL[0]);
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x100, ctx.cpuRegs.GPR.r[3].UL[0]);
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0xfc, ctx.cpuRegs.GPR.r[3].UL[0]);
        // JAL was called 
        // The address after JAL is: 0x10053c
        // THIS IS THE END OF THE BLOCK: 0x100600
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x10053c;
        if (recompiled_functions.count(0x2b5170)) {
            recompiled_functions[0x2b5170](ctx, 0x2b5170);
        } else {
            ctx.cpuRegs.pc = 0x2b5170;
        }
        ctx.cpuRegs.GPR.r[5].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x0);
        ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
        // JAL was called 
        // The address after JAL is: 0x100548
        // THIS IS THE END OF THE BLOCK: 0x100600
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x100548;
        if (recompiled_functions.count(0x2b5170)) {
            recompiled_functions[0x2b5170](ctx, 0x2b5170);
        } else {
            ctx.cpuRegs.pc = 0x2b5170;
        }
        ctx.cpuRegs.GPR.r[5].SD[0] = ctx.cpuRegs.GPR.r[18].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
        ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
        // JAL was called 
        // The address after JAL is: 0x100554
        // THIS IS THE END OF THE BLOCK: 0x100600
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x100554;
        if (recompiled_functions.count(0x2b5170)) {
            recompiled_functions[0x2b5170](ctx, 0x2b5170);
        } else {
            ctx.cpuRegs.pc = 0x2b5170;
        }
        ctx.cpuRegs.GPR.r[5].SD[0] = ctx.cpuRegs.GPR.r[19].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
        ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
        // JAL was called 
        // The address after JAL is: 0x100560
        // THIS IS THE END OF THE BLOCK: 0x100600
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x100560;
        if (recompiled_functions.count(0x2b5170)) {
            recompiled_functions[0x2b5170](ctx, 0x2b5170);
        } else {
            ctx.cpuRegs.pc = 0x2b5170;
        }
        ctx.cpuRegs.GPR.r[5].SD[0] = ctx.cpuRegs.GPR.r[21].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
        ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
        // JAL was called 
        // The address after JAL is: 0x10056c
        // THIS IS THE END OF THE BLOCK: 0x100600
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x10056c;
        if (recompiled_functions.count(0x2b5170)) {
            recompiled_functions[0x2b5170](ctx, 0x2b5170);
        } else {
            ctx.cpuRegs.pc = 0x2b5170;
        }
        ctx.cpuRegs.GPR.r[5].SD[0] = ctx.cpuRegs.GPR.r[23].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
        ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
        // JAL was called 
        // The address after JAL is: 0x100578
        // THIS IS THE END OF THE BLOCK: 0x100600
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x100578;
        if (recompiled_functions.count(0x2b5170)) {
            recompiled_functions[0x2b5170](ctx, 0x2b5170);
        } else {
            ctx.cpuRegs.pc = 0x2b5170;
        }
        ctx.cpuRegs.GPR.r[5].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x4);
        ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
        // JAL was called 
        // The address after JAL is: 0x100584
        // THIS IS THE END OF THE BLOCK: 0x100600
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x100584;
        if (recompiled_functions.count(0x2b5170)) {
            recompiled_functions[0x2b5170](ctx, 0x2b5170);
        } else {
            ctx.cpuRegs.pc = 0x2b5170;
        }
        ctx.cpuRegs.GPR.r[5].SD[0] = ctx.cpuRegs.GPR.r[20].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
        ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
        // JAL was called 
        // The address after JAL is: 0x100590
        // THIS IS THE END OF THE BLOCK: 0x100600
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x100590;
        if (recompiled_functions.count(0x2b5170)) {
            recompiled_functions[0x2b5170](ctx, 0x2b5170);
        } else {
            ctx.cpuRegs.pc = 0x2b5170;
        }
        ctx.cpuRegs.GPR.r[5].SD[0] = ctx.cpuRegs.GPR.r[22].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
        ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
        // JAL was called 
        // The address after JAL is: 0x10059c
        // THIS IS THE END OF THE BLOCK: 0x100600
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x10059c;
        if (recompiled_functions.count(0x2b5170)) {
            recompiled_functions[0x2b5170](ctx, 0x2b5170);
        } else {
            ctx.cpuRegs.pc = 0x2b5170;
        }
        ctx.cpuRegs.GPR.r[5].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x8);
        ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
        // JAL was called 
        // The address after JAL is: 0x1005a8
        // THIS IS THE END OF THE BLOCK: 0x100600
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1005a8;
        if (recompiled_functions.count(0x2b5170)) {
            recompiled_functions[0x2b5170](ctx, 0x2b5170);
        } else {
            ctx.cpuRegs.pc = 0x2b5170;
        }
        //nop 
        // JAL was called 
        // The address after JAL is: 0x1005b0
        // THIS IS THE END OF THE BLOCK: 0x100600
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1005b0;
        if (recompiled_functions.count(0x2069e8)) {
            recompiled_functions[0x2069e8](ctx, 0x2069e8);
        } else {
            ctx.cpuRegs.pc = 0x2069e8;
        }
        memory::write<uint8_t>(ctx.cpuRegs.GPR.r[28].UL[0] + 0xffff8868, ctx.cpuRegs.GPR.r[0].UL[0]);
        // JAL was called 
        // The address after JAL is: 0x1005b8
        // THIS IS THE END OF THE BLOCK: 0x100600
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1005b8;
        if (recompiled_functions.count(0x10c1f0)) {
            recompiled_functions[0x10c1f0](ctx, 0x10c1f0);
        } else {
            ctx.cpuRegs.pc = 0x10c1f0;
        }
        //nop 
        // JAL was called 
        // The address after JAL is: 0x1005c0
        // THIS IS THE END OF THE BLOCK: 0x100600
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1005c0;
        if (recompiled_functions.count(0x1b9a40)) {
            recompiled_functions[0x1b9a40](ctx, 0x1b9a40);
        } else {
            ctx.cpuRegs.pc = 0x1b9a40;
        }
        ctx.cpuRegs.GPR.r[3].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x1c);
        ctx.cpuRegs.GPR.r[2].SD[0] = ctx.cpuRegs.GPR.r[17].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
        ctx.cpuRegs.GPR.r[16].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x10);
        ctx.cpuRegs.GPR.r[3].UL[0] = ctx.cpuRegs.GPR.r[3].UL[0] | 0x8;
        ctx.cpuRegs.GPR.r[18].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x20);
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x1c, ctx.cpuRegs.GPR.r[3].UL[0]);
        ctx.cpuRegs.GPR.r[17].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x18);
        ctx.cpuRegs.GPR.r[19].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x28);
        ctx.cpuRegs.GPR.r[20].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x30);
        ctx.cpuRegs.GPR.r[21].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x38);
        ctx.cpuRegs.GPR.r[22].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x40);
        ctx.cpuRegs.GPR.r[23].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x48);
        ctx.cpuRegs.GPR.r[30].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x50);
        ctx.cpuRegs.GPR.r[31].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x58);
        ctx.cpuRegs.GPR.r[29].SL[0] = ctx.cpuRegs.GPR.r[29].SL[0] + 0x60;
        
        ctx.cpuRegs.GPR.r[2].UL[0] = actual_pCtx; 
        
        // Final Log
        g_logFile << "[DEBUG] 100360 FIXED RETURN: 0x" << std::hex << ctx.cpuRegs.GPR.r[2].UL[0] << std::endl;
        return; // Return from function

    }
    )code" << std::endl;
    return;
    }
    */

if (func.base_address == 0x00255188) {
        // FUN_00255188
        file << R"code(void FUN_00255188(CpuContext& ctx) {
        // Prologue
        ctx.cpuRegs.GPR.r[29].SL[0] = ctx.cpuRegs.GPR.r[29].SL[0] - 0x40;
        memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x10, ctx.cpuRegs.GPR.r[16].UD[0]);
        memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x18, ctx.cpuRegs.GPR.r[17].UD[0]);
        ctx.cpuRegs.GPR.r[16].UD[0] = ctx.cpuRegs.GPR.r[4].UD[0]; // s0 = a0
        memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x20, ctx.cpuRegs.GPR.r[31].UD[0]);
        ctx.cpuRegs.GPR.r[17].UD[0] = ctx.cpuRegs.GPR.r[5].UD[0]; // s1 = a1
        memory::write<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x30, ctx.fpuRegs.fpr[20].UL);

        ctx.cpuRegs.GPR.r[4].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x0);
        {
            uint32_t v0 = ctx.cpuRegs.GPR.r[4].UL[0] & 0x100;
            if (v0 == 0) goto Label_00255264;
        }

        ctx.cpuRegs.GPR.r[6].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x4);
        ctx.cpuRegs.GPR.r[3].UL[0] = ctx.cpuRegs.GPR.r[4].UL[0] >> 4;
        
        {
            uint32_t v0 = ctx.cpuRegs.GPR.r[4].UL[0] & 0xFFFFFFF0;
            uint32_t v1 = ctx.cpuRegs.GPR.r[3].UL[0] & 0xf;
            v0 = v0 | v1;
            uint32_t p1_mask = 0xFFFFFEFF; // -0x101
            ctx.cpuRegs.GPR.r[5].UL[0] = v0 & 0xf; // param_2
            memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0xc, ctx.cpuRegs.GPR.r[6].UL[0]);
            
            v0 = v0 & p1_mask;
            memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x0, v0);
            
            if (ctx.cpuRegs.GPR.r[5].UL[0] == 2) goto Label_0025522c;
            
            memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x18, 0);
            
            if (ctx.cpuRegs.GPR.r[5].UL[0] < 3) {
                if (ctx.cpuRegs.GPR.r[5].UL[0] == 1) goto Label_00255220;
                // param_2 == 0
                ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x34);
                goto Label_0025547c;
            }
        }

    Label_00255204:
        if (ctx.cpuRegs.GPR.r[5].UL[0] == 3) goto Label_0025523c;
        if (ctx.cpuRegs.GPR.r[5].UL[0] == 4) goto Label_0025524c;
        
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x34);
        goto Label_0025547c;

    Label_00255220:
        ctx.cpuRegs.GPR.r[6].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x1c);
        ctx.cpuRegs.GPR.r[4].UL[0] = memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[6].UL[0]) + 0x8);
        goto Label_00255254;

    Label_0025522c:
        ctx.cpuRegs.GPR.r[3].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[6].UL[0]) + 0xc); // Delay slot v1 load
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x34);
        ctx.cpuRegs.GPR.r[4].UL[0] = memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0]) + 0x10);
        goto Label_00255254;

    Label_0025523c:
        ctx.cpuRegs.GPR.r[3].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0]) + 0x14); // Delay slot
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x34);
        ctx.cpuRegs.GPR.r[4].UL[0] = memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0]) + 0x18);
        goto Label_00255254;

    Label_0025524c:
        ctx.cpuRegs.GPR.r[3].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0]) + 0x1c); // Delay slot
        ctx.cpuRegs.GPR.r[4].UL[0] = memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0]) + 0x20);
        ctx.cpuRegs.GPR.r[3].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0]) + 0x24);

    Label_00255254:
        // jalr v1
        {
            uint32_t target = ctx.cpuRegs.GPR.r[3].UL[0];
            ctx.cpuRegs.GPR.r[31].UL[0] = 0x00255258;
            if (recompiled_functions.count(target)) {
                recompiled_functions[target](ctx, target);
            } else {
                ctx.cpuRegs.pc = target;
                return;
            }
        }
        ctx.cpuRegs.GPR.r[4].UL[0] = ctx.cpuRegs.GPR.r[16].UL[0] + ctx.cpuRegs.GPR.r[2].UL[0];
        goto Label_0025547c;


            )code";
        file << R"code(
    Label_00255264:
        ctx.cpuRegs.GPR.r[6].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x1c);
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0xc);
        ctx.cpuRegs.GPR.r[4].UL[0] = ctx.cpuRegs.GPR.r[4].UL[0] & 0xf;
        
        // if (param_1 >= 5) goto caseD_0
        if (ctx.cpuRegs.GPR.r[4].UL[0] >= 5) goto Label_caseD_0;
        
        ctx.cpuRegs.GPR.r[2].UL[0] = ctx.cpuRegs.GPR.r[6].UL[0] - ctx.cpuRegs.GPR.r[2].UL[0];
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x0, ctx.cpuRegs.GPR.r[2].UL[0]);
        
        // Switch Jump Table at 0x302420
        switch(ctx.cpuRegs.GPR.r[4].UL[0]) {
            case 0: goto Label_caseD_0;
            case 1: goto Label_caseD_1;
            case 2: goto Label_caseD_2;
            case 3: goto Label_caseD_3;
            case 4: goto Label_caseD_4;
            default: goto Label_caseD_0;
        }

    Label_caseD_1:
        ctx.cpuRegs.GPR.r[3].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x14);
        if (ctx.cpuRegs.GPR.r[3].UL[0] == 0) goto Label_002552c8;
        
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x0); // local_40
        if (ctx.cpuRegs.GPR.r[2].UL[0] < ctx.cpuRegs.GPR.r[3].UL[0]) {
             ctx.cpuRegs.GPR.r[2].UL[0] = 1;
        } else {
             ctx.cpuRegs.GPR.r[2].UL[0] = 0;
        }
        
        // xori v0, v0, 1 -> if (!(v0 < v1))
        if ((ctx.cpuRegs.GPR.r[2].UL[0] ^ 1) == 0) goto Label_002552c8;
        
        ctx.cpuRegs.GPR.r[3].UL[0] = 0xFFFFFF0F; // -0xF1
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x0);
        memory::write<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x14, 0);
        
        ctx.cpuRegs.GPR.r[2].UL[0] = ctx.cpuRegs.GPR.r[2].UL[0] & ctx.cpuRegs.GPR.r[3].UL[0];
        ctx.cpuRegs.GPR.r[2].UL[0] = ctx.cpuRegs.GPR.r[2].UL[0] | 0x120;
        memory::write<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x0, ctx.cpuRegs.GPR.r[2].UL[0]);

    Label_002552c8:
        memory::write<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x18, 0);
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x34);
        ctx.cpuRegs.GPR.r[5].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x0); // param_2 from local_40
        ctx.cpuRegs.GPR.r[4].UL[0] = memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0]) + 0x28);
        goto Label_002553c4;

    Label_caseD_2:
        ctx.cpuRegs.GPR.r[3].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x10);
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x0); // local_40
        
        // slt v0, v0, v1 (local_40 < s0->0x10)
        if (ctx.cpuRegs.GPR.r[2].UL[0] < ctx.cpuRegs.GPR.r[3].UL[0]) {
             ctx.cpuRegs.GPR.r[2].UL[0] = 1;
        } else {
             ctx.cpuRegs.GPR.r[2].UL[0] = 0;
        }
        
        if (ctx.cpuRegs.GPR.r[2].UL[0] == 0) goto Label_0025534c;
        
        ctx.fpuRegs.fpr[0].UL = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[28].UL[0]) - 0x7d10);
        ctx.fpuRegs.fpr[20].UL = ctx.cpuRegs.GPR.r[3].UL[0]; // mtc1 v1, f20
        ctx.fpuRegs.fpr[20].f = (float)(int32_t)ctx.fpuRegs.fpr[20].UL; // cvt.s.w
        
        // jal FUN_00259908(sp)
        ctx.cpuRegs.GPR.r[4].UL[0] = ctx.cpuRegs.GPR.r[29].UL[0];
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x00255308;
        if (recompiled_functions.count(0x259908)) {
            recompiled_functions[0x259908](ctx, 0x259908);
        } else {
            ctx.cpuRegs.pc = 0x259908;
            return;
        }
        
        // mul.s f20, f20, f0
        ctx.fpuRegs.fpr[20].f = ctx.fpuRegs.fpr[20].f * ctx.fpuRegs.fpr[0].f;
        
        ctx.fpuRegs.fpr[1].UL = memory::read<uint32_t>(0x3097dc);
        ctx.fpuRegs.fpr[0].f = ctx.fpuRegs.fpr[0].f / ctx.fpuRegs.fpr[20].f;
        ctx.fpuRegs.fpr[2].UL = memory::read<uint32_t>(0x3097e0);
        ctx.fpuRegs.fpr[0].f = ctx.fpuRegs.fpr[0].f * ctx.fpuRegs.fpr[1].f;
        
        // cvt.w.s f1, f0
        ctx.fpuRegs.fpr[1].SL = (int32_t)ctx.fpuRegs.fpr[0].f;       // cvt.w.s f1, f0
        ctx.cpuRegs.GPR.r[2].UL[0] = ctx.fpuRegs.fpr[1].UL;          // mfc1 v0, f1
        ctx.fpuRegs.fpr[0].UL = ctx.cpuRegs.GPR.r[2].UL[0];          // mtc1 v0, f0
        ctx.fpuRegs.fpr[0].f = (float)(int32_t)ctx.fpuRegs.fpr[0].UL;// cvt.s.w f0, f0
        ctx.fpuRegs.fpr[0].f = ctx.fpuRegs.fpr[0].f * ctx.fpuRegs.fpr[2].f;
        
        goto Label_0025536c;

            )code";
        file << R"code(
    Label_0025534c:
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x0);
        ctx.cpuRegs.GPR.r[3].UL[0] = 0xFFFFFF0F;
        ctx.fpuRegs.fpr[0].UL = 0x3f800000; // 1.0f
        
        ctx.cpuRegs.GPR.r[2].UL[0] = ctx.cpuRegs.GPR.r[2].UL[0] & ctx.cpuRegs.GPR.r[3].UL[0];
        ctx.cpuRegs.GPR.r[2].UL[0] = ctx.cpuRegs.GPR.r[2].UL[0] | 0x130;
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x18, ctx.fpuRegs.fpr[0].UL); // swc1
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x0, ctx.cpuRegs.GPR.r[2].UL[0]);

    Label_0025536c:
        // delay slot from caseD_2 jump: swc1 f0, 0x18(s0)
        memory::write<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x18, ctx.fpuRegs.fpr[0].UL);
        
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x34);
        ctx.cpuRegs.GPR.r[5].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x0); // local_40
        ctx.cpuRegs.GPR.r[4].UL[0] = memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0]) + 0x30);
        goto Label_002553c4;

    Label_caseD_3:
        ctx.cpuRegs.GPR.r[3].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x14);
        if (ctx.cpuRegs.GPR.r[3].UL[0] == 0) goto Label_002553b0;
        
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x0);
        if (ctx.cpuRegs.GPR.r[2].UL[0] < ctx.cpuRegs.GPR.r[3].UL[0]) ctx.cpuRegs.GPR.r[2].UL[0] = 1; else ctx.cpuRegs.GPR.r[2].UL[0] = 0;
        
        if ((ctx.cpuRegs.GPR.r[2].UL[0] ^ 1) == 0) goto Label_002553b0;
        
        ctx.cpuRegs.GPR.r[3].UL[0] = 0xFFFFFF0F;
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x0);
        memory::write<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x14, 0);
        
        ctx.cpuRegs.GPR.r[2].UL[0] = ctx.cpuRegs.GPR.r[2].UL[0] & ctx.cpuRegs.GPR.r[3].UL[0];
        ctx.cpuRegs.GPR.r[2].UL[0] = ctx.cpuRegs.GPR.r[2].UL[0] | 0x140;
        memory::write<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x0, ctx.cpuRegs.GPR.r[2].UL[0]);
    Label_002553b0:
        memory::write<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x18, 0);
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x34);
        ctx.cpuRegs.GPR.r[5].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x0);
        ctx.cpuRegs.GPR.r[4].UL[0] = memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0]) + 0x38);
        ctx.cpuRegs.GPR.r[3].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0]) + 0x3c); // Delay slot

    Label_002553c4:
        // jalr v1
        {
            uint32_t target = ctx.cpuRegs.GPR.r[3].UL[0];
            ctx.cpuRegs.GPR.r[31].UL[0] = 0x002553c8;
            if (recompiled_functions.count(target)) {
                recompiled_functions[target](ctx, target);
            } else {
                ctx.cpuRegs.pc = target;
                return;
            }
        }
        ctx.cpuRegs.GPR.r[4].UL[0] = ctx.cpuRegs.GPR.r[16].UL[0] + ctx.cpuRegs.GPR.r[2].UL[0];
        goto Label_0025547c;

    Label_caseD_4:
        ctx.cpuRegs.GPR.r[3].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x10);
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x0);
        
        if (ctx.cpuRegs.GPR.r[2].UL[0] < ctx.cpuRegs.GPR.r[3].UL[0]) ctx.cpuRegs.GPR.r[2].UL[0] = 1; else ctx.cpuRegs.GPR.r[2].UL[0] = 0;
        
        if (ctx.cpuRegs.GPR.r[2].UL[0] == 0) goto Label_00255440;
        
        ctx.fpuRegs.fpr[0].UL = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[28].UL[0]) - 0x7d10);
        ctx.fpuRegs.fpr[20].UL = ctx.cpuRegs.GPR.r[3].UL[0];
        ctx.fpuRegs.fpr[20].f = (float)(int32_t)ctx.fpuRegs.fpr[20].UL;
        
        ctx.cpuRegs.GPR.r[4].UL[0] = ctx.cpuRegs.GPR.r[29].UL[0];
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x002553fc;
        if (recompiled_functions.count(0x259908)) {
            recompiled_functions[0x259908](ctx, 0x259908);
        } else {
            ctx.cpuRegs.pc = 0x259908;
            return;
        }
        
        ctx.fpuRegs.fpr[20].f = ctx.fpuRegs.fpr[20].f * ctx.fpuRegs.fpr[0].f;
        ctx.fpuRegs.fpr[1].UL = memory::read<uint32_t>(0x3097dc);
        ctx.fpuRegs.fpr[0].f = ctx.fpuRegs.fpr[0].f / ctx.fpuRegs.fpr[20].f;
        ctx.fpuRegs.fpr[2].UL = memory::read<uint32_t>(0x3097e0);
        ctx.fpuRegs.fpr[0].f = ctx.fpuRegs.fpr[0].f * ctx.fpuRegs.fpr[1].f;
        
        ctx.fpuRegs.fpr[1].SL = (int32_t)ctx.fpuRegs.fpr[0].f;
        ctx.cpuRegs.GPR.r[2].UL[0] = ctx.fpuRegs.fpr[1].UL;
        ctx.fpuRegs.fpr[0].UL = ctx.cpuRegs.GPR.r[2].UL[0];
        ctx.fpuRegs.fpr[0].f = (float)(int32_t)ctx.fpuRegs.fpr[0].UL;
        ctx.fpuRegs.fpr[0].f = ctx.fpuRegs.fpr[0].f * ctx.fpuRegs.fpr[2].f;
        
        goto Label_00255460;
    )code";
        file << R"code(
    Label_00255440:
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x0);
        ctx.cpuRegs.GPR.r[3].UL[0] = 0xFFFFFF0F;
        ctx.fpuRegs.fpr[0].UL = 0x3f800000;
        
        ctx.cpuRegs.GPR.r[2].UL[0] = ctx.cpuRegs.GPR.r[2].UL[0] & ctx.cpuRegs.GPR.r[3].UL[0];
        ctx.cpuRegs.GPR.r[2].UL[0] = ctx.cpuRegs.GPR.r[2].UL[0] | 0x110;
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x18, ctx.fpuRegs.fpr[0].UL);
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x0, ctx.cpuRegs.GPR.r[2].UL[0]);

    Label_00255460:
        // delay slot from caseD_4 jump
        memory::write<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x18, ctx.fpuRegs.fpr[0].UL);
        
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x34);
        ctx.cpuRegs.GPR.r[5].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x0);
        ctx.cpuRegs.GPR.r[4].UL[0] = memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0]) + 0x40);
        ctx.cpuRegs.GPR.r[3].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0]) + 0x44);
        
        {
            uint32_t target = ctx.cpuRegs.GPR.r[3].UL[0];
            ctx.cpuRegs.GPR.r[31].UL[0] = 0x00255474;
            if (recompiled_functions.count(target)) {
                recompiled_functions[target](ctx, target);
            } else {
                ctx.cpuRegs.pc = target;
                return;
            }
        }
        ctx.cpuRegs.GPR.r[4].UL[0] = ctx.cpuRegs.GPR.r[16].UL[0] + ctx.cpuRegs.GPR.r[4].UL[0];

    Label_caseD_0:
        ctx.cpuRegs.GPR.r[6].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x1c);

    Label_0025547c:
        if (ctx.cpuRegs.GPR.r[6].UL[0] == 0) goto Label_00255498;
        
        ctx.cpuRegs.GPR.r[5].UD[0] = ctx.cpuRegs.GPR.r[17].UD[0]; // param_2 = s1
        ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[6].UL[0]) + 0x34);
        ctx.cpuRegs.GPR.r[4].UL[0] = memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0]) + 0x60);
        ctx.cpuRegs.GPR.r[3].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0]) + 0x64);
        
        {
            uint32_t target = ctx.cpuRegs.GPR.r[3].UL[0];
            ctx.cpuRegs.GPR.r[31].UL[0] = 0x00255494;
            if (recompiled_functions.count(target)) {
                recompiled_functions[target](ctx, target);
            } else {
                ctx.cpuRegs.pc = target;
                return;
            }
        }
        ctx.cpuRegs.GPR.r[4].UL[0] = ctx.cpuRegs.GPR.r[6].UL[0] + ctx.cpuRegs.GPR.r[4].UL[0];

    Label_00255498:
        ctx.cpuRegs.GPR.r[16].UD[0] = memory::read<uint64_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x10);
        ctx.cpuRegs.GPR.r[17].UD[0] = memory::read<uint64_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x18);
        ctx.cpuRegs.GPR.r[31].UD[0] = memory::read<uint64_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x20);
        ctx.fpuRegs.fpr[20].UL = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x30);
        ctx.cpuRegs.GPR.r[29].SL[0] = ctx.cpuRegs.GPR.r[29].SL[0] + 0x40;
        return;
    }
    )code" << std::endl;
        return;
    }

if (func.base_address == 0x0029e408) {
    file << R"code(void FUN_0029e408(CpuContext& ctx) {
    // Prologue
    ctx.cpuRegs.GPR.r[29].SL[0] = ctx.cpuRegs.GPR.r[29].SL[0] - 0x10;
    ctx.cpuRegs.GPR.r[5].UD[0] = ctx.cpuRegs.GPR.r[4].UD[0];  // move a1, a0
    memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x0, ctx.cpuRegs.GPR.r[31].UD[0]);  // sd ra, 0x0(sp)

    // Load switch variable: v1 = *(a1 + 0x8)
    ctx.cpuRegs.GPR.r[3].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[5].UL[0]) + 0x8);

    // Bounds check: if (v1 >= 6) goto default
    if (ctx.cpuRegs.GPR.r[3].UL[0] >= 6) {
        goto Label_default;
    }

    // Switch dispatch
    switch (ctx.cpuRegs.GPR.r[3].UL[0]) {
        case 0: goto Label_caseD_0;
        case 1: goto Label_caseD_1;
        case 2: goto Label_caseD_2;
        case 3: goto Label_caseD_3;
        case 4: goto Label_caseD_4;
        case 5: goto Label_caseD_5;
        default: goto Label_default;
    }

Label_caseD_0:
    ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[5].UD[0];  // move a0, a1
    ctx.cpuRegs.GPR.r[31].UL[0] = 0x29e444;
    if (recompiled_functions.count(0x29e580)) {
        recompiled_functions[0x29e580](ctx, 0x29e580);
    } else {
        ctx.cpuRegs.pc = 0x29e580;
        return;
    }
    goto Label_epilogue;

Label_caseD_1:
    ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[5].UD[0];  // move a0, a1
    ctx.cpuRegs.GPR.r[31].UL[0] = 0x29e454;
    if (recompiled_functions.count(0x29aa48)) {
        recompiled_functions[0x29aa48](ctx, 0x29aa48);
    } else {
        ctx.cpuRegs.pc = 0x29aa48;
        return;
    }
    goto Label_epilogue;

Label_caseD_2:
    ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[5].UD[0];  // move a0, a1
    ctx.cpuRegs.GPR.r[31].UL[0] = 0x29e464;
    if (recompiled_functions.count(0x29e588)) {
        recompiled_functions[0x29e588](ctx, 0x29e588);
    } else {
        ctx.cpuRegs.pc = 0x29e588;
        return;
    }
    goto Label_epilogue;

Label_caseD_3:
    ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[5].UD[0];  // move a0, a1
    ctx.cpuRegs.GPR.r[31].UL[0] = 0x29e474;
    if (recompiled_functions.count(0x29a8d0)) {
        recompiled_functions[0x29a8d0](ctx, 0x29a8d0);
    } else {
        ctx.cpuRegs.pc = 0x29a8d0;
        return;
    }
    goto Label_epilogue;

Label_caseD_4:
    ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[5].UD[0];  // move a0, a1
    ctx.cpuRegs.GPR.r[31].UL[0] = 0x29e484;
    if (recompiled_functions.count(0x29e6a8)) {
        recompiled_functions[0x29e6a8](ctx, 0x29e6a8);
    } else {
        ctx.cpuRegs.pc = 0x29e6a8;
        return;
    }
    goto Label_epilogue;

Label_caseD_5:
    ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[5].UD[0];  // move a0, a1
    ctx.cpuRegs.GPR.r[31].UL[0] = 0x29e494;
    if (recompiled_functions.count(0x29e6d8)) {
        recompiled_functions[0x29e6d8](ctx, 0x29e6d8);
    } else {
        ctx.cpuRegs.pc = 0x29e6d8;
        return;
    }
    // Fall through to epilogue

Label_default:
Label_epilogue:
    ctx.cpuRegs.GPR.r[31].UD[0] = memory::read<uint64_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x0);  // ld ra, 0x0(sp)
    ctx.cpuRegs.GPR.r[29].SL[0] = ctx.cpuRegs.GPR.r[29].SL[0] + 0x10;
    return;
}
)code" << std::endl;
    return;
}


if (func.base_address == 0x00202ef0) {
        // FUN_00202ef0
        file << R"code(void FUN_00202ef0(CpuContext& ctx) {
        // Prologue
        ctx.cpuRegs.GPR.r[29].SL[0] = ctx.cpuRegs.GPR.r[29].SL[0] - 0x20;
        memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x08, ctx.cpuRegs.GPR.r[17].UD[0]); // sd s1
        memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x00, ctx.cpuRegs.GPR.r[16].UD[0]); // sd s0
        ctx.cpuRegs.GPR.r[17].UD[0] = ctx.cpuRegs.GPR.r[4].UD[0]; // s1 = a0
        memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x10, ctx.cpuRegs.GPR.r[18].UD[0]); // sd s2
        // v0 = s1 + 0x8000
        ctx.cpuRegs.GPR.r[2].UL[0] = ctx.cpuRegs.GPR.r[17].UL[0] + 0x8000;
        memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x18, ctx.cpuRegs.GPR.r[31].UD[0]); // sd ra

        // Logic
        // v1 = *(param_2)
        uint32_t v1 = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[5].UL[0]) + 0x0);
        // param_1 = *(v0 + 0x144) -> *(s1 + 0x8000 + 0x144)
        ctx.cpuRegs.GPR.r[4].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0]) + 0x144);
        
        // ptr calculation: s1 + (v1 << 2)
        uint32_t ptr_addr = ctx.cpuRegs.GPR.r[17].UL[0] + (v1 << 2);
        
        // v0 = *(param_1 + 4)
        uint32_t check_val = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[4].UL[0]) + 0x4);
        
        // s2 = *(ptr_addr + 0x58)
        ctx.cpuRegs.GPR.r[18].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(ptr_addr) + 0x58);
        
        // v1 = *(s2 + 0x1c)
        uint32_t switch_raw = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[18].UL[0]) + 0x1c);
        
        // check xor: s2 ^ v0
        uint32_t xor_check = ctx.cpuRegs.GPR.r[18].UL[0] ^ check_val;
        
        // param_1 = v1 & 0xf
        ctx.cpuRegs.GPR.r[4].UL[0] = switch_raw & 0xF;
        
        // if (param_1 >= 9) goto default
        if (ctx.cpuRegs.GPR.r[4].UL[0] >= 9) {
            goto Label_caseD_0;
        }
        
        // param_2 (boolean) = (xor_check < 1) ? 1 : 0
        ctx.cpuRegs.GPR.r[5].UL[0] = (xor_check < 1) ? 1 : 0;

        // Switch dispatch
        switch (ctx.cpuRegs.GPR.r[4].UL[0]) {
            case 2: goto Label_caseD_2;
            case 3: goto Label_caseD_3;
            default: goto Label_caseD_0;
        }

    Label_caseD_2:
        if (ctx.cpuRegs.GPR.r[5].UL[0] == 0) {
            goto Label_00202f98;
        }
        
        // s0 = s1 + 0x8000
        ctx.cpuRegs.GPR.r[16].UL[0] = ctx.cpuRegs.GPR.r[17].UL[0] + 0x8000;
        
        // Load struct base: v0 = *(s0 + 0x144)
        uint32_t struct_base = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x144);
        
        // Call FUN_002051d8(s1, *(struct_base), 7, 0)
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[17].UD[0]; // a0
        ctx.cpuRegs.GPR.r[5].UL[0] = memory::read<uint32_t>(static_cast<uint32_t>(struct_base) + 0x0); // a1
        ctx.cpuRegs.GPR.r[6].UL[0] = 7; // a2
        ctx.cpuRegs.GPR.r[7].UL[0] = 0; // a3
        
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x00202f7c;
        if (recompiled_functions.count(0x2051d8)) {
            recompiled_functions[0x2051d8](ctx, 0x2051d8);
        } else {
            ctx.cpuRegs.pc = 0x2051d8;
            return;
        }
        
        // Reload base: v0 = *(s0 + 0x144)
        struct_base = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x144);
        
        // Setup for jump to shared block
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[17].UD[0]; // a0
        ctx.cpuRegs.GPR.r[6].UL[0] = 7; // a2
        ctx.cpuRegs.GPR.r[7].UL[0] = 0; // a3
        
        // Delay slot load: param_2 = *(v0 + 4)
        ctx.cpuRegs.GPR.r[5].UL[0] = memory::read<uint32_t>(struct_base + 0x4);
        
        goto Label_00202fdc;

    Label_00202f98:
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[17].UD[0]; // a0 = s1
        ctx.cpuRegs.GPR.r[5].UD[0] = ctx.cpuRegs.GPR.r[18].UD[0]; // a1 = s2
        ctx.cpuRegs.GPR.r[6].UL[0] = 7; // a2
        ctx.cpuRegs.GPR.r[7].UL[0] = 0; // a3 set before call (delay slot logic applied here)
        
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x00202fa8;
        if (recompiled_functions.count(0x2051d8)) {
            recompiled_functions[0x2051d8](ctx, 0x2051d8);
        } else {
            ctx.cpuRegs.pc = 0x2051d8;
            return;
        }
        goto Label_00203000;

    Label_caseD_3:
        // s0 = s1 + 0x8000
        ctx.cpuRegs.GPR.r[16].UL[0] = ctx.cpuRegs.GPR.r[17].UL[0] + 0x8000;
        
        // v0 = *(s0 + 0x144)
        struct_base = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x144);
        
        // Call FUN_00203230(s1, *(v0 + 4))
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[17].UD[0]; // a0
        ctx.cpuRegs.GPR.r[5].UL[0] = memory::read<uint32_t>(struct_base + 0x4); // a1
        
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x00202fc8;
        if (recompiled_functions.count(0x203230)) {
            recompiled_functions[0x203230](ctx, 0x203230);
        } else {
            ctx.cpuRegs.pc = 0x203230;
            return;
        }
        
        // Setup fallthrough
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[17].UD[0]; // a0
        ctx.cpuRegs.GPR.r[5].UD[0] = ctx.cpuRegs.GPR.r[18].UD[0]; // a1 = s2
        ctx.cpuRegs.GPR.r[6].UL[0] = 7; // a2
        ctx.cpuRegs.GPR.r[7].UL[0] = 0; // a3

    Label_00202fdc:
        // Call FUN_002051d8
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x00202fe4;
        if (recompiled_functions.count(0x2051d8)) {
            recompiled_functions[0x2051d8](ctx, 0x2051d8);
        } else {
            ctx.cpuRegs.pc = 0x2051d8;
            return;
        }
        
        // Zero out struct fields at *(s0 + 0x144)
        struct_base = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x144);
        memory::write<uint32_t>(struct_base + 0x10, 0);
        memory::write<uint32_t>(struct_base + 0x00, 0);
        memory::write<uint32_t>(struct_base + 0x04, 0);
        memory::write<uint32_t>(struct_base + 0x08, 0);
        memory::write<uint32_t>(struct_base + 0x0C, 0);

    Label_caseD_0:
    Label_00203000: // Epilogue
        ctx.cpuRegs.GPR.r[16].UD[0] = memory::read<uint64_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x00);
        ctx.cpuRegs.GPR.r[17].UD[0] = memory::read<uint64_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x08);
        ctx.cpuRegs.GPR.r[18].UD[0] = memory::read<uint64_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x10);
        ctx.cpuRegs.GPR.r[31].UD[0] = memory::read<uint64_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x18);
        ctx.cpuRegs.GPR.r[29].SL[0] = ctx.cpuRegs.GPR.r[29].SL[0] + 0x20;
        return;
    }
    )code" << std::endl;
        return;
    }



    std::cout << "Recompiling function: " << func.name << std::endl;
    std::cout << "Base address: 0x" << std::hex << func.base_address << std::endl;
    std::cout << "Size: " << std::dec << func.size << std::endl;
    std::cout << "Blocks: " << std::dec << func.blocks.size() << std::endl;

    file << "// Function: " << func.name << " at 0x" << std::hex << func.base_address << "\n";
    file << "void " << func.name << "(CpuContext& ctx) {\n";
    if(func.name == "entry"){
        file << "    initialize_recompiled_functions();" << std::endl;
    }
    
    // Generate each basic block with simple labels
    if (func.base_address == 0x2d4cb0 || func.base_address == 0x2d4c60){
        file << "   ctx.cpuRegs.GPR.r[2].UL[0] = 0x10000;" << std::endl;

        file << "   return;" << std::endl;
        file <<    "   }" << std::endl;
    }
    else if (func.base_address == 0x1e8198) {
            file << "    // HLE Hook for DMA Wait: Force 'Success/Not Busy' (0)\n";
            file << "    //recompiled_functions[0x" << std::hex << func.base_address << "] = [](CpuContext& ctx, uint32_t addr);\n";
            file << "        ctx.cpuRegs.GPR.r[2].UL[0] = 0; // v0 = 0 (Breaks the loop)\n";
            file << "    }\n";
        }
    else{
        for (size_t block_idx = 0; block_idx < func.blocks.size(); ++block_idx) {
            std::cout << "Recompiling block: " << block_idx << std::endl;
            const auto& block = func.blocks[block_idx];
            
            // CHANGE: Simple label format Label_0000 instead of block_0
            file << "Label_" << std::setw(4) << std::setfill('0') << std::dec << block_idx << ": // 0x" << std::hex << block.start_address << "\n";
            
            generate_block_code(func, block, block_idx, file);
            
            file << "\n";
        }
        
        file << "}\n\n";
    }

}

// helper function to generate block code
void Recompiler::generate_block_code(const Function& func, const Block& block, size_t block_idx, std::ofstream& file) {
    bool has_terminator = false;
    
    // Process each instruction in the block
    log_file <<  "[PROCESSING FUNCTION BLOCK] Function: " << func.name << std::endl;
    log_file <<  "[PROCESSING FUNCTION BLOCK] Block Instruction Count: " <<  std::dec << block.instructions.size() << std::endl;

    /*
    
    
    */
    for (int i = 0; i < block.instructions.size(); ++i) {

        log_file << "FUNCTION: " << func.name << " BLOCK INDEX: " << block_idx << " BLOCK INSTRUCTION INDEX: " << i << "\n \n" << std::endl;

        const auto& instr_struct = block.instructions[i];
        const auto& instr_word = instr_struct.getCPtr()->word;
        const auto& instr_vram = instr_struct.getCPtr()->vram;
        
        rabbitizer::InstructionR5900 instr(instr_word, instr_vram);
        std::string disasm_string; 
        // Check if this is a branch/jump instruction

        if (has_delay_slot(instr)) {

            bool is_likely_branch = (static_cast<int>(instr.getUniqueId()) == RABBITIZER_INSTR_ID_cpu_beql ||
                                    static_cast<int>(instr.getUniqueId()) == RABBITIZER_INSTR_ID_cpu_bnel ||
                                    static_cast<int>(instr.getUniqueId()) == RABBITIZER_INSTR_ID_cpu_blezl ||
                                    static_cast<int>(instr.getUniqueId()) == RABBITIZER_INSTR_ID_cpu_bgtzl ||
                                    static_cast<int>(instr.getUniqueId()) == RABBITIZER_INSTR_ID_cpu_bltzl ||
                                    static_cast<int>(instr.getUniqueId()) == RABBITIZER_INSTR_ID_cpu_bgezl ||
                                    static_cast<int>(instr.getUniqueId()) == RABBITIZER_INSTR_ID_cpu_bc1tl ||  // ADD THIS
                                    static_cast<int>(instr.getUniqueId()) == RABBITIZER_INSTR_ID_cpu_bc1fl); 
            // Execute delay slot first (if it exists and is next)
            if (!is_likely_branch){
                switch (static_cast<int>(instr.getUniqueId())) {
                    case RABBITIZER_INSTR_ID_cpu_beq:
                    case RABBITIZER_INSTR_ID_cpu_beqz:
                        file << "    bool branch_taken_" << std::hex << instr.getVram() << " = (" 
                            << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] == " 
                            << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0]);\n";
                        break;
                    case RABBITIZER_INSTR_ID_cpu_bne:
                        file << "    bool branch_taken_" << std::hex << instr.getVram() << " = (" 
                            << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] != " 
                            << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0]);\n";
                        break;
                    case RABBITIZER_INSTR_ID_cpu_bnez:
                        file << "    bool branch_taken_" << std::hex << instr.getVram() << " = (" 
                            << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] != 0);\n";
                        break;
                    case RABBITIZER_INSTR_ID_cpu_bgez:
                        file << "    bool branch_taken_" << std::hex << instr.getVram() << " = (" 
                            << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".SL[0] >= 0);\n";
                        break;
                    case RABBITIZER_INSTR_ID_cpu_bgtz:
                        file << "    bool branch_taken_" << std::hex << instr.getVram() << " = (" 
                            << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".SL[0] > 0);\n";
                        break;
                    case RABBITIZER_INSTR_ID_cpu_blez:
                        file << "    bool branch_taken_" << std::hex << instr.getVram() << " = (" 
                            << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".SL[0] <= 0);\n";
                        break;
                    case RABBITIZER_INSTR_ID_cpu_bltz:
                        file << "    bool branch_taken_" << std::hex << instr.getVram() << " = (" 
                            << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".SL[0] < 0);\n";
                        break;
                    case RABBITIZER_INSTR_ID_cpu_bc1t:
                        file << "    bool branch_taken_" << std::hex << instr.getVram() 
                            << " = ((ctx.fpuRegs.fprc[31] & 0x800000) != 0);\n";
                        break;
                    case RABBITIZER_INSTR_ID_cpu_bc1f:
                        file << "    bool branch_taken_" << std::hex << instr.getVram() 
                            << " = ((ctx.fpuRegs.fprc[31] & 0x800000) == 0);\n";
                        break;
                    // j, jal, jr, jalr don't need condition saving - they're unconditional
                    default:
                        break;
                }
                if (i + 1 < block.instructions.size()) {
                    const auto& delay_slot_struct = block.instructions[i + 1];
                    const auto& delay_word = delay_slot_struct.getCPtr()->word;
                    const auto& delay_vram = delay_slot_struct.getCPtr()->vram;
                    rabbitizer::InstructionR5900 delay_slot_instr(delay_word, delay_vram);
                    disasm_string = delay_slot_instr.disassemble(0, "");
                    
                    log_file << "[PROCESSING INSTRUCTION] " << disasm_string<< std::endl;
                    file << "    ";
                    translate_instruction(delay_slot_instr, file);
                }
            }
            // Handle the branch/jump - CHANGE: New function for control flow
            file << "    ";
            disasm_string = instr.disassemble(0, "");
            
            log_file << "[PROCESSING INSTRUCTION THAT CAUSES DELAY SLOT] " << disasm_string << std::endl;
            translate_control_flow(instr, func, block, block_idx, i, file);
            has_terminator = true;
             // No more instructions after branch
             i++;
        } else {
            // Regular instruction
            file << "    ";
            
            disasm_string = instr.disassemble(0, "");
            log_file << "[PROCESSING INSTRUCTION] " << disasm_string<< std::endl;
            translate_instruction(instr, file);
        }
    }
    
    // Handle fall-through if no explicit branch
    if (!has_terminator) {
        uint32_t fall_through_addr = block.end_address;
        file << "// This BLOCK HAS NO BRANCH SO GO TO NEXT BLOCK AFTER THIS, CURRENT BLOCK ENDS AT: 0x" << std::hex << block.end_address << "\n";
        file << "// Fall through to 0x" << std::hex << fall_through_addr << "\n"; 

        bool found_next_block = false;
        for (size_t i = 0; i < func.blocks.size(); ++i) {
            if (func.blocks[i].start_address == fall_through_addr) {
                file << "    goto Label_" << std::setw(4) << std::setfill('0') << std::dec << i << "; // Fall through\n";
                found_next_block = true;
                break;
            }
        }
        
        if (!found_next_block) {
            // CHANGE: Call external function or set PC
            file << "    // Fall through to 0x" << std::hex << fall_through_addr << "\n";
            file << "    if (recompiled_functions.count(0x" << std::hex << fall_through_addr << ")) {\n";
            file << "        recompiled_functions[0x" << std::hex << fall_through_addr << "](ctx, 0x" << std::hex << fall_through_addr << ");\n";
            file << "        return;\n";
            file << "    } else {\n";
            file << "        ctx.cpuRegs.pc = 0x" << std::hex << fall_through_addr << ";\n";
            file << "        return;\n";
            file << "    }\n";
        }
    }
}

// CHANGE: New function for control flow with function calls
void Recompiler::translate_control_flow(const rabbitizer::InstructionR5900& instr, 
                                        const Function& func,
                                        const Block& block,
                                        size_t current_block_idx,
                                        size_t current_instr_idx, 
                                        std::ofstream& file) {
    
    uint32_t current_pc = instr.getVram();
    std::string cond_var = "cond_" + std::to_string(current_pc);

    switch (instr.getUniqueId()) {
        // Conditional branches

        case RABBITIZER_INSTR_ID_cpu_beq: {
            uint32_t target_addr = current_pc + 4 + (static_cast<int16_t>(instr.Get_immediate()) << 2);
            uint32_t fall_through_addr = current_pc + 8;
            
            file << "    if (branch_taken_" << std::hex << current_pc << ") {\n";
            emit_branch_target(target_addr, func, file, "        ");
            file << "    } else {\n";
            emit_branch_target(fall_through_addr, func, file, "        ");
            file << "    }\n";
            break;
        }

        case RABBITIZER_INSTR_ID_cpu_beql: {
            uint32_t target_addr = current_pc + 4 + (static_cast<int16_t>(instr.Get_immediate()) << 2);
            uint32_t fall_through_addr = current_pc + 8;
            
            // 1. PRE-CALCULATE CONDITION
            // We save the result to a bool BEFORE the delay slot has a chance to mess up the registers.
            file << "    bool " << cond_var << " = (" 
                 << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] == " 
                 << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0]);\n";

            // 2. CHECK CONDITION
            file << "    if (" << cond_var << ") {\n";
            
            // 3. EXECUTE DELAY SLOT (Only inside the 'if' for Likely branches)
            if (current_instr_idx + 1 < block.instructions.size()) {
                const auto& delay_slot_struct = block.instructions[current_instr_idx + 1];
                rabbitizer::InstructionR5900 delay_slot_instr(delay_slot_struct.getCPtr()->word, delay_slot_struct.getCPtr()->vram);
                
                file << "        // Delay Slot (Likely)\n        ";
                translate_instruction(delay_slot_instr, file);
            }
            
            // 4. JUMP TO TARGET
            emit_branch_target(target_addr, func, file, "        ");
            
            file << "    } else {\n";
            // Else case: Branch likely NOT taken -> Skip delay slot, go to fallthrough
            emit_branch_target(fall_through_addr, func, file, "        ");
            file << "    }\n";
            break;
        }
        case RABBITIZER_INSTR_ID_cpu_bnel: {
            uint32_t target_addr = current_pc + 4 + (static_cast<int16_t>(instr.Get_immediate()) << 2);
            uint32_t fall_through_addr = current_pc + 8;
            
            file << "if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] != " 
                << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0]) {\n";
            
            // Execute delay slot ONLY when branch is taken
            if (current_instr_idx + 1 < block.instructions.size()) {
                const auto& delay_slot_struct = block.instructions[current_instr_idx + 1];
                const auto& delay_word = delay_slot_struct.getCPtr()->word;
                const auto& delay_vram = delay_slot_struct.getCPtr()->vram;
                rabbitizer::InstructionR5900 delay_slot_instr(delay_word, delay_vram);
                file << "        ";
                translate_instruction(delay_slot_instr, file);
            }
            
            emit_branch_target(target_addr, func, file, "        ");
            file << "    } else {\n";
            emit_branch_target(fall_through_addr, func, file, "        ");
            file << "    }\n";
            break;
        }

        case RABBITIZER_INSTR_ID_cpu_blezl: {
             uint32_t target_addr = current_pc + 4 + (static_cast<int16_t>(instr.Get_immediate()) << 2);
             uint32_t fall_through_addr = current_pc + 8;

             // 1. Pre-calc
             file << "    bool " << cond_var << " = (" 
                  << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".SL[0] <= 0);\n";

             // 2. Check
             file << "    if (" << cond_var << ") {\n";

             // 3. Delay Slot
             if (current_instr_idx + 1 < block.instructions.size()) {
                 const auto& delay_slot_struct = block.instructions[current_instr_idx + 1];
                 rabbitizer::InstructionR5900 delay_slot_instr(delay_slot_struct.getCPtr()->word, delay_slot_struct.getCPtr()->vram);
                 file << "        ";
                 translate_instruction(delay_slot_instr, file);
             }
             
             emit_branch_target(target_addr, func, file, "        ");
             file << "    } else {\n";
             emit_branch_target(fall_through_addr, func, file, "        ");
             file << "    }\n";
             break;
        }

        case RABBITIZER_INSTR_ID_cpu_bgtzl: {
            uint32_t target_addr = current_pc + 4 + (static_cast<int16_t>(instr.Get_immediate()) << 2);
            uint32_t fall_through_addr = current_pc + 8;
            
            file << "if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".SL[0] > 0) {\n";
            
            // Execute delay slot ONLY when branch is taken
            if (current_instr_idx + 1 < block.instructions.size()) {
                const auto& delay_slot_struct = block.instructions[current_instr_idx + 1];
                const auto& delay_word = delay_slot_struct.getCPtr()->word;
                const auto& delay_vram = delay_slot_struct.getCPtr()->vram;
                rabbitizer::InstructionR5900 delay_slot_instr(delay_word, delay_vram);
                file << "        ";
                translate_instruction(delay_slot_instr, file);
            }
            
            emit_branch_target(target_addr, func, file, "        ");
            file << "    } else {\n";
            emit_branch_target(fall_through_addr, func, file, "        ");
            file << "    }\n";
            break;
        }

        case RABBITIZER_INSTR_ID_cpu_bltzl: {
            uint32_t target_addr = current_pc + 4 + (static_cast<int16_t>(instr.Get_immediate()) << 2);
            uint32_t fall_through_addr = current_pc + 8;
            
            file << "if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".SL[0] < 0) {\n";
            
            // Execute delay slot ONLY when branch is taken
            if (current_instr_idx + 1 < block.instructions.size()) {
                const auto& delay_slot_struct = block.instructions[current_instr_idx + 1];
                const auto& delay_word = delay_slot_struct.getCPtr()->word;
                const auto& delay_vram = delay_slot_struct.getCPtr()->vram;
                rabbitizer::InstructionR5900 delay_slot_instr(delay_word, delay_vram);
                file << "        ";
                translate_instruction(delay_slot_instr, file);
            }
            
            emit_branch_target(target_addr, func, file, "        ");
            file << "    } else {\n";
            emit_branch_target(fall_through_addr, func, file, "        ");
            file << "    }\n";
            break;
        }

        case RABBITIZER_INSTR_ID_cpu_bgezl: {
            uint32_t target_addr = current_pc + 4 + (static_cast<int16_t>(instr.Get_immediate()) << 2);
            uint32_t fall_through_addr = current_pc + 8;
            
            file << "if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".SL[0] >= 0) {\n";
            
            // Execute delay slot ONLY when branch is taken
            if (current_instr_idx + 1 < block.instructions.size()) {
                const auto& delay_slot_struct = block.instructions[current_instr_idx + 1];
                const auto& delay_word = delay_slot_struct.getCPtr()->word;
                const auto& delay_vram = delay_slot_struct.getCPtr()->vram;
                rabbitizer::InstructionR5900 delay_slot_instr(delay_word, delay_vram);
                file << "        ";
                translate_instruction(delay_slot_instr, file);
            }
            
            emit_branch_target(target_addr, func, file, "        ");
            file << "    } else {\n";
            emit_branch_target(fall_through_addr, func, file, "        ");
            file << "    }\n";
            break;
        }

        case RABBITIZER_INSTR_ID_cpu_bc1tl: {
            uint32_t target_addr = current_pc + 4 + (static_cast<int16_t>(instr.Get_immediate()) << 2);
            uint32_t fall_through_addr = current_pc + 8;
            
            // 1. PRE-CALCULATE CONDITION
            file << "    bool " << cond_var << " = ((ctx.fpuRegs.fprc[31] & 0x800000) != 0);\n";
            
            // 2. CHECK CONDITION
            file << "    if (" << cond_var << ") {\n";
            
            // 3. EXECUTE DELAY SLOT (Only if branch is taken - this is a "likely" branch)
            if (current_instr_idx + 1 < block.instructions.size()) {
                const auto& delay_slot_struct = block.instructions[current_instr_idx + 1];
                rabbitizer::InstructionR5900 delay_slot_instr(delay_slot_struct.getCPtr()->word, delay_slot_struct.getCPtr()->vram);
                
                file << "        // Delay Slot (Likely)\n";
                file << "        ";
                translate_instruction(delay_slot_instr, file);
            }
            
            // 4. JUMP TO TARGET
            emit_branch_target(target_addr, func, file, "        ");
            
            file << "    } else {\n";
            // Else case: Branch NOT taken -> Skip delay slot, go to fallthrough
            emit_branch_target(fall_through_addr, func, file, "        ");
            file << "    }\n";
            break;
        }

        case RABBITIZER_INSTR_ID_cpu_bc1fl: {
            uint32_t target_addr = current_pc + 4 + (static_cast<int16_t>(instr.Get_immediate()) << 2);
            uint32_t fall_through_addr = current_pc + 8;
            
            // 1. PRE-CALCULATE CONDITION (False = FPU condition bit is NOT set)
            file << "    bool " << cond_var << " = ((ctx.fpuRegs.fprc[31] & 0x800000) == 0);\n";
            
            // 2. CHECK CONDITION
            file << "    if (" << cond_var << ") {\n";
            
            // 3. EXECUTE DELAY SLOT (Only if branch is taken)
            if (current_instr_idx + 1 < block.instructions.size()) {
                const auto& delay_slot_struct = block.instructions[current_instr_idx + 1];
                rabbitizer::InstructionR5900 delay_slot_instr(delay_slot_struct.getCPtr()->word, delay_slot_struct.getCPtr()->vram);
                
                file << "        // Delay Slot (Likely)\n";
                file << "        ";
                translate_instruction(delay_slot_instr, file);
            }
            
            // 4. JUMP TO TARGET
            emit_branch_target(target_addr, func, file, "        ");
            
            file << "    } else {\n";
            emit_branch_target(fall_through_addr, func, file, "        ");
            file << "    }\n";
            break;
        }

        case RABBITIZER_INSTR_ID_cpu_bne: {
            uint32_t target_addr = current_pc + 4 + (static_cast<int16_t>(instr.Get_immediate()) << 2);
            uint32_t fall_through_addr = current_pc + 8;
            
            // USE THE SAVED CONDITION!
            file << "if (branch_taken_" << std::hex << current_pc << ") {\n";
            emit_branch_target(target_addr, func, file, "        ");
            file << "    } else {\n";
            emit_branch_target(fall_through_addr, func, file, "        ");
            file << "    }\n";
            break;
        }

        
        case RABBITIZER_INSTR_ID_cpu_beqz: {
            uint32_t target_addr = current_pc + 4 + (static_cast<int16_t>(instr.Get_immediate()) << 2);
            uint32_t fall_through_addr = current_pc + 8;
            
            file << "if (branch_taken_" << std::hex << current_pc << ") {\n";            
            emit_branch_target(target_addr, func, file, "        ");
            file << "    } else {\n";
            emit_branch_target(fall_through_addr, func, file, "        ");
            file << "    }\n";
            break;
        }
        
        case RABBITIZER_INSTR_ID_cpu_bnez: {
            uint32_t target_addr = current_pc + 4 + (static_cast<int16_t>(instr.Get_immediate()) << 2);
            uint32_t fall_through_addr = current_pc + 8;
            
            file << "if (branch_taken_" << std::hex << current_pc << ") {\n";            
            emit_branch_target(target_addr, func, file, "        ");
            file << "    } else {\n";
            emit_branch_target(fall_through_addr, func, file, "        ");
            file << "    }\n";
            break;
        }
        
        case RABBITIZER_INSTR_ID_cpu_bgtz: {
            uint32_t target_addr = current_pc + 4 + (static_cast<int16_t>(instr.Get_immediate()) << 2);
            uint32_t fall_through_addr = current_pc + 8;
            
            file << "if (branch_taken_" << std::hex << current_pc << ") {\n";            
            emit_branch_target(target_addr, func, file, "        ");
            file << "    } else {\n";
            emit_branch_target(fall_through_addr, func, file, "        ");
            file << "    }\n";
            break;
        }
        
        case RABBITIZER_INSTR_ID_cpu_blez: {
            uint32_t target_addr = current_pc + 4 + (static_cast<int16_t>(instr.Get_immediate()) << 2);
            uint32_t fall_through_addr = current_pc + 8;
            
            file << "if (branch_taken_" << std::hex << current_pc << ") {\n";
            emit_branch_target(target_addr, func, file, "        ");
            file << "    } else {\n";
            emit_branch_target(fall_through_addr, func, file, "        ");
            file << "    }\n";
            break;
        }
        
        case RABBITIZER_INSTR_ID_cpu_bltz: {
            uint32_t target_addr = current_pc + 4 + (static_cast<int16_t>(instr.Get_immediate()) << 2);
            uint32_t fall_through_addr = current_pc + 8;
            
            file << "if (branch_taken_" << std::hex << current_pc << ") {\n";
            emit_branch_target(target_addr, func, file, "        ");
            file << "    } else {\n";
            emit_branch_target(fall_through_addr, func, file, "        ");
            file << "    }\n";
            break;
        }
        
        case RABBITIZER_INSTR_ID_cpu_bgez: {
            uint32_t target_addr = current_pc + 4 + (static_cast<int16_t>(instr.Get_immediate()) << 2);
            uint32_t fall_through_addr = current_pc + 8;
            
            file << "if (branch_taken_" << std::hex << current_pc << ") {\n";
            emit_branch_target(target_addr, func, file, "        ");
            file << "    } else {\n";
            emit_branch_target(fall_through_addr, func, file, "        ");
            file << "    }\n";
            break;
        }

        case RABBITIZER_INSTR_ID_cpu_bc1f: {
            uint32_t target_addr = current_pc + 4 + (static_cast<int16_t>(instr.Get_immediate()) << 2);
            uint32_t fall_through_addr = current_pc + 8;
            
            file << "    if (branch_taken_" << std::hex << current_pc << ") {\n";
            emit_branch_target(target_addr, func, file, "        ");
            file << "    } else {\n";
            emit_branch_target(fall_through_addr, func, file, "        ");
            file << "    }\n";
            break;
        }

        case RABBITIZER_INSTR_ID_cpu_bc1t: {
            uint32_t target_addr = current_pc + 4 + (static_cast<int16_t>(instr.Get_immediate()) << 2);
            uint32_t fall_through_addr = current_pc + 8;
            
            file << "    if (branch_taken_" << std::hex << current_pc << ") {\n";
            emit_branch_target(target_addr, func, file, "        ");
            file << "    } else {\n";
            emit_branch_target(fall_through_addr, func, file, "        ");
            file << "    }\n";
            break;
        }
        
        // Unconditional jump
        case RABBITIZER_INSTR_ID_cpu_j: {
            uint32_t target_addr = (current_pc & 0xF0000000) | (instr.Get_instr_index() << 2);
            emit_branch_target(target_addr, func, file, "");
            break;
        }
        
        case RABBITIZER_INSTR_ID_cpu_b: {
            uint32_t target_addr = current_pc + 4 + (static_cast<int16_t>(instr.Get_immediate()) << 2);
            emit_branch_target(target_addr, func, file, "");
            break;
        }
        
        // CHANGE: Function calls with proper return handling
        case RABBITIZER_INSTR_ID_cpu_jal: {
            uint32_t target_addr = (current_pc & 0xF0000000) | (instr.Get_instr_index() << 2);
            uint32_t return_addr = current_pc + 8;
            file << "   // JAL was called \n";
            file << "   // The address after JAL is: 0x" << std::hex << return_addr << "\n";
            if (func.blocks.size() > current_block_idx + 1){
                file << "   // The next block should be: " << func.blocks[current_block_idx + 1].start_address << std::endl;
            }
            else{
                file << "   // THIS IS THE END OF THE BLOCK: 0x" << std::hex << block.end_address << std::endl;
            }
            file << get_gpr_name(31) << ".UL[0] = 0x" << std::hex << return_addr << ";\n";
            file << "    if (recompiled_functions.count(0x" << std::hex << target_addr << ")) {\n";
            file << "        recompiled_functions[0x" << std::hex << target_addr << "](ctx, 0x" << std::hex << target_addr << ");\n";
            
            // After return, check if we continue in this function
            bool found_return = false;
            for (size_t i = 0; i < func.blocks.size(); ++i) {
            if (func.blocks[i].start_address == return_addr) {
                    file << "        goto Label_" << std::setw(4) << std::setfill('0') << std::dec << i << ";\n";
                    found_return = true;
                    break;
                }
            }
            if (!found_return) {
                file << "\n";
            }
            
            file << "    } else {\n";
            file << "        ctx.cpuRegs.pc = 0x" << std::hex << target_addr << ";\n";
            file << "    }\n";
            break;
        }
        
        // Return from function
        case RABBITIZER_INSTR_ID_cpu_jr: {
            if (static_cast<uint8_t>(instr.GetO32_rs()) == 31) {
                file << "return; // Return from function\n";
            } else {
                // CHANGE: Indirect jump with function lookup
                file << "{\n";
                file << "        uint32_t target = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0];\n";
                file << "        auto func_ptr = find_containing_function(target);\n";
                file << "        if (func_ptr != nullptr) {\n";
                file << "            func_ptr(ctx, target);\n";
                file << "        } else {\n";
                file << "            g_logFile << \"Unimplemented indirect jump to 0x\" << std::hex << target << std::endl;\n";
                file << "            g_logFile << \"This is in FUN_00\" << ctx.cpuRegs.pc << std::endl;\n";
                file << "            exit(1);\n";
                file << "            ctx.cpuRegs.pc = target;\n";
                file << "        }\n";
                file << "    }\n";
            }
            break;
        }
        
        case RABBITIZER_INSTR_ID_cpu_jalr: {
                uint8_t rd = static_cast<uint8_t>(instr.GetO32_rd());
                uint8_t rs = static_cast<uint8_t>(instr.GetO32_rs());
                uint32_t return_addr = current_pc + 8;
                
                file << "    // JALR - Jump and Link Register\n";
                // Set the link register FIRST (before the call)
                file << "    " << get_gpr_name(rd) << ".UL[0] = 0x" << std::hex << return_addr << ";\n";
                file << "    {\n";
                file << "        uint32_t target = " << get_gpr_name(rs) << ".UL[0];\n";
                file << "        auto func_ptr = find_containing_function(target);\n";
                file << "        if (func_ptr != nullptr) {\n";
                file << "            func_ptr(ctx, target);\n";
                
                // After return, continue at the return address if it's within this function
                bool found_return = false;
                for (size_t i = 0; i < func.blocks.size(); ++i) {
                    if (func.blocks[i].start_address == return_addr) {
                        file << "            goto Label_" << std::setw(4) << std::setfill('0') << std::dec << i << ";\n";
                        found_return = true;
                        break;
                    }
                }
                if (!found_return) {
                }
                
                file << "        } else {\n";
                file << "            ctx.cpuRegs.pc = target;\n";
                file << "        }\n";
                file << "    }\n";
                break;
            }
                    
        default:
            file << "// Unhandled control flow: " << instr.getOpcodeName() << "\n";
            file << "    return;\n";
            break;
        }
}

// CHANGE: New helper function to emit branch target code
void Recompiler::emit_branch_target(uint32_t target_addr, const Function& func, std::ofstream& file, const std::string& indent) {
    // Check if target is within current function
    bool found_internal = false;
    for (size_t i = 0; i < func.blocks.size(); ++i) {
        if (func.blocks[i].start_address == target_addr) {
            file << indent << "goto Label_" << std::setw(4) << std::setfill('0') << std::dec << i << ";\n";
            found_internal = true;
            break;
        }
    }
    
    if (!found_internal) {
        // CHANGE: Target is outside current function - call it
        file << indent << "// THIS WAS GENERATED BY EMIT BRANCH TARGET" << std::endl;
        file << indent << "if (recompiled_functions.count(0x" << std::hex << target_addr << ")) {\n";
        file << indent << "    recompiled_functions[0x" << std::hex << target_addr << "](ctx, 0x" << std::hex << target_addr << ");\n";
        file << indent << "    return;\n";
        file << indent << "} else {\n";
        file << indent << "    ctx.cpuRegs.pc = 0x" << std::hex << target_addr << ";\n";
        file << indent << "    return;\n";
        file << indent << "}\n";
    }
}

// ALL INSTRUCTION TRANSLATIONS BELOW ARE UNCHANGED - PRESERVED EXACTLY
void Recompiler::translate_instruction(const rabbitizer::InstructionR5900& instr, std::ofstream& file) {
    switch (instr.getUniqueId()) {
        case RABBITIZER_INSTR_ID_cpu_add:
        case RABBITIZER_INSTR_ID_cpu_addu:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".SD[0] = static_cast<int32_t>(static_cast<int32_t>(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".SL[0]) + static_cast<int32_t>(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".SL[0]));\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_sub:
        case RABBITIZER_INSTR_ID_cpu_subu:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".SD[0] = static_cast<int32_t>(static_cast<int32_t>(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".SL[0]) - static_cast<int32_t>(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".SL[0]));\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_addi:
        case RABBITIZER_INSTR_ID_cpu_addiu:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".SD[0] = static_cast<int32_t>(static_cast<int32_t>(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".SL[0]) + " << format_imm(static_cast<int16_t>(instr.Get_immediate())) << ");\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_multu:
            file << "    {\n";
            file << "        int64_t result = static_cast<int64_t>(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0]) * static_cast<int64_t>(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0]);\n";
            file << "        ctx.cpuRegs.LO.UL[0] = static_cast<uint32_t>(result);\n";
            file << "        ctx.cpuRegs.HI.UL[0] = static_cast<uint32_t>(result >> 32);\n";
            file << "    }\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_div:
        case RABBITIZER_INSTR_ID_cpu_divu:
            file << "    if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".SL[0] != 0) {\n";
            file << "        ctx.cpuRegs.LO.SL[0] = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".SL[0] / " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".SL[0];\n";
            file << "        ctx.cpuRegs.HI.SL[0] = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".SL[0] % " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".SL[0];\n";
            file << "    }\n";
            break;
        case RABBITIZER_INSTR_ID_r5900_vopmula:
            // vopmula - Vector Outer Product Multiply Accumulate
            // Calculates positive cross-product terms and adds to ACC.
            file << "    // vopmula - ACC += OuterProduct(fs, ft)\n";
            file << "    ctx.vuRegs.ACC.x += " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ".y * " 
                 << get_vr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".z;\n";
            file << "    ctx.vuRegs.ACC.y += " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ".z * " 
                 << get_vr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".x;\n";
            file << "    ctx.vuRegs.ACC.z += " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ".x * " 
                 << get_vr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".y;\n";
            break;

        case RABBITIZER_INSTR_ID_r5900_vopmsub:
            // vopmsub fd, fs, ft - Vector Outer Product Multiply Subtract
            // Calculates negative cross-product terms, subtracts from ACC, stores in fd.
            file << "    // vopmsub - fd = ACC - OuterProduct(fs, ft)\n";
            file << "    " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << ".x = ctx.vuRegs.ACC.x - (" 
                 << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ".z * " 
                 << get_vr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".y);\n";
            file << "    " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << ".y = ctx.vuRegs.ACC.y - (" 
                 << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ".x * " 
                 << get_vr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".z);\n";
            file << "    " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << ".z = ctx.vuRegs.ACC.z - (" 
                 << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ".y * " 
                 << get_vr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".x);\n";
            // W is typically undefined or unchanged in cross products; we leave it alone or copy ACC.w depending on exact hardware revision behavior.
            // For safety in HLE:
            file << "    " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << ".w = ctx.vuRegs.ACC.w;\n";
            break;

        // ----------------------------------------------------------------
        // VU0 (COP2) Broadcast Arithmetic (x, y, z, w suffixes)
        // ----------------------------------------------------------------
        case RABBITIZER_INSTR_ID_r5900_vwaitq:
            // vwaitq - Wait for Q register division/sqrt to finish.
            // Since we execute division instantly in HLE, this is a NOP.
            file << "    // vwaitq - Wait for Q (NOP in HLE)\n";
            break;
        case RABBITIZER_INSTR_ID_r5900_vaddy:
            // vaddy fd, fs, ft -> fd = fs + ft.y (Broadcast Y)
            file << "    // vaddy - Vector Add (Broadcast Y)\n";
            file << "    {\n";
            file << "        float val = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".y;\n";
            file << "        " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << " = " 
                 << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << " + VR_reg{val, val, val, val};\n";
            file << "    }\n";
            break;

        case RABBITIZER_INSTR_ID_r5900_vaddz:
            // vaddz fd, fs, ft -> fd = fs + ft.z (Broadcast Z)
            file << "    // vaddz - Vector Add (Broadcast Z)\n";
            file << "    {\n";
            file << "        float val = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".z;\n";
            file << "        " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << " = " 
                 << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << " + VR_reg{val, val, val, val};\n";
            file << "    }\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_or:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UD[0] = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UD[0] | " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UD[0];\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_and:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UD[0] = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UD[0] & " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UD[0];\n";
            break;

        case RABBITIZER_INSTR_ID_cpu_xor:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UD[0] = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UD[0] ^ " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UD[0];\n";
            break;

        case RABBITIZER_INSTR_ID_cpu_nor:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UD[0] = ~(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UD[0] | " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UD[0]);\n";
            break;

        case RABBITIZER_INSTR_ID_cpu_ori:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UD[0] = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UD[0] | static_cast<uint64_t>(" << format_imm(instr.Get_immediate()) << ");\n";
            break;

        case RABBITIZER_INSTR_ID_cpu_xori:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UD[0] = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UD[0] ^ static_cast<uint64_t>(" << format_imm(instr.Get_immediate()) << ");\n";
            break;

        case RABBITIZER_INSTR_ID_cpu_andi:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UD[0] = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UD[0] & static_cast<uint64_t>(" << format_imm(instr.Get_immediate()) << ");\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_sll:
             file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".SD[0] = static_cast<int32_t>(static_cast<uint32_t>(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0]) << " << std::to_string(instr.Get_sa()) << ");\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_srl:
             file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".SD[0] = static_cast<int32_t>(static_cast<uint32_t>(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0]) >> " << std::to_string(instr.Get_sa()) << ");\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_sra:
             file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".SD[0] = static_cast<int32_t>(static_cast<int32_t>(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".SL[0]) >> " << std::to_string(instr.Get_sa()) << ");\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_sllv:
             file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UL[0] = static_cast<uint32_t>(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0]) << (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] & 0x1F);\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_srlv:
             file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UL[0] = static_cast<uint32_t>(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0]) >> (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] & 0x1F);\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_srav:
             file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".SL[0] = static_cast<int32_t>(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".SL[0]) >> (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] & 0x1F);\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_beq:
            file << "    if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] == " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0]) ctx.cpuRegs.pc += " << format_imm(static_cast<int32_t>(static_cast<int16_t>(instr.Get_immediate() << 2))) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_bne:
            file << "    if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] != " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0]) ctx.cpuRegs.pc += " << format_imm(static_cast<int32_t>(static_cast<int16_t>(instr.Get_immediate() << 2))) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_blez:
            file << "    if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".SL[0] <= 0) ctx.cpuRegs.pc += " << format_imm(static_cast<int32_t>(static_cast<int16_t>(instr.Get_immediate() << 2))) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_bgtz:
            file << "    if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".SL[0] > 0) ctx.cpuRegs.pc += " << format_imm(static_cast<int32_t>(static_cast<int16_t>(instr.Get_immediate() << 2))) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_bltz:
            file << "    if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".SL[0] < 0) ctx.cpuRegs.pc += " << format_imm(static_cast<int32_t>(static_cast<int16_t>(instr.Get_immediate() << 2))) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_bgez:
            file << "    if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".SL[0] >= 0) ctx.cpuRegs.pc += " << format_imm(static_cast<int32_t>(static_cast<int16_t>(instr.Get_immediate() << 2))) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_j:
            file << "    ctx.cpuRegs.pc = (ctx.cpuRegs.pc & 0xF0000000) | " << format_imm(instr.Get_instr_index() << 2) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_jal:
            file << "    " << get_gpr_name(31) << ".UL[0] = ctx.cpuRegs.pc + 8;\n";
            file << "    ctx.cpuRegs.pc = (ctx.cpuRegs.pc & 0xF0000000) | " << format_imm(instr.Get_instr_index() << 2) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_jr:
            file << "    ctx.cpuRegs.pc = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0];\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_jalr:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UL[0] = ctx.cpuRegs.pc + 8;\n";
            file << "    ctx.cpuRegs.pc = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0];\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_lb:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".SD[0] = static_cast<int32_t>(static_cast<int8_t>(memory::read<uint8_t>(static_cast<uint32_t>("<< get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0]) + " << format_imm(static_cast<int16_t>(instr.Get_immediate())) << ")));\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_lbu:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UD[0] = static_cast<uint64_t>(memory::read<uint8_t>(static_cast<uint32_t>(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0]) + " << format_imm(static_cast<int16_t>(instr.Get_immediate())) << "));\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_lh:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".SD[0] = static_cast<int32_t>(static_cast<int16_t>(memory::read<uint16_t>(static_cast<uint32_t>(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0]) + " << format_imm(static_cast<int16_t>(instr.Get_immediate())) << ")));\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_lhu:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UD[0] = static_cast<uint64_t>(memory::read<uint16_t>(static_cast<uint32_t>(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0]) + " << format_imm(static_cast<int16_t>(instr.Get_immediate())) << "));\n";
            break;
    case RABBITIZER_INSTR_ID_cpu_lw:
        file << "    // lw instruction - 32-bit load\n";
        // CHANGED: static_cast<uint8_t> -> static_cast<int>
        file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0]) + " << format_imm(static_cast<int16_t>(instr.Get_immediate())) << "));\n";
        break;
        case RABBITIZER_INSTR_ID_cpu_lwu:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UD[0] = static_cast<uint64_t>(memory::read<uint32_t>(static_cast<uint32_t>(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0]) + " << format_imm(static_cast<int16_t>(instr.Get_immediate())) << "));\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_sb:
            file << "    memory::write<uint8_t>(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] + " << format_imm(static_cast<int16_t>(instr.Get_immediate())) << ", " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0]);\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_sh:
            file << "    memory::write<uint16_t>(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] + " << format_imm(static_cast<int16_t>(instr.Get_immediate())) << ", " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0]);\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_sw:
            file << "    memory::write<uint32_t>(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] + " << format_imm(static_cast<int16_t>(instr.Get_immediate())) << ", " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0]);\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_lui:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".SD[0] = static_cast<int32_t>(" << format_imm(instr.Get_immediate() << 16) << ");\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_ld:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UD[0] = memory::read<uint64_t>(static_cast<uint32_t>(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0]) + " << format_imm(static_cast<int16_t>(instr.Get_immediate())) << ");\n";
            break;
        case RABBITIZER_INSTR_ID_r5900_sq:
            file << "    // sq instruction - 128-bit store\n";
            file << "    memory::write_quad(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] + " << format_imm(static_cast<int16_t>(instr.Get_immediate())) << ", *reinterpret_cast<const QuadWord*>(&" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << "));\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_add_s:
            file << "    " << get_fpr_name(static_cast<uint8_t>(instr.GetO32_fd())) << ".f = " << get_fpr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ".f + " << get_fpr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".f;\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_sub_s:
            file << "    " << get_fpr_name(static_cast<uint8_t>(instr.GetO32_fd())) << ".f = " << get_fpr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ".f - " << get_fpr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".f;\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_mul_s:
            file << "    " << get_fpr_name(static_cast<uint8_t>(instr.GetO32_fd())) << ".f = " << get_fpr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ".f * " << get_fpr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".f;\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_div_s:
            file << "    " << get_fpr_name(static_cast<uint8_t>(instr.GetO32_fd())) << ".f = " << get_fpr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ".f / " << get_fpr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".f;\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_mtc1:
            file << "    " << get_fpr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ".UL = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0];\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_mfc1:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".SD[0] = static_cast<int32_t>(" << get_fpr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ".UL);\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_bnez:
            file << "    if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] != 0) ctx.cpuRegs.pc += " << format_imm(static_cast<int32_t>(static_cast<int16_t>(instr.Get_immediate() << 2))) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_bnel:
            file << "    if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] != " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0]) { ctx.cpuRegs.pc += " << format_imm(static_cast<int32_t>(static_cast<int16_t>(instr.Get_immediate() << 2))) << "; } else { /* Branch likely, nullify delay slot */ }\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_beql:
             file << " /* Some reason beql is being executed here. Check to see if beql is being executed by translate_control_flow. Branch likely, nullify delay slot */ }\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_blezl:
            file << "    if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".SL[0] <= 0) { ctx.cpuRegs.pc += " << format_imm(static_cast<int32_t>(static_cast<int16_t>(instr.Get_immediate() << 2))) << "; } else { /* Branch likely, nullify delay slot */ }\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_bltzl:
            file << "    if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".SL[0] < 0) { ctx.cpuRegs.pc += " << format_imm(static_cast<int32_t>(static_cast<int16_t>(instr.Get_immediate() << 2))) << "; } else { /* Branch likely, nullify delay slot */ }\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_bgezl:
            file << "    if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".SL[0] >= 0) { ctx.cpuRegs.pc += " << format_imm(static_cast<int32_t>(static_cast<int16_t>(instr.Get_immediate() << 2))) << "; } else { /* Branch likely, nullify delay slot */ }\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_daddiu:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".SD[0] = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".SD[0] + " << format_imm(static_cast<int16_t>(instr.Get_immediate())) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_daddu:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".SD[0] = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".SD[0] + " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".SD[0];\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_ddivu:
            file << "    if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UD[0] != 0) {\n";
            file << "        ctx.cpuRegs.LO.UD[0] = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UD[0] / " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UD[0];\n";
            file << "        ctx.cpuRegs.HI.UD[0] = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UD[0] % " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UD[0];\n";
            file << "    }\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_mtlo: {
            uint8_t rs = static_cast<uint8_t>(instr.GetO32_rs());
            
            file << "    // mtlo - Move To LO\n";
            file << "    ctx.cpuRegs.LO.UD[0] = " << get_gpr_name(rs) << ".UD[0];\n";
            break;
        }
        case RABBITIZER_INSTR_ID_r5900_madd: {
            uint8_t rd = static_cast<uint8_t>(instr.GetO32_rd());
            uint8_t rs = static_cast<uint8_t>(instr.GetO32_rs());
            uint8_t rt = static_cast<uint8_t>(instr.GetO32_rt());
            
            file << "    // madd - Multiply-Add (signed)\n";
            file << "    {\n";
            file << "        int64_t product = (int64_t)" << get_gpr_name(rs) << ".SL[0] * (int64_t)" << get_gpr_name(rt) << ".SL[0];\n";
            file << "        int64_t acc = ((int64_t)ctx.cpuRegs.HI.SL[0] << 32) | (uint32_t)ctx.cpuRegs.LO.UL[0];\n";
            file << "        int64_t result = acc + product;\n";
            file << "        ctx.cpuRegs.LO.SD[0] = (int32_t)(result & 0xFFFFFFFF);\n";
            file << "        ctx.cpuRegs.HI.SD[0] = (int32_t)(result >> 32);\n";
            // Add this line:
            if (rd != 0) {
                file << "// rd != 0 " << std::endl;
                file << "        " << get_gpr_name(rd) << ".SD[0] = ctx.cpuRegs.LO.SL[0];\n";
            }
            file << "    }\n";
            break;
        }
        case RABBITIZER_INSTR_ID_cpu_cvt_w_s: {
            // cvt.w.s fd, fs - Convert Single Float to Word (32-bit signed integer)
            uint8_t fd = static_cast<uint8_t>(instr.GetO32_fd());
            uint8_t fs = static_cast<uint8_t>(instr.GetO32_fs());
            
            file << "    // cvt.w.s - Convert Float to Word (truncate toward zero)\n";
            file << "    {\n";
            file << "        float fval = " << get_fpr_name(fs) << ".f;\n";
            file << "        int32_t result;\n";
            file << "        if (fval >= 2147483648.0f) result = 0x7FFFFFFF;\n";
            file << "        else if (fval < -2147483648.0f) result = 0x80000000;\n";
            file << "        else result = static_cast<int32_t>(fval);\n";
            file << "        " << get_fpr_name(fd) << ".SL = result;\n";
            file << "    }\n";
            break;
}
        case RABBITIZER_INSTR_ID_cpu_slti:
            // slti rt, rs, immediate - Set on Less Than Immediate (Signed)
            // if GPR[rs] < sign_extended(immediate) then GPR[rt] = 1 else GPR[rt] = 0
            file << "    // slti - Set on Less Than Immediate (Signed)\n";
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UD[0] = (" 
                << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".SL[0] < " 
                << format_imm(static_cast<int16_t>(instr.Get_immediate())) << ") ? 1 : 0;\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_lwc1:
            // lwc1 ft, offset(base) - Load Word to FPU
            file << "    // lwc1 - Load Word to Coprocessor 1\n";
            file << "    " << get_fpr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".UL = memory::read<uint32_t>(static_cast<uint32_t>(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0]) + " << format_imm(static_cast<int16_t>(instr.Get_immediate())) << ");\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_swc1:
            // swc1 ft, offset(base) - Store Word from FPU
            file << "    // swc1 - Store Word from Coprocessor 1\n";
            file << "    memory::write<uint32_t>(static_cast<uint32_t>(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0]) + " << format_imm(static_cast<int16_t>(instr.Get_immediate())) << ", "
                << get_fpr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".UL);\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_mov_s:
            // mov.s fd, fs - Move Single
            file << "    // mov.s - Move Single (FPU)\n";
            file << "    " << get_fpr_name(static_cast<uint8_t>(instr.GetO32_fd())) << ".f = "
                << get_fpr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ".f;\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_cvt_s_w:
            // cvt.s.w fd, fs - Convert Word (integer) to Single (float)
            file << "    // cvt.s.w - Convert Word to Single\n";
            file << "    " << get_fpr_name(static_cast<uint8_t>(instr.GetO32_fd())) << ".f = static_cast<float>("
                << get_fpr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ".SL);\n";
            break;
        case RABBITIZER_INSTR_ID_r5900_pnor:
            // pnor rd, rs, rt - Parallel NOR (128-bit)
            file << "    // pnor - Parallel NOR\n";
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UD[0] = ~("
                << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UD[0] | "
                << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UD[0]);\n";
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UD[1] = ~("
                << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UD[1] | "
                << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UD[1]);\n";
            break;
        case RABBITIZER_INSTR_ID_r5900_pand:
            // pand rd, rs, rt - Parallel AND (128-bit)
            file << "    // pand - Parallel AND\n";
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UD[0] = "
                << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UD[0] & "
                << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UD[0];\n";
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UD[1] = "
                << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UD[1] & "
                << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UD[1];\n";
            break;

        case RABBITIZER_INSTR_ID_r5900_por:
            // por rd, rs, rt - Parallel OR (128-bit)
            file << "    // por - Parallel OR\n";
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UD[0] = "
                << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UD[0] | "
                << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UD[0];\n";
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UD[1] = "
                << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UD[1] | "
                << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UD[1];\n";
            break;
case RABBITIZER_INSTR_ID_r5900_vaddq:
            // vaddq fd, fs, (Q) -> fd = fs + Q
            file << "    // vaddq - Vector Add Q\n";
            file << "    {\n";
            file << "        float q = ctx.vuRegs.Q;\n";
            file << "        " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << " = " 
                 << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << " + VR_reg{q, q, q, q};\n";
            file << "    }\n";
            break;

        case RABBITIZER_INSTR_ID_r5900_vmulq:
            // vmulq fd, fs, (Q) -> fd = fs * Q
            file << "    // vmulq - Vector Multiply Q\n";
            file << "    {\n";
            file << "        float q = ctx.vuRegs.Q;\n";
            file << "        " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << " = " 
                 << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << " * VR_reg{q, q, q, q};\n";
            file << "    }\n";
            break;

        // ----------------------------------------------------------------
        // VU0 (COP2) Broadcast & Accumulator Arithmetic
        // ----------------------------------------------------------------

        case RABBITIZER_INSTR_ID_r5900_vsubx:
            // vsubx fd, fs, ft -> fd = fs - ft.x (Broadcast X)
            file << "    // vsubx - Vector Subtract (Broadcast X)\n";
            file << "    {\n";
            file << "        float val = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".x;\n";
            file << "        " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << " = " 
                 << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << " - VR_reg{val, val, val, val};\n";
            file << "    }\n";
            break;

        case RABBITIZER_INSTR_ID_r5900_vmaddaw:
            // vmaddaw fs, ft -> ACC += fs * ft.w
            // Note: This instruction updates ACC but does NOT write to a general register (fd is unused).
            file << "    // vmaddaw - Multiply-Add Accumulator (Broadcast W)\n";
            file << "    {\n";
            file << "        float val = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".w;\n";
            file << "        ctx.vuRegs.ACC = ctx.vuRegs.ACC + (" 
                 << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << " * VR_reg{val, val, val, val});\n";
            file << "    }\n";
            break;
case RABBITIZER_INSTR_ID_r5900_vsubaz:
            // vsubaz fs, ft -> ACC = fs - ft.z
            // Subtracts broadcast Z from fs and stores result in ACC.
            file << "    // vsubaz - Vector Subtract into Accumulator (Broadcast Z)\n";
            file << "    {\n";
            file << "        float val = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".z;\n";
            file << "        ctx.vuRegs.ACC = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) 
                 << " - VR_reg{val, val, val, val};\n";
            file << "    }\n";
            break;

        case RABBITIZER_INSTR_ID_r5900_vmsubay:
            // vmsubay fs, ft -> ACC -= fs * ft.y
            // Multiply-Subtract from Accumulator using Broadcast Y.
            file << "    // vmsubay - Multiply-Subtract Accumulator (Broadcast Y)\n";
            file << "    {\n";
            file << "        float val = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".y;\n";
            file << "        ctx.vuRegs.ACC = ctx.vuRegs.ACC - (" 
                 << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << " * VR_reg{val, val, val, val});\n";
            file << "    }\n";
            break;

        case RABBITIZER_INSTR_ID_r5900_vmsubx:
            // vmsubx fd, fs, ft -> fd = ACC - (fs * ft.x)
            // Multiply-Subtract, result stored in fd (ACC unchanged).
            file << "    // vmsubx - Vector Multiply-Subtract (Broadcast X)\n";
            file << "    {\n";
            file << "        float val = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".x;\n";
            file << "        " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << " = ctx.vuRegs.ACC - (" 
                 << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << " * VR_reg{val, val, val, val});\n";
            file << "    }\n";
            break;

        case RABBITIZER_INSTR_ID_r5900_vmulax:
            // vmulax fs, ft -> ACC = fs * ft.x
            // Multiply into Accumulator using Broadcast X.
            file << "    // vmulax - Vector Multiply into Accumulator (Broadcast X)\n";
            file << "    {\n";
            file << "        float val = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".x;\n";
            file << "        ctx.vuRegs.ACC = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) 
                 << " * VR_reg{val, val, val, val};\n";
            file << "    }\n";
            break;
    case RABBITIZER_INSTR_ID_r5900_vmadday:
            // vmadday fs, ft -> ACC += fs * ft.y
            file << "    // vmadday - Multiply-Add Accumulator (Broadcast Y)\n";
            file << "    {\n";
            file << "        float val = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".y;\n";
            file << "        ctx.vuRegs.ACC = ctx.vuRegs.ACC + (" 
                 << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << " * VR_reg{val, val, val, val});\n";
            file << "    }\n";
            break;

        case RABBITIZER_INSTR_ID_r5900_vmaddaz:
            // vmaddaz fs, ft -> ACC += fs * ft.z
            file << "    // vmaddaz - Multiply-Add Accumulator (Broadcast Z)\n";
            file << "    {\n";
            file << "        float val = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".z;\n";
            file << "        ctx.vuRegs.ACC = ctx.vuRegs.ACC + (" 
                 << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << " * VR_reg{val, val, val, val});\n";
            file << "    }\n";
            break;

        case RABBITIZER_INSTR_ID_r5900_vmaddz:
            // vmaddz fd, fs, ft -> ACC += fs * ft.z, fd = ACC
            file << "    // vmaddz - Vector Multiply-Add (Broadcast Z)\n";
            file << "    {\n";
            file << "        float val = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".z;\n";
            file << "        ctx.vuRegs.ACC = ctx.vuRegs.ACC + (" 
                 << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << " * VR_reg{val, val, val, val});\n";
            file << "        " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << " = ctx.vuRegs.ACC;\n";
            file << "    }\n";
            break;

        case RABBITIZER_INSTR_ID_r5900_vmaddw:
            // vmaddw fd, fs, ft -> ACC += fs * ft.w, fd = ACC
            file << "    // vmaddw - Vector Multiply-Add (Broadcast W)\n";
            file << "    {\n";
            file << "        float val = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".w;\n";
            file << "        ctx.vuRegs.ACC = ctx.vuRegs.ACC + (" 
                 << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << " * VR_reg{val, val, val, val});\n";
            file << "        " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << " = ctx.vuRegs.ACC;\n";
            file << "    }\n";
            break;

        case RABBITIZER_INSTR_ID_r5900_vmadd:
            // vmadd fd, fs, ft -> ACC += fs * ft, fd = ACC
            file << "    // vmadd - Vector Multiply-Add\n";
            file << "    ctx.vuRegs.ACC = ctx.vuRegs.ACC + (" 
                 << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << " * " 
                 << get_vr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ");\n";
            file << "    " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << " = ctx.vuRegs.ACC;\n";
            break;

        case RABBITIZER_INSTR_ID_r5900_vmsubw:
            // vmsubw fd, fs, ft -> fd = ACC - (fs * ft.w)
            file << "    // vmsubw - Vector Multiply-Subtract (Broadcast W)\n";
            file << "    {\n";
            file << "        float val = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".w;\n";
            file << "        " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << " = ctx.vuRegs.ACC - (" 
                 << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << " * VR_reg{val, val, val, val});\n";
            file << "    }\n";
            break;

        case RABBITIZER_INSTR_ID_r5900_vmula:
            // vmula fs, ft -> ACC = fs * ft
            file << "    // vmula - Vector Multiply into Accumulator\n";
            file << "    ctx.vuRegs.ACC = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) 
                 << " * " << get_vr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ";\n";
            break;

        // ----------------------------------------------------------------
        // VU0 (COP2) Misc & I Register
        // ----------------------------------------------------------------

        case RABBITIZER_INSTR_ID_r5900_vmuli:
            // vmuli fd, fs, I -> fd = fs * I
            // Uses the special floating point I register.
            file << "    // vmuli - Vector Multiply by I Register\n";
            file << "    {\n";
            file << "        float val = ctx.vuRegs.I;\n";
            file << "        " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << " = " 
                 << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << " * VR_reg{val, val, val, val};\n";
            file << "    }\n";
            break;

        case RABBITIZER_INSTR_ID_r5900_vmr32:
            // vmr32 fd, fs -> Rotate elements (XYZW -> YZWX)
            file << "    // vmr32 - Vector Move Rotate 32-bit\n";
            file << "    {\n";
            file << "        auto src = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ";\n";
            file << "        " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << " = VR_reg{src.y, src.z, src.w, src.x};\n";
            file << "    }\n";
            break;
// ----------------------------------------------------------------
        // VU0 (COP2) Broadcast Accumulator Arithmetic
        // ----------------------------------------------------------------

        case RABBITIZER_INSTR_ID_r5900_vaddaw:
            // vaddaw fs, ft -> ACC = fs + ft.w
            // Adds fs and broadcast ft.w, stores result in Accumulator.
            // Note: Unlike vmadd*, this is an ADD, not a Multiply-Add.
            file << "    // vaddaw - Vector Add into Accumulator (Broadcast W)\n";
            file << "    {\n";
            file << "        float val = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".w;\n";
            file << "        ctx.vuRegs.ACC = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) 
                 << " + VR_reg{val, val, val, val};\n";
            file << "    }\n";
            break;

        case RABBITIZER_INSTR_ID_r5900_vmsubax:
            // vmsubax fs, ft -> ACC -= fs * ft.x
            // Multiply-Subtract from Accumulator using Broadcast X.
            file << "    // vmsubax - Multiply-Subtract Accumulator (Broadcast X)\n";
            file << "    {\n";
            file << "        float val = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".x;\n";
            file << "        ctx.vuRegs.ACC = ctx.vuRegs.ACC - (" 
                 << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << " * VR_reg{val, val, val, val});\n";
            file << "    }\n";
            break;

        case RABBITIZER_INSTR_ID_r5900_vmsuby:
            // vmsuby fd, fs, ft -> fd = ACC - (fs * ft.y)
            // Multiply-Subtract, result to fd, Broadcast Y.
            file << "    // vmsuby - Vector Multiply-Subtract (Broadcast Y)\n";
            file << "    {\n";
            file << "        float val = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".y;\n";
            file << "        " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << " = ctx.vuRegs.ACC - (" 
                 << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << " * VR_reg{val, val, val, val});\n";
            file << "    }\n";
            break;

        case RABBITIZER_INSTR_ID_r5900_vmsubz:
            // vmsubz fd, fs, ft -> fd = ACC - (fs * ft.z)
            // Multiply-Subtract, result to fd, Broadcast Z.
            file << "    // vmsubz - Vector Multiply-Subtract (Broadcast Z)\n";
            file << "    {\n";
            file << "        float val = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".z;\n";
            file << "        " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << " = ctx.vuRegs.ACC - (" 
                 << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << " * VR_reg{val, val, val, val});\n";
            file << "    }\n";
            break;

        // ----------------------------------------------------------------
        // VU0 (COP2) Broadcast Arithmetic (Sub/Mul)
        // ----------------------------------------------------------------

        case RABBITIZER_INSTR_ID_r5900_vsubw:
            // vsubw fd, fs, ft -> fd = fs - ft.w
            file << "    // vsubw - Vector Subtract (Broadcast W)\n";
            file << "    {\n";
            file << "        float val = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".w;\n";
            file << "        " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << " = " 
                 << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << " - VR_reg{val, val, val, val};\n";
            file << "    }\n";
            break;

        case RABBITIZER_INSTR_ID_r5900_vmulx:
            // vmulx fd, fs, ft -> fd = fs * ft.x
            file << "    // vmulx - Vector Multiply (Broadcast X)\n";
            file << "    {\n";
            file << "        float val = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".x;\n";
            file << "        " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << " = " 
                 << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << " * VR_reg{val, val, val, val};\n";
            file << "    }\n";
            break;

        case RABBITIZER_INSTR_ID_r5900_vmove:
            // vmove fd, fs -> fd = fs
            file << "    // vmove - Vector Move\n";
            file << "    " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << " = " 
                 << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ";\n";
            break;

        // ----------------------------------------------------------------
        // MMI Parallel Extensions
        // ----------------------------------------------------------------

        case RABBITIZER_INSTR_ID_r5900_pextlw:
            // pextlw rd, rs, rt
            // Extends the lower 32-bit words of rs and rt into rd (interleaved).
            // rd[0] = rt[0], rd[1] = rs[0], rd[2] = rt[1], rd[3] = rs[1]
            file << "    // pextlw - Parallel Extend Lower Word\n";
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UL[0] = " 
                 << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0];\n";
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UL[1] = " 
                 << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0];\n";
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UL[2] = " 
                 << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[1];\n";
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UL[3] = " 
                 << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[1];\n";
            break;

        case RABBITIZER_INSTR_ID_r5900_pextuw:
            // pextuw rd, rs, rt
            // Extends the upper 32-bit words of rs and rt into rd (interleaved).
            // rd[0] = rt[2], rd[1] = rs[2], rd[2] = rt[3], rd[3] = rs[3]
            file << "    // pextuw - Parallel Extend Upper Word\n";
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UL[0] = " 
                 << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[2];\n";
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UL[1] = " 
                 << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[2];\n";
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UL[2] = " 
                 << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[3];\n";
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UL[3] = " 
                 << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[3];\n";
            break;

        case RABBITIZER_INSTR_ID_r5900_vaddw:
            // vaddw fd, fs, ft -> fd = fs + ft.w
            file << "    // vaddw - Vector Add (Broadcast W)\n";
            file << "    {\n";
            file << "        float val = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".w;\n";
            file << "        " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << " = " 
                 << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << " + VR_reg{val, val, val, val};\n";
            file << "    }\n";
            break;
case RABBITIZER_INSTR_ID_cpu_swl:
            // swl rt, offset(base) - Store Word Left
            // Little Endian: Stores the most significant part of rt to memory 
            // starting at effective_addr up to the end of the word.
            file << "    // swl - Store Word Left (Little Endian logic)\n";
            file << "    {\n";
            file << "        uint32_t addr = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) 
                 << ".UL[0] + " << format_imm(static_cast<int16_t>(instr.Get_immediate())) << ";\n";
            file << "        uint32_t aligned_addr = addr & ~3;\n";
            file << "        uint32_t shift = (addr & 3) * 8;\n";
            file << "        uint32_t mem_val = memory::read<uint32_t>(aligned_addr);\n";
            file << "        uint32_t reg_val = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0];\n";
            file << "        uint32_t mask = 0xFFFFFFFF >> (24 - shift);\n";
            file << "        memory::write<uint32_t>(aligned_addr, (mem_val & ~mask) | (reg_val >> (24 - shift)));\n";
            file << "    }\n";
            break;

        case RABBITIZER_INSTR_ID_cpu_swr:
            // swr rt, offset(base) - Store Word Right
            // Little Endian: Stores the least significant part of rt to memory
            // starting at the beginning of the word up to effective_addr.
            file << "    // swr - Store Word Right (Little Endian logic)\n";
            file << "    {\n";
            file << "        uint32_t addr = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) 
                 << ".UL[0] + " << format_imm(static_cast<int16_t>(instr.Get_immediate())) << ";\n";
            file << "        uint32_t aligned_addr = addr & ~3;\n";
            file << "        uint32_t shift = (addr & 3) * 8;\n";
            file << "        uint32_t mem_val = memory::read<uint32_t>(aligned_addr);\n";
            file << "        uint32_t reg_val = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0];\n";
            file << "        uint32_t mask = 0xFFFFFFFF << shift;\n";
            file << "        memory::write<uint32_t>(aligned_addr, (mem_val & ~mask) | (reg_val << shift));\n";
            file << "    }\n";
            break;

        // ----------------------------------------------------------------
        // VU0 (COP2) Min/Max Instructions
        // ----------------------------------------------------------------

        case RABBITIZER_INSTR_ID_r5900_vmini:
            // vmini fd, fs, ft -> fd = min(fs, ft)
            // Note: The instruction is named 'vmini' but performs standard float minimum.
            file << "    // vmini - Vector Minimum\n";
            file << "    {\n";
            file << "        auto fs = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ";\n";
            file << "        auto ft = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ";\n";
            file << "        " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << " = VR_reg{\n";
            file << "            std::min(fs.x, ft.x),\n";
            file << "            std::min(fs.y, ft.y),\n";
            file << "            std::min(fs.z, ft.z),\n";
            file << "            std::min(fs.w, ft.w)\n";
            file << "        };\n";
            file << "    }\n";
            break;

        case RABBITIZER_INSTR_ID_r5900_vmax:
            // vmax fd, fs, ft -> fd = max(fs, ft)
            file << "    // vmax - Vector Maximum\n";
            file << "    {\n";
            file << "        auto fs = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ";\n";
            file << "        auto ft = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ";\n";
            file << "        " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << " = VR_reg{\n";
            file << "            std::max(fs.x, ft.x),\n";
            file << "            std::max(fs.y, ft.y),\n";
            file << "            std::max(fs.z, ft.z),\n";
            file << "            std::max(fs.w, ft.w)\n";
            file << "        };\n";
            file << "    }\n";
            break;

        // ----------------------------------------------------------------
        // VU0 (COP2) Broadcast Min/Max
        // ----------------------------------------------------------------

        case RABBITIZER_INSTR_ID_r5900_vminiy:
            // vminiy fd, fs, ft -> fd = min(fs, ft.y)
            file << "    // vminiy - Vector Minimum (Broadcast Y)\n";
            file << "    {\n";
            file << "        float val = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".y;\n";
            file << "        auto fs = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ";\n";
            file << "        " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << " = VR_reg{\n";
            file << "            std::min(fs.x, val), std::min(fs.y, val), std::min(fs.z, val), std::min(fs.w, val)\n";
            file << "        };\n";
            file << "    }\n";
            break;

        case RABBITIZER_INSTR_ID_r5900_vminiw:
            // vminiw fd, fs, ft -> fd = min(fs, ft.w)
            file << "    // vminiw - Vector Minimum (Broadcast W)\n";
            file << "    {\n";
            file << "        float val = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".w;\n";
            file << "        auto fs = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ";\n";
            file << "        " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << " = VR_reg{\n";
            file << "            std::min(fs.x, val), std::min(fs.y, val), std::min(fs.z, val), std::min(fs.w, val)\n";
            file << "        };\n";
            file << "    }\n";
            break;

        case RABBITIZER_INSTR_ID_r5900_vminiz:
            // vminiz fd, fs, ft -> fd = min(fs, ft.z)
            file << "    // vminiz - Vector Minimum (Broadcast Z)\n";
            file << "    {\n";
            file << "        float val = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".z;\n";
            file << "        auto fs = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ";\n";
            file << "        " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << " = VR_reg{\n";
            file << "            std::min(fs.x, val), std::min(fs.y, val), std::min(fs.z, val), std::min(fs.w, val)\n";
            file << "        };\n";
            file << "    }\n";
            break;

        case RABBITIZER_INSTR_ID_r5900_vmaxy:
            // vmaxy fd, fs, ft -> fd = max(fs, ft.y)
            file << "    // vmaxy - Vector Maximum (Broadcast Y)\n";
            file << "    {\n";
            file << "        float val = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".y;\n";
            file << "        auto fs = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ";\n";
            file << "        " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << " = VR_reg{\n";
            file << "            std::max(fs.x, val), std::max(fs.y, val), std::max(fs.z, val), std::max(fs.w, val)\n";
            file << "        };\n";
            file << "    }\n";
            break;

        case RABBITIZER_INSTR_ID_r5900_vmaxw:
            // vmaxw fd, fs, ft -> fd = max(fs, ft.w)
            file << "    // vmaxw - Vector Maximum (Broadcast W)\n";
            file << "    {\n";
            file << "        float val = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".w;\n";
            file << "        auto fs = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ";\n";
            file << "        " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << " = VR_reg{\n";
            file << "            std::max(fs.x, val), std::max(fs.y, val), std::max(fs.z, val), std::max(fs.w, val)\n";
            file << "        };\n";
            file << "    }\n";
            break;

        // ----------------------------------------------------------------
        // VU0 (COP2) Special Instructions
        // ----------------------------------------------------------------

        case RABBITIZER_INSTR_ID_r5900_vclipw:
            // vclipw fs, ft (Broadcast W)
            // Checks if fs components are within range [-|ft.w|, +|ft.w|].
            // Updates ctx.vuRegs.clip_flag. Shift existing flags left by 6.
            file << "    // vclipw - Vector Clip W\n";
            file << "    {\n";
            file << "        auto fs = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ";\n";
            file << "        float w = std::abs(" << get_vr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".w);\n";
            file << "        uint32_t clip = 0;\n";
            file << "        if (fs.x > w) clip |= 0x1;\n";
            file << "        if (fs.x < -w) clip |= 0x2;\n";
            file << "        if (fs.y > w) clip |= 0x4;\n";
            file << "        if (fs.y < -w) clip |= 0x8;\n";
            file << "        if (fs.z > w) clip |= 0x10;\n";
            file << "        if (fs.z < -w) clip |= 0x20;\n";
            file << "        ctx.vuRegs.clip_flag = (ctx.vuRegs.clip_flag << 6) | clip;\n";
            file << "    }\n";
            break;

case RABBITIZER_INSTR_ID_r5900_vmaxz:
            // vmaxz fd, fs, ft -> fd = max(fs, ft.z)
            file << "    // vmaxz - Vector Maximum (Broadcast Z)\n";
            file << "    {\n";
            file << "        float val = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".z;\n";
            file << "        auto fs = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ";\n";
            file << "        " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << " = VR_reg{\n";
            file << "            std::max(fs.x, val), std::max(fs.y, val), std::max(fs.z, val), std::max(fs.w, val)\n";
            file << "        };\n";
            file << "    }\n";
            break;

        case RABBITIZER_INSTR_ID_r5900_vaddax:
            // vaddax fs, ft -> ACC = ACC + (fs + ft.x) ??? 
            // NOTE: The PS2 instruction set defines VADDA as "Add into Accumulator" (ACC = fs + ft).
            // vaddax is ACC = fs + ft.x
            file << "    // vaddax - Vector Add into Accumulator (Broadcast X)\n";
            file << "    {\n";
            file << "        float val = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".x;\n";
            file << "        ctx.vuRegs.ACC = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) 
                 << " + VR_reg{val, val, val, val};\n";
            file << "    }\n";
            break;

        case RABBITIZER_INSTR_ID_r5900_vmsubaw:
            // vmsubaw fs, ft -> ACC -= fs * ft.w
            file << "    // vmsubaw - Multiply-Subtract Accumulator (Broadcast W)\n";
            file << "    {\n";
            file << "        float val = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".w;\n";
            file << "        ctx.vuRegs.ACC = ctx.vuRegs.ACC - (" 
                 << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << " * VR_reg{val, val, val, val});\n";
            file << "    }\n";
            break;

        // ----------------------------------------------------------------
        // MIPS Control
        // ----------------------------------------------------------------

        case RABBITIZER_INSTR_ID_cpu_break:
            file << "    // break - Breakpoint\n";
            // In a recompiler, we typically raise an exception or exit. 
            // For now, we print and exit to catch it during testing.
            file << "    std::cerr << \"[Recompiler] BREAK instruction executed at \" << std::hex << 0x" 
                 << instr.getVram() << " << std::dec << \"\\n\";\n";
            file << "    exit(1);\n";
            break;

        // ----------------------------------------------------------------
        // MMI Parallel Arithmetic (Halfwords)
        // ----------------------------------------------------------------

        case RABBITIZER_INSTR_ID_r5900_paddh:
            // paddh rd, rs, rt - Parallel Add Halfword (8x16-bit)
            file << "    // paddh - Parallel Add Halfword\n";
            file << "    {\n";
            file << "        auto rs = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ";\n";
            file << "        auto rt = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ";\n";
            file << "        auto& rd = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ";\n";
            file << "        for (int i = 0; i < 8; ++i) {\n";
            file << "            rd.SS[i] = rs.SS[i] + rt.SS[i];\n";
            file << "        }\n";
            file << "    }\n";
            break;

        case RABBITIZER_INSTR_ID_r5900_pminh:
            // pminh rd, rs, rt - Parallel Minimum Halfword
            file << "    // pminh - Parallel Minimum Halfword\n";
            file << "    {\n";
            file << "        auto rs = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ";\n";
            file << "        auto rt = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ";\n";
            file << "        auto& rd = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ";\n";
            file << "        for (int i = 0; i < 8; ++i) {\n";
            file << "            rd.SS[i] = std::min(rs.SS[i], rt.SS[i]);\n";
            file << "        }\n";
            file << "    }\n";
            break;

        case RABBITIZER_INSTR_ID_r5900_pmaxh:
            // pmaxh rd, rs, rt - Parallel Maximum Halfword
            file << "    // pmaxh - Parallel Maximum Halfword\n";
            file << "    {\n";
            file << "        auto rs = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ";\n";
            file << "        auto rt = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ";\n";
            file << "        auto& rd = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ";\n";
            file << "        for (int i = 0; i < 8; ++i) {\n";
            file << "            rd.SS[i] = std::max(rs.SS[i], rt.SS[i]);\n";
            file << "        }\n";
            file << "    }\n";
            break;

        // ----------------------------------------------------------------
        // MMI Pack & Shift
        // ----------------------------------------------------------------

        case RABBITIZER_INSTR_ID_r5900_ppacb:
            // ppacb rd, rs, rt - Parallel Pack Byte
            // Packs 8 halfwords from rs and 8 from rt into 16 bytes in rd.
            // rt is packed into lower 64 bits, rs into upper 64 bits.
            // Saturated pack (signed 16-bit -> signed 8-bit).
            file << "    // ppacb - Parallel Pack Byte\n";
            file << "    {\n";
            file << "        auto pack_half = [](int16_t val) -> int8_t {\n";
            file << "            if (val > 127) return 127;\n";
            file << "            if (val < -128) return -128;\n";
            file << "            return static_cast<int8_t>(val);\n";
            file << "        };\n";
            file << "        auto rs = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ";\n";
            file << "        auto rt = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ";\n";
            file << "        auto& rd = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ";\n";
            file << "        for (int i = 0; i < 8; ++i) rd.SC[i] = pack_half(rt.SS[i]);\n";
            file << "        for (int i = 0; i < 8; ++i) rd.SC[i+8] = pack_half(rs.SS[i]);\n";
            file << "    }\n";
            break;

        case RABBITIZER_INSTR_ID_r5900_mtsab:
            // mtsab rs, immediate - Move to Shift Amount Byte
            // Calculates shift amount from register value and immediate.
            // SA = (rs + imm) & 0xF
            file << "    // mtsab - Move to Shift Amount Byte\n";
            file << "    {\n";
            file << "        uint32_t val = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0];\n";
            file << "        int32_t imm = " << format_imm(static_cast<int16_t>(instr.Get_immediate())) << ";\n";
            file << "        ctx.cpuRegs.sa = (val + imm) & 0xF;\n";
            file << "    }\n";
            break;
case RABBITIZER_INSTR_ID_r5900_pextlb:
            // pextlb rd, rs, rt - Parallel Extend Lower Byte
            // Interleaves the lower 8 bytes of rs and rt.
            // rd[0]=rt[0], rd[1]=rs[0], rd[2]=rt[1], rd[3]=rs[1] ...
            file << "    // pextlb - Parallel Extend Lower Byte\n";
            file << "    {\n";
            file << "        auto rs = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ";\n";
            file << "        auto rt = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ";\n";
            file << "        auto& rd = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ";\n";
            file << "        for (int i = 0; i < 8; ++i) {\n";
            file << "            rd.UC[i * 2]     = rt.UC[i];\n";
            file << "            rd.UC[i * 2 + 1] = rs.UC[i];\n";
            file << "        }\n";
            file << "    }\n";
            break;

        case RABBITIZER_INSTR_ID_r5900_pextub:
            // pextub rd, rs, rt - Parallel Extend Upper Byte
            // Interleaves the upper 8 bytes of rs and rt.
            // rd[0]=rt[8], rd[1]=rs[8], rd[2]=rt[9], rd[3]=rs[9] ...
            file << "    // pextub - Parallel Extend Upper Byte\n";
            file << "    {\n";
            file << "        auto rs = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ";\n";
            file << "        auto rt = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ";\n";
            file << "        auto& rd = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ";\n";
            file << "        for (int i = 0; i < 8; ++i) {\n";
            file << "            rd.UC[i * 2]     = rt.UC[i + 8];\n";
            file << "            rd.UC[i * 2 + 1] = rs.UC[i + 8];\n";
            file << "        }\n";
            file << "    }\n";
            break;

        // ----------------------------------------------------------------
        // MMI Parallel Shifts (Halfwords)
        // ----------------------------------------------------------------

        case RABBITIZER_INSTR_ID_r5900_psrlh:
            // psrlh rd, rt, sa - Parallel Shift Right Logical Halfword
            file << "    // psrlh - Parallel Shift Right Logical Halfword\n";
            file << "    {\n";
            // FIX: Use std::dec
            file << "        uint32_t shift = " << std::dec << static_cast<uint32_t>(instr.Get_sa()) << ";\n";
            file << "        auto rt = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ";\n";
            file << "        auto& rd = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ";\n";
            file << "        for (int i = 0; i < 8; ++i) {\n";
            file << "            rd.US[i] = rt.US[i] >> shift;\n";
            file << "        }\n";
            file << "    }\n";
            break;

        case RABBITIZER_INSTR_ID_r5900_psrah:
            // psrah rd, rt, sa - Parallel Shift Right Arithmetic Halfword
            file << "    // psrah - Parallel Shift Right Arithmetic Halfword\n";
            file << "    {\n";
            // FIX: Use std::dec to prevent '15' being written as 'f'
            file << "        uint32_t shift = " << std::dec << static_cast<uint32_t>(instr.Get_sa()) << ";\n";
            file << "        auto rt = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ";\n";
            file << "        auto& rd = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ";\n";
            file << "        for (int i = 0; i < 8; ++i) {\n";
            file << "            rd.SS[i] = rt.SS[i] >> shift;\n";
            file << "        }\n";
            file << "    }\n";
            break;

        case RABBITIZER_INSTR_ID_r5900_psllh:
            // psllh rd, rt, sa - Parallel Shift Left Logical Halfword
            file << "    // psllh - Parallel Shift Left Logical Halfword\n";
            file << "    {\n";
            // FIX: Use std::dec
            file << "        uint32_t shift = " << std::dec << static_cast<uint32_t>(instr.Get_sa()) << ";\n";
            file << "        auto rt = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ";\n";
            file << "        auto& rd = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ";\n";
            file << "        for (int i = 0; i < 8; ++i) {\n";
            file << "            rd.US[i] = rt.US[i] << shift;\n";
            file << "        }\n";
            file << "    }\n";
            break;

        // ----------------------------------------------------------------
        // MMI Parallel Compare & Subtract
        // ----------------------------------------------------------------

        case RABBITIZER_INSTR_ID_r5900_pcgth:
            // pcgth rd, rs, rt - Parallel Compare Greater Than Halfword
            // If rs > rt, result is 0xFFFF (-1), else 0.
            file << "    // pcgth - Parallel Compare Greater Than Halfword\n";
            file << "    {\n";
            file << "        auto rs = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ";\n";
            file << "        auto rt = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ";\n";
            file << "        auto& rd = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ";\n";
            file << "        for (int i = 0; i < 8; ++i) {\n";
            file << "            rd.SS[i] = (rs.SS[i] > rt.SS[i]) ? -1 : 0;\n";
            file << "        }\n";
            file << "    }\n";
            break;

        case RABBITIZER_INSTR_ID_r5900_psubw:
            // psubw rd, rs, rt - Parallel Subtract Word
            file << "    // psubw - Parallel Subtract Word\n";
            file << "    {\n";
            file << "        auto rs = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ";\n";
            file << "        auto rt = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ";\n";
            file << "        auto& rd = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ";\n";
            file << "        for (int i = 0; i < 4; ++i) {\n";
            file << "            rd.SL[i] = rs.SL[i] - rt.SL[i];\n";
            file << "        }\n";
            file << "    }\n";
            break;

        // ----------------------------------------------------------------
        // Pipeline 1 Transfers (hi1 / lo1)
        // ----------------------------------------------------------------

        case RABBITIZER_INSTR_ID_r5900_mfhi1:
            // mfhi1 rd - Move From HI1
            file << "    // mfhi1 - Move From HI1\n";
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UQ = ctx.cpuRegs.HI.UQ;\n";
            break;

        case RABBITIZER_INSTR_ID_r5900_mflo1:
            // mflo1 rd - Move From LO1
            file << "    // mflo1 - Move From LO1\n";
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UQ = ctx.cpuRegs.LO1.UQ;\n";
            break;

        case RABBITIZER_INSTR_ID_r5900_mthi1:
            // mthi1 rs - Move To HI1
            file << "    // mthi1 - Move To HI1\n";
            file << "    ctx.cpuRegs.HI1.UQ = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UQ;\n";
            break;

        case RABBITIZER_INSTR_ID_r5900_mtlo1:
            // mtlo1 rs - Move To LO1
            file << "    // mtlo1 - Move To LO1\n";
            file << "    ctx.cpuRegs.LO1.UQ = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UQ;\n";
            break;

        // ----------------------------------------------------------------
        // Pipeline 0 Transfers (hi) & Special
        // ----------------------------------------------------------------

        case RABBITIZER_INSTR_ID_cpu_mthi:
            // mthi rs - Move To HI (Pipeline 0)
            file << "    // mthi - Move To HI\n";
            file << "    ctx.cpuRegs.HI.UQ = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UQ;\n";
            break;

        case RABBITIZER_INSTR_ID_r5900_mtsa:
            // mtsa rs - Move To Shift Amount
            // Sets the SA register to the value in rs. Used for qfsrv.
            file << "    // mtsa - Move To Shift Amount\n";
            file << "    ctx.cpuRegs.sa = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0];\n";
            break;

        case RABBITIZER_INSTR_ID_r5900_qfsrv:
            // qfsrv rd, rs, rt - Quadword Funnel Shift Right Variable
            // Concatenates rs (high) and rt (low), shifts right by SA * 8 bits.
            file << "    // qfsrv - Quadword Funnel Shift Right Variable\n";
            file << "    {\n";
            // Use 'sa' (lowercase) and inline the logic to avoid function call errors
            file << "        uint32_t sa_val = ctx.cpuRegs.sa;\n";
            file << "        int shift = (sa_val & 0xF) * 8;\n";
            file << "        auto rs = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ";\n";
            file << "        auto rt = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ";\n";
            file << "        auto& rd = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ";\n";
            file << "        \n";
            file << "        uint64_t rs_lo = rs.UD[0];\n";
            file << "        uint64_t rs_hi = rs.UD[1];\n";
            file << "        uint64_t rt_lo = rt.UD[0];\n";
            file << "        uint64_t rt_hi = rt.UD[1];\n";
            file << "        uint64_t res_lo, res_hi;\n";
            file << "        \n";
            file << "        if (shift == 0) {\n";
            file << "            res_lo = rt_lo;\n";
            file << "            res_hi = rt_hi;\n";
            file << "        } else if (shift < 64) {\n";
            file << "            res_lo = (rt_lo >> shift) | (rt_hi << (64 - shift));\n";
            file << "            res_hi = (rt_hi >> shift) | (rs_lo << (64 - shift));\n";
            file << "        } else { // shift >= 64\n";
            file << "            int s = shift - 64;\n";
            file << "            if (s == 0) {\n";
            file << "                res_lo = rt_hi;\n";
            file << "                res_hi = rs_lo;\n";
            file << "            } else {\n";
            file << "                res_lo = (rt_hi >> s) | (rs_lo << (64 - s));\n";
            file << "                res_hi = (rs_lo >> s) | (rs_hi << (64 - s));\n";
            file << "            }\n";
            file << "        }\n";
            file << "        rd.UD[0] = res_lo;\n";
            file << "        rd.UD[1] = res_hi;\n";
            file << "    }\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_neg_s:
            file << "    // neg.s - Negate Single\n";
            file << "    " << get_fpr_name(static_cast<uint8_t>(instr.GetO32_fd())) << ".f = -" 
                 << get_fpr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ".f;\n";
            break;

        case RABBITIZER_INSTR_ID_cpu_abs_s:
            file << "    // abs.s - Absolute Value Single\n";
            file << "    " << get_fpr_name(static_cast<uint8_t>(instr.GetO32_fd())) << ".f = std::abs(" 
                 << get_fpr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ".f);\n";
            break;

        case RABBITIZER_INSTR_ID_r5900_rsqrt_s:
            // rsqrt.s fd, fs -> fd = 1.0 / sqrt(fs)
            file << "    // rsqrt.s - Reciprocal Square Root Single\n";
            file << "    " << get_fpr_name(static_cast<uint8_t>(instr.GetO32_fd())) << ".f = 1.0f / std::sqrt(" 
                 << get_fpr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ".f);\n";
            break;

        case RABBITIZER_INSTR_ID_r5900_max_s:
            // max.s fd, fs, ft
            file << "    // max.s - Maximum Single\n";
            file << "    " << get_fpr_name(static_cast<uint8_t>(instr.GetO32_fd())) << ".f = std::max(" 
                 << get_fpr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ".f, " 
                 << get_fpr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".f);\n";
            break;

        case RABBITIZER_INSTR_ID_r5900_min_s:
            // min.s fd, fs, ft
            file << "    // min.s - Minimum Single\n";
            file << "    " << get_fpr_name(static_cast<uint8_t>(instr.GetO32_fd())) << ".f = std::min(" 
                 << get_fpr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ".f, " 
                 << get_fpr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".f);\n";
            break;

        // ----------------------------------------------------------------
        // VU0 (COP2) Load/Store & Control
        // ----------------------------------------------------------------

        case RABBITIZER_INSTR_ID_r5900_lqc2:
            // lqc2 vt, offset(base) (Rabbitizer maps 'vt' to the rt field)
            file << "    // lqc2 - Load Quadword to COP2 (VU0 VF)\n";
            // We cast the target VF register to QuadWord* to use the memory::read_quad helper
            file << "    memory::read_quad(" 
                 << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] + " 
                 << format_imm(static_cast<int16_t>(instr.Get_immediate())) << ", " 
                 << "*reinterpret_cast<QuadWord*>(&" << get_vr_name(static_cast<uint8_t>(instr.GetO32_rt())) << "));\n";
            break;

        case RABBITIZER_INSTR_ID_r5900_sqc2:
            // sqc2 vt, offset(base)
            file << "    // sqc2 - Store Quadword from COP2 (VU0 VF)\n";
            file << "    memory::write_quad(" 
                 << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] + " 
                 << format_imm(static_cast<int16_t>(instr.Get_immediate())) << ", " 
                 << "*reinterpret_cast<const QuadWord*>(&" << get_vr_name(static_cast<uint8_t>(instr.GetO32_rt())) << "));\n";
            break;

        case RABBITIZER_INSTR_ID_r5900_vnop:
            file << "    // vnop - Vector No Operation\n";
            // No code needed
            break;

        case RABBITIZER_INSTR_ID_r5900_vcallms:
            // vcallms - Call Micro Subroutine (VU0)
            // This starts the VU0 micro-program at the address immediate.
            // Since this is a static recompiler for the EE Core, you likely want to 
            // trigger the VU0 emulator here. For now, we can log or stub it.
            file << "    // vcallms - Call Micro Subroutine (Stubbed)\n";
            file << "    // TODO: Invoke VU0 microcode execution at " << format_imm(instr.Get_immediate()) << "\n";
            break;

        case RABBITIZER_INSTR_ID_r5900_pxor:
            // pxor rd, rs, rt - Parallel XOR (128-bit)
            file << "    // pxor - Parallel XOR\n";
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UD[0] = "
                << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UD[0] ^ "
                << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UD[0];\n";
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UD[1] = "
                << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UD[1] ^ "
                << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UD[1];\n";
            break;
        case RABBITIZER_INSTR_ID_r5900_pcpyud:
            // pcpyud rd, rs, rt - Parallel Copy Upper Doubleword
            file << "    // pcpyud - Parallel Copy Upper Doubleword\n";
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UD[1] = "
                << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UD[1];\n";
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UD[0] = "
                << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UD[1];\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_sltiu:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UD[0] = (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] < " << format_imm(static_cast<uint16_t>(instr.Get_immediate())) << ") ? 1 : 0;\n";
            break;
        case RABBITIZER_INSTR_ID_r5900_psubb:
            // psubb rd, rs, rt - Parallel Subtract Byte (16 x 8-bit)
            file << "    // psubb - Parallel Subtract Byte\n";
            file << "    for (int i = 0; i < 16; i++) {\n";
            file << "        " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UC[i] = "
                << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UC[i] - "
                << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UC[i];\n";
            file << "    }\n";
            break;
        case RABBITIZER_INSTR_ID_r5900_c1__sqrt_s:
            file << "    // c1 (sqrt.s) - Square Root Single\n";
            file << "    " << get_fpr_name(static_cast<uint8_t>(instr.GetO32_fd())) << ".f = sqrt(" 
                 << get_fpr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ".f);\n";
            break;

        case RABBITIZER_INSTR_ID_r5900_cfc2_ni:
        case RABBITIZER_INSTR_ID_r5900_cfc2_i: {
            // cfc2[.i/.ni] rt, rd - Copy from VU0 control register to GPR
            uint8_t rt = static_cast<uint8_t>(instr.GetO32_rt());
            uint8_t rd = static_cast<uint8_t>(instr.GetO32_rd());
            
            bool interlocked = (static_cast<int>(instr.getUniqueId()) == RABBITIZER_INSTR_ID_r5900_cfc2_i);
            file << "    // cfc2" << (interlocked ? ".i" : ".ni") 
                << " - Copy from VU0 control register " << std::dec << (int)rd << " to GPR[" << (int)rt << "]\n";
            
            if (rt == 0) {
                file << "    // rt=0, write ignored\n";
                break;
            }
            
            file << "    {\n";
            file << "        int32_t value = 0;\n";
            file << "        switch (" << std::dec << (int)rd << ") {\n";
            file << "            case 0: value = 0; break;  // VI0 always 0\n";
            file << "            case 1: case 2: case 3: case 4: case 5: case 6: case 7:\n";
            file << "            case 8: case 9: case 10: case 11: case 12: case 13: case 14: case 15:\n";
            file << "                value = static_cast<int32_t>(static_cast<int16_t>(ctx.vuRegs.VI[" << std::dec << (int)rd << "]));\n";
            file << "                break;\n";
            file << "            case 16: value = ctx.vuRegs.status_flag; break;\n";
            file << "            case 17: value = ctx.vuRegs.mac_flag; break;\n";
            file << "            case 18: value = ctx.vuRegs.clip_flag; break;\n";
            file << "            case 20: value = ctx.vuRegs.R; break;\n";
            file << "            case 21: value = ctx.vuRegs.I; break;\n";
            file << "            case 22: value = *reinterpret_cast<uint32_t*>(&ctx.vuRegs.Q); break;\n";
            file << "            case 26: value = ctx.vuRegs.TPC; break;\n";
            file << "            case 27: value = ctx.vuRegs.CMSAR0; break;\n";
            file << "            case 28: value = ctx.vuRegs.FBRST; break;\n";
            file << "            case 29: value = ctx.vuRegs.VPU_STAT; break;\n";
            file << "            case 31: value = ctx.vuRegs.CMSAR1; break;\n";
            file << "            default: value = 0; break;\n";
            file << "        }\n";
            file << "        " << get_gpr_name(rt) << ".SD[0] = value;  // Sign-extend to 64-bit\n";
            file << "    }\n";
            break;
        }

        case RABBITIZER_INSTR_ID_r5900_ctc2_ni:
        case RABBITIZER_INSTR_ID_r5900_ctc2_i: {
            // ctc2[.i/.ni] rt, rd - Copy from GPR to VU0 control register
            uint8_t rt = static_cast<uint8_t>(instr.GetO32_rt());
            uint8_t rd = static_cast<uint8_t>(instr.GetO32_rd());
            
            bool interlocked = (static_cast<int>(instr.getUniqueId()) == RABBITIZER_INSTR_ID_r5900_ctc2_i);
            file << "    // ctc2" << (interlocked ? ".i" : ".ni")
                << " - Copy from GPR[" << std::dec << (int)rt << "] to VU0 control register " << std::dec << (int)rd << "\n";
            
            file << "    {\n";
            file << "        uint32_t value = " << get_gpr_name(rt) << ".UL[0];\n";
            file << "        switch (" << std::dec << (int)rd << ") {\n";
            file << "            case 0: break;  // VI0 is read-only (always 0)\n";
            file << "            case 1: case 2: case 3: case 4: case 5: case 6: case 7:\n";
            file << "            case 8: case 9: case 10: case 11: case 12: case 13: case 14: case 15:\n";
            file << "                ctx.vuRegs.VI[" << std::dec << (int)rd << "] = static_cast<uint16_t>(value);\n";
            file << "                break;\n";
            file << "            case 16: ctx.vuRegs.status_flag = value & 0xFFF; break;\n";
            file << "            case 17: ctx.vuRegs.mac_flag = value & 0xFFFF; break;\n";
            file << "            case 18: ctx.vuRegs.clip_flag = value & 0xFFFFFF; break;\n";
            file << "            case 20: ctx.vuRegs.R = value; break;\n";
            file << "            case 21: ctx.vuRegs.I = value; break;\n";
            file << "            case 22: ctx.vuRegs.Q = *reinterpret_cast<float*>(&value); break;\n";
            file << "            case 26: ctx.vuRegs.TPC = value; break;\n";
            file << "            case 27: ctx.vuRegs.CMSAR0 = value; break;\n";
            file << "            case 28: ctx.vuRegs.FBRST = value; break;\n";
            // case 29 VPU_STAT is read-only
            file << "            case 31: ctx.vuRegs.CMSAR1 = value; break;\n";
            file << "            default: break;\n";
            file << "        }\n";
            file << "    }\n";
            break;
        }

        case RABBITIZER_INSTR_ID_r5900_qmfc2_ni:
        case RABBITIZER_INSTR_ID_r5900_qmfc2_i: {
            // qmfc2[.i/.ni] rt, vd - Copy 128-bit VF register to GPR
            uint8_t rt = static_cast<uint8_t>(instr.GetO32_rt());
            uint8_t vd = static_cast<uint8_t>(instr.GetO32_rd());  // VF register in rd field
            
            bool interlocked = (static_cast<int>(instr.getUniqueId()) == RABBITIZER_INSTR_ID_r5900_qmfc2_i);
            file << "    // qmfc2" << (interlocked ? ".i" : ".ni")
                << " - Copy VF[" << std::dec << (int)vd << "] to GPR[" << std::dec << (int)rt << "] (128-bit)\n";
            
            if (rt == 0) {
                file << "    // rt=0, write ignored\n";
                break;
            }
            
            // Copy all 128 bits
            file << "    memcpy(&" << get_gpr_name(rt) << ", &ctx.vuRegs.VF[" << std::dec << (int)vd << "], 16);\n";
            break;
        }

        case RABBITIZER_INSTR_ID_r5900_qmtc2_ni:
        case RABBITIZER_INSTR_ID_r5900_qmtc2_i: {
            // qmtc2[.i/.ni] rt, vd - Copy 128-bit GPR to VF register
            uint8_t rt = static_cast<uint8_t>(instr.GetO32_rt());
            uint8_t vd = static_cast<uint8_t>(instr.GetO32_rd());  // VF register in rd field
            
            bool interlocked = (static_cast<int>(instr.getUniqueId()) == RABBITIZER_INSTR_ID_r5900_qmtc2_i);
            file << "    // qmtc2" << (interlocked ? ".i" : ".ni")
                << " - Copy GPR[" << std::dec << (int)rt << "] to VF[" << std::dec << (int)vd << "] (128-bit)\n";
            
            if (vd == 0) {
                file << "    // VF0 is read-only (always {0,0,0,1})\n";
                break;
            }
            
            // Copy all 128 bits
            file << "    memcpy(&ctx.vuRegs.VF[" << std::dec << (int)vd << "], &" << get_gpr_name(rt) << ", 16);\n";
            break;
        }
        case RABBITIZER_INSTR_ID_cpu_ldl: {
            // Load Doubleword Left - loads bytes into HIGH part of register
            uint8_t rt = static_cast<uint8_t>(instr.GetO32_rt());
            uint8_t base = static_cast<uint8_t>(instr.GetO32_rs());
            int16_t offset = static_cast<int16_t>(instr.Get_immediate());
            
            file << "    // ldl - Load Doubleword Left (unaligned load, high bytes)\n";
            file << "    {\n";
            file << "        uint32_t addr = " << get_gpr_name(base) << ".UL[0] + " 
                << std::dec << offset << ";\n";
            file << "        uint32_t shift = (addr & 7) * 8;\n";
            file << "        uint64_t mem = memory::read<uint64_t>(addr & ~7);\n";
            file << "        uint64_t mask = 0x00FFFFFFFFFFFFFFULL >> shift;\n";
            file << "        " << get_gpr_name(rt) << ".UD[0] = (" 
                << get_gpr_name(rt) << ".UD[0] & mask) | (mem << (56 - shift));\n";
            file << "    }\n";
            break;
        }

        case RABBITIZER_INSTR_ID_cpu_ldr: {
            // Load Doubleword Right - loads bytes into LOW part of register
            uint8_t rt = static_cast<uint8_t>(instr.GetO32_rt());
            uint8_t base = static_cast<uint8_t>(instr.GetO32_rs());
            int16_t offset = static_cast<int16_t>(instr.Get_immediate());
            
            file << "    // ldr - Load Doubleword Right (unaligned load, low bytes)\n";
            file << "    {\n";
            file << "        uint32_t addr = " << get_gpr_name(base) << ".UL[0] + " 
                << std::dec << offset << ";\n";
            file << "        uint32_t shift = (addr & 7) * 8;\n";
            file << "        uint64_t mem = memory::read<uint64_t>(addr & ~7);\n";
            file << "        uint64_t mask = 0xFFFFFFFFFFFFFF00ULL << (56 - shift);\n";
            file << "        " << get_gpr_name(rt) << ".UD[0] = (" 
                << get_gpr_name(rt) << ".UD[0] & mask) | (mem >> shift);\n";
            file << "    }\n";
            break;
        }
        case RABBITIZER_INSTR_ID_cpu_lwl: {
            // Load Word Left - loads bytes into HIGH part of register
            uint8_t rt = static_cast<uint8_t>(instr.GetO32_rt());
            uint8_t base = static_cast<uint8_t>(instr.GetO32_rs());
            int16_t offset = static_cast<int16_t>(instr.Get_immediate());
            
            file << "    // lwl - Load Word Left (unaligned load, high bytes)\n";
            file << "    {\n";
            file << "        uint32_t addr = " << get_gpr_name(base) << ".UL[0] + " 
                << std::dec << offset << ";\n";
            file << "        uint32_t shift = (addr & 3) * 8;\n";
            file << "        uint32_t mem = memory::read<uint32_t>(addr & ~3);\n";
            file << "        uint32_t mask = 0x00FFFFFFU >> shift;\n";
            file << "        " << get_gpr_name(rt) << ".UL[0] = (" 
                << get_gpr_name(rt) << ".UL[0] & mask) | (mem << (24 - shift));\n";
            file << "    }\n";
            break;
        }

        case RABBITIZER_INSTR_ID_cpu_lwr: {
            // Load Word Right - loads bytes into LOW part of register
            uint8_t rt = static_cast<uint8_t>(instr.GetO32_rt());
            uint8_t base = static_cast<uint8_t>(instr.GetO32_rs());
            int16_t offset = static_cast<int16_t>(instr.Get_immediate());
            
            file << "    // lwr - Load Word Right (unaligned load, low bytes)\n";
            file << "    {\n";
            file << "        uint32_t addr = " << get_gpr_name(base) << ".UL[0] + " 
                << std::dec << offset << ";\n";
            file << "        uint32_t shift = (addr & 3) * 8;\n";
            file << "        uint32_t mem = memory::read<uint32_t>(addr & ~3);\n";
            file << "        uint32_t mask = 0xFFFFFF00U << (24 - shift);\n";
            file << "        " << get_gpr_name(rt) << ".UL[0] = (" 
                << get_gpr_name(rt) << ".UL[0] & mask) | (mem >> shift);\n";
            file << "    }\n";
            break;
        }
        case RABBITIZER_INSTR_ID_cpu_sdl: {
            uint8_t rt = static_cast<uint8_t>(instr.GetO32_rt());
            uint8_t base = static_cast<uint8_t>(instr.GetO32_rs());
            int16_t offset = static_cast<int16_t>(instr.Get_immediate());
            
            file << "    // sdl - Store Doubleword Left\n";
            file << "    {\n";
            file << "        uint32_t addr = " << get_gpr_name(base) << ".UL[0] + " 
                << std::dec << offset << ";\n";
            file << "        uint32_t shift = (addr & 7) * 8;\n";
            file << "        uint32_t aligned = addr & ~7;\n";
            file << "        uint64_t mem = memory::read<uint64_t>(aligned);\n";
            file << "        uint64_t mask = 0xFFFFFFFFFFFFFF00ULL << shift;\n";
            file << "        uint64_t val = " << get_gpr_name(rt) << ".UD[0] >> (56 - shift);\n";
            file << "        memory::write<uint64_t>(aligned, (mem & mask) | val);\n";
            file << "    }\n";
            break;
        }

        case RABBITIZER_INSTR_ID_cpu_sdr: {
            uint8_t rt = static_cast<uint8_t>(instr.GetO32_rt());
            uint8_t base = static_cast<uint8_t>(instr.GetO32_rs());
            int16_t offset = static_cast<int16_t>(instr.Get_immediate());
            
            file << "    // sdr - Store Doubleword Right\n";
            file << "    {\n";
            file << "        uint32_t addr = " << get_gpr_name(base) << ".UL[0] + " 
                << std::dec << offset << ";\n";
            file << "        uint32_t shift = (addr & 7) * 8;\n";
            file << "        uint32_t aligned = addr & ~7;\n";
            file << "        uint64_t mem = memory::read<uint64_t>(aligned);\n";
            file << "        uint64_t mask = 0x00FFFFFFFFFFFFFFULL >> (56 - shift);\n";
            file << "        uint64_t val = " << get_gpr_name(rt) << ".UD[0] << shift;\n";
            file << "        memory::write<uint64_t>(aligned, (mem & mask) | val);\n";
            file << "    }\n";
            break;
        }
        case RABBITIZER_INSTR_ID_cpu_sltu:
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UD[0] = (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] < " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0]) ? 1 : 0;\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_slt:
            // Set on Less Than (Signed)
            // We use .SL[0] to enforce a signed comparison between registers.
            file << "    // slt - Set on Less Than (Signed)\n";
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UD[0] = (" 
                 << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".SL[0] < " 
                 << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".SL[0]) ? 1 : 0;\n";
            break;
        case RABBITIZER_INSTR_ID_r5900_lq:
            file << "    // lq instruction - 128-bit load\n";
            file << "    memory::read_quad(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] + " << format_imm(static_cast<int16_t>(instr.Get_immediate())) << ", *reinterpret_cast<QuadWord*>(&" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << "));\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_movz:
            file << "    if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0] == 0) " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UD[0] = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UD[0];\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_movn:
            // MOVN rd, rs, rt - Move if Not Zero
            // If GPR[rt] != 0, then GPR[rd] = GPR[rs]
            file << "    // movn - Move Conditional on Not Zero\n";
            file << "    if (" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) 
                << ".UL[0] != 0) " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) 
                << ".UL[0] = " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0];\n";
            break;
        case RABBITIZER_INSTR_ID_r5900_mult:
        case RABBITIZER_INSTR_ID_cpu_mult: {
            // mult rs, rt     - Signed 32x32 -> 64-bit result in HI:LO
            // R5900 extension: mult rd, rs, rt - Also stores LO in rd
            uint8_t rs = static_cast<uint8_t>(instr.GetO32_rs());
            uint8_t rt = static_cast<uint8_t>(instr.GetO32_rt());
            uint8_t rd = static_cast<uint8_t>(instr.GetO32_rd());
            
            file << "    // mult - Signed Multiply\n";
            file << "    {\n";
            file << "        int64_t result = static_cast<int64_t>(" << get_gpr_name(rs) << ".SL[0]) * "
                << "static_cast<int64_t>(" << get_gpr_name(rt) << ".SL[0]);\n";
            file << "        ctx.cpuRegs.LO.SD[0] = static_cast<int32_t>(result & 0xFFFFFFFF);\n";
            file << "        ctx.cpuRegs.HI.SD[0] = static_cast<int32_t>(result >> 32);\n";
            
            // R5900 extension: rd receives LO value if rd != 0
            if (rd != 0) {
                file << "        " << get_gpr_name(rd) << ".SD[0] = ctx.cpuRegs.LO.SL[0];\n";
            }
            file << "    }\n";
            break;
        }
        case RABBITIZER_INSTR_ID_cpu_dmult: {
            // dmult rs, rt    - Signed 64x64 -> 128-bit result in HI:LO
            uint8_t rs = static_cast<uint8_t>(instr.GetO32_rs());
            uint8_t rt = static_cast<uint8_t>(instr.GetO32_rt());
            
            file << "    // dmult - Signed Doubleword Multiply\n";
            file << "    {\n";
            file << "        __int128 result = static_cast<__int128>(static_cast<int64_t>(" 
                << get_gpr_name(rs) << ".SD[0])) * "
                << "static_cast<__int128>(static_cast<int64_t>(" << get_gpr_name(rt) << ".SD[0]));\n";
            file << "        ctx.cpuRegs.LO.SD[0] = static_cast<int64_t>(result);\n";
            file << "        ctx.cpuRegs.HI.SD[0] = static_cast<int64_t>(result >> 64);\n";
            file << "    }\n";
            break;
        }
        case RABBITIZER_INSTR_ID_cpu_dmultu: {
            // dmultu rs, rt   - Unsigned 64x64 -> 128-bit result in HI:LO
            uint8_t rs = static_cast<uint8_t>(instr.GetO32_rs());
            uint8_t rt = static_cast<uint8_t>(instr.GetO32_rt());
            
            file << "    // dmultu - Unsigned Doubleword Multiply\n";
            file << "    {\n";
            file << "        unsigned __int128 result = static_cast<unsigned __int128>(" 
                << get_gpr_name(rs) << ".UD[0]) * "
                << "static_cast<unsigned __int128>(" << get_gpr_name(rt) << ".UD[0]);\n";
            file << "        ctx.cpuRegs.LO.UD[0] = static_cast<uint64_t>(result);\n";
            file << "        ctx.cpuRegs.HI.UD[0] = static_cast<uint64_t>(result >> 64);\n";
            file << "    }\n";
            break;
        }
        case RABBITIZER_INSTR_ID_r5900_mult1: {
            // mult1 rd, rs, rt - Signed 32x32 -> 64-bit result in HI1:LO1, rd = LO1
            uint8_t rs = static_cast<uint8_t>(instr.GetO32_rs());
            uint8_t rt = static_cast<uint8_t>(instr.GetO32_rt());
            uint8_t rd = static_cast<uint8_t>(instr.GetO32_rd());
            
            file << "    // mult1 - Signed Multiply (Pipeline 1)\n";
            file << "    {\n";
            file << "        int64_t result = static_cast<int64_t>(" << get_gpr_name(rs) << ".SL[0]) * "
                << "static_cast<int64_t>(" << get_gpr_name(rt) << ".SL[0]);\n";
            file << "        ctx.cpuRegs.LO1.SD[0] = static_cast<int32_t>(result & 0xFFFFFFFF);\n";
            file << "        ctx.cpuRegs.HI1.SD[0] = static_cast<int32_t>(result >> 32);\n";
            
            if (rd != 0) {
                file << "        " << get_gpr_name(rd) << ".SD[0] = ctx.cpuRegs.LO1.SL[0];\n";
            }
            file << "    }\n";
            break;
        }

        case RABBITIZER_INSTR_ID_r5900_multu1: {
            // multu1 rd, rs, rt - Unsigned 32x32 -> 64-bit result in HI1:LO1, rd = LO1
            uint8_t rs = static_cast<uint8_t>(instr.GetO32_rs());
            uint8_t rt = static_cast<uint8_t>(instr.GetO32_rt());
            uint8_t rd = static_cast<uint8_t>(instr.GetO32_rd());
            
            file << "    // multu1 - Unsigned Multiply (Pipeline 1)\n";
            file << "    {\n";
            file << "        uint64_t result = static_cast<uint64_t>(" << get_gpr_name(rs) << ".UL[0]) * "
                << "static_cast<uint64_t>(" << get_gpr_name(rt) << ".UL[0]);\n";
            file << "        ctx.cpuRegs.LO1.SD[0] = static_cast<int32_t>(result & 0xFFFFFFFF);\n";
            file << "        ctx.cpuRegs.HI1.SD[0] = static_cast<int32_t>(result >> 32);\n";
            
            if (rd != 0) {
                file << "        " << get_gpr_name(rd) << ".SD[0] = ctx.cpuRegs.LO1.SL[0];\n";
            }
            file << "    }\n";
            break;
        }

        case RABBITIZER_INSTR_ID_cpu_syscall:
            file << "    runtime_syscall_dispatcher(" << get_gpr_name(3) << ".UL[0], ctx);\n";
            break;
        case RABBITIZER_INSTR_ID_r5900_vaddx:
            file << "    " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << ".x = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ".x + " << get_vr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".x;\n";
            break;
        case RABBITIZER_INSTR_ID_r5900_vadd:
            file << "    " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << " = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << " + " << get_vr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_r5900_vsub:
            file << "    " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << " = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << " - " << get_vr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_r5900_vmul:
            file << "    " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << " = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << " * " << get_vr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_r5900_vdiv:
            file << "    " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << ".x = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ".x / " << get_vr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".x;\n";
            file << "    " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << ".y = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ".y / " << get_vr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".y;\n";
            file << "    " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << ".z = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ".z / " << get_vr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".z;\n";
            file << "    " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << ".w = " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ".w / " << get_vr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".w;\n";
            break;
        case RABBITIZER_INSTR_ID_r5900_vsqrt:
            file << "    " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << ".x = sqrt(" << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ".x);\n";
            file << "    " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << ".y = sqrt(" << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ".y);\n";
            file << "    " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << ".z = sqrt(" << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ".z);\n";
            file << "    " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << ".w = sqrt(" << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ".w);\n";
            break;
        case RABBITIZER_INSTR_ID_r5900_vabs:
            file << "    " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << ".x = abs(" << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ".x);\n";
            file << "    " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << ".y = abs(" << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ".y);\n";
            file << "    " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << ".z = abs(" << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ".z);\n";
            file << "    " << get_vr_name(static_cast<uint8_t>(instr.GetO32_fd())) << ".w = abs(" << get_vr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ".w);\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_sd:
            file << "    memory::write<uint64_t>(" << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] + " << format_imm(static_cast<int16_t>(instr.Get_immediate())) << ", " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UD[0]);\n";
            break;
        case RABBITIZER_INSTR_ID_r5900_pcpyld:
            // Parallel Copy Lower Doubleword
            file << "    // pcpyld - Parallel Copy Lower Doubleword\n";
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UD[1] = " 
                << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UD[0];\n";
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UD[0] = " 
                << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UD[0];\n";
            break;

        case RABBITIZER_INSTR_ID_r5900_pcpyh:
            // Parallel Copy Halfword - broadcast lowest halfword to all 8 positions
            file << "    // pcpyh - Parallel Copy Halfword\n";
            file << "    {\n";
            file << "        uint16_t hw = static_cast<uint16_t>(" 
                << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0] & 0xFFFF);\n";
            file << "        for (int i = 0; i < 8; i++) {\n";
            file << "            " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".US[i] = hw;\n";
            file << "        }\n";
            file << "    }\n";
            break;

        case RABBITIZER_INSTR_ID_cpu_dsll:
            // Doubleword Shift Left Logical
            file << "    // dsll - Doubleword Shift Left Logical\n";
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UD[0] = " 
                << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UD[0] << " 
                << std::to_string(instr.Get_sa()) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_dsrlv:
            // dsrlv rd, rt, rs - Doubleword Shift Right Logical Variable
            // Shift amount is in the low 6 bits of rs
            file << "    // dsrlv - Doubleword Shift Right Logical Variable\n";
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UD[0] = " 
                 << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UD[0] >> (" 
                 << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] & 0x3F);\n";
            break;

        case RABBITIZER_INSTR_ID_cpu_dsrl32:
            // dsrl32 rd, rt, sa - Doubleword Shift Right Logical + 32
            // Shift amount is sa + 32
            file << "    // dsrl32 - Doubleword Shift Right Logical + 32\n";
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UD[0] = " 
                 << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UD[0] >> " 
                 << std::to_string(instr.Get_sa() + 32) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_mflo:
            // mflo rd - Move From LO
            file << "    // mflo - Move From LO\n";
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UD[0] = ctx.cpuRegs.LO.UD[0];\n";
            break;

        case RABBITIZER_INSTR_ID_cpu_mfhi:
            // mfhi rd - Move From HI
            file << "    // mfhi - Move From HI\n";
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UD[0] = ctx.cpuRegs.HI.UD[0];\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_negu:
            // negu rd, rt - Negate Unsigned
            // rd = 0 - rt
            file << "    // negu - Negate Unsigned\n";
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UD[0] = 0 - " 
                 << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UD[0];\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_dsubu:
            // dsubu rd, rs, rt
            // Operation: GPR[rd] = GPR[rs] - GPR[rt]
            // "Unsigned" means it wraps on overflow instead of trapping. 
            // We use .UD[0] (Unsigned Doubleword) to utilize C++'s defined unsigned wrapping behavior.
            file << "    // dsubu - Doubleword Subtract Unsigned\n";
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UD[0] = " 
                 << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UD[0] - " 
                 << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UD[0];\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_dsll32:
            // Doubleword Shift Left Logical + 32
            file << "    // dsll32 - Doubleword Shift Left Logical + 32\n";
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UD[0] = " 
                << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UD[0] << " 
                << std::to_string(instr.Get_sa() + 32) << ";\n";
            break;

        case RABBITIZER_INSTR_ID_cpu_dsllv:
            // Doubleword Shift Left Logical Variable
            file << "    // dsllv - Doubleword Shift Left Logical Variable\n";
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UD[0] = " 
                << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UD[0] << (" 
                << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rs())) << ".UL[0] & 0x3F);\n";
            break;

        case RABBITIZER_INSTR_ID_cpu_dsrl:
            // Doubleword Shift Right Logical
            file << "    // dsrl - Doubleword Shift Right Logical\n";
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".UD[0] = " 
                << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UD[0] >> " 
                << std::to_string(instr.Get_sa()) << ";\n";
            break;

        case RABBITIZER_INSTR_ID_cpu_dsra:
            // Doubleword Shift Right Arithmetic
            file << "    // dsra - Doubleword Shift Right Arithmetic\n";
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".SD[0] = " 
                << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".SD[0] >> " 
                << std::to_string(instr.Get_sa()) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_dsra32:
            // Doubleword Shift Right Arithmetic + 32
            file << "    // dsra32 - Doubleword Shift Right Arithmetic + 32\n";
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rd())) << ".SD[0] = " 
                 << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".SD[0] >> " 
                 << std::to_string(instr.Get_sa() + 32) << ";\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_nop:
            file << "   //nop \n";
            break;
        case RABBITIZER_INSTR_ID_r5900_ei:
            file << "    ctx.cpuRegs.CP0.n.Status |= 0x1;\n";
            break;
        case RABBITIZER_INSTR_ID_r5900_di:
            file << "    ctx.cpuRegs.CP0.n.Status &= ~0x1;\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_mfc0:
            file << "    // mfc0 - Move From Coprocessor 0 (Register " 
                << (int)static_cast<uint8_t>(instr.GetO32_rd()) << ")\n";
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".SD[0] = "
                << "static_cast<int32_t>(ctx.cpuRegs.CP0.r[" 
                << std::to_string(static_cast<uint8_t>(instr.GetO32_rd())) << "]);\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_cache: {
            // cache op, offset(base)
            // On PS2 EE, this manages I-cache and D-cache operations.
            // In a static recompiler without cache emulation, this is a NOP.
            //
            // If you want to log it for debugging:
            uint8_t op = instr.Get_op();      // The cache operation (bits 16-20)
            int16_t offset = instr.Get_immediate();
            
            file << "    // cache op=0x" << std::hex << (int)op 
                 << ", offset=" << std::dec << offset 
                 << " - NOP\n";
            break;
        }
        case RABBITIZER_INSTR_ID_cpu_cfc1:
            // cfc1 rt, fs - Copy From FPU Control Register to GPR
            // On PS2, control registers are accessed via the fprc array
            file << "    // cfc1 - Copy from FPU Control Register " << (int)static_cast<uint8_t>(instr.GetO32_fs()) << "\n";
            file << "    " << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".SL[0] = static_cast<int32_t>(ctx.fpuRegs.fprc[" 
                 << (int)static_cast<uint8_t>(instr.GetO32_fs()) << "]);\n";
            break;

        case RABBITIZER_INSTR_ID_cpu_ctc1:
            // ctc1 rt, fs - Copy To FPU Control Register from GPR
            file << "    // ctc1 - Copy to FPU Control Register " << (int)static_cast<uint8_t>(instr.GetO32_fs()) << "\n";
            file << "    ctx.fpuRegs.fprc[" << (int)static_cast<uint8_t>(instr.GetO32_fs()) << "] = " 
                 << get_gpr_name(static_cast<uint8_t>(instr.GetO32_rt())) << ".UL[0];\n";
            break;
        // --- Coprocessor 1 (FPU) Comparisons ---
        // These set Bit 23 (0x800000) of FCR31 if true, clear it if false.

        case RABBITIZER_INSTR_ID_cpu_c_eq_s:
            file << "    // c.eq.s - Compare Equal (Single)\n";
            file << "    if (" << get_fpr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ".f == " 
                 << get_fpr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".f) \n";
            file << "        ctx.fpuRegs.fprc[31] |= 0x800000;\n";
            file << "    else ctx.fpuRegs.fprc[31] &= ~0x800000;\n";
            break;

        case RABBITIZER_INSTR_ID_r5900_c_lt_s:
            file << "    // c.lt.s - Compare Less Than (R5900)\n";
            file << "    if (" << get_fpr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ".f < " 
                 << get_fpr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".f) \n";
            file << "        ctx.fpuRegs.fprc[31] |= 0x800000;\n";
            file << "    else ctx.fpuRegs.fprc[31] &= ~0x800000;\n";
            break;

        case RABBITIZER_INSTR_ID_r5900_c_le_s:
            file << "    // c.le.s - Compare Less Than or Equal (R5900)\n";
            file << "    if (" << get_fpr_name(static_cast<uint8_t>(instr.GetO32_fs())) << ".f <= " 
                 << get_fpr_name(static_cast<uint8_t>(instr.GetO32_ft())) << ".f) \n";
            file << "        ctx.fpuRegs.fprc[31] |= 0x800000;\n";
            file << "    else ctx.fpuRegs.fprc[31] &= ~0x800000;\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_sync:
            file << "    // sync - FP pipeline synchronization\n";
            break;
        case RABBITIZER_INSTR_ID_r5900_sync_p:
            file << "    // sync.p - FP pipeline synchronization\n";
            file << "    // Effectively a NOP in this architectural simulation\n";
            break;
        case RABBITIZER_INSTR_ID_cpu_mtc0: {
            uint8_t rd = static_cast<uint8_t>(instr.GetO32_rd());
            uint8_t rt = static_cast<uint8_t>(instr.GetO32_rt());
            
            file << "    // mtc0 - Move To Coprocessor 0 (Register " << (int)rd << ")\n";
            
            // Inline critical path registers for performance
            switch (rd) {
                case 12: // Status register - HOT PATH
                    file << "    {\n";
                    file << "        uint32_t value = " << get_gpr_name(rt) << ".UL[0];\n";
                    file << "        // Mask writable bits: CU, BEV, IM, KU/IE stack\n";
                    file << "        ctx.cpuRegs.CP0.n.Status = (value & 0xF0FF003F) | "
                        << "(ctx.cpuRegs.CP0.n.Status & ~0xF0FF003F);\n";
                    file << "    }\n";
                    break;
                    
                case 13: // Cause register - HOT PATH
                    file << "    {\n";
                    file << "        uint32_t value = " << get_gpr_name(rt) << ".UL[0];\n";
                    file << "        // Only software interrupt bits (IP0-IP1) are writable\n";
                    file << "        ctx.cpuRegs.CP0.n.Cause = (ctx.cpuRegs.CP0.n.Cause & ~0x300) | "
                        << "(value & 0x300);\n";
                    file << "    }\n";
                    break;
                    
                case 11: // Compare register - Clears timer interrupt
                    file << "    ctx.cpuRegs.CP0.r[11] = " << get_gpr_name(rt) << ".UL[0];\n";
                    file << "    ctx.cpuRegs.CP0.n.Cause &= ~0x8000;  // Clear IP7 (timer)\n";
                    break;
                    
                case 9:  // Count register
                    file << "    ctx.cpuRegs.CP0.r[9] = " << get_gpr_name(rt) << ".UL[0];\n";
                    break;
                    
                case 14: // EPC
                    file << "    ctx.cpuRegs.CP0.r[14] = " << get_gpr_name(rt) << ".UL[0];\n";
                    break;
                    
                default: // Use helper for less common registers
                    file << "    handle_mtc0_write(ctx, " << std::dec << static_cast<int>(rd) << ", " 
                        << get_gpr_name(rt) << ".UL[0]);\n";
                    break;
            }
            break;
        }
        case RABBITIZER_INSTR_ID_cpu_eret:
            file << "    // eret - Exception Return\n";
            file << "    // Check ERL (Error Level) bit in Status Register (Bit 2)\n";
            file << "    if (ctx.cpuRegs.CP0.n.Status & 0x4) {\n";
            // CHANGED: ErrorPC -> ErrorEPC
            file << "        ctx.cpuRegs.pc = ctx.cpuRegs.CP0.n.ErrorEPC;\n"; 
            file << "        ctx.cpuRegs.CP0.n.Status &= ~0x4; // Clear ERL\n";
            file << "    } else {\n";
            file << "        // Otherwise use EPC (Exception Program Counter)\n";
            file << "        ctx.cpuRegs.pc = ctx.cpuRegs.CP0.n.EPC;\n";
            file << "        ctx.cpuRegs.CP0.n.Status &= ~0x2; // Clear EXL (Exception Level)\n";
            file << "    }\n";
            
            // Force lookup of the destination
            file << "    if (recompiled_functions.count(ctx.cpuRegs.pc)) {\n";
            file << "        recompiled_functions[ctx.cpuRegs.pc](ctx, ctx.cpuRegs.pc);\n";
            file << "    } else {\n";
            file << "        return; // Return to main loop for dispatcher\n";
            file << "    }\n";
            file << "    return; // Stop execution in the current block\n";
            break;
        default:
            file << "    // ----------------------------------------------------------------\n";
            file << "    // UNHANDLED INSTRUCTION: " << instr.getOpcodeName() << "\n";
            file << "    // Opcode: 0x" << std::hex << static_cast<int>(instr.Get_opcode()) << "\n";
            file << "    // Function: 0x" << std::hex << static_cast<int>(instr.Get_function()) << "\n";
            file << "    // Immediate: 0x" << std::hex << instr.Get_immediate() << "\n";
            file << "    // Address: 0x" << std::hex << instr.getVram() << "\n";
            file << "    // ----------------------------------------------------------------\n";
            file << "      g_logFile << \"Unhandled OP Code: 0x\" << std::hex << 0x" << std::hex << instr.getVram() << " << \" Instruction: \" << \"" << instr.getOpcodeName() << "\";\n";
            file << "    exit(1);\n";

            log_file << "    // ----------------------------------------------------------------\n";
            log_file << "    // UNHANDLED INSTRUCTION: " << instr.getOpcodeName() << "\n";
            log_file << "    // Opcode: 0x" << std::hex << static_cast<int>(instr.Get_opcode()) << "\n";
            log_file << "    // Function: 0x" << std::hex << static_cast<int>(instr.Get_function()) << "\n";
            log_file << "    // Immediate: 0x" << std::hex << instr.Get_immediate() << "\n";
            log_file << "    // Address: 0x" << std::hex << instr.getVram() << "\n";
            log_file << "    // ----------------------------------------------------------------\n";
            log_file << "      g_logFile << \"Unhandled OP Code: 0x\" << std::hex << 0x" << std::hex << instr.getVram() << " << \" Instruction: \" << \"" << instr.getOpcodeName() << "\";\n";
            log_file << "    exit(1);\n";
    }
}