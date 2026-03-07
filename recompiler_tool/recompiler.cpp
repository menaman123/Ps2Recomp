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
        else if (func.base_address == 0x002adb18){
            file << "    // HLE Hook for Intitialize Graphics\n";
            file << "    recompiled_functions[0x2adb18] = [](CpuContext& ctx, uint32_t addr) {\n";
            file << "        HLE_FUN_002adb18(ctx, addr);\n";
            file << "        // Simulate 'jr $ra' return\n";
            file << "        ctx.cpuRegs.pc = ctx.cpuRegs.GPR.r[31].UL[0];\n";
            file << "    };\n";
        
            // Register range so lookups don't fail
            uint32_t end_address = func.base_address + func.size;
            file << "    function_ranges.push_back({0x" << std::hex << func.base_address 
                << ", 0x" << std::hex << end_address << ", recompiled_functions[0x2adb18]});\n";
        }


        else if (func.base_address == 0x002ad8f0){
            file << "    // HLE Hook for Intitialize Graphics\n";
            file << "    recompiled_functions[0x2ad8f0] = [](CpuContext& ctx, uint32_t addr) {\n";
            file << "        HLE_FUN_002ad8f0(ctx, addr);\n";
            file << "        // Simulate 'jr $ra' return\n";
            file << "        ctx.cpuRegs.pc = ctx.cpuRegs.GPR.r[31].UL[0];\n";
            file << "    };\n";
        
            // Register range so lookups don't fail
            uint32_t end_address = func.base_address + func.size;
            file << "    function_ranges.push_back({0x" << std::hex << func.base_address 
                << ", 0x" << std::hex << end_address << ", recompiled_functions[0x2ad8f0]});\n";
        }


        else if (func.base_address == 0x002aac80){
            file << "    // HLE Hook for Intitialize Graphics\n";
            file << "    recompiled_functions[0x2aac80] = [](CpuContext& ctx, uint32_t addr) {\n";
            file << "        HLE_FUN_002aac80(ctx, addr);\n";
            file << "        // Simulate 'jr $ra' return\n";
            file << "        ctx.cpuRegs.pc = ctx.cpuRegs.GPR.r[31].UL[0];\n";
            file << "    };\n";
        
            // Register range so lookups don't fail
            uint32_t end_address = func.base_address + func.size;
            file << "    function_ranges.push_back({0x" << std::hex << func.base_address 
                << ", 0x" << std::hex << end_address << ", recompiled_functions[0x2aac80]});\n";
        }
        else if (func.base_address == 0x002adb40){
            file << "    // HLE Hook for Close Handle\n";
            file << "    recompiled_functions[0x2adb40] = [](CpuContext& ctx, uint32_t addr) {\n";
            file << "        HLE_FUN_002adb40(ctx, addr);\n";
            file << "        // Simulate 'jr $ra' return\n";
            file << "        ctx.cpuRegs.pc = ctx.cpuRegs.GPR.r[31].UL[0];\n";
            file << "    };\n";
        
            // Register range so lookups don't fail
            uint32_t end_address = func.base_address + func.size;
            file << "    function_ranges.push_back({0x" << std::hex << func.base_address 
                << ", 0x" << std::hex << end_address << ", recompiled_functions[0x2adb40]});\n";
        }


        else if (func.base_address == 0x002adb18){
            file << "    // HLE Hook for Intitialize Graphics\n";
            file << "    recompiled_functions[0x2adb18] = [](CpuContext& ctx, uint32_t addr) {\n";
            file << "        HLE_FUN_002adb18(ctx, addr);\n";
            file << "        // Simulate 'jr $ra' return\n";
            file << "        ctx.cpuRegs.pc = ctx.cpuRegs.GPR.r[31].UL[0];\n";
            file << "    };\n";
        
            // Register range so lookups don't fail
            uint32_t end_address = func.base_address + func.size;
            file << "    function_ranges.push_back({0x" << std::hex << func.base_address 
                << ", 0x" << std::hex << end_address << ", recompiled_functions[0x2adb18]});\n";
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


        else if (func.base_address == 0x00172b60) {
            // Diagnostic wrapper for FUN_00172b60 - Texture Load Request
            // Logs key decision points before calling the auto-generated function
            file << "    recompiled_functions[0x172b60] = [](CpuContext& ctx, uint32_t addr) {\n";
            file << "        uint32_t p1 = ctx.cpuRegs.GPR.r[4].UL[0];\n";
            file << "        uint32_t tex_obj = memory::read<uint32_t>(0x309720);\n";
            file << "        uint32_t fcur = memory::read<uint32_t>(p1 + 0x10);\n";
            file << "        uint32_t flast = memory::read<uint32_t>(p1 + 0x18);\n";
            file << "        uint32_t iv1 = memory::read<uint32_t>(p1 + 0x14);\n";
            file << "        uint32_t tref = memory::read<uint32_t>(p1 + 0x1c);\n";
            file << "        uint32_t p2t = memory::read<uint32_t>(0x323534);\n";
            file << "        uint32_t ts = (tex_obj != 0) ? memory::read<uint32_t>(tex_obj) : 0;\n";
            file << "        g_logFile << \"[172b60-DIAG] p1=0x\" << std::hex << p1\n";
            file << "                  << \" tex=0x\" << tex_obj\n";
            file << "                  << \" fcur=\" << std::dec << fcur << \" flast=\" << flast\n";
            file << "                  << \" iv1=\" << iv1 << \" tref=\" << tref << \" p2t=\" << p2t\n";
            file << "                  << \" ts=0x\" << std::hex << ts\n";
            file << "                  << \" delta=\" << std::dec << ctx.cpuRegs.GPR.r[6].SL[0]\n";
            file << "                  << \" idx=\" << ctx.cpuRegs.GPR.r[7].UL[0] << std::endl;\n";
            file << "        if (tex_obj == 0) g_logFile << \"[172b60-DIAG] EARLY-OUT: tex_obj NULL\" << std::endl;\n";
            file << "        else if (fcur == flast) g_logFile << \"[172b60-DIAG] EARLY-OUT: fcur==flast (\" << fcur << \")\" << std::endl;\n";
            file << "        else if (iv1 != 0) g_logFile << \"[172b60-DIAG] PATH: iv1!=0 tex_state=0x\" << std::hex << (ts & 0xf000) << std::dec << std::endl;\n";
            file << "        else g_logFile << \"[172b60-DIAG] PATH: fresh load -> FUN_002afb18\" << std::endl;\n";
            file << "        FUN_00172b60(ctx);\n";
            file << "    };\n";
            uint32_t end_address = func.base_address + func.size;
            file << "    function_ranges.push_back({0x" << std::hex << func.base_address 
                 << ", 0x" << std::hex << end_address << ", recompiled_functions[0x172b60]});\n";
        }


        else if (func.base_address == 0x00172d88) {
            // Diagnostic wrapper for FUN_00172d88 - Streaming State Check
            // Logs the 64-bit state word and which branch is taken
            file << "    recompiled_functions[0x172d88] = [](CpuContext& ctx, uint32_t addr) {\n";
            file << "        uint32_t p1 = ctx.cpuRegs.GPR.r[4].UL[0];\n";
            file << "        uint32_t p2 = ctx.cpuRegs.GPR.r[5].UL[0];\n";
            file << "        uint64_t state = memory::read<uint64_t>(p1 + 0x8e8);\n";
            file << "        uint32_t iv1 = memory::read<uint32_t>(p1 + 0x14);\n";
            file << "        bool bit32 = (state & 0x100000000ULL) != 0;\n";
            file << "        uint64_t bits33_40 = state & 0x1fe00000000ULL;\n";
            file << "        bool bVar1 = false;\n";
            file << "        if (!bit32) bVar1 = (bits33_40 != 0x1800000000ULL);\n";
            file << "        g_logFile << \"[172d88-DIAG] p1=0x\" << std::hex << p1\n";
            file << "                  << \" p2=\" << std::dec << p2\n";
            file << "                  << \" state64=0x\" << std::hex << state\n";
            file << "                  << \" bit32=\" << bit32\n";
            file << "                  << \" bits33_40=0x\" << bits33_40\n";
            file << "                  << \" bVar1=\" << bVar1\n";
            file << "                  << \" iv1=\" << std::dec << iv1\n";
            file << "                  << std::endl;\n";
            file << "        if (!bVar1) {\n";
            file << "            bool topbit = ((state >> 0x20) & 1) == 0;\n";
            file << "            g_logFile << \"[172d88-DIAG] PATH: bVar1=false -> return 0\"\n";
            file << "                      << \" (topbit_check=\" << topbit << \")\" << std::endl;\n";
            file << "        } else {\n";
            file << "            g_logFile << \"[172d88-DIAG] PATH: bVar1=true -> checking animation state\" << std::endl;\n";
            file << "        }\n";
            file << "        FUN_00172d88(ctx);\n";
            file << "        g_logFile << \"[172d88-DIAG] returned v0=\" << std::dec << ctx.cpuRegs.GPR.r[2].UL[0] << std::endl;\n";
            file << "    };\n";
            uint32_t end_address = func.base_address + func.size;
            file << "    function_ranges.push_back({0x" << std::hex << func.base_address 
                 << ", 0x" << std::hex << end_address << ", recompiled_functions[0x172d88]});\n";
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
    /*
    )code" << std::endl;




        file << R"code(
    */


    if (func.base_address == 0x00177168) {
        // FUN_00177168 - Main Dispatcher
        file << R"code(


        void FUN_00177168(CpuContext& ctx) {
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


                // SP tracking
                uint32_t expected_sp = ctx.cpuRegs.GPR.r[29].UL[0];
                g_logFile << "[177168] working sp=0x" << std::hex << expected_sp << std::endl;


        // Macro to reduce repetition
        #define CHECK_SP(call_addr) \
                if (ctx.cpuRegs.GPR.r[29].UL[0] != expected_sp) { \
                    g_logFile << "[177168] SP CORRUPTED after " #call_addr "! expected=0x" \
                            << std::hex << expected_sp \
                            << " got=0x" << ctx.cpuRegs.GPR.r[29].UL[0] << std::endl; \
                }


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
                CHECK_SP(0x2b3548_prologue)


                // Logic Block 1
                ctx.cpuRegs.GPR.r[4].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x34);
                ctx.cpuRegs.GPR.r[4].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0);
                ctx.cpuRegs.GPR.r[2].UL[0] = ctx.cpuRegs.GPR.r[2].UL[0] & 0x1;
                ctx.cpuRegs.GPR.r[5].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x10);
                
                {
                    // MIPS: lui v1, 0xbfff ; ori v1, 0xffff ; dsll v1, 0x1 ; ori v1, 0x1
                    // => 0xFFFFFFFF7FFFFFFF (clears bit 31 only)
                    uint64_t mask = (uint64_t)(int32_t)0xBFFF0000;  // sign-extended lui
                    mask |= 0xffffULL;                               // 0xFFFFFFFFBFFFFFFF
                    mask <<= 1;                                      // 0xFFFFFFFF7FFFFFFE
                    mask |= 1;                                       // 0xFFFFFFFF7FFFFFFF
                    ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[4].UD[0] & mask;
                    uint64_t v0_shifted = (uint64_t)ctx.cpuRegs.GPR.r[2].UL[0] << 31;
                    ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[4].UD[0] | v0_shifted;
                }
                
                ctx.cpuRegs.GPR.r[5].UL[0] = ctx.cpuRegs.GPR.r[5].UL[0] + 1;
                memory::write<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0, ctx.cpuRegs.GPR.r[4].UD[0]);
                memory::write<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x10, ctx.cpuRegs.GPR.r[5].UL[0]);


                // Call FUN_00173238(s1, s5)
                ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[17].UD[0];
                ctx.cpuRegs.GPR.r[5].UD[0] = ctx.cpuRegs.GPR.r[21].UD[0];
                ctx.cpuRegs.GPR.r[31].UL[0] = 0x1771e8;
                if (recompiled_functions.count(0x173238)) {
                    recompiled_functions[0x173238](ctx, 0x173238);
                } else {
                    ctx.cpuRegs.pc = 0x173238;
                    return;
                }
                CHECK_SP(0x173238)
    )code" << std::endl;




        file << R"code(
                // Logic Block 2 — State promotion (instruction-faithful from MIPS 0x1771EC-0x177298)
                // ld a0, 0x0(s1)
                ctx.cpuRegs.GPR.r[4].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0);
                {
                    // ori v0, zero, 0xfc00 ; dsll32 v0, v0, 0x8
                    uint64_t v0 = (uint64_t)0xfc00 << (32 + 8);  // 0xfc0000000000
                    // ori a2, zero, 0xc000 ; dsll32 a2, a2, 0x7
                    uint64_t a2 = (uint64_t)0xc000 << (32 + 7);  // 0x60000000000000
                    // and v0, a0, v0
                    ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[4].UD[0] & v0;
                    
                    // beql v0, a2, 17729c (delay slot: ld v0, 0x0(s1))
                    if (ctx.cpuRegs.GPR.r[2].UD[0] == a2) {
                        ctx.cpuRegs.GPR.r[2].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0);
                        goto Label_17729c;
                    }
                }

                // Not promoted — do the promotion
                // dsrl32 v1, a0, 0xc  (v1 = a0 >> 44)
                ctx.cpuRegs.GPR.r[3].UD[0] = ctx.cpuRegs.GPR.r[4].UD[0] >> 44;
                
                {
                    // lui v0, 0x81ff ; ori v0, 0xffff ; dsll v0, 0xd ; ori v0, 0x1fff
                    uint64_t v0_raw = (uint64_t)(int32_t)0x81ff0000;  // sign-extended lui: 0xFFFFFFFF81FF0000
                    v0_raw |= 0xffffULL;                               // 0xFFFFFFFF81FFFFFF
                    v0_raw <<= 13;                                     // 0xFFFF03FFFFFFE000
                    v0_raw |= 0x1fffULL;                               // 0xFFFF03FFFFFFFFFF
                    
                    // andi v1, v1, 0x3f
                    ctx.cpuRegs.GPR.r[3].UL[0] = ctx.cpuRegs.GPR.r[3].UL[0] & 0x3f;
                    // and v0, a0, v0
                    ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[4].UD[0] & v0_raw;
                    // dsll32 v1, v1, 0x6  (v1 = v1 << 38)
                    uint64_t v1_shifted = (uint64_t)ctx.cpuRegs.GPR.r[3].UL[0] << (32 + 6);
                    
                    // Build mask: ori a0, zero, 0xfffc ; dsll a0, a0, 0x14 ; ori a0, 0xffff ; dsll a0, 0x10 ; ori a0, 0xffff ; dsll a0, 0xc ; ori a0, 0xfff
                    uint64_t a0_mask = 0xfffcULL;
                    a0_mask <<= 0x14; a0_mask |= 0xffffULL;   // 0xfffc0000ffff
                    a0_mask <<= 0x10; a0_mask |= 0xffffULL;   // 0xfffc0000ffffffff
                    a0_mask <<= 0x0c; a0_mask |= 0xfffULL;    // 0xc0000fffffffffff  (need to recheck)
                    // Actually let me just compute step by step:
                    // 0xfffc << 20 = 0xfffc00000
                    // | 0xffff = 0xfffc0000ffff
                    // << 16 = 0xfffc0000ffff0000
                    // | 0xffff = 0xfffc0000ffffffff  -- wait this is only 48 bits
                    // Hmm, let me redo: 0xfffc is 16 bits
                    // After <<20: 0x000000fffc00000 (36 bits)
                    // |0xffff: 0x000000fffc0ffff -- no wait
                    // 0xfffc << 20 = 0xFFFC00000 (36 bits set)
                    // | 0xFFFF = 0xFFFC0FFFF -- no, OR doesn't overlap
                    // Actually: 0xFFFC << 20 = 0x0000_00FF_FC00_0000
                    // | 0xFFFF = 0x0000_00FF_FC00_FFFF
                    // << 16 = 0x00FF_FC00_FFFF_0000
                    // | 0xFFFF = 0x00FF_FC00_FFFF_FFFF
                    // << 12 = 0xFFFC_00FF_FFFF_F000
                    // | 0xFFF = 0xFFFC_00FF_FFFF_FFFF
                    a0_mask = 0xfffcULL;
                    a0_mask <<= 20; // 0xFFFC00000
                    a0_mask |= 0xffffULL; // 0xFFFC0FFFF
                    a0_mask <<= 16; // 0xFFFC0FFFF0000
                    a0_mask |= 0xffffULL; // 0xFFFC0FFFFFFFF
                    a0_mask <<= 12; // 0xFFFC0FFFFFFFF000
                    a0_mask |= 0xfffULL; // 0xFFFC0FFFFFFFFFFF
                    // Hmm, that doesn't look right either. Let me trace the MIPS exactly:
                    // ori a0, zero, 0xfffc  => a0 = 0x000000000000FFFC
                    // dsll a0, a0, 0x14     => a0 = 0x00000FFFC00000 (shift left 20)
                    //   = 0x0000000FFFC00000
                    // ori a0, a0, 0xffff    => a0 = 0x0000000FFFC0FFFF
                    // dsll a0, a0, 0x10     => a0 = 0x000FFFC0FFFF0000 (shift left 16)
                    // ori a0, a0, 0xffff    => a0 = 0x000FFFC0FFFFFFFF
                    // dsll a0, a0, 0xc      => a0 = 0xFFFC0FFFFFFFF000 (shift left 12)
                    // ori a0, a0, 0xfff     => a0 = 0xFFFC0FFFFFFFFFFF
                    a0_mask = 0xfffcULL;
                    a0_mask = (a0_mask << 20);                    // 0x0000000FFFC00000
                    a0_mask = (a0_mask | 0xffffULL);              // 0x0000000FFFC0FFFF
                    a0_mask = (a0_mask << 16);                    // 0x000FFFC0FFFF0000
                    a0_mask = (a0_mask | 0xffffULL);              // 0x000FFFC0FFFFFFFF
                    a0_mask = (a0_mask << 12);                    // 0xFFFC0FFFFFFFF000
                    a0_mask = (a0_mask | 0xfffULL);               // 0xFFFC0FFFFFFFFFFF
                    
                    // or v0, v0, v1
                    ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] | v1_shifted;
                    
                    // Build mask2: ori a1, zero, 0xff03 ; dsll a1, 0x10 ; ori 0xffff ; dsll 0x10 ; ori 0xffff ; dsll 0x10 ; ori 0xffff
                    uint64_t a1_mask = 0xff03ULL;
                    a1_mask <<= 16; a1_mask |= 0xffffULL;  // 0xff03ffff
                    a1_mask <<= 16; a1_mask |= 0xffffULL;  // 0xff03ffffffff
                    a1_mask <<= 16; a1_mask |= 0xffffULL;  // 0xff03ffffffffffff
                    
                    // dsrl32 v1, v0, 0x12  (v1 = v0 >> 50)
                    uint64_t v1_extracted = ctx.cpuRegs.GPR.r[2].UD[0] >> 50;
                    // andi v1, v1, 0x3f
                    v1_extracted &= 0x3f;
                    
                    // and v0, v0, a0  (apply first mask)
                    ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] & a0_mask;
                    // dsll32 v1, v1, 0xc  (v1 = v1 << 44)
                    v1_extracted <<= 44;
                    
                    // lw a0, 0x10(s1)
                    ctx.cpuRegs.GPR.r[4].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x10);
                    
                    // or v0, v0, v1
                    ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] | v1_extracted;
                    // and v0, v0, a1  (apply second mask)
                    ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] & a1_mask;
                    
                    // sw a0, 0x18(s1)  (param_1[0x18] = param_1[0x10])
                    memory::write<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x18, ctx.cpuRegs.GPR.r[4].UL[0]);
                    
                    // or v0, v0, a2  (a2 = 0x60000000000000 from earlier)
                    ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] | ((uint64_t)0xc000 << (32 + 7));
                    
                    // sd v0, 0x0(s1)
                    memory::write<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0, ctx.cpuRegs.GPR.r[2].UD[0]);
                    
                    g_logFile << "[177168-PROMOTE] old=0x" << std::hex << ctx.cpuRegs.GPR.r[4].UD[0]
                              << " new=0x" << ctx.cpuRegs.GPR.r[2].UD[0]
                              << " promoted_case=" << std::dec << ((ctx.cpuRegs.GPR.r[2].UD[0] >> 44) & 0x3f) << std::endl;
                }
                
                // lw v1, 0x4(s5)  ; sw v1, 0x1c(s1)
                ctx.cpuRegs.GPR.r[3].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[21].UL[0] + 0x4);
                memory::write<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x1c, ctx.cpuRegs.GPR.r[3].UL[0]);
                // ld v0, 0x0(s1)
                ctx.cpuRegs.GPR.r[2].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0);


            Label_17729c:
                ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] >> 44;
                ctx.cpuRegs.GPR.r[4].UL[0] = ctx.cpuRegs.GPR.r[2].UL[0] & 0x3f;
                
                // [DIAG] Log the switch case value and the raw 64-bit object word
                {
                    uint64_t raw_qw = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0);
                    g_logFile << "[177168-SWITCH] param1=0x" << std::hex << ctx.cpuRegs.GPR.r[17].UL[0]
                              << " raw_qw=0x" << raw_qw
                              << " case=" << std::dec << ctx.cpuRegs.GPR.r[4].UL[0] << std::endl;
                }


                if (ctx.cpuRegs.GPR.r[4].UL[0] >= 0x17) {
                    goto Label_caseD_17;
                }
    )code" << std::endl;




        file << R"code(
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
                {
                uint32_t ptr = memory::read<uint32_t>(0x309738);
                ctx.cpuRegs.GPR.r[3].UL[0] = memory::read<uint8_t>(ptr + 3);
                ctx.cpuRegs.GPR.r[3].UL[0] = ctx.cpuRegs.GPR.r[3].UL[0] & 0xf;
                
                g_logFile << "[177168-CASE0] DAT_00309738 ptr=0x" << std::hex << ptr
                          << " byte_at_ptr3=0x" << ctx.cpuRegs.GPR.r[3].UL[0]
                          << " (check: & 0xf == 1? " << (ctx.cpuRegs.GPR.r[3].UL[0] == 1 ? "YES-SKIP" : "NO-PROCEED") << ")"
                          << std::dec << std::endl;
                
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
                CHECK_SP(0x1f0e68_case0_first)
                
                ctx.cpuRegs.GPR.r[20].UL[0] = ctx.cpuRegs.GPR.r[17].UL[0] + 0x5d0;
                ctx.cpuRegs.GPR.r[31].UL[0] = 0x1772f0;
                if (recompiled_functions.count(0x1f0e68)) {
                    recompiled_functions[0x1f0e68](ctx, 0x1f0e68);
                } else {
                    ctx.cpuRegs.pc = 0x1f0e68;
                    return;
                }
                CHECK_SP(0x1f0e68_case0_second)
                
                ctx.cpuRegs.GPR.r[19].UL[0] = ctx.cpuRegs.GPR.r[17].UL[0] + 0x4f0;
                ctx.cpuRegs.GPR.r[16].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0] + 0x0);
                
                if (ctx.cpuRegs.GPR.r[16].UL[0] == 0) {
                    goto Label_177324;
                }
                
                ctx.cpuRegs.GPR.r[4].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x154);
                }


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
                CHECK_SP(0x1918a8_case0_loop)
                
                ctx.cpuRegs.GPR.r[16].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x148);


            Label_17731c:
                if (ctx.cpuRegs.GPR.r[16].UL[0] != 0) {
                    ctx.cpuRegs.GPR.r[4].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x154);
                    goto Label_177308;
                }
    )code" << std::endl;




        file << R"code(
            Label_177324:
                ctx.cpuRegs.GPR.r[31].UL[0] = 0x177328;
                if (recompiled_functions.count(0x181400)) {
                    recompiled_functions[0x181400](ctx, 0x181400);
                } else {
                    ctx.cpuRegs.pc = 0x181400;
                    return;
                }
                CHECK_SP(0x181400_case0)
                ctx.cpuRegs.GPR.r[18].UL[0] = 1;
                g_logFile << "[177168-CASE0] Completed! Setting next state r[18]=" << ctx.cpuRegs.GPR.r[18].UL[0] << std::endl;
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
                CHECK_SP(0x173ab0_case1)
                ctx.cpuRegs.GPR.r[5].UD[0] = ctx.cpuRegs.GPR.r[21].UD[0];
                ctx.cpuRegs.GPR.r[20].UL[0] = ctx.cpuRegs.GPR.r[17].UL[0] + 0x5d0;
                ctx.cpuRegs.GPR.r[18].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0];
                
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
                CHECK_SP(0x16d368_case1)
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
                CHECK_SP(0x173ba0_case2)
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
                CHECK_SP(0x173c58_case3)
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
                
                ctx.fpuRegs.fpr[1].f = ctx.fpuRegs.fpr[0].f;
                ctx.fpuRegs.fpr[1].SL = (int32_t)ctx.fpuRegs.fpr[1].f;
                ctx.cpuRegs.GPR.r[6].SL[0] = ctx.fpuRegs.fpr[1].SL;
                
                ctx.cpuRegs.GPR.r[31].UL[0] = 0x1773d0;
                if (recompiled_functions.count(0x172b60)) {
                    recompiled_functions[0x172b60](ctx, 0x172b60);
                } else {
                    ctx.cpuRegs.pc = 0x172b60;
                    return;
                }
                CHECK_SP(0x172b60_case4)
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
                CHECK_SP(0x172b60_case5)
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
                    ctx.cpuRegs.GPR.r[5].UL[0] = 0x1000000;
                    ctx.cpuRegs.GPR.r[6].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x28);
                    
                    ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] & mask;
                    ctx.cpuRegs.GPR.r[4].UL[0] = ctx.cpuRegs.GPR.r[17].UL[0] + 0x594;
                    ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] | ctx.cpuRegs.GPR.r[5].UL[0];
                    ctx.cpuRegs.GPR.r[7].UL[0] = 0;
                    ctx.cpuRegs.GPR.r[5].UD[0] = ctx.cpuRegs.GPR.r[19].UD[0];
                    
                    ctx.cpuRegs.GPR.r[31].UL[0] = 0x17743c;
                    if (recompiled_functions.count(0x168810)) {
                        recompiled_functions[0x168810](ctx, 0x168810);
                    } else {
                        ctx.cpuRegs.pc = 0x168810;
                        return;
                    }
                    CHECK_SP(0x168810_case5)
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
                CHECK_SP(0x173ce8_case6)
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
                CHECK_SP(0x173f38_case7)
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
                CHECK_SP(0x174108_case8)
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
                CHECK_SP(0x174240_case9)
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
                CHECK_SP(0x174478_caseA)
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
                CHECK_SP(0x1745d0_caseB)
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
                CHECK_SP(0x174a70_caseC)
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
                CHECK_SP(0x174f38_caseD)
                ctx.cpuRegs.GPR.r[5].UD[0] = ctx.cpuRegs.GPR.r[21].UD[0];
                ctx.cpuRegs.GPR.r[20].UL[0] = ctx.cpuRegs.GPR.r[17].UL[0] + 0x5d0;
                goto Label_17770c;
    )code" << std::endl;




        file << R"code(
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
                CHECK_SP(0x1f0e68_caseE_first)
                
                ctx.cpuRegs.GPR.r[31].UL[0] = 0x177538;
                if (recompiled_functions.count(0x1f0e68)) {
                    recompiled_functions[0x1f0e68](ctx, 0x1f0e68);
                } else {
                    ctx.cpuRegs.pc = 0x1f0e68;
                    return;
                }
                CHECK_SP(0x1f0e68_caseE_second)
                
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
                CHECK_SP(0x1918a8_caseE_loop)
                
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
                CHECK_SP(0x181400_caseE)


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
                CHECK_SP(0x172b60_caseE)
                ctx.cpuRegs.GPR.r[9].UD[0] = ctx.cpuRegs.GPR.r[29].UD[0];
                
                if (ctx.cpuRegs.GPR.r[2].UL[0] == 0) {
                    goto Label_1775b0;
                }
                
                ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[17].UD[0];
                ctx.cpuRegs.GPR.r[5].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x54);
                ctx.cpuRegs.GPR.r[6].UL[0] = memory::read<uint8_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x0);
                
                ctx.cpuRegs.GPR.r[31].UL[0] = 0x1775a4;
                if (recompiled_functions.count(0x17b980)) {
                    recompiled_functions[0x17b980](ctx, 0x17b980);
                } else {
                    ctx.cpuRegs.pc = 0x17b980;
                    return;
                }
                CHECK_SP(0x17b980_caseE)
                ctx.cpuRegs.GPR.r[18].UL[0] = 0x18;
                goto Label_177714;


            Label_1775b0:
                goto Label_177710;
    )code" << std::endl;




        file << R"code(
            Label_caseD_f:
                ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[17].UD[0];
                ctx.cpuRegs.GPR.r[31].UL[0] = 0x1775c0;
                if (recompiled_functions.count(0x174c70)) {
                    recompiled_functions[0x174c70](ctx, 0x174c70);
                } else {
                    ctx.cpuRegs.pc = 0x174c70;
                    return;
                }
                CHECK_SP(0x174c70_caseF)
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
                CHECK_SP(0x174d90_case10)
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
                ctx.fpuRegs.fpr[1].f = ctx.fpuRegs.fpr[0].f;
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
                CHECK_SP(0x175008_case12)
                ctx.cpuRegs.GPR.r[5].UD[0] = ctx.cpuRegs.GPR.r[21].UD[0];
                ctx.cpuRegs.GPR.r[20].UL[0] = ctx.cpuRegs.GPR.r[17].UL[0] + 0x5d0;
                goto Label_17770c;
    )code" << std::endl;




        file << R"code(
            Label_caseD_13:
                ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[17].UD[0];
                ctx.cpuRegs.GPR.r[31].UL[0] = 0x177658;
                if (recompiled_functions.count(0x175160)) {
                    recompiled_functions[0x175160](ctx, 0x175160);
                } else {
                    ctx.cpuRegs.pc = 0x175160;
                    return;
                }
                CHECK_SP(0x175160_case13)
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
                ctx.fpuRegs.fpr[1].f = ctx.fpuRegs.fpr[0].f;
                ctx.fpuRegs.fpr[1].SL = (int32_t)ctx.fpuRegs.fpr[1].f;
                ctx.cpuRegs.GPR.r[6].SL[0] = ctx.fpuRegs.fpr[1].SL;
                
                ctx.cpuRegs.GPR.r[31].UL[0] = 0x1776a0;
                if (recompiled_functions.count(0x259bc0)) {
                    recompiled_functions[0x259bc0](ctx, 0x259bc0);
                } else {
                    ctx.cpuRegs.pc = 0x259bc0;
                    return;
                }
                CHECK_SP(0x259bc0_case14)
                ctx.cpuRegs.GPR.r[19].UL[0] = ctx.cpuRegs.GPR.r[17].UL[0] + 0x4f0;
                ctx.cpuRegs.GPR.r[4].UL[0] = ctx.cpuRegs.GPR.r[29].UL[0] + 0x10;
                
                ctx.cpuRegs.GPR.r[31].UL[0] = 0x1776ac;
                if (recompiled_functions.count(0x11bf10)) {
                    recompiled_functions[0x11bf10](ctx, 0x11bf10);
                } else {
                    ctx.cpuRegs.pc = 0x11bf10;
                    return;
                }
                CHECK_SP(0x11bf10_case14_first)
                ctx.cpuRegs.GPR.r[5].UL[0] = 0;
                ctx.cpuRegs.GPR.r[16].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x10);
                ctx.cpuRegs.GPR.r[4].UL[0] = ctx.cpuRegs.GPR.r[29].UL[0] + 0x14;
                
                ctx.cpuRegs.GPR.r[31].UL[0] = 0x1776bc;
                if (recompiled_functions.count(0x11bf10)) {
                    recompiled_functions[0x11bf10](ctx, 0x11bf10);
                } else {
                    ctx.cpuRegs.pc = 0x11bf10;
                    return;
                }
                CHECK_SP(0x11bf10_case14_second)
                ctx.cpuRegs.GPR.r[5].UL[0] = 8;
                ctx.cpuRegs.GPR.r[3].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x14);
                ctx.cpuRegs.GPR.r[2].UL[0] = ctx.cpuRegs.GPR.r[17].UL[0] + 0x19c0;
                memory::write<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0] + 0x8, ctx.cpuRegs.GPR.r[16].UL[0]);
                memory::write<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0] + 0xc, ctx.cpuRegs.GPR.r[3].UL[0]);
                goto Label_177710;
    )code" << std::endl;




        file << R"code(
            Label_caseD_15:
                ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[17].UD[0];
                ctx.cpuRegs.GPR.r[31].UL[0] = 0x1776dc;
                if (recompiled_functions.count(0x1754b8)) {
                    recompiled_functions[0x1754b8](ctx, 0x1754b8);
                } else {
                    ctx.cpuRegs.pc = 0x1754b8;
                    return;
                }
                CHECK_SP(0x1754b8_case15)
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
                CHECK_SP(0x175618_case16)
                ctx.cpuRegs.GPR.r[5].UD[0] = ctx.cpuRegs.GPR.r[21].UD[0];
                ctx.cpuRegs.GPR.r[20].UL[0] = ctx.cpuRegs.GPR.r[17].UL[0] + 0x5d0;
                goto Label_17770c;


            Label_caseD_17:
                ctx.cpuRegs.GPR.r[18].UL[0] = 0x18;


            Label_177708:
                g_logFile << "[177168-EXIT] at Label_177708 (default exit) r[18]=0x" << std::hex << ctx.cpuRegs.GPR.r[18].UL[0] << std::dec << std::endl;
                ctx.cpuRegs.GPR.r[20].UL[0] = ctx.cpuRegs.GPR.r[17].UL[0] + 0x5d0;


            Label_17770c:
                ctx.cpuRegs.GPR.r[19].UL[0] = ctx.cpuRegs.GPR.r[17].UL[0] + 0x4f0;


            Label_177710:
                ctx.cpuRegs.GPR.r[2].UL[0] = 0x18;
    )code" << std::endl;




        file << R"code(
            Label_177714:
                // Delay slot: andi v1, s2, 0x3f executes BEFORE the branch
                ctx.cpuRegs.GPR.r[3].UL[0] = ctx.cpuRegs.GPR.r[18].UL[0] & 0x3f;
                
                g_logFile << "[177168-WRITEBACK] r[18]=0x" << std::hex << ctx.cpuRegs.GPR.r[18].UL[0]
                          << " r[2]=0x" << ctx.cpuRegs.GPR.r[2].UL[0]
                          << " (skip write? " << (ctx.cpuRegs.GPR.r[18].UL[0] == ctx.cpuRegs.GPR.r[2].UL[0] ? "YES" : "NO") << ")"
                          << std::dec << std::endl;
                if (ctx.cpuRegs.GPR.r[18].UL[0] == ctx.cpuRegs.GPR.r[2].UL[0]) {
                    goto Label_17774c;
                }
                
                ctx.cpuRegs.GPR.r[2].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0);
                
                {
                    // Build mask 0xff03ffffffffffff (clears bits 48-55 except top 2)
                    // MIPS: ori 0xff03, dsll 16, ori 0xffff, dsll 16, ori 0xffff, dsll 16, ori 0xffff
                    uint64_t mask = 0xff03ULL;
                    mask <<= 16; mask |= 0xffffULL;  // 0xff03ffff
                    mask <<= 16; mask |= 0xffffULL;  // 0xff03ffffffff
                    mask <<= 16; mask |= 0xffffULL;  // 0xff03ffffffffffff
                    
                    uint64_t v1_shift = (uint64_t)ctx.cpuRegs.GPR.r[3].UL[0] << 50;
                    
                    ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] & mask;
                    ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] | v1_shift;
                    
                    g_logFile << "[177168-WRITE] Writing state to addr=0x" << std::hex << ctx.cpuRegs.GPR.r[17].UL[0]
                              << " new_qw=0x" << ctx.cpuRegs.GPR.r[2].UD[0]
                              << " mask=0x" << mask
                              << " shift=0x" << v1_shift
                              << " new_case=" << std::dec << ((ctx.cpuRegs.GPR.r[2].UD[0] >> 44) & 0x3f) << std::endl;
                    memory::write<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0, ctx.cpuRegs.GPR.r[2].UD[0]);
                }


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
                ctx.cpuRegs.GPR.r[16].UD[0] = ctx.cpuRegs.GPR.r[4].UD[0];
                goto Label_177784;


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
                CHECK_SP(0x192118_post_switch)
                ctx.cpuRegs.GPR.r[5].UL[0] = 0xc;
                ctx.cpuRegs.GPR.r[18].UL[0] = 0;
                goto Label_1777a4;


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
                    uint64_t v1 = 0xfc00ULL << 34;
                    uint64_t a0 = 0xc000ULL << 32;
                    ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] & v1;
                    ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] ^ a0;
                    ctx.cpuRegs.GPR.r[2].UL[0] = (ctx.cpuRegs.GPR.r[2].UL[0] < 1) ? 1 : 0;
                    
                    if (ctx.cpuRegs.GPR.r[2].UL[0] == 0) {
                        goto Label_177850;
                    }
                }
                    )code" << std::endl;




        file << R"code(
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
                CHECK_SP(0x2b3548_post_switch)
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
                
                ctx.fpuRegs.fpr[2].f = ctx.fpuRegs.fpr[1].f;
                ctx.fpuRegs.fpr[2].SL = (int32_t)ctx.fpuRegs.fpr[2].f;
                ctx.cpuRegs.GPR.r[6].SL[0] = ctx.fpuRegs.fpr[2].SL;
                
                ctx.fpuRegs.fpr[1].f = ctx.fpuRegs.fpr[0].f;
                ctx.fpuRegs.fpr[1].SL = (int32_t)ctx.fpuRegs.fpr[1].f;
                ctx.cpuRegs.GPR.r[7].SL[0] = ctx.fpuRegs.fpr[1].SL;
                
                ctx.cpuRegs.GPR.r[31].UL[0] = 0x177848;
                if (recompiled_functions.count(0x259b20)) {
                    recompiled_functions[0x259b20](ctx, 0x259b20);
                } else {
                    ctx.cpuRegs.pc = 0x259b20;
                    return;
                }
                CHECK_SP(0x259b20_post_switch)
                ctx.cpuRegs.GPR.r[5].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x6d0);


            Label_17784c:
                ctx.cpuRegs.GPR.r[6].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x34);
    )code" << std::endl;




        file << R"code(
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
                CHECK_SP(0x1714a8_epilogue)
                ctx.cpuRegs.GPR.r[5].UD[0] = ctx.cpuRegs.GPR.r[21].UD[0];
                
                ctx.cpuRegs.GPR.r[4].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0);
                {
                    uint64_t v1 = 0xfc00ULL << 34;
                    uint64_t a0 = 0xc000ULL << 32;
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


            Label_177898:
                ctx.cpuRegs.GPR.r[31].UL[0] = 0x17789c;
                if (recompiled_functions.count(0x11e4b8)) {
                    recompiled_functions[0x11e4b8](ctx, 0x11e4b8);
                } else {
                    ctx.cpuRegs.pc = 0x11e4b8;
                    return;
                }
                CHECK_SP(0x11e4b8_epilogue)
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
                CHECK_SP(0x192118_epilogue)
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
                CHECK_SP(0x1e5818_epilogue)


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
                CHECK_SP(0x15bcc0_epilogue)
                ctx.cpuRegs.GPR.r[2].UL[0] = 1;


            Label_1778dc:
                // Final SP check before epilogue restore
                if (ctx.cpuRegs.GPR.r[29].UL[0] != expected_sp) {
                    g_logFile << "[177168] SP CORRUPTED at epilogue! expected=0x" 
                            << std::hex << expected_sp 
                            << " got=0x" << ctx.cpuRegs.GPR.r[29].UL[0] << std::endl;
                }
    )code" << std::endl;




        file << R"code(
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


        #undef CHECK_SP
            }


        )code" << std::endl;
        return;
    }
    if (func.base_address == 0x00101590) {
        file << R"code(
        
            void FUN_00101590(CpuContext& ctx) {
                Label_0000:
                    ctx.cpuRegs.GPR.r[29].SD[0] = static_cast<int32_t>(ctx.cpuRegs.GPR.r[29].SL[0] + 0xfffffff0);
                    memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x0, ctx.cpuRegs.GPR.r[31].UD[0]);
                    
                    // DELAY SLOT: lw a0, 0xf8(a0) — dereference pointer at obj+0xf8
                    ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(
                        memory::read<uint32_t>(ctx.cpuRegs.GPR.r[4].UL[0] + 0xf8));
                    
                    ctx.cpuRegs.GPR.r[31].UL[0] = 0x1015a0;
                    if (recompiled_functions.count(0x17c060)) {
                        recompiled_functions[0x17c060](ctx, 0x17c060);
                        goto Label_0001;
                    } else {
                        ctx.cpuRegs.pc = 0x17c060;
                    }


                Label_0001:
                    ctx.cpuRegs.GPR.r[31].UD[0] = memory::read<uint64_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x0);
                    ctx.cpuRegs.GPR.r[29].SD[0] = static_cast<int32_t>(ctx.cpuRegs.GPR.r[29].SL[0] + 0x10);
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




    if (func.base_address == 0x002a0250) {
        file << R"code(
// Function: FUN_002a0250 at 0x2a0250
void FUN_002a0250(CpuContext& ctx) {
Label_0000: // 0x2a0250
        ctx.cpuRegs.GPR.r[29].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[29].SL[0]) + 0xffffffe0);
        memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x0, ctx.cpuRegs.GPR.r[16].UD[0]);
        memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x8, ctx.cpuRegs.GPR.r[17].UD[0]);
        memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x10, ctx.cpuRegs.GPR.r[18].UD[0]);
        ctx.cpuRegs.GPR.r[17].SD[0] = ctx.cpuRegs.GPR.r[4].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
        memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x18, ctx.cpuRegs.GPR.r[31].UD[0]);
        ctx.cpuRegs.GPR.r[18].SD[0] = ctx.cpuRegs.GPR.r[5].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x20));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x0));
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0]) + 0x20)));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0]) + 0x24));
        ctx.cpuRegs.GPR.r[16].SD[0] = static_cast<int32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) >> 20);
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[17].SL[0]) + static_cast<int32_t>(ctx.cpuRegs.GPR.r[4].SL[0]));
        ctx.cpuRegs.GPR.r[16].UD[0] = ctx.cpuRegs.GPR.r[16].UD[0] & static_cast<uint64_t>(0x3f);
        // JALR - Jump and Link Register
    ctx.cpuRegs.GPR.r[31].UL[0] = 0x2a028c;
    {
        uint32_t target = ctx.cpuRegs.GPR.r[5].UL[0];
        auto func_ptr = find_containing_function(target);
        if (func_ptr != nullptr) {
            func_ptr(ctx, target);
        } else {
            ctx.cpuRegs.pc = target;
        }
    }
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x0));
        ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[5].UL[0]) >> 12);
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] & static_cast<uint64_t>(0xf);
        ctx.cpuRegs.GPR.r[3].UD[0] = (ctx.cpuRegs.GPR.r[4].UL[0] < 0xe) ? 1 : 0;
    bool branch_taken_2a029c = (ctx.cpuRegs.GPR.r[3].UL[0] == ctx.cpuRegs.GPR.r[0].UL[0]);
        ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0xfffff0ff);
    if (branch_taken_2a029c) {
        goto Label_0002;
    } else {
        goto Label_0001;
    }


