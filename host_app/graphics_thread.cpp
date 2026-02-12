#include "ps2_scheduler.h"
#include "render.h"
#include <iostream>
#include <iomanip>
#include <fstream>
#include <SDL.h> 
#include "gs.h"
#include "intc.h"

extern RenderQueue g_renderQueue;

extern SDL_Renderer* g_renderer; 
extern std::ofstream g_logFile; 

void GraphicsThreadFunc() {
    g_logFile << "[GFX] Graphics Thread Started." << std::endl;
    
    RenderJob job;
    
    // Internal State (Persists between jobs)
    uint8_t clearR = 0, clearG = 0, clearB = 0;
    int windowW = 640, windowH = 448;

    while (true) {
        // Wait for job
        if (!g_renderQueue.Pop(job)) {
            break;
        }

        switch (job.type) {
            // ================================================================
            // 1. VSYNC / PRESENT
            // ================================================================
            case RenderCommandType::VSync:
                //g_logFile << "\n[GFX] VSync Command Received. Presenting Frame." << std::endl;
                SDL_RenderPresent(g_renderer);
                
                // 1. Physical Signal: Update GS Status register
                g_gs_regs.GS_CSR |= (1 << 3); 

                // 2. Interrupt Signal: Raise VBON (Bit 2)
                // This allows FUN_002cf800 to see (stat & 4) != 0 and exit its loop.
                g_intc.RaiseInterrupt(INTC_VBON); 

                // 3. Optional: Trigger VBOF (Bit 3) shortly after if needed
                break;

            // ================================================================
            // 2. STATE MANAGEMENT (The missing pieces)
            // ================================================================
            case RenderCommandType::SetClearColor:
                /*
                                g_logFile << "[GFX] Set Clear Color: R=" 
                           << (int)job.args.arg1 << " G=" 
                           << (int)job.args.arg2 << " B=" 
                           << (int)job.args.arg3 << std::endl;
                */

                clearR = (uint8_t)job.args.arg1;
                clearG = (uint8_t)job.args.arg2;
                clearB = (uint8_t)job.args.arg3;
                break;

            case RenderCommandType::SetWindow:
            {
                /*
                                g_logFile << "[GFX] Set Window Size Command: " 
                           << job.args.arg2 << "x" << job.args.arg3 << std::endl;
                */

                // arg2 = Width, arg3 = Height
                int newW = job.args.arg2;
                int newH = job.args.arg3;
                
                if (newW != windowW || newH != windowH) {
                    windowW = newW;
                    windowH = newH;
                    SDL_Window* win = SDL_RenderGetWindow(g_renderer);
                    SDL_SetWindowSize(win, windowW, windowH); // Temporary small size to avoid artifacts
                    // Center the window again if size changed drastically
                    SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
                    std::cout << "[GFX] Resizing Window to " << windowW << "x" << windowH << std::endl;
                }
                break;
            }

            case RenderCommandType::SetFrontBuffer:
                /*
                                g_logFile << "[GFX] Set Front Buffer Command: Addr=0x" 
                           << std::hex << job.args.arg1 << std::dec 
                           << " Width=" << job.args.arg2 
                           << " PSM=" << job.args.arg3 << std::endl;
                */

                // For a software rasterizer, we might just log this.
                // In OpenGL, you'd swap textures here.
                // arg1=Addr, arg2=Width, arg3=PSM
                break;

            // ================================================================
            // 3. DRAWING (Your logic)
            // ================================================================
            case RenderCommandType::DrawBatch:
            {   /*
                                g_logFile << "[GFX] Draw Batch Command: " 
                           << job.batch.vertices.size() << " vertices of type " 
                           << (int)job.batch.prim_type << std::endl;
                */

                if (!g_renderer) break;
                
                const auto& batch = job.batch;
                size_t vCount = batch.vertices.size();
                if (vCount == 0) break;

                // 
                // The PS2 often maps (0,0) to (2048, 2048) in the 4096 coordinate space.
                // Your heuristic handles this well for now.

                for (size_t i = 0; i < vCount; i++) {
                    const auto& v = batch.vertices[i];
                    
                    // --- Color Handling ---
                    uint8_t r = v.r;
                    uint8_t g = v.g;
                    uint8_t b = v.b;
                    
                    // Debug: Boost invisible dark colors
                    if (r==0 && g==0 && b > 0 && b < 20) { 
                        r=50; g=50; b=255; // Make it bright blue so you definitely see it
                    } 
                    
                    SDL_SetRenderDrawColor(g_renderer, r, g, b, 255);

                    // --- Coordinate Handling ---
                    float x = v.x;
                    float y = v.y;
                    
                    if (x > 1000.0f) x -= 2048.0f;
                    if (y > 1000.0f) y -= 2048.0f;

                    // Clamp to screen logic if needed (optional)

                    switch (batch.prim_type) {
                        case GSPrimType::Point:
                            SDL_RenderDrawPoint(g_renderer, (int)x, (int)y);
                            break;

                        case GSPrimType::Line:
                        case GSPrimType::LineStrip:
                            if (i + 1 < vCount) {
                                const auto& v2 = batch.vertices[i+1];
                                float x2 = v2.x > 1000.0f ? v2.x - 2048.0f : v2.x;
                                float y2 = v2.y > 1000.0f ? v2.y - 2048.0f : v2.y;
                                
                                SDL_RenderDrawLine(g_renderer, (int)x, (int)y, (int)x2, (int)y2);
                                if (batch.prim_type == GSPrimType::Line) i++; 
                            }
                            break;

                        case GSPrimType::Sprite:
                            if (i + 1 < vCount) {
                                const auto& v2 = batch.vertices[i+1];
                                float x2 = v2.x > 1000.0f ? v2.x - 2048.0f : v2.x;
                                float y2 = v2.y > 1000.0f ? v2.y - 2048.0f : v2.y;

                                SDL_Rect rect;
                                rect.x = (int)std::min(x, x2);
                                rect.y = (int)std::min(y, y2);
                                rect.w = (int)std::abs(x2 - x);
                                rect.h = (int)std::abs(y2 - y);
                                
                                // Ensure visibility for debug
                                if(rect.w == 0) rect.w = 2; // Make it slightly fatter
                                if(rect.h == 0) rect.h = 2;

                                SDL_RenderFillRect(g_renderer, &rect);
                                i++; 
                            }
                            break;
                            
                        case GSPrimType::Triangle:
                        case GSPrimType::TriangleStrip:
                        case GSPrimType::TriangleFan:
                            if (i + 2 < vCount) {
                                const auto& v2 = batch.vertices[i+1];
                                const auto& v3 = batch.vertices[i+2];
                                
                                float x2 = v2.x > 1000.0f ? v2.x - 2048.0f : v2.x;
                                float y2 = v2.y > 1000.0f ? v2.y - 2048.0f : v2.y;
                                float x3 = v3.x > 1000.0f ? v3.x - 2048.0f : v3.x;
                                float y3 = v3.y > 1000.0f ? v3.y - 2048.0f : v3.y;

                                // Wireframe Triangle
                                SDL_RenderDrawLine(g_renderer, (int)x, (int)y, (int)x2, (int)y2);
                                SDL_RenderDrawLine(g_renderer, (int)x2, (int)y2, (int)x3, (int)y3);
                                SDL_RenderDrawLine(g_renderer, (int)x3, (int)y3, (int)x, (int)y);
                                
                                if (batch.prim_type == GSPrimType::Triangle) i += 2;
                            }
                            break;
                    }
                }
                break;
            } // End DrawBatch
        }
    }
}