// gs_state.h
#pragma once
#include <vector>
#include <cstdint>
#include <cstring>

enum class GSPrimType : uint8_t {
    Point = 0,
    Line = 1,
    LineStrip = 2,
    Triangle = 3,
    TriangleStrip = 4,
    TriangleFan = 5,
    Sprite = 6
};

struct GSVertex {
    float x, y, z;
    float s, t, q;
    uint8_t r, g, b, a;
    uint8_t fog;
    bool draw_kick;
};

struct GSState {
    // Current drawing context
    GSPrimType prim_type = GSPrimType::Triangle;
    bool gouraud = false;
    bool texture = false;
    bool fog = false;
    bool alpha_blend = false;
    bool antialiasing = false;
    bool use_uv = false;
    int strip_count = 0;
    
    // CONTEXT (0 or 1)
    int context = 0; 

    // CHANGED: Use float to store the decoded 12.4 offset
    struct { float x=0.0f, y=0.0f; } offset[2]; 
    
    struct { uint16_t x0=0, x1=0, y0=0, y1=0; } scissor[2];
    
    // Current vertex attributes
    uint8_t r = 0x80, g = 0x80, b = 0x80, a = 0x80;
    float s = 0.0f, t = 0.0f, q = 1.0f;
    uint16_t u = 0, v = 0;
    uint8_t fog_coef = 0;
    
    std::vector<GSVertex> vertex_queue;
    std::vector<GSVertex> draw_buffer;
    
    void SetPrim(uint64_t value);
    void SetRGBAQ(uint64_t value);
    void SetST(uint64_t lo, uint64_t hi);
    void KickVertex(float x, float y, float z, uint8_t fog_val, bool draw);
    void FlushPrimitive();
    void SetScissor(int ctx, uint64_t value);
    void SetXYOffset(int ctx, uint64_t value);
};
extern GSState g_gs_state;