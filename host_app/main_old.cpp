#include <SDL.h>
#include "cpu_state.h"
#include "syscalls.h"
#include "memory.h"
#include <iostream>
#include <iomanip>
#include <fstream>
#include <filesystem>
#include <cstring>
#include "recompiled.h"
#include "sema.h"
#include "intc.h"
#include "ps2_scheduler.h"
#include "sif.h"




// Global log file stream to be used by the host and recompiled functions
std::ofstream g_logFile;
std::map<int, std::vector<IntcHandler>> g_dmac_queues;

// Initialize Semaphore
std::vector<HostSemaphore> g_semaphores(256);

void init_cpu_context(CpuContext& ctx) {
    memset(&ctx, 0, sizeof(CpuContext));
    
    // Set the entry point (you'll need to get this from the ELF)
    ctx.cpuRegs.pc = 0x100008; // Example starting PC
    
    // Initialize stack pointer to end of main RAM
    // PS2 typically uses the top of main RAM for the stack
    ctx.cpuRegs.GPR.r[29].UD[0] = 0x01FFFF00; // Near end of 32MB main RAM
    
    // Alternative: Use scratchpad for stack (some games do this)
    // ctx.cpuRegs.GPR.r[29].UD[0] = 0x70003FF0; // Near end of scratchpad
    
    // Initialize other important registers
    ctx.cpuRegs.GPR.r[28].UD[0] = 0; // Global pointer (gp)
    ctx.cpuRegs.GPR.r[30].UD[0] = 0; // Frame pointer (fp)
}

// A simple execution loop
void execute_recompiled_code(CpuContext& ctx) {
    while (true) {
        // First try exact function start match
        entry(ctx);
        if (ctx.cpuRegs.pc == 0) {
            break;
        }
    }
}

int main(int argc, char* argv[]) {
    // Create a "logs" directory if it doesn't already exist
    if (!std::filesystem::exists("logs")) {
        std::filesystem::create_directory("logs");
    }

    // Open the log file for writing
    g_logFile.open("logs/runtime_log.txt");
    if (!g_logFile.is_open()) {
        std::cerr << "Fatal Error: Could not open log file for writing." << std::endl;
        return 1;
    }

    if (SDL_Init(SDL_INIT_TIMER) < 0) {
        std::cerr << "SDL could not initialize! SDL_Error: " << SDL_GetError() << std::endl;
        return 1;
    }

    std::cout << "Starting the host application... Logging to logs/runtime_log.txt" << std::endl;
    g_logFile << "Starting the host application..." << std::endl;
    // Initialize memory
    memory::initialize();

    memset(&g_sif, 0, sizeof(g_sif));

    g_sif.smflag = 0x20000; // bypass check at 2d1c60

    memory::write<uint32_t>(0x1000F230, g_sif.smflag);

    g_sif.sys_rpcinit = 1;


    // Initialize the CPU state.
    CpuContext ctx;
    init_cpu_context(ctx);

    // This map is defined in recompiled_functions.cpp and holds pointers
    // to all your recompiled functions.

    std::cout << "Executing recompiled code..." << std::endl;
    g_logFile << "Executing recompiled code..." << std::endl;
    execute_recompiled_code(ctx);

    std::cout << "Host application finished." << std::endl;
    g_logFile << "Host application finished." << std::endl;
    
    g_logFile.close();
    SDL_Quit();
    return 0;
}