Label_0001: // 0x2a02a4
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[4].UL[0]) << 8);
        ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[5].UD[0] & ctx.cpuRegs.GPR.r[2].UD[0];
        ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(0xffff0000);
        ctx.cpuRegs.GPR.r[3].UD[0] = ctx.cpuRegs.GPR.r[3].UD[0] | static_cast<uint64_t>(0xfff);
        ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] | ctx.cpuRegs.GPR.r[4].UD[0];
        ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] & ctx.cpuRegs.GPR.r[3].UD[0];
        ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] | static_cast<uint64_t>(0xe000);
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0, ctx.cpuRegs.GPR.r[2].UL[0]);
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[18].UL[0]) + 0x4));
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x8, ctx.cpuRegs.GPR.r[3].UL[0]);
// This BLOCK HAS NO BRANCH SO GO TO NEXT BLOCK AFTER THIS, CURRENT BLOCK ENDS AT: 0x2a02cc
// Fall through to 0x2a02cc
    goto Label_0002; // Fall through


Label_0002: // 0x2a02cc
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x0));
        ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(0xfff00000);
        ctx.cpuRegs.GPR.r[3].UD[0] = ctx.cpuRegs.GPR.r[3].UD[0] | static_cast<uint64_t>(0xffff);
        ctx.cpuRegs.GPR.r[4].UD[0] = (ctx.cpuRegs.GPR.r[16].UL[0] < 0x21) ? 1 : 0;
        ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] & ctx.cpuRegs.GPR.r[3].UD[0];
    bool branch_taken_2a02e0 = (ctx.cpuRegs.GPR.r[4].UL[0] == ctx.cpuRegs.GPR.r[0].UL[0]);
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0, ctx.cpuRegs.GPR.r[2].UL[0]);
    if (branch_taken_2a02e0) {
        goto Label_0044;
    } else {
        goto Label_0003;
    }


Label_0003: // 0x2a02e8
        ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(0x300000);
        ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) << 2);
        ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[2].SL[0]) + 0x58e0);
        ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[3].SL[0]) + static_cast<int32_t>(ctx.cpuRegs.GPR.r[2].SL[0]));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x0));
// This BLOCK HAS NO BRANCH SO GO TO NEXT BLOCK AFTER THIS, CURRENT BLOCK ENDS AT: 0x2a02fc
// Fall through to 0x2a02fc
    goto Label_0004; // Fall through


Label_0004: // 0x2a02fc - SWITCH DISPATCH (jr to internal labels)
    {
        uint32_t target = ctx.cpuRegs.GPR.r[4].UL[0];
        switch (target) {
            case 0x2a06f4: goto Label_0044;  // case 0
            case 0x2a06d4: goto Label_0042;  // case 1,4,7,0xa,0x13,0x16,0x19,0x1c
            case 0x2a0304: goto Label_0005;  // case 2
            case 0x2a030c: goto Label_0006;  // case 3
            case 0x2a035c: goto Label_0010;  // case 5
            case 0x2a0374: goto Label_0011;  // case 6
            case 0x2a0388: goto Label_0012;  // case 8
            case 0x2a03a0: goto Label_0013;  // case 9
            case 0x2a03b4: goto Label_0014;  // case 0xb
            case 0x2a03d4: goto Label_0015;  // case 0xc
            case 0x2a0410: goto Label_0017;  // case 0xd
            case 0x2a0428: goto Label_0018;  // case 0xe
            case 0x2a043c: goto Label_0019;  // case 0xf
            case 0x2a0450: goto Label_0020;  // case 0x10
            case 0x2a046c: goto Label_0021;  // case 0x11
            case 0x2a04b4: goto Label_0022;  // case 0x12
            case 0x2a04c8: goto Label_0023;  // case 0x14
            case 0x2a04e0: goto Label_0024;  // case 0x15
            case 0x2a04f4: goto Label_0025;  // case 0x17
            case 0x2a05a0: goto Label_0028;  // case 0x18
            case 0x2a05c0: goto Label_0030;  // case 0x1a
            case 0x2a0630: goto Label_0033;  // case 0x1b
            case 0x2a0650: goto Label_0035;  // case 0x1d
            case 0x2a0674: goto Label_0037;  // case 0x1e
            case 0x2a0698: goto Label_0039;  // case 0x1f
            case 0x2a06dc: goto Label_0043;  // case 0x20
            default:
                g_logFile << "[FUN_002a0250] UNKNOWN switch target: 0x"
                          << std::hex << target << " s0=" << ctx.cpuRegs.GPR.r[16].UL[0] << std::endl;
                goto Label_0042;  // default → case 1 (s0=0)
        }
    }




Label_0005: // 0x2a0304
        ctx.cpuRegs.GPR.r[16].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x3);
    goto Label_0044;


Label_0006: // 0x2a030c
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[18].UL[0]) + 0x4));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x8));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0xc));
        ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[2].SL[0]) - static_cast<int32_t>(ctx.cpuRegs.GPR.r[3].SL[0]));
        // slt - Set on Less Than (Signed)
    ctx.cpuRegs.GPR.r[2].UD[0] = (ctx.cpuRegs.GPR.r[2].SL[0] < ctx.cpuRegs.GPR.r[4].SL[0]) ? 1 : 0;
        ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] ^ static_cast<uint64_t>(0x1);
        bool cond_2753316 = (ctx.cpuRegs.GPR.r[2].UL[0] == ctx.cpuRegs.GPR.r[0].UL[0]);
    if (cond_2753316) {
        // Delay Slot (Likely)
            // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x0));
        goto Label_0009;
    } else {
        goto Label_0007;
    }


Label_0007: // 0x2a032c
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x20));
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x38)));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x3c));
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[17].SL[0]) + static_cast<int32_t>(ctx.cpuRegs.GPR.r[4].SL[0]));
        // JALR - Jump and Link Register
    ctx.cpuRegs.GPR.r[31].UL[0] = 0x2a0340;
    {
        uint32_t target = ctx.cpuRegs.GPR.r[2].UL[0];
        auto func_ptr = find_containing_function(target);
        if (func_ptr != nullptr) {
            func_ptr(ctx, target);
        } else {
            ctx.cpuRegs.pc = target;
        }
    }
    bool branch_taken_2a0340 = (ctx.cpuRegs.GPR.r[2].UL[0] == ctx.cpuRegs.GPR.r[0].UL[0]);
        ctx.cpuRegs.GPR.r[16].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x4);
    if (branch_taken_2a0340) {
        goto Label_0027;
    } else {
        goto Label_0008;
    }


Label_0008: // 0x2a0348
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x20));
    goto Label_0045;


Label_0009: // 0x2a0350
        ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0]) >> 20);
        ctx.cpuRegs.GPR.r[16].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] & static_cast<uint64_t>(0x3f);
    goto Label_0044;


Label_0010: // 0x2a035c
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x20));
        ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x2);
        ctx.cpuRegs.GPR.r[6].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x6);
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x18)));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x1c));
    goto Label_0032;


Label_0011: // 0x2a0374
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[8].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x20));
        ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x2);
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x8));
        ctx.cpuRegs.GPR.r[6].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x7);
    goto Label_0016;


Label_0012: // 0x2a0388
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x20));
        ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x3);
        ctx.cpuRegs.GPR.r[6].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x9);
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x18)));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x1c));
    goto Label_0032;


Label_0013: // 0x2a03a0
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[8].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x20));
        ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x3);
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x8));
        ctx.cpuRegs.GPR.r[6].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0xa);
    goto Label_0016;


Label_0014: // 0x2a03b4
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x14));
       // JAL was called 
   // The address after JAL is: 0x2a03bc
   // The next block should be: 2a03d4
ctx.cpuRegs.GPR.r[31].UL[0] = 0x2a03bc;
    if (recompiled_functions.count(0x2a7f60)) {
        recompiled_functions[0x2a7f60](ctx, 0x2a7f60);


    } else {
        ctx.cpuRegs.pc = 0x2a7f60;
    }
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x20));
        ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x4);
        ctx.cpuRegs.GPR.r[6].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0xc);
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x18)));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x1c));
    goto Label_0032;


Label_0015: // 0x2a03d4
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[8].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x20));
        ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x4);
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x8));
        ctx.cpuRegs.GPR.r[6].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0xd);
// This BLOCK HAS NO BRANCH SO GO TO NEXT BLOCK AFTER THIS, CURRENT BLOCK ENDS AT: 0x2a03e4
// Fall through to 0x2a03e4
    goto Label_0016; // Fall through
        )code" << std::endl;


        file << R"code(
Label_0016: // 0x2a03e4
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[7].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[18].UL[0]) + 0x4));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0xc));
        ctx.cpuRegs.GPR.r[7].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[7].SL[0]) - static_cast<int32_t>(ctx.cpuRegs.GPR.r[2].SL[0]));
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[8].UL[0]) + 0x10)));
        // slt - Set on Less Than (Signed)
    ctx.cpuRegs.GPR.r[7].UD[0] = (ctx.cpuRegs.GPR.r[7].SL[0] < ctx.cpuRegs.GPR.r[3].SL[0]) ? 1 : 0;
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[8].UL[0]) + 0x14));
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[17].SL[0]) + static_cast<int32_t>(ctx.cpuRegs.GPR.r[4].SL[0]));
        ctx.cpuRegs.GPR.r[7].UD[0] = ctx.cpuRegs.GPR.r[7].UD[0] ^ static_cast<uint64_t>(0x1);
        // JALR - Jump and Link Register
    ctx.cpuRegs.GPR.r[31].UL[0] = 0x2a0408;
    {
        uint32_t target = ctx.cpuRegs.GPR.r[2].UL[0];
        auto func_ptr = find_containing_function(target);
        if (func_ptr != nullptr) {
            func_ptr(ctx, target);
        } else {
            ctx.cpuRegs.pc = target;
        }
    }
        ctx.cpuRegs.GPR.r[16].SD[0] = ctx.cpuRegs.GPR.r[2].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
    goto Label_0044;


Label_0017: // 0x2a0410
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x0));
        ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(0x4000000);
        ctx.cpuRegs.GPR.r[16].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0xe);
        ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] | ctx.cpuRegs.GPR.r[3].UD[0];
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0, ctx.cpuRegs.GPR.r[2].UL[0]);
    goto Label_0044;


Label_0018: // 0x2a0428
        ctx.cpuRegs.GPR.r[5].SD[0] = ctx.cpuRegs.GPR.r[18].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
        ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[17].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
       // JAL was called 
   // The address after JAL is: 0x2a0434
   // The next block should be: 2a043c
ctx.cpuRegs.GPR.r[31].UL[0] = 0x2a0434;
    if (recompiled_functions.count(0x29fef8)) {
        recompiled_functions[0x29fef8](ctx, 0x29fef8);


    } else {
        ctx.cpuRegs.pc = 0x29fef8;
    }
        ctx.cpuRegs.GPR.r[16].SD[0] = ctx.cpuRegs.GPR.r[2].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
    goto Label_0044;


Label_0019: // 0x2a043c
        ctx.cpuRegs.GPR.r[7].SD[0] = ctx.cpuRegs.GPR.r[18].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
        ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[17].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
        ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x5);
        ctx.cpuRegs.GPR.r[6].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x10);
    goto Label_0029;


Label_0020: // 0x2a0450
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x0));
        ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(0xfbff0000);
        ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] | static_cast<uint64_t>(0xffff);
        ctx.cpuRegs.GPR.r[16].SD[0] = ctx.cpuRegs.GPR.r[0].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
        ctx.cpuRegs.GPR.r[3].UD[0] = ctx.cpuRegs.GPR.r[3].UD[0] & ctx.cpuRegs.GPR.r[2].UD[0];
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0, ctx.cpuRegs.GPR.r[3].UL[0]);
    goto Label_0044;


Label_0021: // 0x2a046c
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x14));
       // JAL was called 
   // The address after JAL is: 0x2a0474
   // The next block should be: 2a04b4
ctx.cpuRegs.GPR.r[31].UL[0] = 0x2a0474;
    if (recompiled_functions.count(0x2a7f60)) {
        recompiled_functions[0x2a7f60](ctx, 0x2a7f60);


    } else {
        ctx.cpuRegs.pc = 0x2a7f60;
    }
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x1c));
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(0x87ff0000);
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x14));
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[4].UD[0] | static_cast<uint64_t>(0xffff);
        ctx.cpuRegs.GPR.r[8].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0xfffffff0);
        ctx.cpuRegs.GPR.r[9].SD[0] = static_cast<int32_t>(0x8000000);
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0] + 0x0, ctx.cpuRegs.GPR.r[2].UL[0]);
        ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x7);
        ctx.cpuRegs.GPR.r[6].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x15);
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x0));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x4));
        ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] & ctx.cpuRegs.GPR.r[4].UD[0];
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[7].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x20));
        ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] | ctx.cpuRegs.GPR.r[9].UD[0];
        ctx.cpuRegs.GPR.r[3].UD[0] = ctx.cpuRegs.GPR.r[3].UD[0] & ctx.cpuRegs.GPR.r[8].UD[0];
    goto Label_0031;


Label_0022: // 0x2a04b4
        ctx.cpuRegs.GPR.r[7].SD[0] = ctx.cpuRegs.GPR.r[18].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
        ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[17].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
        ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x6);
        ctx.cpuRegs.GPR.r[6].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x13);
    goto Label_0034;


Label_0023: // 0x2a04c8
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x20));
        ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x7);
        ctx.cpuRegs.GPR.r[6].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x15);
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x18)));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x1c));
    goto Label_0032;


Label_0024: // 0x2a04e0
        ctx.cpuRegs.GPR.r[5].SD[0] = ctx.cpuRegs.GPR.r[18].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
        ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[17].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
       // JAL was called 
   // The address after JAL is: 0x2a04ec
   // The next block should be: 2a04f4
ctx.cpuRegs.GPR.r[31].UL[0] = 0x2a04ec;
    if (recompiled_functions.count(0x29fde0)) {
        recompiled_functions[0x29fde0](ctx, 0x29fde0);


    } else {
        ctx.cpuRegs.pc = 0x29fde0;
    }
        ctx.cpuRegs.GPR.r[16].SD[0] = ctx.cpuRegs.GPR.r[2].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
    goto Label_0044;


Label_0025: // 0x2a04f4
        ctx.cpuRegs.GPR.r[2].UD[0] = static_cast<uint64_t>(memory::read<uint8_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x0));
        ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(0x87ff0000);
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x18));
        ctx.cpuRegs.GPR.r[5].UD[0] = ctx.cpuRegs.GPR.r[5].UD[0] | static_cast<uint64_t>(0xffff);
        // dsrl - Doubleword Shift Right Logical
    ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] >> 4;
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[7].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x1c));
        ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0]) << 2);
        ctx.cpuRegs.GPR.r[9].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0xfffffff0);
        ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[2].SL[0]) + static_cast<int32_t>(ctx.cpuRegs.GPR.r[3].SL[0]));
        ctx.cpuRegs.GPR.r[8].SD[0] = static_cast<int32_t>(0x10000000);
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[6].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0]) + 0x0));
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[7].UL[0] + 0x0, ctx.cpuRegs.GPR.r[6].UL[0]);
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x1c));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x14));
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[4].UL[0] + 0x4, ctx.cpuRegs.GPR.r[2].UL[0]);
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x0));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x4));
        ctx.cpuRegs.GPR.r[3].UD[0] = ctx.cpuRegs.GPR.r[3].UD[0] & ctx.cpuRegs.GPR.r[5].UD[0];
        ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] & ctx.cpuRegs.GPR.r[9].UD[0];
        ctx.cpuRegs.GPR.r[3].UD[0] = ctx.cpuRegs.GPR.r[3].UD[0] | ctx.cpuRegs.GPR.r[8].UD[0];
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x4, ctx.cpuRegs.GPR.r[2].UL[0]);
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0, ctx.cpuRegs.GPR.r[3].UL[0]);
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[6].UL[0]) + 0x28));
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[5].UL[0]) + 0x40)));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[5].UL[0]) + 0x44));
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[6].SL[0]) + static_cast<int32_t>(ctx.cpuRegs.GPR.r[4].SL[0]));
        // JALR - Jump and Link Register
    ctx.cpuRegs.GPR.r[31].UL[0] = 0x2a0560;
    {
        uint32_t target = ctx.cpuRegs.GPR.r[2].UL[0];
        auto func_ptr = find_containing_function(target);
        if (func_ptr != nullptr) {
            func_ptr(ctx, target);
        } else {
            ctx.cpuRegs.pc = target;
        }
    }
    bool branch_taken_2a0560 = (ctx.cpuRegs.GPR.r[2].UL[0] == ctx.cpuRegs.GPR.r[0].UL[0]);
        ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x8);
    if (branch_taken_2a0560) {
        goto Label_0027;
    } else {
        goto Label_0026;
    }


Label_0026: // 0x2a0568
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x20));
        ctx.cpuRegs.GPR.r[6].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x18);
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x18)));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x1c));
    goto Label_0032;


Label_0027: // 0x2a057c
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x0));
        ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(0xfff00000);
        ctx.cpuRegs.GPR.r[3].UD[0] = ctx.cpuRegs.GPR.r[3].UD[0] | static_cast<uint64_t>(0xffff);
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(0x10000);
        ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] & ctx.cpuRegs.GPR.r[3].UD[0];
        ctx.cpuRegs.GPR.r[16].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x1);
        ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] | ctx.cpuRegs.GPR.r[4].UD[0];
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0, ctx.cpuRegs.GPR.r[2].UL[0]);
    goto Label_0044;


Label_0028: // 0x2a05a0
        ctx.cpuRegs.GPR.r[7].SD[0] = ctx.cpuRegs.GPR.r[18].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
        ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[17].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
        ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x8);
        ctx.cpuRegs.GPR.r[6].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x19);
// This BLOCK HAS NO BRANCH SO GO TO NEXT BLOCK AFTER THIS, CURRENT BLOCK ENDS AT: 0x2a05b0
// Fall through to 0x2a05b0
    goto Label_0029; // Fall through


Label_0029: // 0x2a05b0
       //nop 
       // JAL was called 
   // The address after JAL is: 0x2a05b8
   // The next block should be: 2a05c0
