#pragma once
#include "gs_state.h"
#include <cstdint>
#include <vector>
#include <string>
#include <atomic>
#include <unordered_set>
#include <glm/glm.hpp>
#include <mutex>
#include <condition_variable>
#include <queue>

// Texture state snapshot taken at FlushBatch() time
struct BatchTextureInfo {
 bool enabled = false;       // TME flag from PRIM
 bool fst = false;           // FST: 0=STQ, 1=UV

 // TEX0 fields
 uint32_t tbp = 0;           // Texture base pointer (byte address)
 uint32_t tbw = 0;           // Texture buffer width (pixels)
 uint32_t psm = 0;           // Pixel storage mode
 uint32_t tw = 0;            // Log2 width
 uint32_t th = 0;            // Log2 height
 uint32_t tcc = 0;           // Texture color component (0=RGB, 1=RGBA)
 uint32_t tfx = 0;           // Texture function (0=MODULATE, 1=DECAL, ...)
 uint32_t cbp = 0;           // CLUT base pointer (byte address)
 uint32_t cpsm = 0;          // CLUT pixel format
 uint32_t csm = 0;           // CLUT storage mode

 // TEX1 fields
 bool mmag = false;          // Magnification filter (0=nearest, 1=linear)
 uint8_t mmin = 0;           // Minification filter

 // CLAMP fields
 uint8_t wms = 0;            // Wrap mode S
 uint8_t wmt = 0;            // Wrap mode T
 uint16_t minu = 0, maxu = 0;
 uint16_t minv = 0, maxv = 0;


 // VRAM snapshot — texture pixel data
 std::vector<uint8_t> vram_snapshot;
 uint32_t vram_snapshot_base = 0;  // Base address this snapshot starts at


 // TEXA fields — texture alpha expansion
 uint8_t texa_ta0 = 0x00;
 uint8_t texa_ta1 = 0x80;
 bool texa_aem = false;

 // CLUT snapshot — palette data (for PSMT8/PSMT4)
 std::vector<uint8_t> clut_snapshot;
 uint32_t clut_snapshot_base = 0;
};

struct RenderBatch {
 // Old fields (keep for HLE path)
 uint32_t meshAddress = 0;
 uint32_t renderType = 0;
 uint32_t textureId = 0;
 std::vector<glm::mat4> instances;
  // NEW: For GIF/GS path
 std::vector<GSVertex> vertices;
 GSPrimType prim_type = GSPrimType::Triangle;

 // Texture state for this batch
 BatchTextureInfo tex_info;




 // Per-batch GS register state
 bool colclamp = true;
 bool pabe = false;
 bool fba = false;
 struct { uint8_t r = 0, g = 0, b = 0; } fogcol;
};


enum class RenderCommandType {
 DrawBatch,       // Geometry (GIF PATH 1/2)
  // State Management
 SetScissor,      // GS Register 0x40
 SetBlendMode,    // GS Register 0x42
 SetDepthTest,    // GS Register 0x47
  // Display / Scanout Management (NEW)
 SetFrontBuffer,  // DISPFB: Sets VRAM addr for scanout
 SetWindow,       // DISPLAY: Sets window dimensions
 SetClearColor,   // BGCOLOR: Sets background color
  VSync,           // Present the frame
  InvalidateTextures  // VRAM transfer completed — invalidate overlapping cache entries
};

struct RenderJob {
 RenderCommandType type;
 RenderBatch batch;
 struct {
     uint32_t arg1 = 0; // e.g., Address, Width, Red
     uint32_t arg2 = 0; // e.g., Stride, Height, Green
     uint32_t arg3 = 0; // e.g., Format, Blue
 } args;
};


class RenderQueue{
 std::queue<RenderJob> jobs;
 std::mutex mtx;
 std::condition_variable cv;
 bool active = true;

public:
 void Push(RenderJob job){
     {
         std::lock_guard<std::mutex> lock(mtx);
         jobs.push(job);
     }
     cv.notify_one();
 }

 bool Pop(RenderJob& job){
     std::unique_lock<std::mutex> lock(mtx);
     cv.wait(lock, [this]{return !jobs.empty() || !active;});
     if (jobs.empty() && !active){
         return false;
     }
     job = jobs.front();
     jobs.pop();
     return true;
 }

 void Stop(){
     std::lock_guard<std::mutex> lock(mtx);
     active = false;
     cv.notify_all();
 }

};

extern RenderQueue g_renderQueue;


// TEXFLUSH: atomic flag set by CPU thread, consumed by graphics thread
extern std::atomic<bool> g_texflush_pending;


// Track which VRAM pages have been written by Host→VRAM transfers
extern std::mutex g_vram_pages_mutex;
extern std::unordered_set<uint32_t> g_vram_written_pages;