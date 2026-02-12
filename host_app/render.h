#pragma once
#include "gs_state.h"
#include <cstdint>
#include <vector>
#include <string>
#include <glm/glm.hpp>
#include <mutex>
#include <condition_variable>
#include <queue>

struct RenderBatch {
    // Old fields (keep for HLE path)
    uint32_t meshAddress = 0;
    uint32_t renderType = 0;
    uint32_t textureId = 0;
    std::vector<glm::mat4> instances;
    
    // NEW: For GIF/GS path
    std::vector<GSVertex> vertices;
    GSPrimType prim_type = GSPrimType::Triangle;
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
    
    VSync            // Present the frame
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