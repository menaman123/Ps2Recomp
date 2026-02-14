#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <SDL.h>
#include "cpu_state.h"
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
#include <thread>
#include <atomic>
#include <chrono>
#include "gs_hle.h"
#include "sif_hle.h"
#include "render.h"
#include <cstdio>


// Global log file stream
std::ofstream g_logFile;
std::map<int, std::vector<IntcHandler>> g_dmac_queues;
void GraphicsThreadFunc();
RenderQueue g_renderQueue;

FILE* g_isoFile = nullptr;

// Initialize Semaphore
std::vector<HostSemaphore> g_semaphores(256);

// --- Synchronization Globals (Bridge between CPU and Main Thread) ---
std::atomic<bool> g_emulator_running{true};
std::atomic<bool> g_window_resize_pending{false};
std::atomic<int>  g_new_window_width{640};
std::atomic<int>  g_new_window_height{448};
std::atomic<bool> g_vsync_pending{false}; // Trigger for SDL_RenderPresent

// External references to Graphics handles (defined in syscalls.cpp)
extern SDL_Window* g_window;
extern SDL_Renderer* g_renderer;
CpuContext* g_cpuContext = nullptr;

void init_cpu_context(CpuContext& ctx) {
    memset(&ctx, 0, sizeof(CpuContext));
    
    // Entry Point (Example: standard PS2 LoadExecPS2 usually jumps to this range)
    // You might need to parse the ELF header to get the real entry point
    ctx.cpuRegs.pc = 0x00100008; // Typical CRT0 entry for many games
    
    // Stack Pointer (End of 32MB Main RAM)
    ctx.cpuRegs.GPR.r[29].UD[0] = 0x01FFFF00; 
}

// -----------------------------------------------------------------------
// CPU THREAD: Runs the recompiled game code
// -----------------------------------------------------------------------
void CpuThreadFunc(CpuContext* ctx) {
    g_logFile << "[CPU] Thread Started." << std::endl;

    // This loop drives the CPU execution. 
    // In a static recompiler, you typically jump to the entry point 
    // and let the blocks chain themselves, but you need a dispatcher 
    // for indirect jumps (jalr) or interrupts.
    #ifdef _WIN32
        g_scheduler.scheduler_fiber_ = ConvertThreadToFiber(nullptr);
        if (g_scheduler.scheduler_fiber_ == nullptr) {
            g_logFile << "Failed to convert thread to fiber: " << GetLastError() << std::endl;
            exit(1);
        }
    #endif

        PS2Thread& mainThread = g_scheduler.threads_[1];
        mainThread.active = true;
        mainThread.id = 1;
        mainThread.status = THS_RUN;
        mainThread.ctx = *ctx;
        mainThread.current_priority = 0;
        mainThread.init_priority = 0;
        mainThread.entry_func = ctx->cpuRegs.pc;


    
    #ifdef _WIN32
        mainThread.fiber = CreateFiber(2 * 1024 * 1024, PS2Scheduler::FiberEntry, reinterpret_cast<void*>(1));
    #endif
        mainThread.fiber_created = true;
        g_scheduler.current_thread_id_ = 0;
        g_scheduler.AddToReadyQueue(1);
        g_logFile << "[CPU] Main thread initialized and fiber created. Entering scheduler loop..." << std::endl;
        g_scheduler.RunSchedulerLoop();

}