ctx.cpuRegs.GPR.r[31].UL[0] = 0x2a05b8;
    if (recompiled_functions.count(0x29fc98)) {
        recompiled_functions[0x29fc98](ctx, 0x29fc98);


    } else {
        ctx.cpuRegs.pc = 0x29fc98;
    }
        ctx.cpuRegs.GPR.r[16].SD[0] = ctx.cpuRegs.GPR.r[2].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
    goto Label_0044;
        )code" << std::endl;


        file << R"code(
Label_0030: // 0x2a05c0
        ctx.cpuRegs.GPR.r[2].UD[0] = static_cast<uint64_t>(memory::read<uint8_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x0));
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(0x87ff0000);
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x18));
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[4].UD[0] | static_cast<uint64_t>(0xffff);
        // dsrl - Doubleword Shift Right Logical
    ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] >> 4;
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[7].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x1c));
        ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0]) << 2);
        ctx.cpuRegs.GPR.r[9].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0xfffffff0);
        ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[2].SL[0]) + static_cast<int32_t>(ctx.cpuRegs.GPR.r[3].SL[0]));
        ctx.cpuRegs.GPR.r[8].SD[0] = static_cast<int32_t>(0x8000000);
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0]) + 0x0));
        ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x7);
        ctx.cpuRegs.GPR.r[6].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x15);
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[7].UL[0] + 0x0, ctx.cpuRegs.GPR.r[3].UL[0]);
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x0));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x4));
        ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] & ctx.cpuRegs.GPR.r[4].UD[0];
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[7].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x20));
        ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] | ctx.cpuRegs.GPR.r[8].UD[0];
        ctx.cpuRegs.GPR.r[3].UD[0] = ctx.cpuRegs.GPR.r[3].UD[0] & ctx.cpuRegs.GPR.r[9].UD[0];
// This BLOCK HAS NO BRANCH SO GO TO NEXT BLOCK AFTER THIS, CURRENT BLOCK ENDS AT: 0x2a0610
// Fall through to 0x2a0610
    goto Label_0031; // Fall through


Label_0031: // 0x2a0610
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x4, ctx.cpuRegs.GPR.r[3].UL[0]);
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0, ctx.cpuRegs.GPR.r[2].UL[0]);
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[7].UL[0]) + 0x18)));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[7].UL[0]) + 0x1c));
// This BLOCK HAS NO BRANCH SO GO TO NEXT BLOCK AFTER THIS, CURRENT BLOCK ENDS AT: 0x2a0620
// Fall through to 0x2a0620
    goto Label_0032; // Fall through


Label_0032: // 0x2a0620
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[17].SL[0]) + static_cast<int32_t>(ctx.cpuRegs.GPR.r[4].SL[0]));
        // JALR - Jump and Link Register
    ctx.cpuRegs.GPR.r[31].UL[0] = 0x2a0628;
    {
        uint32_t target = ctx.cpuRegs.GPR.r[2].UL[0];
        auto func_ptr = find_containing_function(target);
        if (func_ptr != nullptr) {
            func_ptr(ctx, target);
        } else {
            ctx.cpuRegs.pc = target;
        }
    }
        ctx.cpuRegs.GPR.r[16].SD[0] = ctx.cpuRegs.GPR.r[2].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
    goto Label_0044;


Label_0033: // 0x2a0630
        ctx.cpuRegs.GPR.r[7].SD[0] = ctx.cpuRegs.GPR.r[18].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
        ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[17].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
        ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x9);
        ctx.cpuRegs.GPR.r[6].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x1c);
// This BLOCK HAS NO BRANCH SO GO TO NEXT BLOCK AFTER THIS, CURRENT BLOCK ENDS AT: 0x2a0640
// Fall through to 0x2a0640
    goto Label_0034; // Fall through


Label_0034: // 0x2a0640
       //nop 
       // JAL was called 
   // The address after JAL is: 0x2a0648
   // The next block should be: 2a0650
ctx.cpuRegs.GPR.r[31].UL[0] = 0x2a0648;
    if (recompiled_functions.count(0x29fb50)) {
        recompiled_functions[0x29fb50](ctx, 0x29fb50);


    } else {
        ctx.cpuRegs.pc = 0x29fb50;
    }
        ctx.cpuRegs.GPR.r[16].SD[0] = ctx.cpuRegs.GPR.r[2].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
    goto Label_0044;


Label_0035: // 0x2a0650
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x20));
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x38)));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x3c));
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[17].SL[0]) + static_cast<int32_t>(ctx.cpuRegs.GPR.r[4].SL[0]));
        // JALR - Jump and Link Register
    ctx.cpuRegs.GPR.r[31].UL[0] = 0x2a0664;
    {
        uint32_t target = ctx.cpuRegs.GPR.r[2].UL[0];
        auto func_ptr = find_containing_function(target);
        if (func_ptr != nullptr) {
            func_ptr(ctx, target);
        } else {
            ctx.cpuRegs.pc = target;
        }
    }
    bool branch_taken_2a0664 = (ctx.cpuRegs.GPR.r[2].UL[0] == ctx.cpuRegs.GPR.r[0].UL[0]);
        ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x1d);
    if (branch_taken_2a0664) {
        goto Label_0042;
    } else {
        goto Label_0036;
    }


Label_0036: // 0x2a066c
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[18].UL[0]) + 0x4));
    goto Label_0041;


Label_0037: // 0x2a0674
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x20));
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x38)));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x3c));
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[17].SL[0]) + static_cast<int32_t>(ctx.cpuRegs.GPR.r[4].SL[0]));
        // JALR - Jump and Link Register
    ctx.cpuRegs.GPR.r[31].UL[0] = 0x2a0688;
    {
        uint32_t target = ctx.cpuRegs.GPR.r[2].UL[0];
        auto func_ptr = find_containing_function(target);
        if (func_ptr != nullptr) {
            func_ptr(ctx, target);
        } else {
            ctx.cpuRegs.pc = target;
        }
    }
    bool branch_taken_2a0688 = (ctx.cpuRegs.GPR.r[2].UL[0] == ctx.cpuRegs.GPR.r[0].UL[0]);
        ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x1e);
    if (branch_taken_2a0688) {
        goto Label_0042;
    } else {
        goto Label_0038;
    }


Label_0038: // 0x2a0690
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[18].UL[0]) + 0x4));
    goto Label_0041;


Label_0039: // 0x2a0698
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x20));
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x38)));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x3c));
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[17].SL[0]) + static_cast<int32_t>(ctx.cpuRegs.GPR.r[4].SL[0]));
        // JALR - Jump and Link Register
    ctx.cpuRegs.GPR.r[31].UL[0] = 0x2a06ac;
    {
        uint32_t target = ctx.cpuRegs.GPR.r[2].UL[0];
        auto func_ptr = find_containing_function(target);
        if (func_ptr != nullptr) {
            func_ptr(ctx, target);
        } else {
            ctx.cpuRegs.pc = target;
        }
    }
    bool branch_taken_2a06ac = (ctx.cpuRegs.GPR.r[2].UL[0] == ctx.cpuRegs.GPR.r[0].UL[0]);
        ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x1f);
    if (branch_taken_2a06ac) {
        goto Label_0042;
    } else {
        goto Label_0040;
    }


Label_0040: // 0x2a06b4
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[18].UL[0]) + 0x4));
// This BLOCK HAS NO BRANCH SO GO TO NEXT BLOCK AFTER THIS, CURRENT BLOCK ENDS AT: 0x2a06b8
// Fall through to 0x2a06b8
    goto Label_0041; // Fall through


Label_0041: // 0x2a06b8
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x8));
        ctx.cpuRegs.GPR.r[16].SD[0] = ctx.cpuRegs.GPR.r[0].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0xc));
        ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[2].SL[0]) - static_cast<int32_t>(ctx.cpuRegs.GPR.r[3].SL[0]));
        // slt - Set on Less Than (Signed)
    ctx.cpuRegs.GPR.r[2].UD[0] = (ctx.cpuRegs.GPR.r[2].SL[0] < ctx.cpuRegs.GPR.r[4].SL[0]) ? 1 : 0;
        // movn - Move Conditional on Not Zero
    if (ctx.cpuRegs.GPR.r[2].UL[0] != 0) ctx.cpuRegs.GPR.r[16].UL[0] = ctx.cpuRegs.GPR.r[5].UL[0];
    goto Label_0044;


Label_0042: // 0x2a06d4
        ctx.cpuRegs.GPR.r[16].SD[0] = ctx.cpuRegs.GPR.r[0].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
    goto Label_0044;


Label_0043: // 0x2a06dc
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[18].UL[0]) + 0x4));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x8));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0xc));
        ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[2].SL[0]) - static_cast<int32_t>(ctx.cpuRegs.GPR.r[3].SL[0]));
        // slt - Set on Less Than (Signed)
    ctx.cpuRegs.GPR.r[2].UD[0] = (ctx.cpuRegs.GPR.r[2].SL[0] < ctx.cpuRegs.GPR.r[4].SL[0]) ? 1 : 0;
        ctx.cpuRegs.GPR.r[16].SD[0] = static_cast<int32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0]) << 5);
// This BLOCK HAS NO BRANCH SO GO TO NEXT BLOCK AFTER THIS, CURRENT BLOCK ENDS AT: 0x2a06f4
// Fall through to 0x2a06f4
    goto Label_0044; // Fall through


Label_0044: // 0x2a06f4
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x20));
// This BLOCK HAS NO BRANCH SO GO TO NEXT BLOCK AFTER THIS, CURRENT BLOCK ENDS AT: 0x2a06f8
// Fall through to 0x2a06f8
    goto Label_0045; // Fall through


Label_0045: // 0x2a06f8
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0]) + 0x28)));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0]) + 0x2c));
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[17].SL[0]) + static_cast<int32_t>(ctx.cpuRegs.GPR.r[4].SL[0]));
        // JALR - Jump and Link Register
    ctx.cpuRegs.GPR.r[31].UL[0] = 0x2a0708;
    {
        uint32_t target = ctx.cpuRegs.GPR.r[3].UL[0];
        auto func_ptr = find_containing_function(target);
        if (func_ptr != nullptr) {
            func_ptr(ctx, target);
        } else {
            ctx.cpuRegs.pc = target;
        }
    }
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x0));
        ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(0xfc0f0000);
        ctx.cpuRegs.GPR.r[3].UD[0] = ctx.cpuRegs.GPR.r[16].UD[0] & static_cast<uint64_t>(0x3f);
        ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] | static_cast<uint64_t>(0xffff);
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[4].UD[0] & ctx.cpuRegs.GPR.r[2].UD[0];
        ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) << 20);
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[4].UD[0] | ctx.cpuRegs.GPR.r[3].UD[0];
    bool branch_taken_2a0724 = (ctx.cpuRegs.GPR.r[16].UL[0] == ctx.cpuRegs.GPR.r[0].UL[0]);
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0, ctx.cpuRegs.GPR.r[4].UL[0]);
    if (branch_taken_2a0724) {
        goto Label_0048;
    } else {
        goto Label_0046;
    }


Label_0046: // 0x2a072c
        ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x1);
    bool branch_taken_2a0730 = (ctx.cpuRegs.GPR.r[16].UL[0] == ctx.cpuRegs.GPR.r[2].UL[0]);
        ctx.cpuRegs.GPR.r[16].UD[0] = memory::read<uint64_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x0);
        if (branch_taken_2a0730) {
        goto Label_0049;
    } else {
        goto Label_0047;
    }


Label_0047: // 0x2a0738
        ctx.cpuRegs.GPR.r[17].UD[0] = memory::read<uint64_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x8);
    goto Label_0051;


Label_0048: // 0x2a0740
        ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(0xffff0000);
        ctx.cpuRegs.GPR.r[2].SD[0] = ctx.cpuRegs.GPR.r[0].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
        ctx.cpuRegs.GPR.r[3].UD[0] = ctx.cpuRegs.GPR.r[3].UD[0] | static_cast<uint64_t>(0xfff);
        ctx.cpuRegs.GPR.r[3].UD[0] = ctx.cpuRegs.GPR.r[4].UD[0] & ctx.cpuRegs.GPR.r[3].UD[0];
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x0, ctx.cpuRegs.GPR.r[3].UL[0]);
    goto Label_0050;
        )code" << std::endl;


        file << R"code(
Label_0049: // 0x2a0758
        ctx.cpuRegs.GPR.r[2].SD[0] = ctx.cpuRegs.GPR.r[0].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
// This BLOCK HAS NO BRANCH SO GO TO NEXT BLOCK AFTER THIS, CURRENT BLOCK ENDS AT: 0x2a075c
// Fall through to 0x2a075c
    goto Label_0050; // Fall through


Label_0050: // 0x2a075c
        ctx.cpuRegs.GPR.r[16].UD[0] = memory::read<uint64_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x0);
        ctx.cpuRegs.GPR.r[17].UD[0] = memory::read<uint64_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x8);
// This BLOCK HAS NO BRANCH SO GO TO NEXT BLOCK AFTER THIS, CURRENT BLOCK ENDS AT: 0x2a0764
// Fall through to 0x2a0764
    goto Label_0051; // Fall through


Label_0051: // 0x2a0764
        ctx.cpuRegs.GPR.r[18].UD[0] = memory::read<uint64_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x10);
        ctx.cpuRegs.GPR.r[31].UD[0] = memory::read<uint64_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x18);
        ctx.cpuRegs.GPR.r[29].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[29].SL[0]) + 0x20);
    return; // Return from function


}


        
        
        )code" << std::endl;
        return;


    }


    if (func.base_address == 0x002a0bb0) {


        file << R"code(
        // Function: FUN_002a0bb0 at 0x2a0bb0
void FUN_002a0bb0(CpuContext& ctx) {
Label_0000: // 0x2a0bb0
        ctx.cpuRegs.GPR.r[29].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[29].SL[0]) + 0xffffffe0);
        memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x0, ctx.cpuRegs.GPR.r[16].UD[0]);
        memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x8, ctx.cpuRegs.GPR.r[17].UD[0]);
        memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x10, ctx.cpuRegs.GPR.r[18].UD[0]);
        ctx.cpuRegs.GPR.r[17].SD[0] = ctx.cpuRegs.GPR.r[4].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
        memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x18, ctx.cpuRegs.GPR.r[31].UD[0]);
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x0));
        ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] & static_cast<uint64_t>(0xf);
        ctx.cpuRegs.GPR.r[3].UD[0] = (ctx.cpuRegs.GPR.r[4].UL[0] < 0x7) ? 1 : 0;
    bool branch_taken_2a0bd4 = (ctx.cpuRegs.GPR.r[3].UL[0] == ctx.cpuRegs.GPR.r[0].UL[0]);
        ctx.cpuRegs.GPR.r[16].SD[0] = ctx.cpuRegs.GPR.r[5].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
    if (branch_taken_2a0bd4) {
        goto Label_0058;
    } else {
        goto Label_0001;
    }


Label_0001: // 0x2a0bdc
        ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(0x300000);
        ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[4].UL[0]) << 2);
        ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[2].SL[0]) + 0x5d00);
        ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[3].SL[0]) + static_cast<int32_t>(ctx.cpuRegs.GPR.r[2].SL[0]));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x0));
// This BLOCK HAS NO BRANCH SO GO TO NEXT BLOCK AFTER THIS, CURRENT BLOCK ENDS AT: 0x2a0bf0
// Fall through to 0x2a0bf0
    goto Label_0002; // Fall through


Label_0002: // 0x2a0bf0 - SWITCH on (*param_1 & 0xf)
    {
        uint32_t target = ctx.cpuRegs.GPR.r[4].UL[0];
        switch (target) {
            case 0x2a0bf8: goto Label_0003;  // case 1
            case 0x2a0d34: goto Label_0017;  // case 2
            case 0x2a0d60: goto Label_0018;  // case 3,4
            case 0x2a0f4c: goto Label_0040;  // case 5
            case 0x2a1040: goto Label_0054;  // case 6
            case 0x2a1080: goto Label_0058;  // case 0 / default
            default:
                g_logFile << "[FUN_2a0bb0] SW1 unknown: 0x" << std::hex << target << std::endl;
                goto Label_0058;
        }
    }




Label_0003: // 0x2a0bf8
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x14));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[5].UL[0]) + 0x20));
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x38)));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x3c));
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[5].SL[0]) + static_cast<int32_t>(ctx.cpuRegs.GPR.r[4].SL[0]));
        // JALR - Jump and Link Register
    ctx.cpuRegs.GPR.r[31].UL[0] = 0x2a0c10;
    {
        uint32_t target = ctx.cpuRegs.GPR.r[2].UL[0];
        auto func_ptr = find_containing_function(target);
        if (func_ptr != nullptr) {
            func_ptr(ctx, target);
        } else {
            ctx.cpuRegs.pc = target;
        }
    }
        bool cond_2755600 = (ctx.cpuRegs.GPR.r[2].UL[0] == ctx.cpuRegs.GPR.r[0].UL[0]);
    if (cond_2755600) {
        // Delay Slot (Likely)
            // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x18));
        goto Label_0016;
    } else {
        goto Label_0004;
    }
)code" << std::endl;




        file << R"code(
Label_0004: // 0x2a0c18
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x14));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[5].UL[0]) + 0x20));
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x40)));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x44));
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[5].SL[0]) + static_cast<int32_t>(ctx.cpuRegs.GPR.r[4].SL[0]));
        // JALR - Jump and Link Register
    ctx.cpuRegs.GPR.r[31].UL[0] = 0x2a0c30;
    {
        uint32_t target = ctx.cpuRegs.GPR.r[2].UL[0];
        auto func_ptr = find_containing_function(target);
        if (func_ptr != nullptr) {
            func_ptr(ctx, target);
        } else {
            ctx.cpuRegs.pc = target;
        }
    }
    bool branch_taken_2a0c30 = (ctx.cpuRegs.GPR.r[2].UL[0] == ctx.cpuRegs.GPR.r[0].UL[0]);
        ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x3);
    if (branch_taken_2a0c30) {
        goto Label_0015;
    } else {
        goto Label_0005;
    }


Label_0005: // 0x2a0c38
    if (ctx.cpuRegs.GPR.r[16].UL[0] != ctx.cpuRegs.GPR.r[2].UL[0]) {
            // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x18));
        goto Label_0014;
    } else {
        goto Label_0006;
    }


Label_0006: // 0x2a0c40
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x14));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[5].UL[0]) + 0x20));
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x68)));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x6c));
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[5].SL[0]) + static_cast<int32_t>(ctx.cpuRegs.GPR.r[4].SL[0]));
        // JALR - Jump and Link Register
    ctx.cpuRegs.GPR.r[31].UL[0] = 0x2a0c58;
    {
        uint32_t target = ctx.cpuRegs.GPR.r[2].UL[0];
        auto func_ptr = find_containing_function(target);
        if (func_ptr != nullptr) {
            func_ptr(ctx, target);
        } else {
            ctx.cpuRegs.pc = target;
        }
    }
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x14));
        ctx.cpuRegs.GPR.r[18].SD[0] = ctx.cpuRegs.GPR.r[2].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[5].UL[0]) + 0x20));
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x60)));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x64));
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[5].SL[0]) + static_cast<int32_t>(ctx.cpuRegs.GPR.r[4].SL[0]));
        // JALR - Jump and Link Register
    ctx.cpuRegs.GPR.r[31].UL[0] = 0x2a0c74;
    {
        uint32_t target = ctx.cpuRegs.GPR.r[2].UL[0];
        auto func_ptr = find_containing_function(target);
        if (func_ptr != nullptr) {
            func_ptr(ctx, target);
        } else {
            ctx.cpuRegs.pc = target;
        }
    }
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x14));
        ctx.cpuRegs.GPR.r[16].SD[0] = ctx.cpuRegs.GPR.r[2].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[5].UL[0]) + 0x20));
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x48)));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x4c));
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[5].SL[0]) + static_cast<int32_t>(ctx.cpuRegs.GPR.r[4].SL[0]));
        // JALR - Jump and Link Register
    ctx.cpuRegs.GPR.r[31].UL[0] = 0x2a0c90;
    {
        uint32_t target = ctx.cpuRegs.GPR.r[2].UL[0];
        auto func_ptr = find_containing_function(target);
        if (func_ptr != nullptr) {
            func_ptr(ctx, target);
        } else {
            ctx.cpuRegs.pc = target;
        }
    }
    bool branch_taken_2a0c90 = (ctx.cpuRegs.GPR.r[2].UL[0] == ctx.cpuRegs.GPR.r[0].UL[0]);
        ctx.cpuRegs.GPR.r[2].UD[0] = (ctx.cpuRegs.GPR.r[18].UL[0] < ctx.cpuRegs.GPR.r[16].UL[0]) ? 1 : 0;
    if (branch_taken_2a0c90) {
        goto Label_0010;
    } else {
        goto Label_0007;
    }
)code" << std::endl;




        file << R"code(
Label_0007: // 0x2a0c98
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x14));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[5].UL[0]) + 0x20));
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x50)));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x54));
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[5].SL[0]) + static_cast<int32_t>(ctx.cpuRegs.GPR.r[4].SL[0]));
        // JALR - Jump and Link Register
    ctx.cpuRegs.GPR.r[31].UL[0] = 0x2a0cb0;
    {
        uint32_t target = ctx.cpuRegs.GPR.r[2].UL[0];
        auto func_ptr = find_containing_function(target);
        if (func_ptr != nullptr) {
            func_ptr(ctx, target);
        } else {
            ctx.cpuRegs.pc = target;
        }
    }
    bool branch_taken_2a0cb0 = (ctx.cpuRegs.GPR.r[2].UL[0] == ctx.cpuRegs.GPR.r[16].UL[0]);
        ctx.cpuRegs.GPR.r[3].SD[0] = ctx.cpuRegs.GPR.r[0].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
        if (branch_taken_2a0cb0) {
        goto Label_0009;
    } else {
        goto Label_0008;
    }


Label_0008: // 0x2a0cb8
        ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[18].SL[0]) + static_cast<int32_t>(ctx.cpuRegs.GPR.r[2].SL[0]));
        ctx.cpuRegs.GPR.r[2].UD[0] = (ctx.cpuRegs.GPR.r[2].UL[0] < ctx.cpuRegs.GPR.r[16].UL[0]) ? 1 : 0;
    bool branch_taken_2a0cc0 = (ctx.cpuRegs.GPR.r[2].UL[0] != 0);
        ctx.cpuRegs.GPR.r[2].SD[0] = ctx.cpuRegs.GPR.r[3].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
    if (branch_taken_2a0cc0) {
        goto Label_0011;
    } else {
        goto Label_0009;
    }


Label_0009: // 0x2a0cc8
        ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x1);
        ctx.cpuRegs.GPR.r[2].SD[0] = ctx.cpuRegs.GPR.r[3].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
    goto Label_0011;


Label_0010: // 0x2a0cd4
        ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] ^ static_cast<uint64_t>(0x1);
// This BLOCK HAS NO BRANCH SO GO TO NEXT BLOCK AFTER THIS, CURRENT BLOCK ENDS AT: 0x2a0cd8
// Fall through to 0x2a0cd8
    goto Label_0011; // Fall through


Label_0011: // 0x2a0cd8
    bool branch_taken_2a0cd8 = (ctx.cpuRegs.GPR.r[2].UL[0] == ctx.cpuRegs.GPR.r[0].UL[0]);
        ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x1);
    if (branch_taken_2a0cd8) {
        goto Label_0013;
    } else {
        goto Label_0012;
    }


Label_0012: // 0x2a0ce0
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x18));
        ctx.cpuRegs.GPR.r[6].SD[0] = ctx.cpuRegs.GPR.r[0].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
    goto Label_0053;


Label_0013: // 0x2a0cec
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x18));
        ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x2);
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x10)));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x14));
    goto Label_0048;


Label_0014: // 0x2a0d00
        ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x3);
        ctx.cpuRegs.GPR.r[6].SD[0] = ctx.cpuRegs.GPR.r[0].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x8)));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0xc));
    goto Label_0051;


Label_0015: // 0x2a0d14
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x18));
        ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x1);
        ctx.cpuRegs.GPR.r[6].SD[0] = ctx.cpuRegs.GPR.r[0].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
    goto Label_0053;


Label_0016: // 0x2a0d24
        ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x1);
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x10)));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x14));
    goto Label_0048;
)code" << std::endl;




        file << R"code(
Label_0017: // 0x2a0d34
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x18));
        ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x1);
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[6].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x0));
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x30)));
        ctx.cpuRegs.GPR.r[6].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[6].SL[0]) >> 28);
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x34));
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[17].SL[0]) + static_cast<int32_t>(ctx.cpuRegs.GPR.r[4].SL[0]));
        ctx.cpuRegs.GPR.r[6].UD[0] = ctx.cpuRegs.GPR.r[6].UD[0] & static_cast<uint64_t>(0x1);
        // JALR - Jump and Link Register
    ctx.cpuRegs.GPR.r[31].UL[0] = 0x2a0d58;
    {
        uint32_t target = ctx.cpuRegs.GPR.r[2].UL[0];
        auto func_ptr = find_containing_function(target);
        if (func_ptr != nullptr) {
            func_ptr(ctx, target);
        } else {
            ctx.cpuRegs.pc = target;
        }
    }
        ctx.cpuRegs.GPR.r[16].UD[0] = memory::read<uint64_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x0);
    goto Label_0059;


Label_0018: // 0x2a0d60
        ctx.cpuRegs.GPR.r[2].UD[0] = (ctx.cpuRegs.GPR.r[16].UL[0] < 0xe) ? 1 : 0;
    bool branch_taken_2a0d64 = (ctx.cpuRegs.GPR.r[2].UL[0] == ctx.cpuRegs.GPR.r[0].UL[0]);
        ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(0x300000);
    if (branch_taken_2a0d64) {
        goto Label_0058;
    } else {
        goto Label_0019;
    }


Label_0019: // 0x2a0d6c
        ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) << 2);
        ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[2].SL[0]) + 0x5d20);
        ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[3].SL[0]) + static_cast<int32_t>(ctx.cpuRegs.GPR.r[2].SL[0]));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x0));
// This BLOCK HAS NO BRANCH SO GO TO NEXT BLOCK AFTER THIS, CURRENT BLOCK ENDS AT: 0x2a0d7c
// Fall through to 0x2a0d7c
    goto Label_0020; // Fall through


Label_0020: // 0x2a0d7c - SWITCH on param_2 (cases 3/4 sub-switch)
    {
        uint32_t target = ctx.cpuRegs.GPR.r[4].UL[0];
        switch (target) {
            case 0x2a0d84: goto Label_0021;  // case 2
            case 0x2a0d9c: goto Label_0022;  // case 0xa
            case 0x2a0db4: goto Label_0023;  // case 4
            case 0x2a0dc0: goto Label_0024;  // case 3
            case 0x2a0ebc: goto Label_0034;  // case 5
            case 0x2a0f18: goto Label_0037;  // case 6
            case 0x2a0f24: goto Label_0038;  // case 8
            case 0x2a0f3c: goto Label_0039;  // case 0xd
            case 0x2a101c: goto Label_0052;  // case 0xb
            case 0x2a1080: goto Label_0058;  // case 0,1,7,9,0xc / default
            default:
                g_logFile << "[FUN_2a0bb0] SW2 unknown: 0x" << std::hex << target << std::endl;
                goto Label_0058;
        }
    }




Label_0021: // 0x2a0d84
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x18));
        ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0xa);
        ctx.cpuRegs.GPR.r[6].SD[0] = ctx.cpuRegs.GPR.r[0].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x8)));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0xc));
    goto Label_0051;


Label_0022: // 0x2a0d9c
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x18));
        ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x4);
        ctx.cpuRegs.GPR.r[6].SD[0] = ctx.cpuRegs.GPR.r[0].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x8)));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0xc));
    goto Label_0051;


Label_0023: // 0x2a0db4
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x18));
        ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0xb);
    goto Label_0047;
)code" << std::endl;




        file << R"code(
Label_0024: // 0x2a0dc0
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x14));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[5].UL[0]) + 0x20));
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x68)));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x6c));
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[5].SL[0]) + static_cast<int32_t>(ctx.cpuRegs.GPR.r[4].SL[0]));
        // JALR - Jump and Link Register
    ctx.cpuRegs.GPR.r[31].UL[0] = 0x2a0dd8;
    {
        uint32_t target = ctx.cpuRegs.GPR.r[2].UL[0];
        auto func_ptr = find_containing_function(target);
        if (func_ptr != nullptr) {
            func_ptr(ctx, target);
        } else {
            ctx.cpuRegs.pc = target;
        }
    }
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x14));
        ctx.cpuRegs.GPR.r[18].SD[0] = ctx.cpuRegs.GPR.r[2].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[5].UL[0]) + 0x20));
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x60)));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x64));
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[5].SL[0]) + static_cast<int32_t>(ctx.cpuRegs.GPR.r[4].SL[0]));
        // JALR - Jump and Link Register
    ctx.cpuRegs.GPR.r[31].UL[0] = 0x2a0df4;
    {
        uint32_t target = ctx.cpuRegs.GPR.r[2].UL[0];
        auto func_ptr = find_containing_function(target);
        if (func_ptr != nullptr) {
            func_ptr(ctx, target);
        } else {
            ctx.cpuRegs.pc = target;
        }
    }
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x14));
        ctx.cpuRegs.GPR.r[16].SD[0] = ctx.cpuRegs.GPR.r[2].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[5].UL[0]) + 0x20));
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x48)));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x4c));
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[5].SL[0]) + static_cast<int32_t>(ctx.cpuRegs.GPR.r[4].SL[0]));
        // JALR - Jump and Link Register
    ctx.cpuRegs.GPR.r[31].UL[0] = 0x2a0e10;
    {
        uint32_t target = ctx.cpuRegs.GPR.r[2].UL[0];
        auto func_ptr = find_containing_function(target);
        if (func_ptr != nullptr) {
            func_ptr(ctx, target);
        } else {
            ctx.cpuRegs.pc = target;
        }
    }
    bool branch_taken_2a0e10 = (ctx.cpuRegs.GPR.r[2].UL[0] == ctx.cpuRegs.GPR.r[0].UL[0]);
        ctx.cpuRegs.GPR.r[2].UD[0] = (ctx.cpuRegs.GPR.r[18].UL[0] < ctx.cpuRegs.GPR.r[16].UL[0]) ? 1 : 0;
    if (branch_taken_2a0e10) {
        goto Label_0028;
    } else {
        goto Label_0025;
    }
)code" << std::endl;




        file << R"code(
