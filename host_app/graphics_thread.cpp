#include "ps2_scheduler.h"
#include "render.h"
#include "texture_decoder.h"
#include "texture_cache.h"
#include <iostream>
#include <iomanip>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <SDL.h>
#include "gs.h"
#include "gs_state.h"
#include "intc.h"
































extern RenderQueue g_renderQueue;
extern SDL_Renderer* g_renderer;
extern std::ofstream g_logFile;
































































// Global texture cache — lives on the graphics thread
static TextureCache g_textureCache;
































































// Defined in main.cpp or wherever globals live — declared in render.h
std::atomic<bool> g_texflush_pending{false};
std::mutex g_vram_pages_mutex;
std::unordered_set<uint32_t> g_vram_written_pages;
































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
     {
         static uint32_t s_frame_count = 0;
         s_frame_count++;
















         // [XFER-SUMMARY] Log session-wide transfer counters at each VSync
         g_logFile << "[XFER-SUMMARY] Frame " << s_frame_count
                   << ": BITBLTBUF=#" << g_bitbltbuf_count
                   << " TRXDIR=#" << g_trxdir_count
                   << " HWREG=#" << g_hwreg_count
                   << " (total transfers: " << g_trxdir_count << ")" << std::endl;
















         SDL_RenderPresent(g_renderer);
  
         g_gs_regs.GS_CSR |= (1 << 3);
         g_intc.RaiseInterrupt(INTC_VBON);
































































































































         // Clear for next frame
         SDL_SetRenderDrawColor(g_renderer, clearR, clearG, clearB, 255);
         SDL_RenderClear(g_renderer);
         break;
     }
































































































































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
































































     case RenderCommandType::InvalidateTextures:
     {
         uint32_t base = job.args.arg1;
         uint32_t size = job.args.arg2;
         g_textureCache.InvalidateRegion(base, size);
         g_logFile << "[TEX] Transfer invalidation: base=0x" << std::hex << base
                   << " size=0x" << size << std::dec << std::endl;
         break;
     }
































































































































     case RenderCommandType::DrawBatch:
     {
         if (!g_renderer) break;








         // ===== DIAGNOSTIC: Log every DrawBatch received =====
         {
             const auto& db = job.batch;
             size_t vc = db.vertices.size();
             uint8_t r0=0,g0=0,b0=0,a0=0;
             float x0=0,y0=0;
             if (vc > 0) { r0=db.vertices[0].r; g0=db.vertices[0].g; b0=db.vertices[0].b; a0=db.vertices[0].a; x0=db.vertices[0].x; y0=db.vertices[0].y; }
             g_logFile << "[DRAW-BATCH] verts=" << vc
                       << " type=" << (int)db.prim_type
                       << " tex=" << db.tex_info.enabled
                       << " alpha_blend=" << db.alpha_blend
                       << " v0=(" << x0 << "," << y0 << ") RGBA=(" << (int)r0 << "," << (int)g0 << "," << (int)b0 << "," << (int)a0 << ")"
                       << " TBP=0x" << std::hex << db.tex_info.tbp << std::dec
                       << " PSM=0x" << std::hex << db.tex_info.psm << std::dec
                       << std::endl;
         }
































































         // TEXFLUSH: check and clear the atomic flag, invalidate cache
         if (g_texflush_pending.exchange(false, std::memory_order_acquire)) {
             g_textureCache.InvalidateAll();
             g_logFile << "[TEX] TEXFLUSH consumed — cache invalidated" << std::endl;
         }
  
         const auto& batch = job.batch;
         size_t vCount = batch.vertices.size();
         if (vCount < 3) break;
































































         const auto& ti = batch.tex_info;
         int tex_w = ti.enabled ? (1 << ti.tw) : 0;
         int tex_h = ti.enabled ? (1 << ti.th) : 0;
































































         // --- 5.1: Texture lookup, decode, and SDL_Texture creation ---
         SDL_Texture* sdl_tex = nullptr;
































































         if (ti.enabled && !ti.vram_snapshot.empty() && tex_w > 0 && tex_h > 0) {
             // Reject Z-buffer PSM formats as texture source
             if (ti.psm >= 0x30 && ti.psm <= 0x3A) {
                 g_logFile << "[TEX] Z-buffer PSM=0x" << std::hex << ti.psm
                           << std::dec << " rejected, vertex-color fallback" << std::endl;
             } else {
                 // Task 9.3: Check if texture VRAM pages have been written
                 bool pages_written = false;
                 {
                     static constexpr uint32_t PAGE_SIZE = 8192;
                     uint32_t tex_start_page = (ti.tbp / PAGE_SIZE) * PAGE_SIZE;
                     // Compute how many pages the texture spans
                     size_t tex_bytes = 0;
                     switch (ti.psm) {
                         case 0x00: tex_bytes = (size_t)tex_w * tex_h * 4; break;
                         case 0x02: tex_bytes = (size_t)tex_w * tex_h * 2; break;
                         case 0x13: tex_bytes = (size_t)tex_w * tex_h; break;
                         case 0x14: tex_bytes = (size_t)tex_w * tex_h / 2; break;
                         default:   tex_bytes = (size_t)tex_w * tex_h * 4; break;
                     }
                     uint32_t tex_end = ti.tbp + (uint32_t)tex_bytes;
                     std::lock_guard<std::mutex> lock(g_vram_pages_mutex);
                     for (uint32_t page = tex_start_page; page < tex_end; page += PAGE_SIZE) {
                         if (g_vram_written_pages.count(page)) {
                             pages_written = true;
                             break;
                         }
                     }
                 }
































































                 if (!pages_written) {
                     // RC7: Allow all textures through — page tracker misses
                     // framebuffer writes and VRAM-to-VRAM copies
                     g_logFile << "[TEX] VRAM pages not tracked for TBP=0x" << std::hex << ti.tbp
                               << std::dec << " — attempting decode anyway" << std::endl;
                 }
                 {
                 // Check texture cache first
                 TextureCacheKey key{ti.tbp, ti.tbw, ti.psm, ti.tw, ti.th, ti.cbp, ti.cpsm};
                 sdl_tex = g_textureCache.Lookup(key);
































































                 if (!sdl_tex) {
                     // Cache miss — decode VRAM snapshot via TextureDecoder
                     auto pixels = TextureDecoder::Decode(ti);
































































                     if (!pixels.empty()) {
                         sdl_tex = SDL_CreateTexture(g_renderer,
                             SDL_PIXELFORMAT_ABGR8888,
                             SDL_TEXTUREACCESS_STATIC,
                             tex_w, tex_h);
































































                         if (sdl_tex) {
                             SDL_UpdateTexture(sdl_tex, nullptr, pixels.data(), tex_w * 4);
                             SDL_SetTextureBlendMode(sdl_tex, SDL_BLENDMODE_BLEND);
                             g_textureCache.Insert(key, sdl_tex);
                         } else {
                             g_logFile << "[TEX] SDL_CreateTexture failed: " << SDL_GetError() << std::endl;
                         }
                     } else {
                         // Empty decode result — fallback to vertex-color rendering
                         g_logFile << "[TEX] Decode returned empty for TBP=0x" << std::hex << ti.tbp
                                   << " PSM=0x" << ti.psm << std::dec << std::endl;
                     }
                 }
                 } // texture decode block
             }
         }
































































         // --- Sprite expansion: PS2 SPRITE (type 6) uses 2 verts per prim ---
         // SDL needs triangles, so expand each sprite pair into 2 triangles (6 verts)
         const auto* src_verts = &batch.vertices;
         std::vector<GSVertex> expanded_verts;
         if (batch.prim_type == GSPrimType::Sprite && vCount >= 2) {
             expanded_verts.reserve((vCount / 2) * 6);
             for (size_t i = 0; i + 1 < vCount; i += 2) {
                 const auto& v0 = batch.vertices[i];     // top-left
                 const auto& v1 = batch.vertices[i + 1]; // bottom-right
                 // Build 4 corners: TL, TR, BL, BR
                 // v0 = (x0, y0), v1 = (x1, y1)
                 // Use v1's color/UV for all corners (PS2 SPRITE uses v1's attributes)
                 GSVertex tl = v1; tl.x = v0.x; tl.y = v0.y;
                 GSVertex tr = v1; tr.x = v1.x; tr.y = v0.y;
                 GSVertex bl = v1; bl.x = v0.x; bl.y = v1.y;
                 GSVertex br = v1; // already (x1, y1)
                 // UV interpolation for corners
                 if (sdl_tex) {
                     tl.s = v0.s; tl.t = v0.t;
                     tr.s = v1.s; tr.t = v0.t;
                     bl.s = v0.s; bl.t = v1.t;
                     br.s = v1.s; br.t = v1.t;
                 }
                 // Triangle 1: TL, TR, BL
                 expanded_verts.push_back(tl);
                 expanded_verts.push_back(tr);
                 expanded_verts.push_back(bl);
                 // Triangle 2: TR, BR, BL
                 expanded_verts.push_back(tr);
                 expanded_verts.push_back(br);
                 expanded_verts.push_back(bl);
             }
             src_verts = &expanded_verts;
             vCount = expanded_verts.size();
             g_logFile << "[SPRITE-EXPAND] Expanded " << batch.vertices.size()
                       << " sprite verts -> " << vCount << " triangle verts ("
                       << (vCount / 6) << " sprites)" << std::endl;
         }




         // --- Build SDL_Vertex array ---
         std::vector<SDL_Vertex> sdl_verts;
         sdl_verts.reserve(vCount);
































































         for (size_t i = 0; i < vCount; i++) {
             const auto& v = (*src_verts)[i];
             SDL_Vertex sv;
      
             float x = v.x;
             float y = v.y;
             if (x > 1000.0f) x -= 2048.0f;
             if (y > 1000.0f) y -= 2048.0f;
































































             sv.position.x = x;
             sv.position.y = y;
































































             // --- 5.3: TFX vertex color adjustment ---
             uint8_t vr = v.r, vg = v.g, vb = v.b, va = v.a;
































































             if (sdl_tex) {
                 uint32_t tfx = ti.tfx;
                 // HIGHLIGHT/HIGHLIGHT2 fall back to MODULATE
                 if (tfx == 2 || tfx == 3) tfx = 0;
































































                 if (tfx == 0) {
                     // MODULATE: scale vertex color by 2 (PS2 128=1.0 → SDL 255=1.0)
                     vr = (uint8_t)std::min((int)vr * 2, 255);
                     vg = (uint8_t)std::min((int)vg * 2, 255);
                     vb = (uint8_t)std::min((int)vb * 2, 255);
                     if (ti.tcc == 1) {
                         // TCC=1: alpha from texture×vertex
                         va = (uint8_t)std::min((int)va * 2, 255);
                     } else {
                         // TCC=0: alpha from vertex only, still scale
                         va = (uint8_t)std::min((int)va * 2, 255);
                     }
                 } else if (tfx == 1) {
                     // DECAL
                     if (ti.tcc == 1) {
                         // TCC=1: texture RGBA passes through, vertex = white opaque
                         vr = 255; vg = 255; vb = 255; va = 255;
                     } else {
                         // TCC=0: texture RGB, alpha from vertex
                         vr = 255; vg = 255; vb = 255;
                         va = (uint8_t)std::min((int)va * 2, 255);
                     }
                 }
             } else {
                 // No texture — apply TEXA alpha expansion for TCC=0
                 uint8_t effective_alpha = va;
                 if (ti.enabled && ti.tcc == 0) {
                     effective_alpha = (va == 0) ? ti.texa_ta0 : ti.texa_ta1;
                 }
                 va = (uint8_t)std::min((int)effective_alpha * 2, 255);
             }
































































             sv.color.r = vr;
             sv.color.g = vg;
             sv.color.b = vb;
             sv.color.a = va;
































             // PS2: When alpha blending is disabled (ABE=0), pixels are always
             // written to the framebuffer regardless of alpha value. SDL needs
             // alpha=255 to render opaque pixels with SDL_BLENDMODE_BLEND.
             if (!batch.alpha_blend) {
                 sv.color.a = 255;
             }
































































             // --- 5.2: UV coordinate computation ---
             if (sdl_tex && tex_w > 0 && tex_h > 0) {
                 if (ti.fst) {
                     // FST=1 (UV mode): s,t already in texel units
                     sv.tex_coord.x = v.s / (float)tex_w;
                     sv.tex_coord.y = v.t / (float)tex_h;
                 } else {
                     // FST=0 (STQ mode): perspective-correct, divide by Q
                     float q = (v.q != 0.0f) ? v.q : 1.0f;
                     sv.tex_coord.x = (v.s / q) / (float)tex_w;
                     sv.tex_coord.y = (v.t / q) / (float)tex_h;
                 }
































































                 // --- 6.1: Apply wrap/clamp modes ---
                 // WMS applied to U (S axis), WMT applied to V (T axis)
                 auto applyWrapClamp = [](float coord, uint8_t mode,
                                          uint16_t minc, uint16_t maxc, int dim) -> float {
                     switch (mode) {
                         case 0: // REPEAT
                             return coord - std::floor(coord);
                         case 1: // CLAMP
                             return std::clamp(coord, 0.0f, 1.0f);
                         case 2: // REGION_CLAMP
                             if (dim > 0) {
                                 float lo = (float)minc / (float)dim;
                                 float hi = (float)maxc / (float)dim;
                                 return std::clamp(coord, lo, hi);
                             }
                             return coord;
                         case 3: // REGION_REPEAT
                             if (dim > 0) {
                                 int mask   = (int)minc;  // MINU/MINV is mask
                                 int offset = (int)maxc;  // MAXU/MAXV is offset
                                 return (float)((int(coord * dim) & mask) | offset) / (float)dim;
                             }
                             return coord;
                         default:
                             return coord;
                     }
                 };
































































                 sv.tex_coord.x = applyWrapClamp(sv.tex_coord.x, ti.wms,
                                                  ti.minu, ti.maxu, tex_w);
                 sv.tex_coord.y = applyWrapClamp(sv.tex_coord.y, ti.wmt,
                                                  ti.minv, ti.maxv, tex_h);
             } else {
                 sv.tex_coord.x = 0.0f;
                 sv.tex_coord.y = 0.0f;
             }
































































             sdl_verts.push_back(sv);
         }
































































         // Ensure vertex count is multiple of 3 (triangle list)
         size_t tri_verts = (sdl_verts.size() / 3) * 3;
  
         if (tri_verts >= 3) {
             g_logFile << "[SDL-DRAW] Rendering " << tri_verts << " verts ("
                       << (tri_verts / 3) << " triangles) tex=" << (sdl_tex ? "yes" : "no")
                       << " prim_type=" << (int)batch.prim_type << std::endl;
             SDL_RenderGeometry(g_renderer,
                               sdl_tex,
                               sdl_verts.data(),
                               (int)tri_verts,
                               nullptr, 0);
         }
































































         break;
     }
 }
}
}





























































