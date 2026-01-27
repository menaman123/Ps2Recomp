#include "gs_hle.h"
#include "memory.h"
#include "cpu_state.h"
#include <glm/glm.hpp>
#include "render.h"
#include <iostream>
#include <iomanip>
#include <fstream>
#include <cstring>
#include <SDL_timer.h>


SDL_Window* g_window = nullptr;
SDL_Renderer* g_renderer = nullptr;

extern std::atomic<bool> g_window_resize_pending;
extern std::atomic<int>  g_new_window_width;
extern std::atomic<int>  g_new_window_height;
extern std::ofstream g_logFile;

struct Ps2RenderNode {
    uint32_t matrixPtr;     // Offset 0x0C
    uint32_t meshPtr;       // Offset 0x18
    uint32_t visibility1;   // Offset 0x20
    uint32_t visibility2;   // Offset 0x24
    uint32_t nextNodePtr;   // Offset 0x2C

    static Ps2RenderNode Load(uint32_t addr) {
        Ps2RenderNode node;
        node.matrixPtr   = memory::read<uint32_t>(addr + 0x0C);
        node.meshPtr     = memory::read<uint32_t>(addr + 0x18);
        node.visibility1 = memory::read<uint32_t>(addr + 0x20);
        node.visibility2 = memory::read<uint32_t>(addr + 0x24);
        node.nextNodePtr = memory::read<uint32_t>(addr + 0x2C);
        return node;
    }
};
/*

void hle_RenderLoop(CpuContext& ctx) { // 0x002aa8d0
    // 1. Read Arguments from Registers
    // a0 = param_1 (List Head Pointer)
    // a1 = param_2 (Mode?)
    // a2 = param_3 (Result Pointer)
    uint32_t listHeadPtr = ctx.cpuRegs.GPR.r[4].UL[0]; 
    uint32_t param_2     = ctx.cpuRegs.GPR.r[5].UL[0];
    uint32_t resultPtr   = ctx.cpuRegs.GPR.r[6].UL[0];

    // 2. Read List Head Data
    // "uVar1 = *param_1"
    uint32_t flags = memory::read<uint32_t>(listHeadPtr);
    uint32_t type  = (flags >> 24) & 0xF; 

    // "uVar2 = param_1[6]" -> Start of Linked List
    uint32_t currentNodePtr = memory::read<uint32_t>(listHeadPtr + 0x18);

    RenderBatch currentBatch;
    currentBatch.renderType = type;
    bool hasWork = false;

    // 3. Iterate the Linked List
    while (currentNodePtr != 0) {
        Ps2RenderNode node = Ps2RenderNode::Load(currentNodePtr);

        // Check Type (Logic from disassembly: "else if (type == 5)")
        if (type == 5) { 
            if (node.matrixPtr != 0 && node.meshPtr != 0) {
                // READ MATRIX (16 floats / 64 bytes)
                glm::mat4 mat;
                uint8_t* ramPtr = memory::translate_address(node.matrixPtr, 64);
                if (ramPtr) {
                    memcpy(&mat, ramPtr, 64);
                    
                    // Add to batch
                    currentBatch.meshAddress = node.meshPtr;
                    currentBatch.instances.push_back(mat);
                    hasWork = true;
                }
            }
        }

        // Move to next node
        currentNodePtr = node.nextNodePtr;
    }

    // 4. Send to Graphics Thread
    if (hasWork) {
        RenderJob job;
        job.type = RenderCommandType::DrawBatch;
        job.batch = currentBatch;
        g_renderQueue.Push(job);
    }

    // 5. Side Effects (Update Game State)
    // "param_1[4] = param_1[4] + 1" (Increment processed count)
    uint32_t countAddr = listHeadPtr + 0x10;
    uint32_t count = memory::read<uint32_t>(countAddr);
    memory::write<uint32_t>(countAddr, count + 1);

    // "*param_3 = result" (Write success flag)
    memory::write<uint8_t>(resultPtr, hasWork ? 1 : 0);

    // Return value v0
    ctx.cpuRegs.GPR.r[2].UL[0] = hasWork ? 1 : 0;
}
*/


void hle_InitGraphics(CpuContext& ctx) {
    g_logFile << "HLE: Init_graphics_system (0x0019fb18) called" << std::endl;

    // 1. Read Parameters
    uint32_t game_system_ptr = ctx.cpuRegs.GPR.r[4].UL[0]; // a0
    uint16_t width_param     = (uint16_t)ctx.cpuRegs.GPR.r[5].UL[0]; // a1
    uint16_t height_param    = (uint16_t)ctx.cpuRegs.GPR.r[6].UL[0]; // a2
    // uint32_t interlace    = ctx.cpuRegs.GPR.r[7].UL[0]; // a3 (unused for HLE setup)

    // 2. Persist Game State (Mimic Side Effects from Assembly)
    
    // [CRITICAL] Initialize the GameSystem struct member at offset 0x1C
    // The original code sets this to a static address (0x002f6498).
    // If we don't set this, the game might crash accessing a null pointer later.
    memory::write<uint32_t>(game_system_ptr + 0x1C, 0x002f6498);

    // [CRITICAL] Set Global Configuration variables
    memory::write<uint16_t>(0x0030a788, width_param);
    memory::write<uint16_t>(0x0030a78a, height_param);
    
    // Set the "stride" or internal width found in disassembly (li param_2, 0x200)
    memory::write<uint16_t>(0x0030a792, 0x200);

    // 3. Request Window Creation via Main Thread (Thread Safe)
    // Do NOT call SDL_CreateWindow here directly.
    g_logFile << "HLE: Requesting Window " << width_param << "x" << height_param << std::endl;
    
    g_new_window_width = 640; // Force 640 for consistency, or use width_param
    g_new_window_height = 448;
    g_window_resize_pending = true;

    // Optional: Block until the window is actually ready
    // This prevents the game from sending draw commands before the window exists.
    // while (g_window_resize_pending) { std::this_thread::yield(); }

    // 4. Return the GameSystem pointer (v0 = a0)
    ctx.cpuRegs.GPR.r[2].UL[0] = game_system_ptr;
}