Label_0025: // 0x2a0e18
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x14));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[5].UL[0]) + 0x20));
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x50)));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x54));
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[5].SL[0]) + static_cast<int32_t>(ctx.cpuRegs.GPR.r[4].SL[0]));
        // JALR - Jump and Link Register
    ctx.cpuRegs.GPR.r[31].UL[0] = 0x2a0e30;
    {
        uint32_t target = ctx.cpuRegs.GPR.r[2].UL[0];
        auto func_ptr = find_containing_function(target);
        if (func_ptr != nullptr) {
            func_ptr(ctx, target);
        } else {
            ctx.cpuRegs.pc = target;
        }
    }
    bool branch_taken_2a0e30 = (ctx.cpuRegs.GPR.r[2].UL[0] == ctx.cpuRegs.GPR.r[16].UL[0]);
        ctx.cpuRegs.GPR.r[3].SD[0] = ctx.cpuRegs.GPR.r[0].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
        if (branch_taken_2a0e30) {
        goto Label_0027;
    } else {
        goto Label_0026;
    }


Label_0026: // 0x2a0e38
        ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[18].SL[0]) + static_cast<int32_t>(ctx.cpuRegs.GPR.r[2].SL[0]));
        ctx.cpuRegs.GPR.r[2].UD[0] = (ctx.cpuRegs.GPR.r[2].UL[0] < ctx.cpuRegs.GPR.r[16].UL[0]) ? 1 : 0;
    bool branch_taken_2a0e40 = (ctx.cpuRegs.GPR.r[2].UL[0] != 0);
        ctx.cpuRegs.GPR.r[2].SD[0] = ctx.cpuRegs.GPR.r[3].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
    if (branch_taken_2a0e40) {
        goto Label_0029;
    } else {
        goto Label_0027;
    }


Label_0027: // 0x2a0e48
        ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x1);
        ctx.cpuRegs.GPR.r[2].SD[0] = ctx.cpuRegs.GPR.r[3].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
    goto Label_0029;


Label_0028: // 0x2a0e54
        ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] ^ static_cast<uint64_t>(0x1);
// This BLOCK HAS NO BRANCH SO GO TO NEXT BLOCK AFTER THIS, CURRENT BLOCK ENDS AT: 0x2a0e58
// Fall through to 0x2a0e58
    goto Label_0029; // Fall through


Label_0029: // 0x2a0e58
        bool cond_2756184 = (ctx.cpuRegs.GPR.r[2].UL[0] == ctx.cpuRegs.GPR.r[0].UL[0]);
    if (cond_2756184) {
        // Delay Slot (Likely)
            // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x18));
        goto Label_0033;
    } else {
        goto Label_0030;
    }


Label_0030: // 0x2a0e60
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x14));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[5].UL[0]) + 0x20));
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x48)));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x4c));
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[5].SL[0]) + static_cast<int32_t>(ctx.cpuRegs.GPR.r[4].SL[0]));
        // JALR - Jump and Link Register
    ctx.cpuRegs.GPR.r[31].UL[0] = 0x2a0e78;
    {
        uint32_t target = ctx.cpuRegs.GPR.r[2].UL[0];
        auto func_ptr = find_containing_function(target);
        if (func_ptr != nullptr) {
            func_ptr(ctx, target);
        } else {
            ctx.cpuRegs.pc = target;
        }
    }
    bool branch_taken_2a0e78 = (ctx.cpuRegs.GPR.r[2].UL[0] == ctx.cpuRegs.GPR.r[0].UL[0]);
        ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0xffffffdf);
    if (branch_taken_2a0e78) {
        goto Label_0032;
    } else {
        goto Label_0031;
    }
)code" << std::endl;




        file << R"code(
Label_0031: // 0x2a0e80
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x4));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[7].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x18));
        ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x6);
        ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] & ctx.cpuRegs.GPR.r[3].UD[0];
        ctx.cpuRegs.GPR.r[6].SD[0] = ctx.cpuRegs.GPR.r[0].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
    goto Label_0050;


Label_0032: // 0x2a0e98
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x18));
        ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x3);
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x10)));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x14));
    goto Label_0048;


Label_0033: // 0x2a0eac
        ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x8);
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x10)));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x14));
    goto Label_0048;


Label_0034: // 0x2a0ebc
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[16].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x4));
        ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[16].UD[0] & static_cast<uint64_t>(0x20);
    bool branch_taken_2a0ec4 = (ctx.cpuRegs.GPR.r[2].UL[0] == ctx.cpuRegs.GPR.r[0].UL[0]);
        ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0xffffffdf);
    if (branch_taken_2a0ec4) {
        goto Label_0036;
    } else {
        goto Label_0035;
    }


Label_0035: // 0x2a0ecc
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x18));
        ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[16].UD[0] & ctx.cpuRegs.GPR.r[2].UD[0];
        ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x8);
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x4, ctx.cpuRegs.GPR.r[2].UL[0]);
        ctx.cpuRegs.GPR.r[16].UD[0] = ctx.cpuRegs.GPR.r[16].UD[0] & static_cast<uint64_t>(0xf);
        ctx.cpuRegs.GPR.r[6].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x8)));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0xc));
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[17].SL[0]) + static_cast<int32_t>(ctx.cpuRegs.GPR.r[4].SL[0]));
        // JALR - Jump and Link Register
    ctx.cpuRegs.GPR.r[31].UL[0] = 0x2a0ef4;
    {
        uint32_t target = ctx.cpuRegs.GPR.r[2].UL[0];
        auto func_ptr = find_containing_function(target);
        if (func_ptr != nullptr) {
            func_ptr(ctx, target);
        } else {
            ctx.cpuRegs.pc = target;
        }
    }
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x4));
        ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0xfffffff0);
        ctx.cpuRegs.GPR.r[3].UD[0] = ctx.cpuRegs.GPR.r[3].UD[0] & ctx.cpuRegs.GPR.r[2].UD[0];
        ctx.cpuRegs.GPR.r[3].UD[0] = ctx.cpuRegs.GPR.r[3].UD[0] | ctx.cpuRegs.GPR.r[16].UD[0];
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x4, ctx.cpuRegs.GPR.r[3].UL[0]);
    goto Label_0058;


Label_0036: // 0x2a0f0c
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x18));
        ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0xb);
    goto Label_0047;


Label_0037: // 0x2a0f18
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x18));
        ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0xb);
    goto Label_0047;


Label_0038: // 0x2a0f24
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x18));
        ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0xb);
        ctx.cpuRegs.GPR.r[6].SD[0] = ctx.cpuRegs.GPR.r[0].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x8)));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0xc));
    goto Label_0051;


Label_0039: // 0x2a0f3c
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x18));
        ctx.cpuRegs.GPR.r[5].SD[0] = ctx.cpuRegs.GPR.r[0].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
        ctx.cpuRegs.GPR.r[6].SD[0] = ctx.cpuRegs.GPR.r[0].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
    goto Label_0053;
)code" << std::endl;




        file << R"code(
Label_0040: // 0x2a0f4c
        ctx.cpuRegs.GPR.r[2].UD[0] = (ctx.cpuRegs.GPR.r[16].UL[0] < 0xd) ? 1 : 0;
    bool branch_taken_2a0f50 = (ctx.cpuRegs.GPR.r[2].UL[0] == ctx.cpuRegs.GPR.r[0].UL[0]);
        ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(0x300000);
    if (branch_taken_2a0f50) {
        goto Label_0058;
    } else {
        goto Label_0041;
    }


Label_0041: // 0x2a0f58
        ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) << 2);
        ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[2].SL[0]) + 0x5d60);
        ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[3].SL[0]) + static_cast<int32_t>(ctx.cpuRegs.GPR.r[2].SL[0]));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x0));
// This BLOCK HAS NO BRANCH SO GO TO NEXT BLOCK AFTER THIS, CURRENT BLOCK ENDS AT: 0x2a0f68
// Fall through to 0x2a0f68
    goto Label_0042; // Fall through


Label_0042: // 0x2a0f68 - SWITCH on param_2 (case 5 sub-switch)
    {
        uint32_t target = ctx.cpuRegs.GPR.r[4].UL[0];
        switch (target) {
            case 0x2a0f70: goto Label_0043;  // case 3
            case 0x2a0fbc: goto Label_0046;  // case 6
            case 0x2a0fdc: goto Label_0049;  // case 9
            case 0x2a101c: goto Label_0052;  // case 0xc
            case 0x2a1080: goto Label_0058;  // case 0,1,2,4,5,7,8,0xa,0xb / default
            default:
                g_logFile << "[FUN_2a0bb0] SW3 unknown: 0x" << std::hex << target << std::endl;
                goto Label_0058;
        }
    }




Label_0043: // 0x2a0f70
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x14));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[5].UL[0]) + 0x20));
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x48)));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x4c));
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[5].SL[0]) + static_cast<int32_t>(ctx.cpuRegs.GPR.r[4].SL[0]));
        // JALR - Jump and Link Register
    ctx.cpuRegs.GPR.r[31].UL[0] = 0x2a0f88;
    {
        uint32_t target = ctx.cpuRegs.GPR.r[2].UL[0];
        auto func_ptr = find_containing_function(target);
        if (func_ptr != nullptr) {
            func_ptr(ctx, target);
        } else {
            ctx.cpuRegs.pc = target;
        }
    }
    bool branch_taken_2a0f88 = (ctx.cpuRegs.GPR.r[2].UL[0] == ctx.cpuRegs.GPR.r[0].UL[0]);
        ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0xffffffdf);
    if (branch_taken_2a0f88) {
        goto Label_0045;
    } else {
        goto Label_0044;
    }


Label_0044: // 0x2a0f90
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x4));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[7].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x18));
        ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x6);
        ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] & ctx.cpuRegs.GPR.r[3].UD[0];
        ctx.cpuRegs.GPR.r[6].SD[0] = ctx.cpuRegs.GPR.r[0].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
    goto Label_0050;
)code" << std::endl;




        file << R"code(
Label_0045: // 0x2a0fa8
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x18));
        ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x7);
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x10)));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x14));
    goto Label_0048;


Label_0046: // 0x2a0fbc
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x18));
        ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0xc);
// This BLOCK HAS NO BRANCH SO GO TO NEXT BLOCK AFTER THIS, CURRENT BLOCK ENDS AT: 0x2a0fc4
// Fall through to 0x2a0fc4
    goto Label_0047; // Fall through


Label_0047: // 0x2a0fc4
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x18)));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x1c));
// This BLOCK HAS NO BRANCH SO GO TO NEXT BLOCK AFTER THIS, CURRENT BLOCK ENDS AT: 0x2a0fcc
// Fall through to 0x2a0fcc
    goto Label_0048; // Fall through


Label_0048: // 0x2a0fcc
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[17].SL[0]) + static_cast<int32_t>(ctx.cpuRegs.GPR.r[4].SL[0]));
        // JALR - Jump and Link Register
    ctx.cpuRegs.GPR.r[31].UL[0] = 0x2a0fd4;
    {
        uint32_t target = ctx.cpuRegs.GPR.r[2].UL[0];
        auto func_ptr = find_containing_function(target);
        if (func_ptr != nullptr) {
            func_ptr(ctx, target);
        } else {
            ctx.cpuRegs.pc = target;
        }
    }
        ctx.cpuRegs.GPR.r[16].UD[0] = memory::read<uint64_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x0);
    goto Label_0059;


Label_0049: // 0x2a0fdc
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x4));
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0xfffffff0);
        ctx.cpuRegs.GPR.r[3].UD[0] = static_cast<uint64_t>(memory::read<uint8_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x3));
        ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0xc);
        ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] & ctx.cpuRegs.GPR.r[4].UD[0];
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[7].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x18));
        ctx.cpuRegs.GPR.r[3].UD[0] = ctx.cpuRegs.GPR.r[3].UD[0] & static_cast<uint64_t>(0xf);
        ctx.cpuRegs.GPR.r[6].SD[0] = ctx.cpuRegs.GPR.r[0].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
        ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] | ctx.cpuRegs.GPR.r[3].UD[0];
// This BLOCK HAS NO BRANCH SO GO TO NEXT BLOCK AFTER THIS, CURRENT BLOCK ENDS AT: 0x2a1000
// Fall through to 0x2a1000
    goto Label_0050; // Fall through
)code" << std::endl;




        file << R"code(
Label_0050: // 0x2a1000
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x4, ctx.cpuRegs.GPR.r[2].UL[0]);
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[7].UL[0]) + 0x8)));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[7].UL[0]) + 0xc));
// This BLOCK HAS NO BRANCH SO GO TO NEXT BLOCK AFTER THIS, CURRENT BLOCK ENDS AT: 0x2a100c
// Fall through to 0x2a100c
    goto Label_0051; // Fall through


Label_0051: // 0x2a100c
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[17].SL[0]) + static_cast<int32_t>(ctx.cpuRegs.GPR.r[4].SL[0]));
        // JALR - Jump and Link Register
    ctx.cpuRegs.GPR.r[31].UL[0] = 0x2a1014;
    {
        uint32_t target = ctx.cpuRegs.GPR.r[2].UL[0];
        auto func_ptr = find_containing_function(target);
        if (func_ptr != nullptr) {
            func_ptr(ctx, target);
        } else {
            ctx.cpuRegs.pc = target;
        }
    }
        ctx.cpuRegs.GPR.r[16].UD[0] = memory::read<uint64_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x0);
    goto Label_0059;


Label_0052: // 0x2a101c
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x18));
        ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x1);
        ctx.cpuRegs.GPR.r[6].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x1);
// This BLOCK HAS NO BRANCH SO GO TO NEXT BLOCK AFTER THIS, CURRENT BLOCK ENDS AT: 0x2a1028
// Fall through to 0x2a1028
    goto Label_0053; // Fall through


Label_0053: // 0x2a1028
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0]) + 0x30)));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0]) + 0x34));
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[17].SL[0]) + static_cast<int32_t>(ctx.cpuRegs.GPR.r[4].SL[0]));
        // JALR - Jump and Link Register
    ctx.cpuRegs.GPR.r[31].UL[0] = 0x2a1038;
    {
        uint32_t target = ctx.cpuRegs.GPR.r[3].UL[0];
        auto func_ptr = find_containing_function(target);
        if (func_ptr != nullptr) {
            func_ptr(ctx, target);
        } else {
            ctx.cpuRegs.pc = target;
        }
    }
        ctx.cpuRegs.GPR.r[16].UD[0] = memory::read<uint64_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x0);
    goto Label_0059;


Label_0054: // 0x2a1040
        ctx.cpuRegs.GPR.r[2].UD[0] = (ctx.cpuRegs.GPR.r[16].UL[0] < 0xa) ? 1 : 0;
    bool branch_taken_2a1044 = (ctx.cpuRegs.GPR.r[2].UL[0] == ctx.cpuRegs.GPR.r[0].UL[0]);
        ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(0x300000);
    if (branch_taken_2a1044) {
        goto Label_0058;
    } else {
        goto Label_0055;
    }
)code" << std::endl;




        file << R"code(
Label_0055: // 0x2a104c
        ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) << 2);
        ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[2].SL[0]) + 0x5da0);
        ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[3].SL[0]) + static_cast<int32_t>(ctx.cpuRegs.GPR.r[2].SL[0]));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x0));
// This BLOCK HAS NO BRANCH SO GO TO NEXT BLOCK AFTER THIS, CURRENT BLOCK ENDS AT: 0x2a105c
// Fall through to 0x2a105c
    goto Label_0056; // Fall through


Label_0056: // 0x2a105c - SWITCH on param_2 (case 6 sub-switch)
    {
        uint32_t target = ctx.cpuRegs.GPR.r[4].UL[0];
        switch (target) {
            case 0x2a1064: goto Label_0057;  // case 8
            case 0x2a1080: goto Label_0058;  // case 0-7,9 / default
            default:
                g_logFile << "[FUN_2a0bb0] SW4 unknown: 0x" << std::hex << target << std::endl;
                goto Label_0058;
        }
    }




Label_0057: // 0x2a1064
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x18));
        ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x1);
        ctx.cpuRegs.GPR.r[6].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x1);
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0]) + 0x30)));
        // lw instruction - 32-bit load
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0]) + 0x34));
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[17].SL[0]) + static_cast<int32_t>(ctx.cpuRegs.GPR.r[4].SL[0]));
        // JALR - Jump and Link Register
    ctx.cpuRegs.GPR.r[31].UL[0] = 0x2a1080;
    {
        uint32_t target = ctx.cpuRegs.GPR.r[3].UL[0];
        auto func_ptr = find_containing_function(target);
        if (func_ptr != nullptr) {
            func_ptr(ctx, target);
            goto Label_0058;
        } else {
            ctx.cpuRegs.pc = target;
        }
    }


Label_0058: // 0x2a1080
        ctx.cpuRegs.GPR.r[16].UD[0] = memory::read<uint64_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x0);
// This BLOCK HAS NO BRANCH SO GO TO NEXT BLOCK AFTER THIS, CURRENT BLOCK ENDS AT: 0x2a1084
// Fall through to 0x2a1084
    goto Label_0059; // Fall through


