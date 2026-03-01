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


               const auto& ti = batch.tex_info;
               int tex_w = ti.enabled ? (1 << ti.tw) : 0;
               int tex_h = ti.enabled ? (1 << ti.th) : 0;


               // RC3 FIX: Create SDL_Texture from VRAM snapshot when texturing is enabled
               SDL_Texture* texture = nullptr;
               bool texture_created = false;


               if (ti.enabled && !ti.vram_snapshot.empty() && tex_w > 0 && tex_h > 0) {
                   // RC6 guard: Reject Z-buffer PSM formats as texture source
                   if (ti.psm >= 0x30 && ti.psm <= 0x3A) {
                       g_logFile << "[PSM-FIX] Z-buffer PSM=0x" << std::hex << ti.psm
                                 << std::dec << " rejected as texture, rendering with vertex colors"
                                 << std::endl;
                   } else {
                       // Decode VRAM snapshot into RGBA8888 pixel buffer
                       std::vector<uint8_t> rgba(tex_w * tex_h * 4, 0);
                       size_t non_zero_pixels = 0;
                       const uint8_t* src = ti.vram_snapshot.data();
                       size_t src_size = ti.vram_snapshot.size();


                       switch (ti.psm) {
                           case 0x00: // PSMCT32 — 32bpp RGBA
                           {
                               for (int py = 0; py < tex_h; py++) {
                                   for (int px = 0; px < tex_w; px++) {
                                       size_t src_off = ((size_t)py * tex_w + px) * 4;
                                       size_t dst_off = ((size_t)py * tex_w + px) * 4;
                                       if (src_off + 3 < src_size) {
                                           rgba[dst_off + 0] = src[src_off + 0]; // R
                                           rgba[dst_off + 1] = src[src_off + 1]; // G
                                           rgba[dst_off + 2] = src[src_off + 2]; // B
                                           rgba[dst_off + 3] = src[src_off + 3]; // A
                                           if (src[src_off] | src[src_off+1] | src[src_off+2] | src[src_off+3])
                                               non_zero_pixels++;
                                       }
                                   }
                               }
                               break;
                           }
                           case 0x02: // PSMCT16 — 16bpp (1-5-5-5 ABGR)
                           {
                               for (int py = 0; py < tex_h; py++) {
                                   for (int px = 0; px < tex_w; px++) {
                                       size_t src_off = ((size_t)py * tex_w + px) * 2;
                                       size_t dst_off = ((size_t)py * tex_w + px) * 4;
                                       if (src_off + 1 < src_size) {
                                           uint16_t pixel = src[src_off] | (src[src_off + 1] << 8);
                                           rgba[dst_off + 0] = (uint8_t)(((pixel >>  0) & 0x1F) << 3); // R
                                           rgba[dst_off + 1] = (uint8_t)(((pixel >>  5) & 0x1F) << 3); // G
                                           rgba[dst_off + 2] = (uint8_t)(((pixel >> 10) & 0x1F) << 3); // B
                                           rgba[dst_off + 3] = (pixel & 0x8000) ? 0xFF : 0x00;         // A
                                           if (pixel) non_zero_pixels++;
                                       }
                                   }
                               }
                               break;
                           }
                           case 0x13: // PSMT8 — 8-bit indexed (CLUT lookup)
                           {
                               const uint8_t* clut = ti.clut_snapshot.data();
                               size_t clut_size = ti.clut_snapshot.size();
                               bool clut32 = (ti.cpsm == 0x00); // PSMCT32 CLUT
                               for (int py = 0; py < tex_h; py++) {
                                   for (int px = 0; px < tex_w; px++) {
                                       size_t src_off = (size_t)py * tex_w + px;
                                       size_t dst_off = ((size_t)py * tex_w + px) * 4;
                                       if (src_off < src_size) {
                                           uint8_t idx = src[src_off];
                                           if (clut32) {
                                               size_t c_off = (size_t)idx * 4;
                                               if (c_off + 3 < clut_size) {
                                                   rgba[dst_off + 0] = clut[c_off + 0];
                                                   rgba[dst_off + 1] = clut[c_off + 1];
                                                   rgba[dst_off + 2] = clut[c_off + 2];
                                                   rgba[dst_off + 3] = clut[c_off + 3];
                                                   if (clut[c_off] | clut[c_off+1] | clut[c_off+2] | clut[c_off+3])
                                                       non_zero_pixels++;
                                               }
                                           } else { // PSMCT16 CLUT
                                               size_t c_off = (size_t)idx * 2;
                                               if (c_off + 1 < clut_size) {
                                                   uint16_t pixel = clut[c_off] | (clut[c_off + 1] << 8);
                                                   rgba[dst_off + 0] = (uint8_t)(((pixel >>  0) & 0x1F) << 3);
                                                   rgba[dst_off + 1] = (uint8_t)(((pixel >>  5) & 0x1F) << 3);
                                                   rgba[dst_off + 2] = (uint8_t)(((pixel >> 10) & 0x1F) << 3);
                                                   rgba[dst_off + 3] = (pixel & 0x8000) ? 0xFF : 0x00;
                                                   if (pixel) non_zero_pixels++;
                                               }
                                           }
                                       }
                                   }
                               }
                               break;
                           }
                           case 0x14: // PSMT4 — 4-bit indexed (CLUT lookup)
                           {
                               const uint8_t* clut = ti.clut_snapshot.data();
                               size_t clut_size = ti.clut_snapshot.size();
                               bool clut32 = (ti.cpsm == 0x00);
                               for (int py = 0; py < tex_h; py++) {
                                   for (int px = 0; px < tex_w; px++) {
                                       size_t src_off = ((size_t)py * tex_w + px) / 2;
                                       size_t dst_off = ((size_t)py * tex_w + px) * 4;
                                       if (src_off < src_size) {
                                           uint8_t nibble = (px & 1)
                                               ? (src[src_off] >> 4) & 0x0F
                                               : src[src_off] & 0x0F;
                                           if (clut32) {
                                               size_t c_off = (size_t)nibble * 4;
                                               if (c_off + 3 < clut_size) {
                                                   rgba[dst_off + 0] = clut[c_off + 0];
                                                   rgba[dst_off + 1] = clut[c_off + 1];
                                                   rgba[dst_off + 2] = clut[c_off + 2];
                                                   rgba[dst_off + 3] = clut[c_off + 3];
                                                   if (clut[c_off] | clut[c_off+1] | clut[c_off+2] | clut[c_off+3])
                                                       non_zero_pixels++;
                                               }
                                           } else {
                                               size_t c_off = (size_t)nibble * 2;
                                               if (c_off + 1 < clut_size) {
                                                   uint16_t pixel = clut[c_off] | (clut[c_off + 1] << 8);
                                                   rgba[dst_off + 0] = (uint8_t)(((pixel >>  0) & 0x1F) << 3);
                                                   rgba[dst_off + 1] = (uint8_t)(((pixel >>  5) & 0x1F) << 3);
                                                   rgba[dst_off + 2] = (uint8_t)(((pixel >> 10) & 0x1F) << 3);
                                                   rgba[dst_off + 3] = (pixel & 0x8000) ? 0xFF : 0x00;
                                                   if (pixel) non_zero_pixels++;
                                               }
                                           }
                                       }
                                   }
                               }
                               break;
                           }
                           default:
                               // Unknown PSM — fall back to PSMCT32 interpretation
                               for (int py = 0; py < tex_h; py++) {
                                   for (int px = 0; px < tex_w; px++) {
                                       size_t src_off = ((size_t)py * tex_w + px) * 4;
                                       size_t dst_off = ((size_t)py * tex_w + px) * 4;
                                       if (src_off + 3 < src_size) {
                                           rgba[dst_off + 0] = src[src_off + 0];
                                           rgba[dst_off + 1] = src[src_off + 1];
                                           rgba[dst_off + 2] = src[src_off + 2];
                                           rgba[dst_off + 3] = src[src_off + 3];
                                           if (src[src_off] | src[src_off+1] | src[src_off+2] | src[src_off+3])
                                               non_zero_pixels++;
                                       }
                                   }
                               }
                               break;
                       }


                       // Create SDL_Texture and upload decoded pixels
                       texture = SDL_CreateTexture(g_renderer,
                                                   SDL_PIXELFORMAT_RGBA32,
                                                   SDL_TEXTUREACCESS_STATIC,
                                                   tex_w, tex_h);
                       if (texture) {
                           SDL_UpdateTexture(texture, nullptr, rgba.data(), tex_w * 4);
                           SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
                           texture_created = true;


                           g_logFile << "[TEX-FIX] Created texture " << tex_w << "x" << tex_h
                                     << " PSM=0x" << std::hex << ti.psm << std::dec
                                     << ", non_zero_pixels=" << non_zero_pixels << std::endl;
                       }
                   }
               }


               // Build SDL_Vertex array — all primitives are pre-decomposed
               // into individual triangles by FlushPrimitive()
               std::vector<SDL_Vertex> sdl_verts;
               sdl_verts.reserve(vCount);


               int texa_applied_count = 0;


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


                   // RC4 FIX: Apply TEXA alpha expansion for TCC=0 (RGB-only texture)
                   // When TCC=0, PS2 hardware uses TEXA register to provide alpha:
                   //   TA0 for vertices with alpha==0, TA1 for vertices with alpha!=0
                   uint8_t effective_alpha = v.a;
                   if (ti.enabled && ti.tcc == 0) {
                       effective_alpha = (v.a == 0) ? ti.texa_ta0 : ti.texa_ta1;
                       texa_applied_count++;
                   }
                   // PS2 alpha range is 0-128 (128=opaque), SDL expects 0-255
                   sv.color.a = (uint8_t)std::min((int)effective_alpha * 2, 255);


                   // RC3 FIX: Set tex_coord from vertex ST/UV when texture is active
                   if (texture_created && tex_w > 0 && tex_h > 0) {
                       if (ti.fst) {
                           // FST=1: UV mode — integer coords, normalize to [0,1]
                           sv.tex_coord.x = v.s / (float)tex_w;
                           sv.tex_coord.y = v.t / (float)tex_h;
                       } else {
                           // FST=0: STQ mode — perspective-correct, divide by Q
                           float q = (v.q != 0.0f) ? v.q : 1.0f;
                           sv.tex_coord.x = (v.s / q) / (float)tex_w;
                           sv.tex_coord.y = (v.t / q) / (float)tex_h;
                       }
                   } else {
                       sv.tex_coord.x = 0.0f;
                       sv.tex_coord.y = 0.0f;
                   }


                   sdl_verts.push_back(sv);
               }


               // RC4 FIX: Log TEXA expansion stats
               if (texa_applied_count > 0) {
                   g_logFile << "[ALPHA-FIX] TEXA expansion: ta0=" << (int)ti.texa_ta0
                             << " ta1=" << (int)ti.texa_ta1
                             << " applied to " << texa_applied_count << " vertices" << std::endl;
               }


               // Ensure vertex count is multiple of 3 (triangle list)
               size_t tri_verts = (sdl_verts.size() / 3) * 3;
              
               if (tri_verts >= 3) {
                   SDL_RenderGeometry(g_renderer,
                                     texture,              // RC3: pass texture (nullptr if untextured)
                                     sdl_verts.data(),
                                     (int)tri_verts,
                                     nullptr,              // no indices
                                     0);
               }


               // RC3 FIX: Destroy texture after draw call
               if (texture_created && texture) {
                   SDL_DestroyTexture(texture);
               }


               break;
           }
       }
   }
}