// -----------------------------------------------------------------------
// MAIN THREAD: Handles Window, Events, and Rendering
// -----------------------------------------------------------------------
int main(int argc, char* argv[]) {
    // 1. Setup Logging
    if (!std::filesystem::exists("logs")) {
        std::filesystem::create_directory("logs");
    }
    g_logFile.open("logs/runtime_log.txt");
    if (!g_logFile.is_open()) {
        std::cerr << "Fatal Error: Could not open log file." << std::endl;
        return 1;
    }

    // 2. Initialize SDL (Video is mandatory for Windowing)
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) < 0) {
        std::cerr << "SDL Error: " << SDL_GetError() << std::endl;
        return 1;
    }

    std::cout << "Starting Host Application..." << std::endl;
    g_logFile << "Starting Host Application..." << std::endl;

    // 3. Initialize Subsystems
    memory::initialize();


    const char* isoPath = "C:\\Users\\Owner\\Desktop\\PS2_Recomp\\Crash_TwinSanity.rep\\Crash Twinsanity.iso";
    g_isoFile = fopen(isoPath, "rb");
    
    if (g_isoFile) {
        std::cout << "ISO Loaded Successfully: " << isoPath << std::endl;
        g_logFile << "ISO Loaded Successfully: " << isoPath << std::endl;
    } else {
        std::cerr << "Fatal Error: Could not open ISO file at: " << isoPath << std::endl;
        g_logFile << "Fatal Error: Could not open ISO file at: " << isoPath << std::endl;
        return 1; // Exit if we can't load the game data
    }

    sif_bind_rpc::InitBindingTracker();
    
    // Initialize SIF state
    memset(&g_sif, 0, sizeof(g_sif));
    g_sif.smflag = 0x20000; // SIF_STAT_CMDINIT
    memory::write<uint32_t>(0x1000F230, g_sif.smflag); // Write to hardware register
    g_sif.sys_rpcinit = 1;

    // Initialize CPU Context
    CpuContext ctx = g_cpuContext ? *g_cpuContext : CpuContext{};
    if (!g_cpuContext) g_cpuContext = &ctx;
    
    init_cpu_context(ctx);
    
    // Initialize Recompiled Functions map (Important!)
    // This function should be generated by your recompiler to populate the map
    initialize_recompiled_functions(); 

    // 4. Create Initial Hidden Window
    // We create it hidden; hle_InitGraphics will resize and show it later.
    g_window = SDL_CreateWindow(
        "PS2 HLE", 
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 
        640, 448, 
        SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE
    );
    
    if (!g_window) {
        g_logFile << "Failed to create SDL Window: " << SDL_GetError() << std::endl;
        return 1;
    }
    
    g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED);

    // 5. Start the CPU Emulation Thread
    std::thread cpu_thread(CpuThreadFunc, &ctx);
    std::thread gfx_thread(GraphicsThreadFunc);

    // 6. Main Event Loop
    while (g_emulator_running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                g_emulator_running = false;
            }
        }

        // --- HLE REQUEST HANDLING ---

        // Check for Window Resize Request (from hle_InitGraphics)
        if (g_window_resize_pending) {
            SDL_SetWindowSize(g_window, g_new_window_width, g_new_window_height);
            SDL_ShowWindow(g_window); // Make sure it's visible now
            
            // Clear flag
            g_window_resize_pending = false;
            g_logFile << "[MAIN] Window resized and shown." << std::endl;
        }

        // Check for VSync/Present Request (from hle_WaitForVblank)
        // Note: You need to update hle_WaitForVblank to set this flag!
        if (g_vsync_pending) {
            if (g_renderer) {
                SDL_RenderPresent(g_renderer);
            }
            g_vsync_pending = false;
        }

        // Sleep briefly to yield the main thread (60 FPS cap approx)
        SDL_Delay(1);
    }

    // 7. Cleanup
    g_emulator_running = false;
    g_renderQueue.Stop(); // [NEW] Signal graphics thread to stop
    if (cpu_thread.joinable()) {
        cpu_thread.join();
    }
    if (gfx_thread.joinable()) {
        gfx_thread.join();
    }

    if (g_isoFile) {
        fclose(g_isoFile);
        g_isoFile = nullptr;
        g_logFile << "ISO File Closed." << std::endl;
    }

    if (g_renderer) SDL_DestroyRenderer(g_renderer);
    if (g_window) SDL_DestroyWindow(g_window);
    SDL_Quit();
    
    g_logFile << "Host Application Exited." << std::endl;
    return 0;
}