Label_0059: // 0x2a1084
        ctx.cpuRegs.GPR.r[17].UD[0] = memory::read<uint64_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x8);
        ctx.cpuRegs.GPR.r[18].UD[0] = memory::read<uint64_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x10);
        ctx.cpuRegs.GPR.r[31].UD[0] = memory::read<uint64_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x18);
        ctx.cpuRegs.GPR.r[29].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[29].SL[0]) + 0x20);
    return; // Return from function


}


        )code" << std::endl;
        return;
    }




    if (func.base_address == 0x0001cca78){
        file << R"code(
        
        // Function: FUN_001cca78 at 0x1cca78
        void FUN_001cca78(CpuContext& ctx) {
        Label_0000: // 0x1cca78
                ctx.cpuRegs.GPR.r[29].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[29].SL[0]) + 0xffffffc0);
                memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x20, ctx.cpuRegs.GPR.r[16].UD[0]);
                // swc1 - Store Word from Coprocessor 1
            memory::write<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x38, ctx.fpuRegs.fpr[21].UL);
                ctx.cpuRegs.GPR.r[16].SD[0] = ctx.cpuRegs.GPR.r[4].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x28, ctx.cpuRegs.GPR.r[31].UD[0]);
                // swc1 - Store Word from Coprocessor 1
            memory::write<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x30, ctx.fpuRegs.fpr[20].UL);
                // lw instruction - 32-bit load
            ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x4));
                ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] & static_cast<uint64_t>(0x7);
                ctx.cpuRegs.GPR.r[3].UD[0] = (ctx.cpuRegs.GPR.r[4].UL[0] < 0x5) ? 1 : 0;
            bool branch_taken_1cca9c = (ctx.cpuRegs.GPR.r[3].UL[0] == ctx.cpuRegs.GPR.r[0].UL[0]);
                // mov.s - Move Single (FPU)
            ctx.fpuRegs.fpr[21].f = ctx.fpuRegs.fpr[12].f;
            if (branch_taken_1cca9c) {
                goto Label_0007;
            } else {
                goto Label_0001;
            }




        Label_0001: // 0x1ccaa4
                ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(0x300000);
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[4].UL[0]) << 2);
                ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[2].SL[0]) + 0xffff9cf0);
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[3].SL[0]) + static_cast<int32_t>(ctx.cpuRegs.GPR.r[2].SL[0]));
                // lw instruction - 32-bit load
            ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x0));
        // This BLOCK HAS NO BRANCH SO GO TO NEXT BLOCK AFTER THIS, CURRENT BLOCK ENDS AT: 0x1ccab8
        // Fall through to 0x1ccab8
            goto Label_0002; // Fall through




        Label_0002: // 0x1ccab8 - SWITCH on (param_2[1] & 7)
            {
                uint32_t target = ctx.cpuRegs.GPR.r[4].UL[0];
                switch (target) {
                    case 0x1ccac0: goto Label_0003;  // case 2
                    case 0x1ccb18: goto Label_0005;  // case 3
                    case 0x1ccb64: goto Label_0006;  // case 4
                    case 0x1ccbac: goto Label_0007;  // case 0,1 / default
                    default:
                        g_logFile << "[FUN_1cca78] SW1 unknown: 0x" << std::hex << target << std::endl;
                        goto Label_0007;
                }
            }




        )code" << std::endl;




        file << R"code(


        Label_0003: // 0x1ccac0
                // lwc1 - Load Word to Coprocessor 1
            ctx.fpuRegs.fpr[0].UL = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x48);
                // lwc1 - Load Word to Coprocessor 1
            ctx.fpuRegs.fpr[20].UL = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x40);
                ctx.fpuRegs.fpr[0].f = ctx.fpuRegs.fpr[0].f * ctx.fpuRegs.fpr[21].f;
                ctx.fpuRegs.fpr[20].f = ctx.fpuRegs.fpr[20].f + ctx.fpuRegs.fpr[0].f;
                // swc1 - Store Word from Coprocessor 1
            memory::write<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x40, ctx.fpuRegs.fpr[20].UL);
                // mov.s - Move Single (FPU)
            ctx.fpuRegs.fpr[12].f = ctx.fpuRegs.fpr[20].f;
            // JAL was called
        // The address after JAL is: 0x1ccadc
        // The next block should be: 1ccaf8
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1ccadc;
            if (recompiled_functions.count(0x18adf0)) {
                recompiled_functions[0x18adf0](ctx, 0x18adf0);




            } else {
                ctx.cpuRegs.pc = 0x18adf0;
            }
                ctx.fpuRegs.fpr[0].UL = ctx.cpuRegs.GPR.r[2].UL[0];
            //nop
                // cvt.s.w - Convert Word to Single
            ctx.fpuRegs.fpr[0].f = static_cast<float>(ctx.fpuRegs.fpr[0].SL);
                // c.lt.s - Compare Less Than (R5900)
            if (ctx.fpuRegs.fpr[20].f < ctx.fpuRegs.fpr[0].f)
                ctx.fpuRegs.fprc[31] |= 0x800000;
            else ctx.fpuRegs.fprc[31] &= ~0x800000;
            //nop
                bool cond_1886960 = ((ctx.fpuRegs.fprc[31] & 0x800000) != 0);
            if (cond_1886960) {
                // Delay Slot (Likely)
                    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[2].SL[0]) + 0xffffffff);
                goto Label_0004;
            } else {
                goto Label_0004;
            }




        Label_0004: // 0x1ccaf8
                ctx.fpuRegs.fpr[1].UL = ctx.cpuRegs.GPR.r[2].UL[0];
            //nop
                // cvt.s.w - Convert Word to Single
            ctx.fpuRegs.fpr[1].f = static_cast<float>(ctx.fpuRegs.fpr[1].SL);
                // lwc1 - Load Word to Coprocessor 1
            ctx.fpuRegs.fpr[0].UL = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x40);
                ctx.fpuRegs.fpr[0].f = ctx.fpuRegs.fpr[0].f - ctx.fpuRegs.fpr[1].f;
                // swc1 - Store Word from Coprocessor 1
            memory::write<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x50, ctx.fpuRegs.fpr[0].UL);
                // swc1 - Store Word from Coprocessor 1
            memory::write<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x40, ctx.fpuRegs.fpr[0].UL);
            goto Label_0007;




        )code" << std::endl;




        file << R"code(
        Label_0005: // 0x1ccb18
                // lwc1 - Load Word to Coprocessor 1
            ctx.fpuRegs.fpr[0].UL = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x48);
                // lwc1 - Load Word to Coprocessor 1
            ctx.fpuRegs.fpr[20].UL = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x40);
                ctx.fpuRegs.fpr[0].f = ctx.fpuRegs.fpr[0].f * ctx.fpuRegs.fpr[21].f;
                ctx.fpuRegs.fpr[20].f = ctx.fpuRegs.fpr[20].f + ctx.fpuRegs.fpr[0].f;
                // swc1 - Store Word from Coprocessor 1
            memory::write<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x40, ctx.fpuRegs.fpr[20].UL);
                // mov.s - Move Single (FPU)
            ctx.fpuRegs.fpr[12].f = ctx.fpuRegs.fpr[20].f;
            // JAL was called
        // The address after JAL is: 0x1ccb34
        // The next block should be: 1ccb64
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1ccb34;
            if (recompiled_functions.count(0x18adf0)) {
                recompiled_functions[0x18adf0](ctx, 0x18adf0);




            } else {
                ctx.cpuRegs.pc = 0x18adf0;
            }
                // lwc1 - Load Word to Coprocessor 1
            ctx.fpuRegs.fpr[0].UL = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x40);
                ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[29].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[1].SD[0] = static_cast<int32_t>(0x40c90000);
                ctx.cpuRegs.GPR.r[1].UD[0] = ctx.cpuRegs.GPR.r[1].UD[0] | static_cast<uint64_t>(0xfdb);
                ctx.fpuRegs.fpr[12].UL = ctx.cpuRegs.GPR.r[1].UL[0];
                ctx.cpuRegs.GPR.r[5].SD[0] = ctx.cpuRegs.GPR.r[0].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.fpuRegs.fpr[12].f = ctx.fpuRegs.fpr[0].f * ctx.fpuRegs.fpr[12].f;
            // JAL was called
        // The address after JAL is: 0x1ccb54
        // The next block should be: 1ccb64
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1ccb54;
            if (recompiled_functions.count(0x11bfa0)) {
                recompiled_functions[0x11bfa0](ctx, 0x11bfa0);




            } else {
                ctx.cpuRegs.pc = 0x11bfa0;
            }
                ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[29].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
            // JAL was called
        // The address after JAL is: 0x1ccb5c
        // The next block should be: 1ccb64
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1ccb5c;
            if (recompiled_functions.count(0x18b8c0)) {
                recompiled_functions[0x18b8c0](ctx, 0x18b8c0);




            } else {
                ctx.cpuRegs.pc = 0x18b8c0;
            }
                // swc1 - Store Word from Coprocessor 1
            memory::write<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x50, ctx.fpuRegs.fpr[0].UL);
            goto Label_0007;




        Label_0006: // 0x1ccb64
                // lwc1 - Load Word to Coprocessor 1
            ctx.fpuRegs.fpr[0].UL = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x48);
                // lwc1 - Load Word to Coprocessor 1
            ctx.fpuRegs.fpr[20].UL = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x40);
                ctx.fpuRegs.fpr[0].f = ctx.fpuRegs.fpr[0].f * ctx.fpuRegs.fpr[21].f;
                ctx.fpuRegs.fpr[20].f = ctx.fpuRegs.fpr[20].f + ctx.fpuRegs.fpr[0].f;
                // swc1 - Store Word from Coprocessor 1
            memory::write<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x40, ctx.fpuRegs.fpr[20].UL);
                // mov.s - Move Single (FPU)
            ctx.fpuRegs.fpr[12].f = ctx.fpuRegs.fpr[20].f;
            // JAL was called
        // The address after JAL is: 0x1ccb80
        // The next block should be: 1ccbac
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1ccb80;
            if (recompiled_functions.count(0x18adf0)) {
                recompiled_functions[0x18adf0](ctx, 0x18adf0);




            } else {
                ctx.cpuRegs.pc = 0x18adf0;
            }
                // lwc1 - Load Word to Coprocessor 1
            ctx.fpuRegs.fpr[0].UL = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x40);
                ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[29].SL[0]) + 0x10);
                ctx.cpuRegs.GPR.r[1].SD[0] = static_cast<int32_t>(0x40c90000);
                ctx.cpuRegs.GPR.r[1].UD[0] = ctx.cpuRegs.GPR.r[1].UD[0] | static_cast<uint64_t>(0xfdb);
                ctx.fpuRegs.fpr[12].UL = ctx.cpuRegs.GPR.r[1].UL[0];
                ctx.cpuRegs.GPR.r[5].SD[0] = ctx.cpuRegs.GPR.r[0].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.fpuRegs.fpr[12].f = ctx.fpuRegs.fpr[0].f * ctx.fpuRegs.fpr[12].f;
            // JAL was called
        // The address after JAL is: 0x1ccba0
        // The next block should be: 1ccbac
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1ccba0;
            if (recompiled_functions.count(0x11bfa0)) {
                recompiled_functions[0x11bfa0](ctx, 0x11bfa0);




            } else {
                ctx.cpuRegs.pc = 0x11bfa0;
            }
                ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[29].SL[0]) + 0x10);
            // JAL was called
        // The address after JAL is: 0x1ccba8
        // The next block should be: 1ccbac
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1ccba8;
            if (recompiled_functions.count(0x18b8e8)) {
                recompiled_functions[0x18b8e8](ctx, 0x18b8e8);




            } else {
                ctx.cpuRegs.pc = 0x18b8e8;
            }
                // swc1 - Store Word from Coprocessor 1
            memory::write<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x54, ctx.fpuRegs.fpr[0].UL);




        )code" << std::endl;




        file << R"code(
        Label_0007: // 0x1ccbac
                ctx.cpuRegs.GPR.r[2].UD[0] = memory::read<uint64_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x0);
                // dsrl32 - Doubleword Shift Right Logical + 32
            ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] >> 35;
                ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] & static_cast<uint64_t>(0x7);
                ctx.cpuRegs.GPR.r[3].UD[0] = (ctx.cpuRegs.GPR.r[4].UL[0] < 0x5) ? 1 : 0;
            bool branch_taken_1ccbbc = (ctx.cpuRegs.GPR.r[3].UL[0] == ctx.cpuRegs.GPR.r[0].UL[0]);
                ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(0x300000);
            if (branch_taken_1ccbbc) {
                goto Label_0014;
            } else {
                goto Label_0008;
            }




        Label_0008: // 0x1ccbc4
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[4].UL[0]) << 2);
                ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[2].SL[0]) + 0xffff9d10);
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[3].SL[0]) + static_cast<int32_t>(ctx.cpuRegs.GPR.r[2].SL[0]));
                // lw instruction - 32-bit load
            ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x0));
        // This BLOCK HAS NO BRANCH SO GO TO NEXT BLOCK AFTER THIS, CURRENT BLOCK ENDS AT: 0x1ccbd4
        // Fall through to 0x1ccbd4
            goto Label_0009; // Fall through




        Label_0009: // 0x1ccbd4 - SWITCH on (*param_2 >> 35) & 7
            {
                uint32_t target = ctx.cpuRegs.GPR.r[4].UL[0];
                switch (target) {
                    case 0x1ccbdc: goto Label_0010;  // case 2
                    case 0x1ccc34: goto Label_0012;  // case 3
                    case 0x1ccc80: goto Label_0013;  // case 4
                    case 0x1cccc8: goto Label_0014;  // case 0,1 / default
                    default:
                        g_logFile << "[FUN_1cca78] SW2 unknown: 0x" << std::hex << target << std::endl;
                        goto Label_0014;
                }
            }








        Label_0010: // 0x1ccbdc
                // lwc1 - Load Word to Coprocessor 1
            ctx.fpuRegs.fpr[0].UL = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x4c);
                // lwc1 - Load Word to Coprocessor 1
            ctx.fpuRegs.fpr[20].UL = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x44);
                ctx.fpuRegs.fpr[0].f = ctx.fpuRegs.fpr[0].f * ctx.fpuRegs.fpr[21].f;
                ctx.fpuRegs.fpr[20].f = ctx.fpuRegs.fpr[20].f + ctx.fpuRegs.fpr[0].f;
                // swc1 - Store Word from Coprocessor 1
            memory::write<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x44, ctx.fpuRegs.fpr[20].UL);
                // mov.s - Move Single (FPU)
            ctx.fpuRegs.fpr[12].f = ctx.fpuRegs.fpr[20].f;
            // JAL was called
        // The address after JAL is: 0x1ccbf8
        // The next block should be: 1ccc14
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1ccbf8;
            if (recompiled_functions.count(0x18adf0)) {
                recompiled_functions[0x18adf0](ctx, 0x18adf0);




            } else {
                ctx.cpuRegs.pc = 0x18adf0;
            }
                ctx.fpuRegs.fpr[0].UL = ctx.cpuRegs.GPR.r[2].UL[0];
            //nop
                // cvt.s.w - Convert Word to Single
            ctx.fpuRegs.fpr[0].f = static_cast<float>(ctx.fpuRegs.fpr[0].SL);
                // c.lt.s - Compare Less Than (R5900)
            if (ctx.fpuRegs.fpr[20].f < ctx.fpuRegs.fpr[0].f)
                ctx.fpuRegs.fprc[31] |= 0x800000;
            else ctx.fpuRegs.fprc[31] &= ~0x800000;
            //nop
                bool cond_1887244 = ((ctx.fpuRegs.fprc[31] & 0x800000) != 0);
            if (cond_1887244) {
                // Delay Slot (Likely)
                    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[2].SL[0]) + 0xffffffff);
                goto Label_0011;
            } else {
                goto Label_0011;
            }




        )code" << std::endl;




        file << R"code(
        Label_0011: // 0x1ccc14
                ctx.fpuRegs.fpr[1].UL = ctx.cpuRegs.GPR.r[2].UL[0];
            //nop
                // cvt.s.w - Convert Word to Single
            ctx.fpuRegs.fpr[1].f = static_cast<float>(ctx.fpuRegs.fpr[1].SL);
                // lwc1 - Load Word to Coprocessor 1
            ctx.fpuRegs.fpr[0].UL = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x44);
                ctx.fpuRegs.fpr[0].f = ctx.fpuRegs.fpr[0].f - ctx.fpuRegs.fpr[1].f;
                // swc1 - Store Word from Coprocessor 1
            memory::write<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x54, ctx.fpuRegs.fpr[0].UL);
                // swc1 - Store Word from Coprocessor 1
            memory::write<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x44, ctx.fpuRegs.fpr[0].UL);
            goto Label_0014;




        Label_0012: // 0x1ccc34
                // lwc1 - Load Word to Coprocessor 1
            ctx.fpuRegs.fpr[0].UL = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x4c);
                // lwc1 - Load Word to Coprocessor 1
            ctx.fpuRegs.fpr[20].UL = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x44);
                ctx.fpuRegs.fpr[0].f = ctx.fpuRegs.fpr[0].f * ctx.fpuRegs.fpr[21].f;
                ctx.fpuRegs.fpr[20].f = ctx.fpuRegs.fpr[20].f + ctx.fpuRegs.fpr[0].f;
                // swc1 - Store Word from Coprocessor 1
            memory::write<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x44, ctx.fpuRegs.fpr[20].UL);
                // mov.s - Move Single (FPU)
            ctx.fpuRegs.fpr[12].f = ctx.fpuRegs.fpr[20].f;
            // JAL was called
        // The address after JAL is: 0x1ccc50
        // The next block should be: 1ccc80
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1ccc50;
            if (recompiled_functions.count(0x18adf0)) {
                recompiled_functions[0x18adf0](ctx, 0x18adf0);




            } else {
                ctx.cpuRegs.pc = 0x18adf0;
            }
                // lwc1 - Load Word to Coprocessor 1
            ctx.fpuRegs.fpr[0].UL = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x44);
                ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[29].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[1].SD[0] = static_cast<int32_t>(0x40c90000);
                ctx.cpuRegs.GPR.r[1].UD[0] = ctx.cpuRegs.GPR.r[1].UD[0] | static_cast<uint64_t>(0xfdb);
                ctx.fpuRegs.fpr[12].UL = ctx.cpuRegs.GPR.r[1].UL[0];
                ctx.cpuRegs.GPR.r[5].SD[0] = ctx.cpuRegs.GPR.r[0].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.fpuRegs.fpr[12].f = ctx.fpuRegs.fpr[0].f * ctx.fpuRegs.fpr[12].f;
            // JAL was called
        // The address after JAL is: 0x1ccc70
        // The next block should be: 1ccc80
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1ccc70;
            if (recompiled_functions.count(0x11bfa0)) {
                recompiled_functions[0x11bfa0](ctx, 0x11bfa0);




            } else {
                ctx.cpuRegs.pc = 0x11bfa0;
            }
                ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[29].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
            // JAL was called
        // The address after JAL is: 0x1ccc78
        // The next block should be: 1ccc80
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1ccc78;
            if (recompiled_functions.count(0x18b8c0)) {
                recompiled_functions[0x18b8c0](ctx, 0x18b8c0);




            } else {
                ctx.cpuRegs.pc = 0x18b8c0;
            }
                // swc1 - Store Word from Coprocessor 1
            memory::write<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x54, ctx.fpuRegs.fpr[0].UL);
            goto Label_0014;




        Label_0013: // 0x1ccc80
                // lwc1 - Load Word to Coprocessor 1
            ctx.fpuRegs.fpr[0].UL = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x4c);
                // lwc1 - Load Word to Coprocessor 1
            ctx.fpuRegs.fpr[20].UL = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x44);
                ctx.fpuRegs.fpr[0].f = ctx.fpuRegs.fpr[0].f * ctx.fpuRegs.fpr[21].f;
                ctx.fpuRegs.fpr[20].f = ctx.fpuRegs.fpr[20].f + ctx.fpuRegs.fpr[0].f;
                // swc1 - Store Word from Coprocessor 1
            memory::write<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x44, ctx.fpuRegs.fpr[20].UL);
                // mov.s - Move Single (FPU)
            ctx.fpuRegs.fpr[12].f = ctx.fpuRegs.fpr[20].f;
            // JAL was called
        // The address after JAL is: 0x1ccc9c
        // The next block should be: 1cccc8
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1ccc9c;
            if (recompiled_functions.count(0x18adf0)) {
                recompiled_functions[0x18adf0](ctx, 0x18adf0);




            } else {
                ctx.cpuRegs.pc = 0x18adf0;
            }
                // lwc1 - Load Word to Coprocessor 1
            ctx.fpuRegs.fpr[0].UL = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x44);
                ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[29].SL[0]) + 0x10);
                ctx.cpuRegs.GPR.r[1].SD[0] = static_cast<int32_t>(0x40c90000);
                ctx.cpuRegs.GPR.r[1].UD[0] = ctx.cpuRegs.GPR.r[1].UD[0] | static_cast<uint64_t>(0xfdb);
                ctx.fpuRegs.fpr[12].UL = ctx.cpuRegs.GPR.r[1].UL[0];
                ctx.cpuRegs.GPR.r[5].SD[0] = ctx.cpuRegs.GPR.r[0].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.fpuRegs.fpr[12].f = ctx.fpuRegs.fpr[0].f * ctx.fpuRegs.fpr[12].f;
            // JAL was called
        // The address after JAL is: 0x1cccbc
        // The next block should be: 1cccc8
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1cccbc;
            if (recompiled_functions.count(0x11bfa0)) {
                recompiled_functions[0x11bfa0](ctx, 0x11bfa0);




            } else {
                ctx.cpuRegs.pc = 0x11bfa0;
            }
                ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[29].SL[0]) + 0x10);
            // JAL was called
        // The address after JAL is: 0x1cccc4
        // The next block should be: 1cccc8
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1cccc4;
            if (recompiled_functions.count(0x18b8e8)) {
                recompiled_functions[0x18b8e8](ctx, 0x18b8e8);




            } else {
                ctx.cpuRegs.pc = 0x18b8e8;
            }
                // swc1 - Store Word from Coprocessor 1
            memory::write<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x54, ctx.fpuRegs.fpr[0].UL);




        )code" << std::endl;




        file << R"code(
        Label_0014: // 0x1cccc8
                // lw instruction - 32-bit load
            ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x68));
                bool cond_1887436 = (ctx.cpuRegs.GPR.r[4].UL[0] == ctx.cpuRegs.GPR.r[0].UL[0]);
            if (cond_1887436) {
                // Delay Slot (Likely)
                    ctx.cpuRegs.GPR.r[16].UD[0] = memory::read<uint64_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x20);
                goto Label_0022;
            } else {
                goto Label_0015;
            }




        Label_0015: // 0x1cccd4
                // mov.s - Move Single (FPU)
            ctx.fpuRegs.fpr[12].f = ctx.fpuRegs.fpr[21].f;
            // JAL was called
        // The address after JAL is: 0x1cccdc
        // The next block should be: 1cccfc
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1cccdc;
            if (recompiled_functions.count(0x298e60)) {
                recompiled_functions[0x298e60](ctx, 0x298e60);




            } else {
                ctx.cpuRegs.pc = 0x298e60;
            }
                ctx.cpuRegs.GPR.r[2].UD[0] = memory::read<uint64_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x0);
                ctx.cpuRegs.GPR.r[3].UD[0] = ctx.cpuRegs.GPR.r[0].UD[0] | static_cast<uint64_t>(0xe000);
                // dsll - Doubleword Shift Left Logical
            ctx.cpuRegs.GPR.r[3].UD[0] = ctx.cpuRegs.GPR.r[3].UD[0] << 19;
                ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[0].UD[0] | static_cast<uint64_t>(0x8000);
                // dsll - Doubleword Shift Left Logical
            ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[4].UD[0] << 17;
                ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] & ctx.cpuRegs.GPR.r[3].UD[0];
            if (ctx.cpuRegs.GPR.r[2].UL[0] != ctx.cpuRegs.GPR.r[4].UL[0]) {
                    ctx.cpuRegs.GPR.r[2].UD[0] = memory::read<uint64_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x0);
                goto Label_0017;
            } else {
                goto Label_0016;
            }




        Label_0016: // 0x1cccfc
                // lw instruction - 32-bit load
            ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x68));
                // lwc1 - Load Word to Coprocessor 1
            ctx.fpuRegs.fpr[0].UL = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0]) + 0x28);
                // swc1 - Store Word from Coprocessor 1
            memory::write<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x50, ctx.fpuRegs.fpr[0].UL);
                ctx.cpuRegs.GPR.r[2].UD[0] = memory::read<uint64_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x0);
        // This BLOCK HAS NO BRANCH SO GO TO NEXT BLOCK AFTER THIS, CURRENT BLOCK ENDS AT: 0x1ccd0c
        // Fall through to 0x1ccd0c
            goto Label_0017; // Fall through




        Label_0017: // 0x1ccd0c
                ctx.cpuRegs.GPR.r[3].UD[0] = ctx.cpuRegs.GPR.r[0].UD[0] | static_cast<uint64_t>(0xe000);
                // dsll - Doubleword Shift Left Logical
            ctx.cpuRegs.GPR.r[3].UD[0] = ctx.cpuRegs.GPR.r[3].UD[0] << 22;
                ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[0].UD[0] | static_cast<uint64_t>(0x8000);
                // dsll - Doubleword Shift Left Logical
            ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[4].UD[0] << 20;
                ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] & ctx.cpuRegs.GPR.r[3].UD[0];
            if (ctx.cpuRegs.GPR.r[2].UL[0] != ctx.cpuRegs.GPR.r[4].UL[0]) {
                    ctx.cpuRegs.GPR.r[2].UD[0] = memory::read<uint64_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x0);
                goto Label_0019;
            } else {
                goto Label_0018;
            }




        Label_0018: // 0x1ccd28
                // lw instruction - 32-bit load
            ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x68));
                // lwc1 - Load Word to Coprocessor 1
            ctx.fpuRegs.fpr[0].UL = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0]) + 0x2c);
                // swc1 - Store Word from Coprocessor 1
            memory::write<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x54, ctx.fpuRegs.fpr[0].UL);
                ctx.cpuRegs.GPR.r[2].UD[0] = memory::read<uint64_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x0);
        // This BLOCK HAS NO BRANCH SO GO TO NEXT BLOCK AFTER THIS, CURRENT BLOCK ENDS AT: 0x1ccd38
        // Fall through to 0x1ccd38
            goto Label_0019; // Fall through




        Label_0019: // 0x1ccd38
                ctx.cpuRegs.GPR.r[3].UD[0] = ctx.cpuRegs.GPR.r[0].UD[0] | static_cast<uint64_t>(0x8000);
                // dsll32 - Doubleword Shift Left Logical + 32
            ctx.cpuRegs.GPR.r[3].UD[0] = ctx.cpuRegs.GPR.r[3].UD[0] << 45;
                ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] & ctx.cpuRegs.GPR.r[3].UD[0];
            bool branch_taken_1ccd44 = (ctx.cpuRegs.GPR.r[2].UL[0] == ctx.cpuRegs.GPR.r[0].UL[0]);
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[16].SL[0]) + 0x30);
            if (branch_taken_1ccd44) {
                goto Label_0021;
            } else {
                goto Label_0020;
            }




        )code" << std::endl;




        file << R"code(
        Label_0020: // 0x1ccd4c
                // lw instruction - 32-bit load
            ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x68));
                ctx.cpuRegs.GPR.r[1].SD[0] = static_cast<int32_t>(0x43800000);
                ctx.fpuRegs.fpr[2].UL = ctx.cpuRegs.GPR.r[1].UL[0];
                ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                // lwc1 - Load Word to Coprocessor 1
            ctx.fpuRegs.fpr[1].UL = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0]) + 0x30);
                ctx.cpuRegs.GPR.r[1].SD[0] = static_cast<int32_t>(0x42fe0000);
                ctx.fpuRegs.fpr[3].UL = ctx.cpuRegs.GPR.r[1].UL[0];
                ctx.fpuRegs.fpr[1].f = ctx.fpuRegs.fpr[1].f * ctx.fpuRegs.fpr[2].f;
                // swc1 - Store Word from Coprocessor 1
            memory::write<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x30, ctx.fpuRegs.fpr[1].UL);
                // lwc1 - Load Word to Coprocessor 1
            ctx.fpuRegs.fpr[0].UL = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0]) + 0x34);
                ctx.fpuRegs.fpr[0].f = ctx.fpuRegs.fpr[0].f * ctx.fpuRegs.fpr[2].f;
                // swc1 - Store Word from Coprocessor 1
            memory::write<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x4, ctx.fpuRegs.fpr[0].UL);
                // lw instruction - 32-bit load
            ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x68));
                // lwc1 - Load Word to Coprocessor 1
            ctx.fpuRegs.fpr[0].UL = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0]) + 0x38);
                ctx.fpuRegs.fpr[0].f = ctx.fpuRegs.fpr[0].f * ctx.fpuRegs.fpr[2].f;
                // swc1 - Store Word from Coprocessor 1
            memory::write<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x8, ctx.fpuRegs.fpr[0].UL);
                // lw instruction - 32-bit load
            ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x68));
                // lwc1 - Load Word to Coprocessor 1
            ctx.fpuRegs.fpr[0].UL = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0]) + 0x3c);
                ctx.fpuRegs.fpr[0].f = ctx.fpuRegs.fpr[0].f * ctx.fpuRegs.fpr[3].f;
                // swc1 - Store Word from Coprocessor 1
            memory::write<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0xc, ctx.fpuRegs.fpr[0].UL);
            // JAL was called
        // The address after JAL is: 0x1ccda0
        // The next block should be: 1ccda0
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1ccda0;
            if (recompiled_functions.count(0x1da3a0)) {
                recompiled_functions[0x1da3a0](ctx, 0x1da3a0);
                goto Label_0021;
            } else {
                ctx.cpuRegs.pc = 0x1da3a0;
            }




        Label_0021: // 0x1ccda0
                ctx.cpuRegs.GPR.r[16].UD[0] = memory::read<uint64_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x20);
        // This BLOCK HAS NO BRANCH SO GO TO NEXT BLOCK AFTER THIS, CURRENT BLOCK ENDS AT: 0x1ccda4
        // Fall through to 0x1ccda4
            goto Label_0022; // Fall through




        Label_0022: // 0x1ccda4
                ctx.cpuRegs.GPR.r[2].SD[0] = ctx.cpuRegs.GPR.r[0].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[31].UD[0] = memory::read<uint64_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x28);
                // lwc1 - Load Word to Coprocessor 1
            ctx.fpuRegs.fpr[21].UL = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x38);
                // lwc1 - Load Word to Coprocessor 1
            ctx.fpuRegs.fpr[20].UL = memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x30);
                ctx.cpuRegs.GPR.r[29].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[29].SL[0]) + 0x40);
            return; // Return from function




        }






        
        
        )code" << std::endl;
    
        return;


    }
    if(func.base_address == 0x001bb710){
        file << R"code(
        
        // Function: FUN_001bb710 at 0x1bb710
        void FUN_001bb710(CpuContext& ctx) {
            // DEBUG: Log every call to this function
            uint32_t caller_ra = ctx.cpuRegs.GPR.r[31].UL[0];
            uint32_t param1 = ctx.cpuRegs.GPR.r[4].UL[0];  // a0
            uint32_t param2 = ctx.cpuRegs.GPR.r[5].UL[0];  // a1 = the object
            uint32_t obj_field0 = memory::read<uint32_t>(param2);
            
            g_logFile << "[FUN_001bb710 ENTRY] caller=0x" << std::hex << caller_ra
                    << " a0=0x" << param1
                    << " a1(obj)=0x" << param2
                    << " obj[0]=0x" << obj_field0;
            
            // Check if obj[0] looks like a valid vtable (should be in code/data segment)
            if (obj_field0 >= 0x100000 && obj_field0 < 0x400000) {
                g_logFile << " (valid vtable range)";
            } else {
                g_logFile << " (NOT A VTABLE - value too small/large!)";
                
                // Dump the caller's context to understand why wrong object was passed
                g_logFile << std::endl << "[FUN_001bb710] BAD OBJECT - dumping caller context:" << std::endl;
                g_logFile << "  s0=0x" << ctx.cpuRegs.GPR.r[16].UL[0]
                        << " s1=0x" << ctx.cpuRegs.GPR.r[17].UL[0]
                        << " s2=0x" << ctx.cpuRegs.GPR.r[18].UL[0]
                        << " s3=0x" << ctx.cpuRegs.GPR.r[19].UL[0] << std::endl;
                g_logFile << "  s4=0x" << ctx.cpuRegs.GPR.r[20].UL[0]
                        << " s5=0x" << ctx.cpuRegs.GPR.r[21].UL[0]
                        << " s6=0x" << ctx.cpuRegs.GPR.r[22].UL[0]
                        << " s7=0x" << ctx.cpuRegs.GPR.r[23].UL[0] << std::endl;
            }
            g_logFile << std::endl;


        Label_0000: // 0x1bb710
                ctx.cpuRegs.GPR.r[29].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[29].SL[0]) + 0xffffffb0);
                memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x30, ctx.cpuRegs.GPR.r[20].UD[0]);
                memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x38, ctx.cpuRegs.GPR.r[21].UD[0]);
                ctx.cpuRegs.GPR.r[20].SD[0] = ctx.cpuRegs.GPR.r[5].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x10, ctx.cpuRegs.GPR.r[16].UD[0]);
                ctx.cpuRegs.GPR.r[21].SD[0] = ctx.cpuRegs.GPR.r[4].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x18, ctx.cpuRegs.GPR.r[17].UD[0]);
                ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[21].SL[0]) + 0x18);
                memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x20, ctx.cpuRegs.GPR.r[18].UD[0]);
                memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x28, ctx.cpuRegs.GPR.r[19].UD[0]);
                memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x40, ctx.cpuRegs.GPR.r[31].UD[0]);
                // lw instruction - 32-bit load
            ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[20].UL[0]) + 0x0));
                ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0]) + 0x80)));
                // lw instruction - 32-bit load
            ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0]) + 0x84));
                ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[20].SL[0]) + static_cast<int32_t>(ctx.cpuRegs.GPR.r[4].SL[0]));
                // JALR - Jump and Link Register
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bb750;
            {
                uint32_t target = ctx.cpuRegs.GPR.r[3].UL[0];
                
                // DEBUG: Log the virtual call details
                uint32_t obj_ptr = ctx.cpuRegs.GPR.r[20].UL[0];  // s4 = the object
                uint32_t vtable = memory::read<uint32_t>(obj_ptr);
                g_logFile << "[FUN_001bb710 VCALL] obj=0x" << std::hex << obj_ptr
                        << " vtable=0x" << vtable
                        << " vtable[0x80]=" << (int16_t)memory::read<uint16_t>(vtable + 0x80)
                        << " vtable[0x84]=0x" << memory::read<uint32_t>(vtable + 0x84)
                        << " target=0x" << target << std::endl;
                
                if (target == 0) {
                    g_logFile << "[FUN_001bb710] NULL VTABLE CALL! Dumping vtable:" << std::endl;
                    for (int i = 0; i < 40; i++) {
                        uint32_t entry = memory::read<uint32_t>(vtable + i * 4);
                        g_logFile << "  vtable[0x" << std::hex << (i * 4) << "] = 0x" 
                                << entry << std::endl;
                    }
                    // Also dump the object itself
                    g_logFile << "[FUN_001bb710] Object dump:" << std::endl;
                    for (int i = 0; i < 16; i++) {
                        g_logFile << "  obj[0x" << std::hex << (i * 4) << "] = 0x"
                                << memory::read<uint32_t>(obj_ptr + i * 4) << std::endl;
                    }
                    return;  // Don't crash - bail out for now
                }
                
                auto func_ptr = find_containing_function(target);
                if (func_ptr != nullptr) {
                    func_ptr(ctx, target);
                } else {
                    ctx.cpuRegs.pc = target;
                }
            }
        )code" << std::endl;




        file << R"code(
                // lw instruction - 32-bit load
            ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[20].UL[0]) + 0x0));
                ctx.cpuRegs.GPR.r[5].SD[0] = ctx.cpuRegs.GPR.r[29].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0]) + 0x88)));
                // lw instruction - 32-bit load
            ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0]) + 0x8c));
                ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[20].SL[0]) + static_cast<int32_t>(ctx.cpuRegs.GPR.r[4].SL[0]));
                // JALR - Jump and Link Register
            ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bb768;
            {
                uint32_t target = ctx.cpuRegs.GPR.r[3].UL[0];
                auto func_ptr = find_containing_function(target);
                if (func_ptr != nullptr) {
                    func_ptr(ctx, target);
                } else {
                    ctx.cpuRegs.pc = target;
                }
            }
                // lw instruction - 32-bit load
            ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x0));
                ctx.cpuRegs.GPR.r[5].UD[0] = ctx.cpuRegs.GPR.r[29].UD[0] | static_cast<uint64_t>(0x4);
                memory::write<uint32_t>(ctx.cpuRegs.GPR.r[21].UL[0] + 0x20, ctx.cpuRegs.GPR.r[4].UL[0]);
                // lw instruction - 32-bit load
            ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[20].UL[0]) + 0x0));
                ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0]) + 0x88)));
                // lw instruction - 32-bit load
            ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0]) + 0x8c));
                ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[20].SL[0]) + static_cast<int32_t>(ctx.cpuRegs.GPR.r[4].SL[0]));
                // JALR - Jump and Link Register
            ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bb788;
            {
                uint32_t target = ctx.cpuRegs.GPR.r[3].UL[0];
                auto func_ptr = find_containing_function(target);
                if (func_ptr != nullptr) {
                    func_ptr(ctx, target);
                } else {
                    ctx.cpuRegs.pc = target;
                }
            }
                // lw instruction - 32-bit load
            ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x4));
                bool cond_1816460 = (ctx.cpuRegs.GPR.r[4].UL[0] == ctx.cpuRegs.GPR.r[0].UL[0]);
            if (cond_1816460) {
                // Delay Slot (Likely)
                    // lw instruction - 32-bit load
            ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[20].UL[0]) + 0x0));
                goto Label_0002;
            } else {
                goto Label_0001;
            }


        Label_0001: // 0x1bb794
            //nop 
            // JAL was called 
        // The address after JAL is: 0x1bb79c
        // The next block should be: 1bb7cc
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bb79c;
            if (recompiled_functions.count(0x1815c0)) {
                recompiled_functions[0x1815c0](ctx, 0x1815c0);


            } else {
                ctx.cpuRegs.pc = 0x1815c0;
            }
                // lw instruction - 32-bit load
            ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[20].UL[0]) + 0x0));
                ctx.cpuRegs.GPR.r[16].SD[0] = ctx.cpuRegs.GPR.r[2].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                // lw instruction - 32-bit load
            ctx.cpuRegs.GPR.r[6].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x4));
                ctx.cpuRegs.GPR.r[7].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x1);
                ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x10)));
                ctx.cpuRegs.GPR.r[5].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                // lw instruction - 32-bit load
            ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x14));
                ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[20].SL[0]) + static_cast<int32_t>(ctx.cpuRegs.GPR.r[4].SL[0]));
                // JALR - Jump and Link Register
            ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bb7c0;
            {
                uint32_t target = ctx.cpuRegs.GPR.r[2].UL[0];
                auto func_ptr = find_containing_function(target);
                if (func_ptr != nullptr) {
                    func_ptr(ctx, target);
                } else {
                    ctx.cpuRegs.pc = target;
                }
            }
                ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
            // JAL was called 
        // The address after JAL is: 0x1bb7c8
        // The next block should be: 1bb7cc
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bb7c8;
            if (recompiled_functions.count(0x181590)) {
                recompiled_functions[0x181590](ctx, 0x181590);


            } else {
                ctx.cpuRegs.pc = 0x181590;
            }
                // lw instruction - 32-bit load
            ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[20].UL[0]) + 0x0));


        Label_0002: // 0x1bb7cc
                ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[21].SL[0]) + 0x10);
                ctx.cpuRegs.GPR.r[16].SD[0] = ctx.cpuRegs.GPR.r[0].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0]) + 0x88)));
                // lw instruction - 32-bit load
            ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0]) + 0x8c));
                ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[20].SL[0]) + static_cast<int32_t>(ctx.cpuRegs.GPR.r[4].SL[0]));
                // JALR - Jump and Link Register
            ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bb7e4;
            {
                uint32_t target = ctx.cpuRegs.GPR.r[3].UL[0];
                auto func_ptr = find_containing_function(target);
                if (func_ptr != nullptr) {
                    func_ptr(ctx, target);
                } else {
                    ctx.cpuRegs.pc = target;
                }
            }
                // lw instruction - 32-bit load
            ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[21].UL[0]) + 0x10));
            bool branch_taken_1bb7e8 = (ctx.cpuRegs.GPR.r[2].UL[0] == ctx.cpuRegs.GPR.r[0].UL[0]);
            //nop 
            if (branch_taken_1bb7e8) {
                goto Label_0039;
            } else {
                goto Label_0003;
            }


        Label_0003: // 0x1bb7f0
                // lw instruction - 32-bit load
            ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[20].UL[0]) + 0x0));
            //nop 
        // This BLOCK HAS NO BRANCH SO GO TO NEXT BLOCK AFTER THIS, CURRENT BLOCK ENDS AT: 0x1bb7f8
        // Fall through to 0x1bb7f8
            goto Label_0004; // Fall through


        Label_0004: // 0x1bb7f8
                ctx.cpuRegs.GPR.r[5].UD[0] = ctx.cpuRegs.GPR.r[29].UD[0] | static_cast<uint64_t>(0x8);
                ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0]) + 0x88)));
                // lw instruction - 32-bit load
            ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0]) + 0x8c));
                ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[20].SL[0]) + static_cast<int32_t>(ctx.cpuRegs.GPR.r[4].SL[0]));
                // JALR - Jump and Link Register
            ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bb80c;
            {
                uint32_t target = ctx.cpuRegs.GPR.r[3].UL[0];
                auto func_ptr = find_containing_function(target);
                if (func_ptr != nullptr) {
                    func_ptr(ctx, target);
                } else {
                    ctx.cpuRegs.pc = target;
                }
            }
                // lw instruction - 32-bit load
            ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x8));
                ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[2].SL[0]) + 0xffffffff);
                ctx.cpuRegs.GPR.r[3].UD[0] = (ctx.cpuRegs.GPR.r[4].UL[0] < 0x20) ? 1 : 0;
            bool branch_taken_1bb818 = (ctx.cpuRegs.GPR.r[3].UL[0] == ctx.cpuRegs.GPR.r[0].UL[0]);
                ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(0x300000);
            if (branch_taken_1bb818) {
                goto Label_0037;
            } else {
                goto Label_0005;
            }


        Label_0005: // 0x1bb820
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[4].UL[0]) << 2);
                ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[2].SL[0]) + 0xffff8c90);
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[3].SL[0]) + static_cast<int32_t>(ctx.cpuRegs.GPR.r[2].SL[0]));
                // lw instruction - 32-bit load
            ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x0));
        // This BLOCK HAS NO BRANCH SO GO TO NEXT BLOCK AFTER THIS, CURRENT BLOCK ENDS AT: 0x1bb830
        // Fall through to 0x1bb830
            goto Label_0006; // Fall through


        Label_0006: // 0x1bb830 - SWITCH DISPATCH (was incorrectly a function call)
            {
                uint32_t target = ctx.cpuRegs.GPR.r[4].UL[0];
                switch (target) {
                    case 0x1bb838: goto Label_0007;  // case 1
                    case 0x1bb874: goto Label_0008;  // case 2
                    case 0x1bb8b0: goto Label_0009;  // case 4
                    case 0x1bb8ec: goto Label_0010;  // case 10 (0xa)
                    case 0x1bb928: goto Label_0011;  // case 11 (0xb)
                    case 0x1bb964: goto Label_0012;  // case 12 (0xc)
                    case 0x1bb9a0: goto Label_0013;  // case 13 (0xd)
                    case 0x1bb9dc: goto Label_0014;  // case 15 (0xf)
                    case 0x1bba18: goto Label_0015;  // case 16 (0x10)
                    case 0x1bba54: goto Label_0016;  // case 17 (0x11)
                    case 0x1bba90: goto Label_0017;  // case 18 (0x12)
                    case 0x1bbacc: goto Label_0018;  // case 19 (0x13)
                    case 0x1bbb08: goto Label_0019;  // case 20 (0x14)
                    case 0x1bbb44: goto Label_0020;  // case 21 (0x15)
                    case 0x1bbb80: goto Label_0021;  // case 22 (0x16)
                    case 0x1bbbbc: goto Label_0022;  // case 23 (0x17)
                    case 0x1bbc64: goto Label_0027;  // case 25 (0x19)
                    case 0x1bbca0: goto Label_0028;  // case 26 (0x1a)
                    case 0x1bbd54: goto Label_0033;  // case 27 (0x1b)
                    case 0x1bbd90: goto Label_0034;  // case 30 (0x1e)
                    case 0x1bbdcc: goto Label_0035;  // case 31 (0x1f)
                    case 0x1bbe08: goto Label_0036;  // case 32 (0x20)
                    case 0x1bbe44: goto Label_0037;  // default
                    default:
                        g_logFile << "[FUN_001bb710] UNKNOWN switch target: 0x" 
                                << std::hex << target << std::endl;
                        goto Label_0037;  // default case
                }
            }
                )code" << std::endl;




        file << R"code(


        Label_0007: // 0x1bb838
                ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x70);
                ctx.cpuRegs.GPR.r[19].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[16].SL[0]) + 0x1);
            // JAL was called 
        // The address after JAL is: 0x1bb844
        // The next block should be: 1bb874
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bb844;
            if (recompiled_functions.count(0x181560)) {
                recompiled_functions[0x181560](ctx, 0x181560);


            } else {
                ctx.cpuRegs.pc = 0x181560;
            }
                ctx.cpuRegs.GPR.r[18].SD[0] = static_cast<int32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) << 2);
                ctx.cpuRegs.GPR.r[16].SD[0] = ctx.cpuRegs.GPR.r[2].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[17].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
            // JAL was called 
        // The address after JAL is: 0x1bb858
        // The next block should be: 1bb874
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bb858;
            if (recompiled_functions.count(0x1cc8e0)) {
                recompiled_functions[0x1cc8e0](ctx, 0x1cc8e0);


            } else {
                ctx.cpuRegs.pc = 0x1cc8e0;
            }
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(0x300000);
                ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[3].SL[0]) + 0xffffaec8);
                memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x6c, ctx.cpuRegs.GPR.r[3].UL[0]);
            // JAL was called 
        // The address after JAL is: 0x1bb86c
        // The next block should be: 1bb874
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bb86c;
            if (recompiled_functions.count(0x1d8ae8)) {
                recompiled_functions[0x1d8ae8](ctx, 0x1d8ae8);


            } else {
                ctx.cpuRegs.pc = 0x1d8ae8;
            }
                // lw instruction - 32-bit load
            ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x6c));
            goto Label_0038;


        Label_0008: // 0x1bb874
                ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x70);
                ctx.cpuRegs.GPR.r[19].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[16].SL[0]) + 0x1);
            // JAL was called 
        // The address after JAL is: 0x1bb880
        // The next block should be: 1bb8b0
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bb880;
            if (recompiled_functions.count(0x181560)) {
                recompiled_functions[0x181560](ctx, 0x181560);


            } else {
                ctx.cpuRegs.pc = 0x181560;
            }
                ctx.cpuRegs.GPR.r[18].SD[0] = static_cast<int32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) << 2);
                ctx.cpuRegs.GPR.r[16].SD[0] = ctx.cpuRegs.GPR.r[2].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[17].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
            // JAL was called 
        // The address after JAL is: 0x1bb894
        // The next block should be: 1bb8b0
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bb894;
            if (recompiled_functions.count(0x1cc8e0)) {
                recompiled_functions[0x1cc8e0](ctx, 0x1cc8e0);


            } else {
                ctx.cpuRegs.pc = 0x1cc8e0;
            }
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(0x300000);
                ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[3].SL[0]) + 0xffffae38);
                memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x6c, ctx.cpuRegs.GPR.r[3].UL[0]);
            // JAL was called 
        // The address after JAL is: 0x1bb8a8
        // The next block should be: 1bb8b0
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bb8a8;
            if (recompiled_functions.count(0x1d8ba8)) {
                recompiled_functions[0x1d8ba8](ctx, 0x1d8ba8);


            } else {
                ctx.cpuRegs.pc = 0x1d8ba8;
            }
                // lw instruction - 32-bit load
            ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x6c));
            goto Label_0038;


        Label_0009: // 0x1bb8b0
                ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x70);
                ctx.cpuRegs.GPR.r[19].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[16].SL[0]) + 0x1);
            // JAL was called 
        // The address after JAL is: 0x1bb8bc
        // The next block should be: 1bb8ec
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bb8bc;
            if (recompiled_functions.count(0x181560)) {
                recompiled_functions[0x181560](ctx, 0x181560);


            } else {
                ctx.cpuRegs.pc = 0x181560;
            }
                ctx.cpuRegs.GPR.r[18].SD[0] = static_cast<int32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) << 2);
                ctx.cpuRegs.GPR.r[16].SD[0] = ctx.cpuRegs.GPR.r[2].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[17].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
            // JAL was called 
        // The address after JAL is: 0x1bb8d0
        // The next block should be: 1bb8ec
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bb8d0;
            if (recompiled_functions.count(0x1cc8e0)) {
                recompiled_functions[0x1cc8e0](ctx, 0x1cc8e0);


            } else {
                ctx.cpuRegs.pc = 0x1cc8e0;
            }
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(0x300000);
                ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[3].SL[0]) + 0xffffac08);
                memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x6c, ctx.cpuRegs.GPR.r[3].UL[0]);
            // JAL was called 
        // The address after JAL is: 0x1bb8e4
        // The next block should be: 1bb8ec
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bb8e4;
            if (recompiled_functions.count(0x1d8eb0)) {
                recompiled_functions[0x1d8eb0](ctx, 0x1d8eb0);


            } else {
                ctx.cpuRegs.pc = 0x1d8eb0;
            }
                // lw instruction - 32-bit load
            ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x6c));
            goto Label_0038;


        Label_0010: // 0x1bb8ec
                ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x70);
                ctx.cpuRegs.GPR.r[19].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[16].SL[0]) + 0x1);
            // JAL was called 
        // The address after JAL is: 0x1bb8f8
        // The next block should be: 1bb928
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bb8f8;
            if (recompiled_functions.count(0x181560)) {
                recompiled_functions[0x181560](ctx, 0x181560);


            } else {
                ctx.cpuRegs.pc = 0x181560;
            }
                ctx.cpuRegs.GPR.r[18].SD[0] = static_cast<int32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) << 2);
                ctx.cpuRegs.GPR.r[16].SD[0] = ctx.cpuRegs.GPR.r[2].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[17].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
            // JAL was called 
        // The address after JAL is: 0x1bb90c
        // The next block should be: 1bb928
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bb90c;
            if (recompiled_functions.count(0x1cc8e0)) {
                recompiled_functions[0x1cc8e0](ctx, 0x1cc8e0);


            } else {
                ctx.cpuRegs.pc = 0x1cc8e0;
            }
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(0x300000);
                ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[3].SL[0]) + 0xffffa948);
                memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x6c, ctx.cpuRegs.GPR.r[3].UL[0]);
            // JAL was called 
        // The address after JAL is: 0x1bb920
        // The next block should be: 1bb928
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bb920;
            if (recompiled_functions.count(0x1d9270)) {
                recompiled_functions[0x1d9270](ctx, 0x1d9270);


            } else {
                ctx.cpuRegs.pc = 0x1d9270;
            }
                // lw instruction - 32-bit load
            ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x6c));
            goto Label_0038;


        Label_0011: // 0x1bb928
                ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x70);
                ctx.cpuRegs.GPR.r[19].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[16].SL[0]) + 0x1);
            // JAL was called 
        // The address after JAL is: 0x1bb934
        // The next block should be: 1bb964
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bb934;
            if (recompiled_functions.count(0x181560)) {
                recompiled_functions[0x181560](ctx, 0x181560);


            } else {
                ctx.cpuRegs.pc = 0x181560;
            }
                ctx.cpuRegs.GPR.r[18].SD[0] = static_cast<int32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) << 2);
                ctx.cpuRegs.GPR.r[16].SD[0] = ctx.cpuRegs.GPR.r[2].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[17].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
            // JAL was called 
        // The address after JAL is: 0x1bb948
        // The next block should be: 1bb964
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bb948;
            if (recompiled_functions.count(0x1cc8e0)) {
                recompiled_functions[0x1cc8e0](ctx, 0x1cc8e0);


            } else {
                ctx.cpuRegs.pc = 0x1cc8e0;
            }
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(0x300000);
                ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[3].SL[0]) + 0xffffa8c8);
                memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x6c, ctx.cpuRegs.GPR.r[3].UL[0]);
            // JAL was called 
        // The address after JAL is: 0x1bb95c
        // The next block should be: 1bb964
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bb95c;
            if (recompiled_functions.count(0x1d9338)) {
                recompiled_functions[0x1d9338](ctx, 0x1d9338);


            } else {
                ctx.cpuRegs.pc = 0x1d9338;
            }
                // lw instruction - 32-bit load
            ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x6c));
            goto Label_0038;


        Label_0012: // 0x1bb964
                ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x70);
                ctx.cpuRegs.GPR.r[19].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[16].SL[0]) + 0x1);
            // JAL was called 
        // The address after JAL is: 0x1bb970
        // The next block should be: 1bb9a0
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bb970;
            if (recompiled_functions.count(0x181560)) {
                recompiled_functions[0x181560](ctx, 0x181560);


            } else {
                ctx.cpuRegs.pc = 0x181560;
            }
                ctx.cpuRegs.GPR.r[18].SD[0] = static_cast<int32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) << 2);
                ctx.cpuRegs.GPR.r[16].SD[0] = ctx.cpuRegs.GPR.r[2].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[17].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
            // JAL was called 
        // The address after JAL is: 0x1bb984
        // The next block should be: 1bb9a0
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bb984;
            if (recompiled_functions.count(0x1cc8e0)) {
                recompiled_functions[0x1cc8e0](ctx, 0x1cc8e0);


            } else {
                ctx.cpuRegs.pc = 0x1cc8e0;
            }
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(0x300000);
                ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[3].SL[0]) + 0xffffa848);
                memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x6c, ctx.cpuRegs.GPR.r[3].UL[0]);
            // JAL was called 
        // The address after JAL is: 0x1bb998
        // The next block should be: 1bb9a0
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bb998;
            if (recompiled_functions.count(0x1d93f8)) {
                recompiled_functions[0x1d93f8](ctx, 0x1d93f8);


            } else {
                ctx.cpuRegs.pc = 0x1d93f8;
            }
                // lw instruction - 32-bit load
            ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x6c));
            goto Label_0038;


        Label_0013: // 0x1bb9a0
                ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x70);
                ctx.cpuRegs.GPR.r[19].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[16].SL[0]) + 0x1);
            // JAL was called 
        // The address after JAL is: 0x1bb9ac
        // The next block should be: 1bb9dc
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bb9ac;
            if (recompiled_functions.count(0x181560)) {
                recompiled_functions[0x181560](ctx, 0x181560);


            } else {
                ctx.cpuRegs.pc = 0x181560;
            }
                ctx.cpuRegs.GPR.r[18].SD[0] = static_cast<int32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) << 2);
                ctx.cpuRegs.GPR.r[16].SD[0] = ctx.cpuRegs.GPR.r[2].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[17].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
            // JAL was called 
        // The address after JAL is: 0x1bb9c0
        // The next block should be: 1bb9dc
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bb9c0;
            if (recompiled_functions.count(0x1cc8e0)) {
                recompiled_functions[0x1cc8e0](ctx, 0x1cc8e0);


            } else {
                ctx.cpuRegs.pc = 0x1cc8e0;
            }
        )code" << std::endl;




        file << R"code(
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(0x300000);
                ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[3].SL[0]) + 0xffff9ed8);
                memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x6c, ctx.cpuRegs.GPR.r[3].UL[0]);
            // JAL was called 
        // The address after JAL is: 0x1bb9d4
        // The next block should be: 1bb9dc
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bb9d4;
            if (recompiled_functions.count(0x1d9f78)) {
                recompiled_functions[0x1d9f78](ctx, 0x1d9f78);


            } else {
                ctx.cpuRegs.pc = 0x1d9f78;
            }
                // lw instruction - 32-bit load
            ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x6c));
            goto Label_0038;


        Label_0014: // 0x1bb9dc
                ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x70);
                ctx.cpuRegs.GPR.r[19].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[16].SL[0]) + 0x1);
            // JAL was called 
        // The address after JAL is: 0x1bb9e8
        // The next block should be: 1bba18
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bb9e8;
            if (recompiled_functions.count(0x181560)) {
                recompiled_functions[0x181560](ctx, 0x181560);


            } else {
                ctx.cpuRegs.pc = 0x181560;
            }
                ctx.cpuRegs.GPR.r[18].SD[0] = static_cast<int32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) << 2);
                ctx.cpuRegs.GPR.r[16].SD[0] = ctx.cpuRegs.GPR.r[2].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[17].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
            // JAL was called 
        // The address after JAL is: 0x1bb9fc
        // The next block should be: 1bba18
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bb9fc;
            if (recompiled_functions.count(0x1cc8e0)) {
                recompiled_functions[0x1cc8e0](ctx, 0x1cc8e0);


            } else {
                ctx.cpuRegs.pc = 0x1cc8e0;
            }
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(0x300000);
                ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[3].SL[0]) + 0xffffa7c8);
                memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x6c, ctx.cpuRegs.GPR.r[3].UL[0]);
            // JAL was called 
        // The address after JAL is: 0x1bba10
        // The next block should be: 1bba18
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bba10;
            if (recompiled_functions.count(0x1d94c8)) {
                recompiled_functions[0x1d94c8](ctx, 0x1d94c8);


            } else {
                ctx.cpuRegs.pc = 0x1d94c8;
            }
                // lw instruction - 32-bit load
            ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x6c));
            goto Label_0038;
                )code" << std::endl;




        file << R"code(
        Label_0015: // 0x1bba18
                ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x80);
                ctx.cpuRegs.GPR.r[19].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[16].SL[0]) + 0x1);
            // JAL was called 
        // The address after JAL is: 0x1bba24
        // The next block should be: 1bba54
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bba24;
            if (recompiled_functions.count(0x181560)) {
                recompiled_functions[0x181560](ctx, 0x181560);


            } else {
                ctx.cpuRegs.pc = 0x181560;
            }
                ctx.cpuRegs.GPR.r[18].SD[0] = static_cast<int32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) << 2);
                ctx.cpuRegs.GPR.r[16].SD[0] = ctx.cpuRegs.GPR.r[2].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[17].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
            // JAL was called 
        // The address after JAL is: 0x1bba38
        // The next block should be: 1bba54
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bba38;
            if (recompiled_functions.count(0x1cc8e0)) {
                recompiled_functions[0x1cc8e0](ctx, 0x1cc8e0);


            } else {
                ctx.cpuRegs.pc = 0x1cc8e0;
            }
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(0x300000);
                ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[3].SL[0]) + 0xffffa738);
                memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x6c, ctx.cpuRegs.GPR.r[3].UL[0]);
            // JAL was called 
        // The address after JAL is: 0x1bba4c
        // The next block should be: 1bba54
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bba4c;
            if (recompiled_functions.count(0x1d9598)) {
                recompiled_functions[0x1d9598](ctx, 0x1d9598);


            } else {
                ctx.cpuRegs.pc = 0x1d9598;
            }
                // lw instruction - 32-bit load
            ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x6c));
            goto Label_0038;


        Label_0016: // 0x1bba54
                ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x80);
                ctx.cpuRegs.GPR.r[19].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[16].SL[0]) + 0x1);
            // JAL was called 
        // The address after JAL is: 0x1bba60
        // The next block should be: 1bba90
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bba60;
            if (recompiled_functions.count(0x181560)) {
                recompiled_functions[0x181560](ctx, 0x181560);


            } else {
                ctx.cpuRegs.pc = 0x181560;
            }
                ctx.cpuRegs.GPR.r[18].SD[0] = static_cast<int32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) << 2);
                ctx.cpuRegs.GPR.r[16].SD[0] = ctx.cpuRegs.GPR.r[2].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[17].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
            // JAL was called 
        // The address after JAL is: 0x1bba74
        // The next block should be: 1bba90
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bba74;
            if (recompiled_functions.count(0x1cc8e0)) {
                recompiled_functions[0x1cc8e0](ctx, 0x1cc8e0);


            } else {
                ctx.cpuRegs.pc = 0x1cc8e0;
            }
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(0x300000);
                ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[3].SL[0]) + 0xffffa6a8);
                memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x6c, ctx.cpuRegs.GPR.r[3].UL[0]);
            // JAL was called 
        // The address after JAL is: 0x1bba88
        // The next block should be: 1bba90
                )code" << std::endl;




        file << R"code(
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bba88;
            if (recompiled_functions.count(0x1d9670)) {
                recompiled_functions[0x1d9670](ctx, 0x1d9670);


            } else {
                ctx.cpuRegs.pc = 0x1d9670;
            }
                // lw instruction - 32-bit load
            ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x6c));
            goto Label_0038;


        Label_0017: // 0x1bba90
                ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x70);
                ctx.cpuRegs.GPR.r[19].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[16].SL[0]) + 0x1);
            // JAL was called 
        // The address after JAL is: 0x1bba9c
        // The next block should be: 1bbacc
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bba9c;
            if (recompiled_functions.count(0x181560)) {
                recompiled_functions[0x181560](ctx, 0x181560);


            } else {
                ctx.cpuRegs.pc = 0x181560;
            }
                ctx.cpuRegs.GPR.r[18].SD[0] = static_cast<int32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) << 2);
                ctx.cpuRegs.GPR.r[16].SD[0] = ctx.cpuRegs.GPR.r[2].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[17].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
            // JAL was called 
        // The address after JAL is: 0x1bbab0
        // The next block should be: 1bbacc
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bbab0;
            if (recompiled_functions.count(0x1cc8e0)) {
                recompiled_functions[0x1cc8e0](ctx, 0x1cc8e0);


            } else {
                ctx.cpuRegs.pc = 0x1cc8e0;
            }
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(0x300000);
                ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[3].SL[0]) + 0xffffa618);
                memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x6c, ctx.cpuRegs.GPR.r[3].UL[0]);
            // JAL was called 
        // The address after JAL is: 0x1bbac4
        // The next block should be: 1bbacc
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bbac4;
            if (recompiled_functions.count(0x1d9728)) {
                recompiled_functions[0x1d9728](ctx, 0x1d9728);


            } else {
                ctx.cpuRegs.pc = 0x1d9728;
            }
                // lw instruction - 32-bit load
            ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x6c));
            goto Label_0038;


        Label_0018: // 0x1bbacc
                ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x70);
                ctx.cpuRegs.GPR.r[19].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[16].SL[0]) + 0x1);
            // JAL was called 
        // The address after JAL is: 0x1bbad8
        // The next block should be: 1bbb08
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bbad8;
            if (recompiled_functions.count(0x181560)) {
                recompiled_functions[0x181560](ctx, 0x181560);


            } else {
                ctx.cpuRegs.pc = 0x181560;
            }
                ctx.cpuRegs.GPR.r[18].SD[0] = static_cast<int32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) << 2);
                ctx.cpuRegs.GPR.r[16].SD[0] = ctx.cpuRegs.GPR.r[2].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[17].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
            // JAL was called 
        // The address after JAL is: 0x1bbaec
        // The next block should be: 1bbb08
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bbaec;
            if (recompiled_functions.count(0x1cc8e0)) {
                recompiled_functions[0x1cc8e0](ctx, 0x1cc8e0);


            } else {
                ctx.cpuRegs.pc = 0x1cc8e0;
            }
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(0x300000);
                ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[3].SL[0]) + 0xffffa588);
                memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x6c, ctx.cpuRegs.GPR.r[3].UL[0]);
            // JAL was called 
        // The address after JAL is: 0x1bbb00
        // The next block should be: 1bbb08
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bbb00;
            if (recompiled_functions.count(0x1d97e0)) {
                recompiled_functions[0x1d97e0](ctx, 0x1d97e0);


            } else {
                ctx.cpuRegs.pc = 0x1d97e0;
            }
                // lw instruction - 32-bit load
            ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x6c));
            goto Label_0038;


        Label_0019: // 0x1bbb08
                ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x70);
                ctx.cpuRegs.GPR.r[19].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[16].SL[0]) + 0x1);
            // JAL was called 
        // The address after JAL is: 0x1bbb14
        // The next block should be: 1bbb44
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bbb14;
            if (recompiled_functions.count(0x181560)) {
                recompiled_functions[0x181560](ctx, 0x181560);


            } else {
                ctx.cpuRegs.pc = 0x181560;
            }
                ctx.cpuRegs.GPR.r[18].SD[0] = static_cast<int32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) << 2);
                ctx.cpuRegs.GPR.r[16].SD[0] = ctx.cpuRegs.GPR.r[2].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[17].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
            // JAL was called 
        // The address after JAL is: 0x1bbb28
        // The next block should be: 1bbb44
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bbb28;
            if (recompiled_functions.count(0x1cc8e0)) {
                recompiled_functions[0x1cc8e0](ctx, 0x1cc8e0);


            } else {
                ctx.cpuRegs.pc = 0x1cc8e0;
            }
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(0x300000);
                ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[3].SL[0]) + 0xffff9e48);
                memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x6c, ctx.cpuRegs.GPR.r[3].UL[0]);
            // JAL was called 
        // The address after JAL is: 0x1bbb3c
        // The next block should be: 1bbb44
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bbb3c;
            if (recompiled_functions.count(0x1da458)) {
                recompiled_functions[0x1da458](ctx, 0x1da458);


            } else {
                ctx.cpuRegs.pc = 0x1da458;
            }
                // lw instruction - 32-bit load
            ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x6c));
            goto Label_0038;


        Label_0020: // 0x1bbb44
                ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x70);
                ctx.cpuRegs.GPR.r[19].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[16].SL[0]) + 0x1);
            // JAL was called 
        // The address after JAL is: 0x1bbb50
        // The next block should be: 1bbb80
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bbb50;
            if (recompiled_functions.count(0x181560)) {
                recompiled_functions[0x181560](ctx, 0x181560);
                )code" << std::endl;
        file << R"code(

            } else {
                ctx.cpuRegs.pc = 0x181560;
            }
                ctx.cpuRegs.GPR.r[18].SD[0] = static_cast<int32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) << 2);
                ctx.cpuRegs.GPR.r[16].SD[0] = ctx.cpuRegs.GPR.r[2].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[17].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
            // JAL was called 
        // The address after JAL is: 0x1bbb64
        // The next block should be: 1bbb80
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bbb64;
            if (recompiled_functions.count(0x1cc8e0)) {
                recompiled_functions[0x1cc8e0](ctx, 0x1cc8e0);


            } else {
                ctx.cpuRegs.pc = 0x1cc8e0;
            }
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(0x300000);
                ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[3].SL[0]) + 0xffffa4f8);
                memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x6c, ctx.cpuRegs.GPR.r[3].UL[0]);
            // JAL was called 
        // The address after JAL is: 0x1bbb78
        // The next block should be: 1bbb80
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bbb78;
            if (recompiled_functions.count(0x1d9898)) {
                recompiled_functions[0x1d9898](ctx, 0x1d9898);


            } else {
                ctx.cpuRegs.pc = 0x1d9898;
            }
                // lw instruction - 32-bit load
            ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x6c));
            goto Label_0038;


        Label_0021: // 0x1bbb80
                ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x70);
                ctx.cpuRegs.GPR.r[19].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[16].SL[0]) + 0x1);
            // JAL was called 
        // The address after JAL is: 0x1bbb8c
        // The next block should be: 1bbbbc
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bbb8c;
            if (recompiled_functions.count(0x181560)) {
                recompiled_functions[0x181560](ctx, 0x181560);


            } else {
                ctx.cpuRegs.pc = 0x181560;
            }
                ctx.cpuRegs.GPR.r[18].SD[0] = static_cast<int32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) << 2);
                ctx.cpuRegs.GPR.r[16].SD[0] = ctx.cpuRegs.GPR.r[2].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[17].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
            // JAL was called 
        // The address after JAL is: 0x1bbba0
        // The next block should be: 1bbbbc
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bbba0;
            if (recompiled_functions.count(0x1cc8e0)) {
                recompiled_functions[0x1cc8e0](ctx, 0x1cc8e0);
                )code" << std::endl;
        file << R"code(

            } else {
                ctx.cpuRegs.pc = 0x1cc8e0;
            }
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(0x300000);
                ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[3].SL[0]) + 0xffffa468);
                memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x6c, ctx.cpuRegs.GPR.r[3].UL[0]);
            // JAL was called 
        // The address after JAL is: 0x1bbbb4
        // The next block should be: 1bbbbc
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bbbb4;
            if (recompiled_functions.count(0x1d9968)) {
                recompiled_functions[0x1d9968](ctx, 0x1d9968);


            } else {
                ctx.cpuRegs.pc = 0x1d9968;
            }
                // lw instruction - 32-bit load
            ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x6c));
            goto Label_0038;


        Label_0022: // 0x1bbbbc
                ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x280);
                ctx.cpuRegs.GPR.r[19].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[16].SL[0]) + 0x1);
            // JAL was called 
        // The address after JAL is: 0x1bbbc8
        // The next block should be: 1bbbf0
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bbbc8;
            if (recompiled_functions.count(0x181560)) {
                recompiled_functions[0x181560](ctx, 0x181560);
                )code" << std::endl;
        file << R"code(

            } else {
                ctx.cpuRegs.pc = 0x181560;
            }
                ctx.cpuRegs.GPR.r[18].SD[0] = static_cast<int32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) << 2);
                ctx.cpuRegs.GPR.r[16].SD[0] = ctx.cpuRegs.GPR.r[2].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
            // JAL was called 
        // The address after JAL is: 0x1bbbd8
        // The next block should be: 1bbbf0
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bbbd8;
            if (recompiled_functions.count(0x1cc8e0)) {
                recompiled_functions[0x1cc8e0](ctx, 0x1cc8e0);


            } else {
                ctx.cpuRegs.pc = 0x1cc8e0;
            }
                ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(0x300000);
                ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0xffffffff);
                ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[2].SL[0]) + 0xffffa3d8);
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0xf);
                memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x6c, ctx.cpuRegs.GPR.r[2].UL[0]);
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[3].SL[0]) + 0xffffffff);


        Label_0023: // 0x1bbbf0
            //nop 
            //nop 
            //nop 
            //nop 
            bool branch_taken_1bbc00 = (ctx.cpuRegs.GPR.r[3].UL[0] != ctx.cpuRegs.GPR.r[4].UL[0]);
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[3].SL[0]) + 0xffffffff);
            if (branch_taken_1bbc00) {
                goto Label_0023;
            } else {
                goto Label_0024;
            }


        Label_0024: // 0x1bbc08
                ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0xf);
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0xffffffff);
                ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[2].SL[0]) + 0xffffffff);
            //nop 
        // This BLOCK HAS NO BRANCH SO GO TO NEXT BLOCK AFTER THIS, CURRENT BLOCK ENDS AT: 0x1bbc18
        // Fall through to 0x1bbc18
            goto Label_0025; // Fall through

                )code" << std::endl;
        file << R"code(
        Label_0025: // 0x1bbc18
            //nop 
            //nop 
            //nop 
            //nop 
            bool branch_taken_1bbc28 = (ctx.cpuRegs.GPR.r[2].UL[0] != ctx.cpuRegs.GPR.r[3].UL[0]);
                ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[2].SL[0]) + 0xffffffff);
            if (branch_taken_1bbc28) {
                goto Label_0025;
            } else {
                goto Label_0026;
            }
                )code" << std::endl;
        file << R"code(
        Label_0026: // 0x1bbc30
                ctx.cpuRegs.GPR.r[1].SD[0] = static_cast<int32_t>(0x3f800000);
                ctx.fpuRegs.fpr[1].UL = ctx.cpuRegs.GPR.r[1].UL[0];
                ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[1].SD[0] = static_cast<int32_t>(0x3dcc0000);
                ctx.cpuRegs.GPR.r[1].UD[0] = ctx.cpuRegs.GPR.r[1].UD[0] | static_cast<uint64_t>(0xcccd);
                ctx.fpuRegs.fpr[0].UL = ctx.cpuRegs.GPR.r[1].UL[0];
                ctx.cpuRegs.GPR.r[17].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                // swc1 - Store Word from Coprocessor 1
            memory::write<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x278, ctx.fpuRegs.fpr[1].UL);
                // swc1 - Store Word from Coprocessor 1
            memory::write<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x27c, ctx.fpuRegs.fpr[0].UL);
                memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x274, ctx.cpuRegs.GPR.r[0].UL[0]);
            // JAL was called 
        // The address after JAL is: 0x1bbc5c
        // The next block should be: 1bbc64
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bbc5c;
            if (recompiled_functions.count(0x1d99a0)) {
                recompiled_functions[0x1d99a0](ctx, 0x1d99a0);


            } else {
                ctx.cpuRegs.pc = 0x1d99a0;
            }
                // lw instruction - 32-bit load
            ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x6c));
            goto Label_0038;


        Label_0027: // 0x1bbc64
                ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x70);
                ctx.cpuRegs.GPR.r[19].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[16].SL[0]) + 0x1);
            // JAL was called 
        // The address after JAL is: 0x1bbc70
        // The next block should be: 1bbca0
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bbc70;
            if (recompiled_functions.count(0x181560)) {
                recompiled_functions[0x181560](ctx, 0x181560);


            } else {
                ctx.cpuRegs.pc = 0x181560;
            }
                ctx.cpuRegs.GPR.r[18].SD[0] = static_cast<int32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) << 2);
                ctx.cpuRegs.GPR.r[16].SD[0] = ctx.cpuRegs.GPR.r[2].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[17].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
            // JAL was called 
        // The address after JAL is: 0x1bbc84
        // The next block should be: 1bbca0
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bbc84;
            if (recompiled_functions.count(0x1cc8e0)) {
                recompiled_functions[0x1cc8e0](ctx, 0x1cc8e0);


            } else {
                ctx.cpuRegs.pc = 0x1cc8e0;
            }
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(0x300000);
                ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[3].SL[0]) + 0xffffa2b8);
                memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x6c, ctx.cpuRegs.GPR.r[3].UL[0]);
            // JAL was called 
        // The address after JAL is: 0x1bbc98
        // The next block should be: 1bbca0
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bbc98;
            if (recompiled_functions.count(0x1d9b28)) {
                recompiled_functions[0x1d9b28](ctx, 0x1d9b28);


            } else {
                ctx.cpuRegs.pc = 0x1d9b28;
            }
                // lw instruction - 32-bit load
            ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x6c));
            goto Label_0038;


        Label_0028: // 0x1bbca0
                ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x290);
                ctx.cpuRegs.GPR.r[19].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[16].SL[0]) + 0x1);
            // JAL was called 
        // The address after JAL is: 0x1bbcac
        // The next block should be: 1bbcd8
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bbcac;
            if (recompiled_functions.count(0x181560)) {
                recompiled_functions[0x181560](ctx, 0x181560);


            } else {
                ctx.cpuRegs.pc = 0x181560;
            }
                ctx.cpuRegs.GPR.r[18].SD[0] = static_cast<int32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) << 2);
                ctx.cpuRegs.GPR.r[16].SD[0] = ctx.cpuRegs.GPR.r[2].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
            // JAL was called 
        // The address after JAL is: 0x1bbcbc
        // The next block should be: 1bbcd8
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bbcbc;
            if (recompiled_functions.count(0x1cc8e0)) {
                recompiled_functions[0x1cc8e0](ctx, 0x1cc8e0);


            } else {
                ctx.cpuRegs.pc = 0x1cc8e0;
            }
                ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(0x300000);
                ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0xffffffff);
                ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[2].SL[0]) + 0xffffa228);
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0xf);
                memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x6c, ctx.cpuRegs.GPR.r[2].UL[0]);
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[3].SL[0]) + 0xffffffff);
            //nop 


        Label_0029: // 0x1bbcd8
            //nop 
            //nop 
            //nop 
            //nop 
            bool branch_taken_1bbce8 = (ctx.cpuRegs.GPR.r[3].UL[0] != ctx.cpuRegs.GPR.r[4].UL[0]);
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[3].SL[0]) + 0xffffffff);
            if (branch_taken_1bbce8) {
                goto Label_0029;
            } else {
                goto Label_0030;
            }


        Label_0030: // 0x1bbcf0
                ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0xf);
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0xffffffff);
                ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[2].SL[0]) + 0xffffffff);
            //nop 
        // This BLOCK HAS NO BRANCH SO GO TO NEXT BLOCK AFTER THIS, CURRENT BLOCK ENDS AT: 0x1bbd00
        // Fall through to 0x1bbd00
            goto Label_0031; // Fall through


        Label_0031: // 0x1bbd00
            //nop 
            //nop 
            //nop 
            //nop 
            bool branch_taken_1bbd10 = (ctx.cpuRegs.GPR.r[2].UL[0] != ctx.cpuRegs.GPR.r[3].UL[0]);
                ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[2].SL[0]) + 0xffffffff);
            if (branch_taken_1bbd10) {
                goto Label_0031;
            } else {
                goto Label_0032;
            }


        Label_0032: // 0x1bbd18
                ctx.cpuRegs.GPR.r[1].SD[0] = static_cast<int32_t>(0x3dcc0000);
                ctx.cpuRegs.GPR.r[1].UD[0] = ctx.cpuRegs.GPR.r[1].UD[0] | static_cast<uint64_t>(0xcccd);
                ctx.fpuRegs.fpr[0].UL = ctx.cpuRegs.GPR.r[1].UL[0];
                ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[1].SD[0] = static_cast<int32_t>(0x3f800000);
                ctx.fpuRegs.fpr[1].UL = ctx.cpuRegs.GPR.r[1].UL[0];
                ctx.cpuRegs.GPR.r[17].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                // swc1 - Store Word from Coprocessor 1
            memory::write<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x284, ctx.fpuRegs.fpr[0].UL);
                // swc1 - Store Word from Coprocessor 1
            memory::write<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x278, ctx.fpuRegs.fpr[1].UL);
                memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x274, ctx.cpuRegs.GPR.r[0].UL[0]);
                // swc1 - Store Word from Coprocessor 1
            memory::write<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x27c, ctx.fpuRegs.fpr[0].UL);
                // swc1 - Store Word from Coprocessor 1
            memory::write<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) + 0x280, ctx.fpuRegs.fpr[0].UL);
            // JAL was called 
        // The address after JAL is: 0x1bbd4c
        // The next block should be: 1bbd54
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bbd4c;
            if (recompiled_functions.count(0x1d9b60)) {
                recompiled_functions[0x1d9b60](ctx, 0x1d9b60);


            } else {
                ctx.cpuRegs.pc = 0x1d9b60;
            }
                // lw instruction - 32-bit load
            ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x6c));
            goto Label_0038;


        Label_0033: // 0x1bbd54
                ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x70);
                ctx.cpuRegs.GPR.r[19].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[16].SL[0]) + 0x1);
            // JAL was called 
        // The address after JAL is: 0x1bbd60
        // The next block should be: 1bbd90
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bbd60;
            if (recompiled_functions.count(0x181560)) {
                recompiled_functions[0x181560](ctx, 0x181560);


            } else {
                ctx.cpuRegs.pc = 0x181560;
            }
                ctx.cpuRegs.GPR.r[18].SD[0] = static_cast<int32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) << 2);
                ctx.cpuRegs.GPR.r[16].SD[0] = ctx.cpuRegs.GPR.r[2].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[17].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
            // JAL was called 
        // The address after JAL is: 0x1bbd74
        // The next block should be: 1bbd90
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bbd74;
            if (recompiled_functions.count(0x1cc8e0)) {
                recompiled_functions[0x1cc8e0](ctx, 0x1cc8e0);


            } else {
                ctx.cpuRegs.pc = 0x1cc8e0;
            }
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(0x300000);
                ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[3].SL[0]) + 0xffffa198);
                memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x6c, ctx.cpuRegs.GPR.r[3].UL[0]);
            // JAL was called 
        // The address after JAL is: 0x1bbd88
        // The next block should be: 1bbd90
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bbd88;
            if (recompiled_functions.count(0x1d9c30)) {
                recompiled_functions[0x1d9c30](ctx, 0x1d9c30);


            } else {
                ctx.cpuRegs.pc = 0x1d9c30;
            }
                // lw instruction - 32-bit load
            ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x6c));
            goto Label_0038;


        Label_0034: // 0x1bbd90
                ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x70);
                ctx.cpuRegs.GPR.r[19].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[16].SL[0]) + 0x1);
            // JAL was called 
        // The address after JAL is: 0x1bbd9c
        // The next block should be: 1bbdcc
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bbd9c;
            if (recompiled_functions.count(0x181560)) {
                recompiled_functions[0x181560](ctx, 0x181560);


            } else {
                ctx.cpuRegs.pc = 0x181560;
            }
                ctx.cpuRegs.GPR.r[18].SD[0] = static_cast<int32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) << 2);
                ctx.cpuRegs.GPR.r[16].SD[0] = ctx.cpuRegs.GPR.r[2].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[17].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
            // JAL was called 
        // The address after JAL is: 0x1bbdb0
        // The next block should be: 1bbdcc
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bbdb0;
            if (recompiled_functions.count(0x1cc8e0)) {
                recompiled_functions[0x1cc8e0](ctx, 0x1cc8e0);


            } else {
                ctx.cpuRegs.pc = 0x1cc8e0;
            }
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(0x300000);
                ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[3].SL[0]) + 0xffffa078);
                memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x6c, ctx.cpuRegs.GPR.r[3].UL[0]);
            // JAL was called 
        // The address after JAL is: 0x1bbdc4
        // The next block should be: 1bbdcc
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bbdc4;
            if (recompiled_functions.count(0x1d9d28)) {
                recompiled_functions[0x1d9d28](ctx, 0x1d9d28);


            } else {
                ctx.cpuRegs.pc = 0x1d9d28;
            }
                // lw instruction - 32-bit load
            ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x6c));
            goto Label_0038;


        Label_0035: // 0x1bbdcc
                ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x70);
                ctx.cpuRegs.GPR.r[19].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[16].SL[0]) + 0x1);
            // JAL was called 
        // The address after JAL is: 0x1bbdd8
        // The next block should be: 1bbe08
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bbdd8;
            if (recompiled_functions.count(0x181560)) {
                recompiled_functions[0x181560](ctx, 0x181560);


            } else {
                ctx.cpuRegs.pc = 0x181560;
            }
                ctx.cpuRegs.GPR.r[18].SD[0] = static_cast<int32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) << 2);
                ctx.cpuRegs.GPR.r[16].SD[0] = ctx.cpuRegs.GPR.r[2].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[17].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
            // JAL was called 
        // The address after JAL is: 0x1bbdec
        // The next block should be: 1bbe08


                        )code" << std::endl;




        file << R"code(
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bbdec;
            if (recompiled_functions.count(0x1cc8e0)) {
                recompiled_functions[0x1cc8e0](ctx, 0x1cc8e0);


            } else {
                ctx.cpuRegs.pc = 0x1cc8e0;
            }
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(0x300000);
                ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[3].SL[0]) + 0xffff9fe8);
                memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x6c, ctx.cpuRegs.GPR.r[3].UL[0]);
            // JAL was called 
        // The address after JAL is: 0x1bbe00
        // The next block should be: 1bbe08
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bbe00;
            if (recompiled_functions.count(0x1d9de8)) {
                recompiled_functions[0x1d9de8](ctx, 0x1d9de8);


            } else {
                ctx.cpuRegs.pc = 0x1d9de8;
            }
                // lw instruction - 32-bit load
            ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x6c));
            goto Label_0038;


        Label_0036: // 0x1bbe08
                ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x70);
                ctx.cpuRegs.GPR.r[19].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[16].SL[0]) + 0x1);
            // JAL was called 
        // The address after JAL is: 0x1bbe14
        // The next block should be: 1bbe44
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bbe14;
            if (recompiled_functions.count(0x181560)) {
                recompiled_functions[0x181560](ctx, 0x181560);


            } else {
                ctx.cpuRegs.pc = 0x181560;
            }
                ctx.cpuRegs.GPR.r[18].SD[0] = static_cast<int32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) << 2);
                ctx.cpuRegs.GPR.r[16].SD[0] = ctx.cpuRegs.GPR.r[2].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[17].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
            // JAL was called 
        // The address after JAL is: 0x1bbe28
        // The next block should be: 1bbe44
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bbe28;
            if (recompiled_functions.count(0x1cc8e0)) {
                recompiled_functions[0x1cc8e0](ctx, 0x1cc8e0);


            } else {
                ctx.cpuRegs.pc = 0x1cc8e0;
            }
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(0x300000);
                ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[16].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[3].SL[0]) + 0xffff9f58);
                memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x6c, ctx.cpuRegs.GPR.r[3].UL[0]);
            // JAL was called 
        // The address after JAL is: 0x1bbe3c
        // The next block should be: 1bbe44
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bbe3c;
            if (recompiled_functions.count(0x1d9ea8)) {
                recompiled_functions[0x1d9ea8](ctx, 0x1d9ea8);


            } else {
                ctx.cpuRegs.pc = 0x1d9ea8;
            }
                // lw instruction - 32-bit load
            ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x6c));
            goto Label_0038;


        Label_0037: // 0x1bbe44
                ctx.cpuRegs.GPR.r[19].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[16].SL[0]) + 0x1);
                ctx.cpuRegs.GPR.r[17].SD[0] = ctx.cpuRegs.GPR.r[0].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[18].SD[0] = static_cast<int32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0]) << 2);
                // lw instruction - 32-bit load
            ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0]) + 0x6c));
        // This BLOCK HAS NO BRANCH SO GO TO NEXT BLOCK AFTER THIS, CURRENT BLOCK ENDS AT: 0x1bbe54
        // Fall through to 0x1bbe54
            goto Label_0038; // Fall through


        Label_0038: // 0x1bbe54
                ctx.cpuRegs.GPR.r[5].SD[0] = ctx.cpuRegs.GPR.r[20].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[16].SD[0] = ctx.cpuRegs.GPR.r[19].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
                ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(memory::read<uint16_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x70)));
                // lw instruction - 32-bit load
            ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) + 0x74));
                ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[17].SL[0]) + static_cast<int32_t>(ctx.cpuRegs.GPR.r[4].SL[0]));
                // JALR - Jump and Link Register
            ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bbe6c;
            {
                uint32_t target = ctx.cpuRegs.GPR.r[2].UL[0];
                auto func_ptr = find_containing_function(target);
                if (func_ptr != nullptr) {
                    func_ptr(ctx, target);
                } else {
                    ctx.cpuRegs.pc = target;
                }
            }
                ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[21].SL[0]) + static_cast<int32_t>(ctx.cpuRegs.GPR.r[18].SL[0]));
                memory::write<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0] + 0x0, ctx.cpuRegs.GPR.r[17].UL[0]);
                // lw instruction - 32-bit load
            ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[21].UL[0]) + 0x10));
                ctx.cpuRegs.GPR.r[2].UD[0] = (ctx.cpuRegs.GPR.r[16].UL[0] < ctx.cpuRegs.GPR.r[2].UL[0]) ? 1 : 0;
            if (ctx.cpuRegs.GPR.r[2].UL[0] != ctx.cpuRegs.GPR.r[0].UL[0]) {
                    // lw instruction - 32-bit load
            ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[20].UL[0]) + 0x0));
                goto Label_0004;
            } else {
                goto Label_0039;
            }
        )code" << std::endl;




        file << R"code(
        Label_0039: // 0x1bbe84
                ctx.cpuRegs.GPR.r[4].SD[0] = ctx.cpuRegs.GPR.r[21].SD[0] + ctx.cpuRegs.GPR.r[0].SD[0];
            // JAL was called 
        // The address after JAL is: 0x1bbe8c
        // The next block should be: 1bbe94
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x1bbe8c;
            if (recompiled_functions.count(0x1c03c0)) {
                recompiled_functions[0x1c03c0](ctx, 0x1c03c0);


            } else {
                ctx.cpuRegs.pc = 0x1c03c0;
            }
            bool branch_taken_1bbe8c = (ctx.cpuRegs.GPR.r[2].UL[0] == ctx.cpuRegs.GPR.r[0].UL[0]);
                ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[0].SL[0]) + 0x2);
            if (branch_taken_1bbe8c) {
                goto Label_0041;
            } else {
                goto Label_0040;
            }


        Label_0040: // 0x1bbe94
                memory::write<uint32_t>(ctx.cpuRegs.GPR.r[21].UL[0] + 0x60, ctx.cpuRegs.GPR.r[2].UL[0]);
        // This BLOCK HAS NO BRANCH SO GO TO NEXT BLOCK AFTER THIS, CURRENT BLOCK ENDS AT: 0x1bbe98
        // Fall through to 0x1bbe98
            goto Label_0041; // Fall through


        Label_0041: // 0x1bbe98
                ctx.cpuRegs.GPR.r[16].UD[0] = memory::read<uint64_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x10);
                ctx.cpuRegs.GPR.r[17].UD[0] = memory::read<uint64_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x18);
                ctx.cpuRegs.GPR.r[18].UD[0] = memory::read<uint64_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x20);
                ctx.cpuRegs.GPR.r[19].UD[0] = memory::read<uint64_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x28);
                ctx.cpuRegs.GPR.r[20].UD[0] = memory::read<uint64_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x30);
                ctx.cpuRegs.GPR.r[21].UD[0] = memory::read<uint64_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x38);
                ctx.cpuRegs.GPR.r[31].UD[0] = memory::read<uint64_t>(static_cast<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0]) + 0x40);
                ctx.cpuRegs.GPR.r[29].SD[0] = static_cast<int32_t>(static_cast<int32_t>(ctx.cpuRegs.GPR.r[29].SL[0]) + 0x50);
            return; // Return from function


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


    if (func.base_address == 0x002ad8f0) {
        std::cout << "Skipping recompile for 0x002ad8f0 (Hooked via HLE)" << std::endl;
        file << "// Function 0x2ad8f0 skipped - replaced by HLE hook\n";
        return;
    }


    if (func.base_address == 0x002aac80) {
        std::cout << "Skipping recompile for 0x002aac80 (Hooked via HLE)" << std::endl;
        file << "// Function 0x2aac80 skipped - replaced by HLE hook\n";
        return;
    }




    if (func.base_address == 0x002adb18) {
        std::cout << "Skipping recompile for 0x002adb18 (Hooked via HLE)" << std::endl;
        file << "// Function 0x2adb18 skipped - replaced by HLE hook\n";
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
    file << R"code(
void FUN_00255188(CpuContext& ctx) {
    // ================================================================
    // Prologue: 0x255188 - 0x2551a0
    // ================================================================
    ctx.cpuRegs.GPR.r[29].SL[0] = ctx.cpuRegs.GPR.r[29].SL[0] - 0x40;
    memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x10, ctx.cpuRegs.GPR.r[16].UD[0]); // sd s0
    memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x18, ctx.cpuRegs.GPR.r[17].UD[0]); // sd s1
    ctx.cpuRegs.GPR.r[16].UD[0] = ctx.cpuRegs.GPR.r[4].UD[0]; // s0 = a0
    memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x20, ctx.cpuRegs.GPR.r[31].UD[0]); // sd ra
    ctx.cpuRegs.GPR.r[17].UD[0] = ctx.cpuRegs.GPR.r[5].UD[0]; // s1 = a1
    memory::write<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x30, ctx.fpuRegs.fpr[20].UL);      // swc1 f20


    // 0x2551a4: lw a0, 0x0(s0)
    ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(
        memory::read<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x0));


    // 0x2551a8: andi v0, a0, 0x100
    // 0x2551ac: beq v0, zero, 255264
    // 0x2551b0: _lw a2, 0x4(s1)  — delay slot, ALWAYS executes
    ctx.cpuRegs.GPR.r[6].SD[0] = static_cast<int32_t>(
        memory::read<uint32_t>(ctx.cpuRegs.GPR.r[17].UL[0] + 0x4));
    if ((ctx.cpuRegs.GPR.r[4].UL[0] & 0x100) == 0) goto Label_00255264;


    // ================================================================
    // BIT 0x100 SET PATH: animation state transition
    // 0x2551b4 - 0x2551dc
    // ================================================================
    {
        uint32_t a0_val = ctx.cpuRegs.GPR.r[4].UL[0];
        // 0x2551b4: srl v1, a0, 4
        // 0x2551b8: li v0, -0x10
        // 0x2551bc: and v0, a0, v0
        // 0x2551c0: andi v1, v1, 0xf
        // 0x2551c4: or v0, v0, v1
        uint32_t v1 = (a0_val >> 4) & 0xf;
        uint32_t v0 = (a0_val & 0xFFFFFFF0) | v1;
        // 0x2551c8: li a0, -0x101
        // 0x2551cc: andi a1, v0, 0xf
        ctx.cpuRegs.GPR.r[5].UL[0] = v0 & 0xf;
        // 0x2551d0: sw a2, 0xc(s0)
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0xc, ctx.cpuRegs.GPR.r[6].UL[0]);
        // 0x2551d4: and v0, v0, a0
        v0 = v0 & 0xFFFFFEFF;
        // 0x2551d8: li v1, 2
        // 0x2551dc: sw v0, 0x0(s0)
        memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x0, v0);
    }


    // 0x2551e0: beq a1, v1, LAB_25522c  (if a1 == 2)
    // 0x2551e4: _sw zero, 0x18(s0)  — delay slot, ALWAYS executes
    memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x18, 0);
    if (ctx.cpuRegs.GPR.r[5].UL[0] == 2) goto Label_0025522c;


    // 0x2551e8: sltiu v0, a1, 3
    // 0x2551ec: beq v0, zero, 255204  (if a1 >= 3)
    // 0x2551f0: _li v0, 1  — delay slot, ALWAYS executes
    ctx.cpuRegs.GPR.r[2].UL[0] = 1;
    if (ctx.cpuRegs.GPR.r[5].UL[0] >= 3) goto Label_00255204;


    // 0x2551f4: beql a1, v0, LAB_255220  (if a1 == 1, likely branch)
    // 0x2551f8: _lw v0, 0x34(s0)  — delay slot, ONLY if taken
    if (ctx.cpuRegs.GPR.r[5].UL[0] == 1) {
        ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(
            memory::read<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x34));
        goto Label_00255220;
    }


    // 0x2551fc: b LAB_25547c  (a1 == 0 path, default)
    // 0x255200: _lw a2, 0x1c(s0)  — delay slot
    ctx.cpuRegs.GPR.r[6].SD[0] = static_cast<int32_t>(
        memory::read<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x1c));
    goto Label_0025547c;


