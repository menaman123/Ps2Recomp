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
    
    uint8_t clearR = 0, clearG = 0, clearB = 0;
    int windowW = 640, windowH = 448;

    // Initial clear so first frame isn't garbage
    if (g_renderer) {
        SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
        SDL_RenderClear(g_renderer);
    }

    while (true) {
        if (!g_renderQueue.Pop(job)) {
            break;
        }

        switch (job.type) {
            case RenderCommandType::VSync:
                SDL_RenderPresent(g_renderer);
                
                g_gs_regs.GS_CSR |= (1 << 3); 
                g_intc.RaiseInterrupt(INTC_VBON); 

                // Clear for next frame
                SDL_SetRenderDrawColor(g_renderer, clearR, clearG, clearB, 255);
                SDL_RenderClear(g_renderer);
                break;

            case RenderCommandType::SetClearColor:
                clearR = (uint8_t)job.args.arg1;
                clearG = (uint8_t)job.args.arg2;
                clearB = (uint8_t)job.args.arg3;
                break;

            case RenderCommandType::SetWindow:
            {
                int newW = job.args.arg2;
                int newH = job.args.arg3;
                
                if (newW != windowW || newH != windowH) {
                    windowW = newW;
                    windowH = newH;
                    SDL_Window* win = SDL_RenderGetWindow(g_renderer);
                    SDL_SetWindowSize(win, windowW, windowH);
                    SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
                    std::cout << "[GFX] Resizing Window to " << windowW << "x" << windowH << std::endl;
                }
                break;
            }

            case RenderCommandType::SetFrontBuffer:
                break;

            case RenderCommandType::DrawBatch:
            {
                if (!g_renderer) break;
                
                const auto& batch = job.batch;
                size_t vCount = batch.vertices.size();
                if (vCount < 3) break;

                // Build SDL_Vertex array — all primitives are pre-decomposed 
                // into individual triangles by FlushPrimitive()
                std::vector<SDL_Vertex> sdl_verts;
                sdl_verts.reserve(vCount);

                for (size_t i = 0; i < vCount; i++) {
                    const auto& v = batch.vertices[i];
                    SDL_Vertex sv;
                    
                    float x = v.x;
                    float y = v.y;
                    if (x > 1000.0f) x -= 2048.0f;
                    if (y > 1000.0f) y -= 2048.0f;

                    sv.position.x = x;
                    sv.position.y = y;

                    sv.color.r = v.r;
                    sv.color.g = v.g;
                    sv.color.b = v.b;
                    // PS2 alpha range is 0-128 (128=opaque), SDL expects 0-255
                    sv.color.a = (uint8_t)std::min((int)v.a * 2, 255);

                    sv.tex_coord.x = 0.0f;
                    sv.tex_coord.y = 0.0f;

                    sdl_verts.push_back(sv);
                }

                // Ensure vertex count is multiple of 3 (triangle list)
                size_t tri_verts = (sdl_verts.size() / 3) * 3;
                
                if (tri_verts >= 3) {
                    SDL_RenderGeometry(g_renderer,
                                      nullptr,              // no texture
                                      sdl_verts.data(),
                                      (int)tri_verts,
                                      nullptr,              // no indices
                                      0);
                }

                break;
            }
        }
    }
}

