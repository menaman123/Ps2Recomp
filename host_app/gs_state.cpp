// gs_state.cpp
#include "gs_state.h"
#include "render.h"
#include <fstream>

extern std::ofstream g_logFile;

GSState g_gs_state;

void GSState::SetPrim(uint64_t value) {

    // 1. Padding Check (Optional but risky)
    // Be careful here. Technically '0' is a valid command (Point, Flat, No Texture).
    // It is safer to filter padding in the GIF parser loop (where you know if it's 
    // an A+D padding entry) rather than here.
    // If you are sure this is only called for valid PRIM tags:
    /* if (value == 0) { 
        return; 
    }
    */

    GSPrimType new_type = static_cast<GSPrimType>(value & 0x7);

    // 2. State Change Detection (Flush)
    // If the Primitive Type changes (e.g., Triangle -> Line), we MUST draw
    // whatever is currently in the buffer before switching gears.
    if (new_type != prim_type) {
        if (!draw_buffer.empty()) {
            
            // Your Quick Fix for RenderQueue is fine for now
            extern RenderQueue g_renderQueue;
            extern std::ofstream g_logFile;

            RenderJob job;
            job.type = RenderCommandType::DrawBatch;
            job.batch.vertices = std::move(draw_buffer);
            job.batch.prim_type = prim_type;
            
            g_renderQueue.Push(job);
            
            // Note: std::move clears the source, but calling clear() is good practice
            draw_buffer.clear(); 
            
            if (g_logFile.is_open()) {
                g_logFile << "[GS] Primitive Change Flush: " << job.batch.vertices.size() 
                          << " vertices as Type " << (int)prim_type << std::endl;
            }
        }
        prim_type = new_type;
    }

    // 3. Update Flags (Always trust the register!)
    gouraud      = (value >> 3) & 1;
    texture      = (value >> 4) & 1;
    fog          = (value >> 5) & 1;
    alpha_blend  = (value >> 6) & 1;
    antialiasing = (value >> 7) & 1;
    use_uv       = (value >> 8) & 1;
    context      = (value >> 9) & 1;
    
    // Log the change for debugging
    if (g_logFile.is_open()) {
        g_logFile << "[GS] SetPrim: Type=" << (int)prim_type 
                  << " Gouraud=" << gouraud 
                  << " Context=" << context 
                  << " (Val=0x" << std::hex << value << std::dec << ")" << std::endl;
    }
}

void GSState::SetRGBAQ(uint64_t value) {
    r = (value >>  0) & 0xFF;
    g = (value >>  8) & 0xFF;
    b = (value >> 16) & 0xFF;
    a = (value >> 24) & 0xFF;
    // Q comes from ST register, not here in PACKED mode
}

void GSState::SetScissor(int ctx, uint64_t value) {
    scissor[ctx].x0 = (value >> 0)  & 0x7FF;
    scissor[ctx].x1 = (value >> 16) & 0x7FF;
    scissor[ctx].y0 = (value >> 32) & 0x7FF;
    scissor[ctx].y1 = (value >> 48) & 0x7FF;
    g_logFile << "[GS] SCISSOR[" << ctx << "] set: " << scissor[ctx].x0 << "..." << std::endl;
}

void GSState::SetXYOffset(int ctx, uint64_t value) {
    // PS2 12.4 fixed point format
    uint16_t raw_x = (value >> 0)  & 0xFFFF;
    uint16_t raw_y = (value >> 32) & 0xFFFF;
    
    // Pre-calculate to float
    offset[ctx].x = raw_x / 16.0f;
    offset[ctx].y = raw_y / 16.0f;
    
    g_logFile << "[GS] XYOFFSET[" << ctx << "] set: " 
              << offset[ctx].x << ", " << offset[ctx].y << std::endl;
}

void GSState::SetST(uint64_t lo, uint64_t hi) {
    uint32_t s_bits = lo & 0xFFFFFFFF;
    uint32_t t_bits = (lo >> 32) & 0xFFFFFFFF;
    uint32_t q_bits = hi & 0xFFFFFFFF;
    
    // Explicitly copy bits into floats
    std::memcpy(&this->s, &s_bits, 4);
    std::memcpy(&this->t, &t_bits, 4);
    
    // Documentation: Q is set by the STQ command
    float new_q;
    std::memcpy(&new_q, &q_bits, 4);
    if (new_q != 0.0f) this->q = new_q; 
}