Label_00255204:
    // 0x255204: li v0, 3
    // 0x255208: beq a1, v0, LAB_25523c  (if a1 == 3)
    // 0x25520c: _li v0, 4  — delay slot, ALWAYS executes
    ctx.cpuRegs.GPR.r[2].UL[0] = 4;
    if (ctx.cpuRegs.GPR.r[5].UL[0] == 3) goto Label_0025523c;


    // 0x255210: beql a1, v0, LAB_25524c  (if a1 == 4, likely branch)
    // 0x255214: _lw v0, 0x34(s0)  — delay slot, ONLY if taken
    if (ctx.cpuRegs.GPR.r[5].UL[0] == 4) {
        ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(
            memory::read<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x34));
        goto Label_0025524c;
    }


    // 0x255218: b LAB_25547c  (default path)
    // 0x25521c: _lw a2, 0x1c(s0)  — delay slot
    ctx.cpuRegs.GPR.r[6].SD[0] = static_cast<int32_t>(
        memory::read<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x1c));
    goto Label_0025547c;


Label_00255220:
    // a1==1 path. v0 (r[2]) = *(s0+0x34) from beql delay slot
    // 0x255220: lh a0, 0x8(v0)
    ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(
        memory::read<uint16_t>(ctx.cpuRegs.GPR.r[2].UL[0] + 0x8)));
    // 0x255224: b LAB_255254
    // 0x255228: _lw v1, 0xc(v0)  — delay slot: function pointer
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(
        memory::read<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0] + 0xc));
    goto Label_00255254;


Label_0025522c:
    // a1==2 path.
    // 0x25522c: lw v0, 0x34(s0)
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(
        memory::read<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x34));
    // 0x255230: lh a0, 0x10(v0)
    ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(
        memory::read<uint16_t>(ctx.cpuRegs.GPR.r[2].UL[0] + 0x10)));
    // 0x255234: b LAB_255254
    // 0x255238: _lw v1, 0x14(v0)  — delay slot: function pointer
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(
        memory::read<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0] + 0x14));
    goto Label_00255254;


Label_0025523c:
    // a1==3 path.
    // 0x25523c: lw v0, 0x34(s0)
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(
        memory::read<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x34));
    // 0x255240: lh a0, 0x18(v0)
    ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(
        memory::read<uint16_t>(ctx.cpuRegs.GPR.r[2].UL[0] + 0x18)));
    // 0x255244: b LAB_255254
    // 0x255248: _lw v1, 0x1c(v0)  — delay slot: function pointer
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(
        memory::read<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0] + 0x1c));
    goto Label_00255254;


Label_0025524c:
    // a1==4 path. v0 (r[2]) = *(s0+0x34) from beql delay slot
    // 0x25524c: lh a0, 0x20(v0)
    ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(
        memory::read<uint16_t>(ctx.cpuRegs.GPR.r[2].UL[0] + 0x20)));
    // 0x255250: lw v1, 0x24(v0)  — function pointer, falls through
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(
        memory::read<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0] + 0x24));
    // fall through


Label_00255254:
    // 0x255254: jalr v1
    // 0x255258: _addu a0, s0, a0  — delay slot, BEFORE call
    ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(
        ctx.cpuRegs.GPR.r[16].SL[0] + ctx.cpuRegs.GPR.r[4].SL[0]);
    {
        uint32_t target = ctx.cpuRegs.GPR.r[3].UL[0];
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x0025525c;
        auto it = recompiled_functions.find(target);
        if (it != recompiled_functions.end()) {
            it->second(ctx, target);
        } else {
            g_logFile << "[255188] MISSING JALR@255254 target=0x" << std::hex << target << std::endl;
            ctx.cpuRegs.pc = target;
            goto Label_00255498;
        }
    }
    // 0x25525c: b LAB_25547c
    // 0x255260: _lw a2, 0x1c(s0)  — delay slot
    ctx.cpuRegs.GPR.r[6].SD[0] = static_cast<int32_t>(
        memory::read<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x1c));
    goto Label_0025547c;
)code" << std::endl;


    file << R"code(
    // ================================================================
    // BIT 0x100 CLEAR PATH: normal update with switch on (a0 & 0xf)
    // ================================================================
Label_00255264:
    // a2 (r[6]) already loaded from delay slot at 0x2551b0
    // 0x255264: lw v0, 0xc(s0)
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(
        memory::read<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0xc));
    // 0x255268: andi a0, a0, 0xf
    ctx.cpuRegs.GPR.r[4].UL[0] = ctx.cpuRegs.GPR.r[4].UL[0] & 0xf;
    // 0x25526c: sltiu v1, a0, 5
    // 0x255270: subu v0, a2, v0
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(
        ctx.cpuRegs.GPR.r[6].SL[0] - ctx.cpuRegs.GPR.r[2].SL[0]);
    // 0x255274: beq v1, zero, caseD_0  (if a0 >= 5)
    // 0x255278: _sw v0, 0x0(sp)  — delay slot, ALWAYS executes
    memory::write<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x0, ctx.cpuRegs.GPR.r[2].UL[0]);
    if (ctx.cpuRegs.GPR.r[4].UL[0] >= 5) goto Label_caseD_0;


    // 0x25527c-0x255290: switch table
    switch (ctx.cpuRegs.GPR.r[4].UL[0]) {
        case 0: goto Label_caseD_0;
        case 1: goto Label_caseD_1;
        case 2: goto Label_caseD_2;
        case 3: goto Label_caseD_3;
        case 4: goto Label_caseD_4;
        default: goto Label_caseD_0;
    }


    // ================================================================
    // caseD_1: 0x255298
    // ================================================================
Label_caseD_1:
    // 0x255298: lw v1, 0x14(s0)
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(
        memory::read<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x14));
    // 0x25529c: beq v1, zero, 2552c8
    // 0x2552a0: _lw v0, 0x0(sp)  — delay slot, ALWAYS executes
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(
        memory::read<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x0));
    if (ctx.cpuRegs.GPR.r[3].UL[0] == 0) goto Label_002552c8;


    // 0x2552a4: slt v0, v0, v1  — v0 = (local_40 < param[5])
    // 0x2552a8: xori v0, v0, 1  — v0 = !(local_40 < param[5])
    // 0x2552ac: beq v0, zero, 2552c8  — if (local_40 < param[5]) goto 2552c8
    // 0x2552b0: _li v1, -0xf1  — delay slot, ALWAYS executes
    if (ctx.cpuRegs.GPR.r[2].SL[0] < ctx.cpuRegs.GPR.r[3].SL[0]) goto Label_002552c8;


    // Timer expired — transition to state 2
    // 0x2552b4: lw v0, 0x0(s0)
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(
        memory::read<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x0));
    // 0x2552b8: sw zero, 0x14(s0)
    memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x14, 0);
    // 0x2552bc: and v0, v0, v1  (v1 = 0xFFFFFF0F from delay slot)
    // 0x2552c0: ori v0, v0, 0x120
    // 0x2552c4: sw v0, 0x0(s0)
    ctx.cpuRegs.GPR.r[2].UL[0] = (ctx.cpuRegs.GPR.r[2].UL[0] & 0xFFFFFF0F) | 0x120;
    memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x0, ctx.cpuRegs.GPR.r[2].UL[0]);


Label_002552c8:
    // 0x2552c8: sw zero, 0x18(s0)
    memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x18, 0);
    // 0x2552cc: lw v0, 0x34(s0)
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(
        memory::read<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x34));
    // 0x2552d0: lw a1, 0x0(sp)
    ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(
        memory::read<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x0));
    // 0x2552d4: lh a0, 0x28(v0)
    ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(
        memory::read<uint16_t>(ctx.cpuRegs.GPR.r[2].UL[0] + 0x28)));
    // 0x2552d8: b LAB_2553c4
    // 0x2552dc: _lw v1, 0x2c(v0)  — delay slot: function pointer
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(
        memory::read<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0] + 0x2c));
    goto Label_002553c4;


    // ================================================================
    // caseD_2: 0x2552e0
    // ================================================================
Label_caseD_2:
    // 0x2552e0: lw v1, 0x10(s0)  — duration
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(
        memory::read<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x10));
    // 0x2552e4: lw v0, 0x0(sp)  — local_40
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(
        memory::read<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x0));
    // 0x2552e8: slt v0, v0, v1  — v0 = (local_40 < duration)
    // 0x2552ec: beq v0, zero, 25534c  — if (local_40 >= duration) goto expired
    // 0x2552f0: _lwc1 f0, -0x7d10(gp)  — delay slot, ALWAYS executes
    ctx.fpuRegs.fpr[0].UL = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[28].UL[0] - 0x7d10);
    if (ctx.cpuRegs.GPR.r[2].SL[0] >= ctx.cpuRegs.GPR.r[3].SL[0]) goto Label_0025534c;


    // 0x2552f4: mtc1 v1, f20
    ctx.fpuRegs.fpr[20].UL = ctx.cpuRegs.GPR.r[3].UL[0];
    // 0x2552fc: cvt.s.w f20, f20
    ctx.fpuRegs.fpr[20].f = static_cast<float>(static_cast<int32_t>(ctx.fpuRegs.fpr[20].UL));
    // 0x255300: move a0, sp
    ctx.cpuRegs.GPR.r[4].UL[0] = ctx.cpuRegs.GPR.r[29].UL[0];
    // 0x255304: jal FUN_00259908
    // 0x255308: _mul.s f20, f20, f0  — delay slot, BEFORE call
    ctx.fpuRegs.fpr[20].f = ctx.fpuRegs.fpr[20].f * ctx.fpuRegs.fpr[0].f;
    ctx.cpuRegs.GPR.r[31].UL[0] = 0x0025530c;
    {
        auto it = recompiled_functions.find(0x259908u);
        if (it != recompiled_functions.end()) {
            it->second(ctx, 0x259908);
        } else {
            ctx.cpuRegs.pc = 0x259908;
            goto Label_00255498;
        }
    }


    // 0x25530c-0x255310: lwc1 f1, DAT_003097dc
    ctx.fpuRegs.fpr[1].UL = memory::read<uint32_t>(0x3097dc);
    // 0x25531c: div.s f0, f0, f20
    ctx.fpuRegs.fpr[0].f = ctx.fpuRegs.fpr[0].f / ctx.fpuRegs.fpr[20].f;
    // 0x255324: lwc1 f2, DAT_003097e0
    ctx.fpuRegs.fpr[2].UL = memory::read<uint32_t>(0x3097e0);
    // 0x255328: mul.s f0, f0, f1
    ctx.fpuRegs.fpr[0].f = ctx.fpuRegs.fpr[0].f * ctx.fpuRegs.fpr[1].f;
    // 0x25532c: cvt.w.s f1, f0
    {
        float fval = ctx.fpuRegs.fpr[0].f;
        int32_t ival;
        if (fval >= 2147483648.0f) ival = 0x7FFFFFFF;
        else if (fval < -2147483648.0f) ival = (int32_t)0x80000000;
        else ival = static_cast<int32_t>(fval);
        ctx.fpuRegs.fpr[1].SL = ival;
    }
    // 0x255330: mfc1 v0, f1
    ctx.cpuRegs.GPR.r[2].UL[0] = ctx.fpuRegs.fpr[1].UL;
    // 0x255334: mtc1 v0, f0
    ctx.fpuRegs.fpr[0].UL = ctx.cpuRegs.GPR.r[2].UL[0];
    // 0x25533c: cvt.s.w f0, f0
    ctx.fpuRegs.fpr[0].f = static_cast<float>(static_cast<int32_t>(ctx.fpuRegs.fpr[0].UL));
    // 0x255340: mul.s f0, f0, f2
    ctx.fpuRegs.fpr[0].f = ctx.fpuRegs.fpr[0].f * ctx.fpuRegs.fpr[2].f;
    // 0x255344: b LAB_25536c
    // 0x255348: _swc1 f0, 0x18(s0)  — delay slot
    memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x18, ctx.fpuRegs.fpr[0].UL);
    goto Label_0025536c;