void GSState::KickVertex(float x, float y, float z, uint8_t fog_val, bool draw) {
    GSVertex vtx;
    vtx.s = s; vtx.t = t; vtx.q = q;
    vtx.r = r; vtx.g = g; vtx.b = b; vtx.a = a;
    vtx.fog = fog_val;
    vtx.z = z; 
    vtx.x = x - offset[context].x;
    vtx.y = y - offset[context].y;
    vtx.draw_kick = draw;
    g_logFile << "[GS] KickVertext x before offset: " << x << " after offset: " << vtx.x << std::endl;
    g_logFile << "[GS] KickVertext y before offset: " << y << " after offset: " << vtx.y << std::endl;

    g_logFile << "[GS] KickVertex: (" << vtx.x << "," << vtx.y << "," << vtx.z << ") "
              << "RGBA=(" << (int)vtx.r << "," << (int)vtx.g << "," << (int)vtx.b << "," << (int)vtx.a << ") "
              << "ST=(" << vtx.s << "," << vtx.t << "," << vtx.q << ") "
              << "Fog=" << (int)vtx.fog << " Draw=" << draw << std::endl;



    vertex_queue.push_back(vtx);
    
    if (draw) {
        int needed = 0;
        switch (prim_type) {
            case GSPrimType::Point:         needed = 1; break;
            case GSPrimType::Line:          needed = 2; break;
            case GSPrimType::LineStrip:     needed = 2; break;
            case GSPrimType::Triangle:      needed = 3; break;
            case GSPrimType::TriangleStrip: needed = 3; break;
            case GSPrimType::TriangleFan:   needed = 3; break;
            case GSPrimType::Sprite:        needed = 2; break;
        }
        
        if ((int)vertex_queue.size() >= needed) {
            FlushPrimitive();
        }

        if (vertex_queue.size() > 3) {
            vertex_queue.erase(vertex_queue.begin());
        }
    } else {
        // If it's a non-drawing vertex (XYZ3/XYZF3), reset the strip winding
        strip_count = 0;
        
        // If the queue is too large, clear it to maintain the sliding window
        if (vertex_queue.size() > 3) vertex_queue.erase(vertex_queue.begin());
    }
}

void GSState::FlushPrimitive() {
    if (vertex_queue.empty()) return;
    
    switch (prim_type) {
        case GSPrimType::Point:
            draw_buffer.push_back(vertex_queue.back());
            vertex_queue.clear(); 
            break;

        case GSPrimType::Line:
            draw_buffer.push_back(vertex_queue[0]);
            draw_buffer.push_back(vertex_queue[1]);
            vertex_queue.clear();
            break;

        case GSPrimType::LineStrip:
            draw_buffer.push_back(vertex_queue[0]);
            draw_buffer.push_back(vertex_queue[1]);
            vertex_queue.erase(vertex_queue.begin()); 
            break;

        case GSPrimType::Triangle:
            draw_buffer.push_back(vertex_queue[0]);
            draw_buffer.push_back(vertex_queue[1]);
            draw_buffer.push_back(vertex_queue[2]);
            vertex_queue.clear();
            break;
            
        case GSPrimType::TriangleStrip:
            // NEW FIX: Alternating Winding
            // PS2 Documentation: Alternates v0-v1-v2 then v2-v1-v3
            if (strip_count % 2 == 0) {
                draw_buffer.push_back(vertex_queue[0]);
                draw_buffer.push_back(vertex_queue[1]);
                draw_buffer.push_back(vertex_queue[2]);
            } else {
                // Flip winding to keep front-facing for host GPU
                draw_buffer.push_back(vertex_queue[1]);
                draw_buffer.push_back(vertex_queue[0]);
                draw_buffer.push_back(vertex_queue[2]);
            }
            vertex_queue.erase(vertex_queue.begin());
            strip_count++;
            break;

        case GSPrimType::TriangleFan:
            // NEW FIX: Proper Pivot
            // v0 is the pivot, v1 and v2 are the edges
            g_logFile << "TRI_STRIP: Start at (" << vertex_queue[0].x << "," << vertex_queue[0].y << ") and (" << vertex_queue[1].x << "," << vertex_queue[1].y << ")" << std::endl;
            draw_buffer.push_back(vertex_queue[0]);
            draw_buffer.push_back(vertex_queue[1]);
            draw_buffer.push_back(vertex_queue[2]);
            vertex_queue.erase(vertex_queue.begin() + 1); // Keep v0, remove v1
            break;
            
        case GSPrimType::Sprite:
            // NEW FIX: Convert 2 vertices into 2 Triangles (6 vertices)
            // Documentation: "Sprite (2D rectangle with two points)"
            {
                GSVertex v0 = vertex_queue[0];
                GSVertex v1 = vertex_queue[1];
                
                // Create two triangles (v0, top-right, v1) and (v0, v1, bottom-left)
                GSVertex tr = v0; tr.x = v1.x; // Top-Right
                GSVertex bl = v1; bl.x = v0.x; // Bottom-Left
                
                draw_buffer.push_back(v0); draw_buffer.push_back(tr); draw_buffer.push_back(v1);
                draw_buffer.push_back(v0); draw_buffer.push_back(v1); draw_buffer.push_back(bl);
                
                vertex_queue.clear();
            }
            break;
    }
}