Label_0025534c:
    // Duration expired — set state to 3
    // 0x25534c: lw v0, 0x0(s0)
    ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x0);
    // 0x255354-0x255358: lui at, 0x3f80 / mtc1 at, f0  — f0 = 1.0f
    ctx.fpuRegs.fpr[0].UL = 0x3f800000;
    // 0x25535c-0x255360: and v0, v0, 0xFFFFFF0F / ori v0, v0, 0x130
    ctx.cpuRegs.GPR.r[2].UL[0] = (ctx.cpuRegs.GPR.r[2].UL[0] & 0xFFFFFF0F) | 0x130;
    // 0x255364: swc1 f0, 0x18(s0)
    memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x18, ctx.fpuRegs.fpr[0].UL);
    // 0x255368: sw v0, 0x0(s0)
    memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x0, ctx.cpuRegs.GPR.r[2].UL[0]);
)code" << std::endl;


   file << R"code(
Label_0025536c:
    // 0x25536c: lw v0, 0x34(s0)
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(
        memory::read<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x34));
    // 0x255370: lw a1, 0x0(sp)
    ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(
        memory::read<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x0));
    // 0x255374: lh a0, 0x30(v0)
    ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(
        memory::read<uint16_t>(ctx.cpuRegs.GPR.r[2].UL[0] + 0x30)));
    // 0x255378: b LAB_002553c4
    // 0x25537c: _lw v1, 0x34(v0)  — delay slot: function pointer
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(
        memory::read<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0] + 0x34));
    goto Label_002553c4;


    // ================================================================
    // caseD_3: 0x255380
    // ================================================================
Label_caseD_3:
    // 0x255380: lw v1, 0x14(s0)
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(
        memory::read<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x14));
    // 0x255384: beq v1, zero, LAB_002553b0
    // 0x255388: _lw v0, 0x0(sp)  — delay slot, ALWAYS executes
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(
        memory::read<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x0));
    if (ctx.cpuRegs.GPR.r[3].UL[0] == 0) goto Label_002553b0;


    // 0x25538c: slt v0, v0, v1
    // 0x255390: xori v0, v0, 1
    // 0x255394: beq v0, zero, LAB_002553b0  — if (local_40 < v1) goto 2553b0
    // 0x255398: _li v1, -0xf1  — delay slot, ALWAYS executes (but only used if not branching)
    if (ctx.cpuRegs.GPR.r[2].SL[0] < ctx.cpuRegs.GPR.r[3].SL[0]) goto Label_002553b0;


    // Timer expired — transition to state 4
    // 0x25539c: lw v0, 0x0(s0)
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(
        memory::read<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x0));
    // 0x2553a0: sw zero, 0x14(s0)
    memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x14, 0);
    // 0x2553a4: and v0, v0, v1  (v1 = 0xFFFFFF0F)
    // 0x2553a8: ori v0, v0, 0x140
    // 0x2553ac: sw v0, 0x0(s0)
    ctx.cpuRegs.GPR.r[2].UL[0] = (ctx.cpuRegs.GPR.r[2].UL[0] & 0xFFFFFF0F) | 0x140;
    memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x0, ctx.cpuRegs.GPR.r[2].UL[0]);


Label_002553b0:
    // 0x2553b0: sw zero, 0x18(s0)
    memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x18, 0);
    // 0x2553b4: lw v0, 0x34(s0)
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(
        memory::read<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x34));
    // 0x2553b8: lw a1, 0x0(sp)
    ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(
        memory::read<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x0));
    // 0x2553bc: lh a0, 0x38(v0)
    ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(
        memory::read<uint16_t>(ctx.cpuRegs.GPR.r[2].UL[0] + 0x38)));
    // 0x2553c0: lw v1, 0x3c(v0)  — function pointer, falls through
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(
        memory::read<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0] + 0x3c));
    // fall through to Label_002553c4


Label_002553c4:
    // 0x2553c4: jalr v1
    // 0x2553c8: _addu a0, s0, a0  — delay slot, BEFORE call
    ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(
        ctx.cpuRegs.GPR.r[16].SL[0] + ctx.cpuRegs.GPR.r[4].SL[0]);
    {
        uint32_t target = ctx.cpuRegs.GPR.r[3].UL[0];
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x002553cc;
        auto it = recompiled_functions.find(target);
        if (it != recompiled_functions.end()) {
            it->second(ctx, target);
        } else {
            g_logFile << "[255188] MISSING JALR@2553c4 target=0x" << std::hex << target << std::endl;
            ctx.cpuRegs.pc = target;
            goto Label_00255498;
        }
    }
    // 0x2553cc: b LAB_0025547c
    // 0x2553d0: _lw a2, 0x1c(s0)  — delay slot
    ctx.cpuRegs.GPR.r[6].SD[0] = static_cast<int32_t>(
        memory::read<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x1c));
    goto Label_0025547c;


    // ================================================================
    // caseD_4: 0x2553d4
    // ================================================================
Label_caseD_4:
    // 0x2553d4: lw v1, 0x10(s0)  — duration
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(
        memory::read<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x10));
    // 0x2553d8: lw v0, 0x0(sp)  — local_40
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(
        memory::read<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x0));
    // 0x2553dc: slt v0, v0, v1
    // 0x2553e0: beq v0, zero, LAB_00255440  — if (local_40 >= duration)
    // 0x2553e4: _lwc1 f0, -0x7d10(gp)  — delay slot, ALWAYS executes
    ctx.fpuRegs.fpr[0].UL = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[28].UL[0] - 0x7d10);
    if (ctx.cpuRegs.GPR.r[2].SL[0] >= ctx.cpuRegs.GPR.r[3].SL[0]) goto Label_00255440;


    // 0x2553e8: mtc1 v1, f20
    ctx.fpuRegs.fpr[20].UL = ctx.cpuRegs.GPR.r[3].UL[0];
    // 0x2553f0: cvt.s.w f20, f20
    ctx.fpuRegs.fpr[20].f = static_cast<float>(static_cast<int32_t>(ctx.fpuRegs.fpr[20].UL));
    // 0x2553f4: move a0, sp
    ctx.cpuRegs.GPR.r[4].UL[0] = ctx.cpuRegs.GPR.r[29].UL[0];
    // 0x2553f8: jal FUN_00259908
    // 0x2553fc: _mul.s f20, f20, f0  — delay slot, BEFORE call
    ctx.fpuRegs.fpr[20].f = ctx.fpuRegs.fpr[20].f * ctx.fpuRegs.fpr[0].f;
    ctx.cpuRegs.GPR.r[31].UL[0] = 0x00255400;
    {
        auto it = recompiled_functions.find(0x259908u);
        if (it != recompiled_functions.end()) {
            it->second(ctx, 0x259908);
        } else {
            ctx.cpuRegs.pc = 0x259908;
            goto Label_00255498;
        }
    }


    // 0x255400-0x255404: lwc1 f1, DAT_003097dc
    ctx.fpuRegs.fpr[1].UL = memory::read<uint32_t>(0x3097dc);
    // 0x255410: div.s f0, f0, f20
    ctx.fpuRegs.fpr[0].f = ctx.fpuRegs.fpr[0].f / ctx.fpuRegs.fpr[20].f;
    // 0x255414-0x255418: lwc1 f2, DAT_003097e0
    ctx.fpuRegs.fpr[2].UL = memory::read<uint32_t>(0x3097e0);
    // 0x25541c: mul.s f0, f0, f1
    ctx.fpuRegs.fpr[0].f = ctx.fpuRegs.fpr[0].f * ctx.fpuRegs.fpr[1].f;
    // 0x255420: cvt.w.s f1, f0
    {
        float fval = ctx.fpuRegs.fpr[0].f;
        int32_t ival;
        if (fval >= 2147483648.0f) ival = 0x7FFFFFFF;
        else if (fval < -2147483648.0f) ival = (int32_t)0x80000000;
        else ival = static_cast<int32_t>(fval);
        ctx.fpuRegs.fpr[1].SL = ival;
    }
    // 0x255424: mfc1 v0, f1
    ctx.cpuRegs.GPR.r[2].UL[0] = ctx.fpuRegs.fpr[1].UL;
    // 0x255428: mtc1 v0, f0
    ctx.fpuRegs.fpr[0].UL = ctx.cpuRegs.GPR.r[2].UL[0];
    // 0x255430: cvt.s.w f0, f0
    ctx.fpuRegs.fpr[0].f = static_cast<float>(static_cast<int32_t>(ctx.fpuRegs.fpr[0].UL));
    // 0x255434: mul.s f0, f0, f2
    ctx.fpuRegs.fpr[0].f = ctx.fpuRegs.fpr[0].f * ctx.fpuRegs.fpr[2].f;
    // 0x255438: b LAB_00255460
    // 0x25543c: _swc1 f0, 0x18(s0)  — delay slot
    memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x18, ctx.fpuRegs.fpr[0].UL);
    goto Label_00255460;


Label_00255440:
    // Duration expired — set state to 1
    // 0x255440: lw v0, 0x0(s0)
    ctx.cpuRegs.GPR.r[2].UL[0] = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x0);
    // 0x255448-0x25544c: lui at, 0x3f80 / mtc1 at, f0  — f0 = 1.0f
    ctx.fpuRegs.fpr[0].UL = 0x3f800000;
    // 0x255450: and v0, v0, v1  (v1 = 0xFFFFFF0F)
    // 0x255454: ori v0, v0, 0x110
    ctx.cpuRegs.GPR.r[2].UL[0] = (ctx.cpuRegs.GPR.r[2].UL[0] & 0xFFFFFF0F) | 0x110;
    // 0x255458: swc1 f0, 0x18(s0)
    memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x18, ctx.fpuRegs.fpr[0].UL);
    // 0x25545c: sw v0, 0x0(s0)
    memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x0, ctx.cpuRegs.GPR.r[2].UL[0]);
    // fall through to Label_00255460


Label_00255460:
    // 0x255460: lw v0, 0x34(s0)
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(
        memory::read<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x34));
    // 0x255464: lw a1, 0x0(sp)
    ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(
        memory::read<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x0));
    // 0x255468: lh a0, 0x40(v0)
    ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(
        memory::read<uint16_t>(ctx.cpuRegs.GPR.r[2].UL[0] + 0x40)));
    // 0x25546c: lw v1, 0x44(v0)
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(
        memory::read<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0] + 0x44));
    // 0x255470: jalr v1
    // 0x255474: _addu a0, s0, a0  — delay slot, BEFORE call
    ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(
        ctx.cpuRegs.GPR.r[16].SL[0] + ctx.cpuRegs.GPR.r[4].SL[0]);
    {
        uint32_t target = ctx.cpuRegs.GPR.r[3].UL[0];
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x00255478;
        auto it = recompiled_functions.find(target);
        if (it != recompiled_functions.end()) {
            it->second(ctx, target);
        } else {
            g_logFile << "[255188] MISSING JALR@255470 target=0x" << std::hex << target << std::endl;
            ctx.cpuRegs.pc = target;
            goto Label_00255498;
        }
    }
    // falls through to caseD_0


    // ================================================================
    // caseD_0: 0x255478
    // ================================================================
Label_caseD_0:
    // 0x255478: lw a2, 0x1c(s0)
    ctx.cpuRegs.GPR.r[6].SD[0] = static_cast<int32_t>(
        memory::read<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x1c));
    // fall through to Label_0025547c


Label_0025547c:
    // 0x25547c: beq a2, zero, LAB_00255498
    // 0x255480: _move a1, s1  — delay slot, ALWAYS executes
    ctx.cpuRegs.GPR.r[5].UD[0] = ctx.cpuRegs.GPR.r[17].UD[0];
    if (ctx.cpuRegs.GPR.r[6].UL[0] == 0) goto Label_00255498;


    // 0x255484: lw v0, 0x34(a2)
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(
        memory::read<uint32_t>(ctx.cpuRegs.GPR.r[6].UL[0] + 0x34));
    // 0x255488: lh a0, 0x60(v0)
    ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(static_cast<int16_t>(
        memory::read<uint16_t>(ctx.cpuRegs.GPR.r[2].UL[0] + 0x60)));
    // 0x25548c: lw v1, 0x64(v0)
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(
        memory::read<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0] + 0x64));
    // 0x255490: jalr v1
    // 0x255494: _addu a0, a2, a0  — delay slot, BEFORE call (NOTE: a2 not s0!)
    ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(
        ctx.cpuRegs.GPR.r[6].SL[0] + ctx.cpuRegs.GPR.r[4].SL[0]);
    {
        uint32_t target = ctx.cpuRegs.GPR.r[3].UL[0];
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x00255498;
        auto it = recompiled_functions.find(target);
        if (it != recompiled_functions.end()) {
            it->second(ctx, target);
        } else {
            g_logFile << "[255188] MISSING JALR@255490 target=0x" << std::hex << target << std::endl;
            ctx.cpuRegs.pc = target;
            goto Label_00255498;
        }
    }
    // falls through to epilogue


    // ================================================================
    // Epilogue: 0x255498
    // ================================================================
Label_00255498:
    // 0x255498: ld s0, 0x10(sp)
    ctx.cpuRegs.GPR.r[16].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x10);
    // 0x25549c: ld s1, 0x18(sp)
    ctx.cpuRegs.GPR.r[17].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x18);
    // 0x2554a0: ld ra, 0x20(sp)
    ctx.cpuRegs.GPR.r[31].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x20);
    // 0x2554a4: lwc1 f20, 0x30(sp)
    ctx.fpuRegs.fpr[20].UL = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x30);
    // 0x2554a8: jr ra
    // 0x2554ac: _addiu sp, sp, 0x40  — delay slot
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


if (func.base_address == 0x00202e28) {
    file << R"code(
void FUN_00202e28(CpuContext& ctx) {
    // ================================================================
    // Prologue: 0x202e28 - 0x202e38
    // ================================================================
    ctx.cpuRegs.GPR.r[29].SL[0] = ctx.cpuRegs.GPR.r[29].SL[0] - 0x10;
    // 0x202e2c: move a2, a0  — save param_1 in a2
    ctx.cpuRegs.GPR.r[6].UD[0] = ctx.cpuRegs.GPR.r[4].UD[0];
    // 0x202e30: sd s0, 0x0(sp)
    memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x0, ctx.cpuRegs.GPR.r[16].UD[0]);
    // 0x202e34: ori v0, zero, 0x8000
    ctx.cpuRegs.GPR.r[2].UD[0] = 0x8000;
    // 0x202e38: sd ra, 0x8(sp)
    memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x8, ctx.cpuRegs.GPR.r[31].UD[0]);


    // ================================================================
    // Setup: 0x202e3c - 0x202e6c
    // ================================================================
    // 0x202e3c: addu v0, a2, v0  — v0 = param_1 + 0x8000
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(
        ctx.cpuRegs.GPR.r[6].SL[0] + ctx.cpuRegs.GPR.r[2].SL[0]);
    // 0x202e40: lw v1, 0x0(a1)  — v1 = *param_2
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(
        memory::read<uint32_t>(ctx.cpuRegs.GPR.r[5].UL[0] + 0x0));
    // 0x202e44: lw a0, 0x144(v0)  — a0 = *(param_1 + 0x8144)
    ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(
        memory::read<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0] + 0x144));
    // 0x202e48: sll v1, v1, 2  — v1 = *param_2 << 2
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(
        static_cast<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0]) << 2);
    // 0x202e4c: addu v1, a2, v1  — v1 = param_1 + *param_2 * 4
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(
        ctx.cpuRegs.GPR.r[6].SL[0] + ctx.cpuRegs.GPR.r[3].SL[0]);
    // 0x202e50: lw v0, 0x4(a0)  — v0 = piVar2[1]
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(
        memory::read<uint32_t>(ctx.cpuRegs.GPR.r[4].UL[0] + 0x4));
    // 0x202e54: lw a1, 0x58(v1)  — a1 = iVar1 = *(param_1 + *param_2 * 4 + 0x58)
    ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(
        memory::read<uint32_t>(ctx.cpuRegs.GPR.r[3].UL[0] + 0x58));
    // 0x202e58: lw v1, 0x1c(a1)  — v1 = *(iVar1 + 0x1c)
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(
        memory::read<uint32_t>(ctx.cpuRegs.GPR.r[5].UL[0] + 0x1c));
    // 0x202e5c: xor v0, a1, v0  — v0 = iVar1 ^ piVar2[1]
    ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[5].UD[0] ^ ctx.cpuRegs.GPR.r[2].UD[0];
    // 0x202e60: andi a0, v1, 0xf  — a0 = switch value
    ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[3].UD[0] & 0xf;
    // 0x202e64: sltiu v1, a0, 9
    // 0x202e68: beq v1, zero, caseD_0  — if a0 >= 9, goto default
    // 0x202e6c: _sltiu a3, v0, 1  — delay slot, ALWAYS: a3 = (v0 == 0) = (iVar1 == piVar2[1])
    ctx.cpuRegs.GPR.r[7].UD[0] = (ctx.cpuRegs.GPR.r[2].UL[0] < 1) ? 1 : 0;
    if (ctx.cpuRegs.GPR.r[4].UL[0] >= 9) goto Label_caseD_0;


    // ================================================================
    // Switch dispatch: 0x202e70 - 0x202e88
    // Cases 0,1,4,5,6,7,8 → caseD_0 (default/return)
    // Case 2 → caseD_2
    // Case 3 → caseD_3
    // ================================================================
    switch (ctx.cpuRegs.GPR.r[4].UL[0]) {
        case 2: goto Label_caseD_2;
        case 3: goto Label_caseD_3;
        default: goto Label_caseD_0;
    }


    // ================================================================
    // caseD_2: 0x202e8c
    // ================================================================
Label_caseD_2:
    // 0x202e8c: bne a3, zero, LAB_202ea8  — if iVar1 == piVar2[1], goto shared cleanup
    // 0x202e90: _ori v0, zero, 0x8000  — delay slot, ALWAYS executes
    ctx.cpuRegs.GPR.r[2].UD[0] = 0x8000;
    if (ctx.cpuRegs.GPR.r[7].UL[0] != 0) goto Label_00202ea8;


    // iVar1 != piVar2[1] path:
    // 0x202e94: jal FUN_00203230
    // 0x202e98: _move a0, a2  — delay slot: a0 = param_1
    // a1 still = iVar1 from 0x202e54
    ctx.cpuRegs.GPR.r[4].UD[0] = ctx.cpuRegs.GPR.r[6].UD[0];
    ctx.cpuRegs.GPR.r[31].UL[0] = 0x00202e9c;
    {
        auto it = recompiled_functions.find(0x203230u);
        if (it != recompiled_functions.end()) {
            it->second(ctx, 0x203230);
        } else {
            ctx.cpuRegs.pc = 0x203230;
            goto Label_00202ee0;
        }
    }
    // 0x202e9c: b LAB_202ee0
    // 0x202ea0: _ld s0, 0x0(sp)  — delay slot: restore s0
    ctx.cpuRegs.GPR.r[16].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x0);
    goto Label_00202ee0;


    // ================================================================
    // caseD_3: 0x202ea4
    // ================================================================
Label_caseD_3:
    // 0x202ea4: ori v0, zero, 0x8000
    ctx.cpuRegs.GPR.r[2].UD[0] = 0x8000;
    // fall through to Label_00202ea8


    // ================================================================
    // Shared cleanup path: 0x202ea8 (from caseD_2 bne and caseD_3 fallthrough)
    // ================================================================
Label_00202ea8:
    // 0x202ea8: addu v0, a2, v0  — v0 = param_1 + 0x8000
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(
        ctx.cpuRegs.GPR.r[6].SL[0] + ctx.cpuRegs.GPR.r[2].SL[0]);
    // 0x202eac: lw s0, 0x144(v0)  — s0 = piVar2 = *(param_1 + 0x8144)
    ctx.cpuRegs.GPR.r[16].SD[0] = static_cast<int32_t>(
        memory::read<uint32_t>(ctx.cpuRegs.GPR.r[2].UL[0] + 0x144));
    // 0x202eb0: lw a0, 0x14(s0)  — a0 = piVar2[5]
    ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(
        memory::read<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x14));
    // 0x202eb4: jal FUN_00203230
    // 0x202eb8: _lw a1, 0x0(s0)  — delay slot: a1 = piVar2[0]
    ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(
        memory::read<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x0));
    ctx.cpuRegs.GPR.r[31].UL[0] = 0x00202ebc;
    {
        auto it = recompiled_functions.find(0x203230u);
        if (it != recompiled_functions.end()) {
            it->second(ctx, 0x203230);
        } else {
            ctx.cpuRegs.pc = 0x203230;
            goto Label_caseD_0;
        }
    }


    // 0x202ebc: lw a0, 0x14(s0)  — a0 = piVar2[5]
    ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(
        memory::read<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x14));
    // 0x202ec0: jal FUN_00203230
    // 0x202ec4: _lw a1, 0x4(s0)  — delay slot: a1 = piVar2[1]
    ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(
        memory::read<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x4));
    ctx.cpuRegs.GPR.r[31].UL[0] = 0x00202ec8;
    {
        auto it = recompiled_functions.find(0x203230u);
        if (it != recompiled_functions.end()) {
            it->second(ctx, 0x203230);
        } else {
            ctx.cpuRegs.pc = 0x203230;
            goto Label_caseD_0;
        }
    }


    // 0x202ec8-0x202ed8: zero out piVar2[0..4]
    memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x10, 0);
    memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x00, 0);
    memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x04, 0);
    memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x08, 0);
    memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x0c, 0);
    // fall through to caseD_0


    // ================================================================
    // caseD_0 / default: 0x202edc
    // ================================================================
Label_caseD_0:
    // 0x202edc: ld s0, 0x0(sp)  — restore s0
    ctx.cpuRegs.GPR.r[16].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x0);
    // fall through to epilogue


    // ================================================================
    // Epilogue: 0x202ee0
    // ================================================================
Label_00202ee0:
    // 0x202ee0: ld ra, 0x8(sp)
    ctx.cpuRegs.GPR.r[31].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x8);
    // 0x202ee4: jr ra
    // 0x202ee8: _addiu sp, sp, 0x10
    ctx.cpuRegs.GPR.r[29].SL[0] = ctx.cpuRegs.GPR.r[29].SL[0] + 0x10;
    return;
}
)code" << std::endl;
    return;
}


/*
)code" << std::endl;


   file << R"code(
*/
if (func.base_address == 0x00255040){
    file << R"code(
    
void FUN_00255040(CpuContext& ctx) {
    // Prologue - save registers
    ctx.cpuRegs.GPR.r[29].SD[0] = static_cast<int32_t>(ctx.cpuRegs.GPR.r[29].SL[0] + (-0x30));
    memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x00, ctx.cpuRegs.GPR.r[16].UD[0]);
    memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x08, ctx.cpuRegs.GPR.r[17].UD[0]);
    ctx.cpuRegs.GPR.r[16].SD[0] = ctx.cpuRegs.GPR.r[4].SD[0];  // s0 = param_1
    memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x10, ctx.cpuRegs.GPR.r[18].UD[0]);
    ctx.cpuRegs.GPR.r[17].SD[0] = ctx.cpuRegs.GPR.r[5].SD[0];  // s1 = param_2
    memory::write<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x18, ctx.cpuRegs.GPR.r[31].UD[0]);
    memory::write<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x20, ctx.fpuRegs.fpr[20].UL);


    // lw v1, 0x0(s0)
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x0));
    // andi v0, v1, 0x100
    ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[3].UD[0] & 0x100;
    // beq v0, zero, 0x255074 — delay slot: move s2, a2
    ctx.cpuRegs.GPR.r[18].SD[0] = ctx.cpuRegs.GPR.r[6].SD[0];  // s2 = param_3
    if (ctx.cpuRegs.GPR.r[2].UL[0] != 0) {
        // srl v1, v1, 0x4
        ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(ctx.cpuRegs.GPR.r[3].UL[0] >> 4);
    }


Label_255074:
    // andi v1, v1, 0xf
    ctx.cpuRegs.GPR.r[3].UD[0] = ctx.cpuRegs.GPR.r[3].UD[0] & 0xf;


    {
        uint32_t switch_val = ctx.cpuRegs.GPR.r[3].UL[0];
        if (switch_val >= 5) {
            goto Label_caseD_0;
        }
        switch (switch_val) {
            case 0: goto Label_caseD_0;
            case 1: goto Label_case1;
            case 2: goto Label_case2;
            case 3: goto Label_case3;
            case 4: goto Label_case4;
        }
    }


Label_case1: // 0x25509c
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x0));
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(-0xf1);
    memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x10, ctx.cpuRegs.GPR.r[17].UL[0]);
    ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] & ctx.cpuRegs.GPR.r[3].UD[0];
    memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x14, ctx.cpuRegs.GPR.r[18].UL[0]);
    goto Label_25514c;


Label_case2: // 0x2550b4
    memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x10, ctx.cpuRegs.GPR.r[17].UL[0]);
    ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(ctx.cpuRegs.GPR.r[16].SL[0] + 0x10);
    // lwc1 f20, 0x18(s0) — delay slot of jal
    ctx.fpuRegs.fpr[20].UL = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x18);
    // jal FUN_00259908
    ctx.cpuRegs.GPR.r[31].UL[0] = 0x2550c4;
    if (recompiled_functions.count(0x259908)) {
        recompiled_functions[0x259908](ctx, 0x259908);
    } else {
        ctx.cpuRegs.pc = 0x259908;
    }
    // mul.S f0, f0, f20
    ctx.fpuRegs.fpr[0].f = ctx.fpuRegs.fpr[0].f * ctx.fpuRegs.fpr[20].f;
    // lwc1 f1, DAT_003097dc
    ctx.fpuRegs.fpr[1].UL = memory::read<uint32_t>(0x003097dc);
    // lw v0, 0x0(s0)
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x0));
    // li a0, 0x4
    ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(0x4);
    // li v1, 0x1
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(0x1);
    // li a1, -0xf1
    ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(-0xf1);
    // movn v1, a0, s1
    if (ctx.cpuRegs.GPR.r[17].UD[0] != 0) {
        ctx.cpuRegs.GPR.r[3].UD[0] = ctx.cpuRegs.GPR.r[4].UD[0];
    }
    // mul.S f0, f0, f1
    ctx.fpuRegs.fpr[0].f = ctx.fpuRegs.fpr[0].f * ctx.fpuRegs.fpr[1].f;
    // sll v1, v1, 0x4
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(ctx.cpuRegs.GPR.r[3].UL[0] << 4);
    // and v0, v0, a1
    ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] & ctx.cpuRegs.GPR.r[5].UD[0];
    // or v0, v0, v1
    ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] | ctx.cpuRegs.GPR.r[3].UD[0];
    // sw s2, 0x14(s0)
    memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x14, ctx.cpuRegs.GPR.r[18].UL[0]);
    // cvt.w.S f1, f0
    ctx.fpuRegs.fpr[1].SL = static_cast<int32_t>(ctx.fpuRegs.fpr[0].f);
    // swc1 f1, 0x10(s0)
    memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x10, ctx.fpuRegs.fpr[1].UL);
    // ori v0, v0, 0x100
    ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] | 0x100;
    goto Label_255150;
)code" << std::endl;


   file << R"code(
Label_case3: // 0x255108
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x0));
    ctx.cpuRegs.GPR.r[5].SD[0] = static_cast<int32_t>(0x4);
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(0x1);
    ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(-0xf1);
    // movn v1, a1, s1
    if (ctx.cpuRegs.GPR.r[17].UD[0] != 0) {
        ctx.cpuRegs.GPR.r[3].UD[0] = ctx.cpuRegs.GPR.r[5].UD[0];
    }
    ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] & ctx.cpuRegs.GPR.r[4].UD[0];
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(ctx.cpuRegs.GPR.r[3].UL[0] << 4);
    memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x10, ctx.cpuRegs.GPR.r[17].UL[0]);
    ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] | ctx.cpuRegs.GPR.r[3].UD[0];
    memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x14, ctx.cpuRegs.GPR.r[18].UL[0]);
    ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] | 0x100;
    goto Label_255150;


Label_case4: // 0x255138
    // bnel s1, zero, LAB_00255158 — likely branch
    if (ctx.cpuRegs.GPR.r[17].UL[0] != 0) {
        ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x1c));
        goto Label_255158;
    }
    ctx.cpuRegs.GPR.r[2].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x0));
    ctx.cpuRegs.GPR.r[3].SD[0] = static_cast<int32_t>(-0xf1);
    ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] & ctx.cpuRegs.GPR.r[3].UD[0];


Label_25514c: // 0x25514c
    ctx.cpuRegs.GPR.r[2].UD[0] = ctx.cpuRegs.GPR.r[2].UD[0] | 0x110;


Label_255150: // 0x255150
    memory::write<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x0, ctx.cpuRegs.GPR.r[2].UL[0]);


Label_caseD_0: // 0x255154
    ctx.cpuRegs.GPR.r[4].SD[0] = static_cast<int32_t>(memory::read<uint32_t>(ctx.cpuRegs.GPR.r[16].UL[0] + 0x1c));
)code" << std::endl;


   file << R"code(
Label_255158: // 0x255158
    ctx.cpuRegs.GPR.r[5].SD[0] = ctx.cpuRegs.GPR.r[17].SD[0];
    if (ctx.cpuRegs.GPR.r[4].UL[0] != 0) {
        ctx.cpuRegs.GPR.r[6].SD[0] = ctx.cpuRegs.GPR.r[18].SD[0];
        ctx.cpuRegs.GPR.r[31].UL[0] = 0x255168;
        if (recompiled_functions.count(0x255040)) {
            recompiled_functions[0x255040](ctx, 0x255040);
        } else {
            ctx.cpuRegs.pc = 0x255040;
        }
    }


    // Epilogue
    ctx.cpuRegs.GPR.r[16].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x00);
    ctx.cpuRegs.GPR.r[17].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x08);
    ctx.cpuRegs.GPR.r[18].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x10);
    ctx.cpuRegs.GPR.r[31].UD[0] = memory::read<uint64_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x18);
    ctx.fpuRegs.fpr[20].UL = memory::read<uint32_t>(ctx.cpuRegs.GPR.r[29].UL[0] + 0x20);
    ctx.cpuRegs.GPR.r[29].SD[0] = static_cast<int32_t>(ctx.cpuRegs.GPR.r[29].SL[0] + 0x30);
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












