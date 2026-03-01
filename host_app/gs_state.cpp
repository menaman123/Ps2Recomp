// gs_state.cpp
#include "gs_state.h"
#include "render.h"
#include <fstream>




extern std::ofstream g_logFile;
extern RenderQueue g_renderQueue;
static constexpr size_t VRAM_SIZE = 4 * 1024 * 1024;
static constexpr uint32_t PAGE_SIZE = 8192;
GSState g_gs_state;




void GSState::Flush() {
  if (draw_buffer.empty()) return;
   extern RenderQueue g_renderQueue;
   RenderJob job;
  job.type = RenderCommandType::DrawBatch;
  job.batch.vertices = draw_buffer;
  job.batch.prim_type = prim_type;
   g_renderQueue.Push(job);
   if (g_logFile.is_open()) {
      g_logFile << "[GS] Flush: " << draw_buffer.size()
                << " vertices as Type " << (int)prim_type << std::endl;
  }
   draw_buffer.clear();
  strip_count = 0;
}








void GSState::SetPrim(uint64_t value) {
  GSPrimType new_type = static_cast<GSPrimType>(value & 0x7);




  // State Change Detection - flush if primitive type changes
  if (new_type != prim_type) {
      if (!draw_buffer.empty()) {
          extern RenderQueue g_renderQueue;




          RenderJob job;
          job.type = RenderCommandType::DrawBatch;
          job.batch.vertices = std::move(draw_buffer);
          job.batch.prim_type = prim_type;
        
          g_renderQueue.Push(job);
          draw_buffer.clear();
        
          if (g_logFile.is_open()) {
              g_logFile << "[GS] Primitive Change Flush: " << job.batch.vertices.size()
                        << " vertices as Type " << (int)prim_type << std::endl;
          }
      }
      prim_type = new_type;
  }




  if (prmodecont) {
      // PRIM controls everything (default behavior)
      gouraud      = (value >> 3) & 1;  // IIP
      texture      = (value >> 4) & 1;  // TME
      fog          = (value >> 5) & 1;  // FGE
      alpha_blend  = (value >> 6) & 1;  // ABE
      antialiasing = (value >> 7) & 1;  // AA1
      use_uv       = (value >> 8) & 1;  // FST (0=STQ, 1=UV)
      context      = (value >> 9) & 1;  // CTXT
  }
  // else: attributes come from PRMODE, only type changes
   if (g_logFile.is_open()) {
      g_logFile << "[GS] SetPrim: Type=" << (int)prim_type
                << " Gouraud=" << gouraud
                << " Texture=" << texture
                << " Fog=" << fog
                << " AlphaBlend=" << alpha_blend
                << " UseUV=" << use_uv
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
   if (g_logFile.is_open()) {
      g_logFile << "[GS] SCISSOR[" << ctx << "]: ("
                << scissor[ctx].x0 << "," << scissor[ctx].y0 << ") to ("
                << scissor[ctx].x1 << "," << scissor[ctx].y1 << ")" << std::endl;
  }
}




void GSState::SetXYOffset(int ctx, uint64_t value) {
  // PS2 12.4 fixed point format
  uint16_t raw_x = (value >> 0)  & 0xFFFF;
  uint16_t raw_y = (value >> 32) & 0xFFFF;
   // Pre-calculate to float
  offset[ctx].x = raw_x / 16.0f;
  offset[ctx].y = raw_y / 16.0f;
   if (g_logFile.is_open()) {
      g_logFile << "[GS] XYOFFSET[" << ctx << "]: "
                << offset[ctx].x << ", " << offset[ctx].y << std::endl;
  }
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
   // === FIX: Use correct texture coordinates based on FST flag ===
  if (use_uv) {
      // FST=1: Use UV coordinates (10.4 fixed point format)
      // UV values are 14-bit with 4 fractional bits
      vtx.s = u / 16.0f;  // Convert from 10.4 fixed point to float
      vtx.t = v / 16.0f;
      vtx.q = 1.0f;       // Q not used in UV mode
  } else {
      // FST=0: Use STQ coordinates (perspective-correct texturing)
      vtx.s = s;
      vtx.t = t;
      vtx.q = q;
  }
   vtx.r = r; vtx.g = g; vtx.b = b; vtx.a = a;
  vtx.fog = fog_val;
  vtx.z = z;
  vtx.x = x - offset[context].x;
  vtx.y = y - offset[context].y;
  vtx.draw_kick = draw;




  if (g_logFile.is_open()) {
      g_logFile << "[GS] KickVertex: (" << vtx.x << "," << vtx.y << "," << vtx.z << ") "
                << "RGBA=(" << (int)vtx.r << "," << (int)vtx.g << "," << (int)vtx.b << "," << (int)vtx.a << ") ";
      if (use_uv) {
          g_logFile << "UV=(" << vtx.s << "," << vtx.t << ") ";
      } else {
          g_logFile << "STQ=(" << vtx.s << "," << vtx.t << "," << vtx.q << ") ";
      }
      g_logFile << "Fog=" << (int)vtx.fog << " Draw=" << draw << std::endl;
  }




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
      strip_count = 0;
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
          if (strip_count % 2 == 0) {
              draw_buffer.push_back(vertex_queue[0]);
              draw_buffer.push_back(vertex_queue[1]);
              draw_buffer.push_back(vertex_queue[2]);
          } else {
              draw_buffer.push_back(vertex_queue[1]);
              draw_buffer.push_back(vertex_queue[0]);
              draw_buffer.push_back(vertex_queue[2]);
          }
          vertex_queue.erase(vertex_queue.begin());
          strip_count++;
          break;




      case GSPrimType::TriangleFan:
          draw_buffer.push_back(vertex_queue[0]);
          draw_buffer.push_back(vertex_queue[1]);
          draw_buffer.push_back(vertex_queue[2]);
          vertex_queue.erase(vertex_queue.begin() + 1);
          break;
        
      case GSPrimType::Sprite:
          {
              GSVertex v0 = vertex_queue[0];  // Top-left corner
              GSVertex v1 = vertex_queue[1];  // Bottom-right corner
            
              // Top-right: X from v1, Y from v0, U from v1, V from v0
              GSVertex tr = v0;
              tr.x = v1.x;
              tr.s = v1.s;  // U from v1
              // tr.t stays from v0 (V from v0)
            
              // Bottom-left: X from v0, Y from v1, U from v0, V from v1
              GSVertex bl = v1;
              bl.x = v0.x;
              bl.s = v0.s;  // U from v0
              // bl.t stays from v1 (V from v1)
            
              // Also interpolate Z for proper depth
              tr.z = v0.z;  // Use v0's Z (PS2 sprites use first vertex Z)
              bl.z = v1.z;
            
              // Color: PS2 sprites use v1's color for the entire sprite (flat shading)
              tr.r = v1.r; tr.g = v1.g; tr.b = v1.b; tr.a = v1.a;
              v0.r = v1.r; v0.g = v1.g; v0.b = v1.b; v0.a = v1.a;
              bl.r = v1.r; bl.g = v1.g; bl.b = v1.b; bl.a = v1.a;
            
              // Two triangles forming the quad
              // Triangle 1: top-left, top-right, bottom-right
              draw_buffer.push_back(v0);
              draw_buffer.push_back(tr);
              draw_buffer.push_back(v1);
            
              // Triangle 2: top-left, bottom-right, bottom-left
              draw_buffer.push_back(v0);
              draw_buffer.push_back(v1);
              draw_buffer.push_back(bl);
            
              vertex_queue.clear();
          }
          break;
  }
}








void GSState::SetFrame(int ctx, uint64_t value) {
  frame[ctx].fbp   = (value & 0x1FF) * 8192;        // Convert to byte address
  frame[ctx].fbw   = ((value >> 16) & 0x3F) * 64;   // Convert to pixels
  frame[ctx].psm   = (value >> 24) & 0x3F;
  frame[ctx].fbmsk = (value >> 32) & 0xFFFFFFFF;
   if (g_logFile.is_open()) {
      g_logFile << "[GS] FRAME[" << ctx << "]: FBP=0x" << std::hex << frame[ctx].fbp
                << " FBW=" << std::dec << frame[ctx].fbw
                << " PSM=" << frame[ctx].psm
                << " FBMSK=0x" << std::hex << frame[ctx].fbmsk << std::dec << std::endl;
  }
}




void GSState::SetZBuf(int ctx, uint64_t value) {
  zbuf[ctx].zbp  = (value & 0x1FF) * 8192;
  zbuf[ctx].psm  = (value >> 24) & 0xF;
  zbuf[ctx].zmsk = (value >> 32) & 1;
   if (g_logFile.is_open()) {
      g_logFile << "[GS] ZBUF[" << ctx << "]: ZBP=0x" << std::hex << zbuf[ctx].zbp
                << " PSM=" << std::dec << zbuf[ctx].psm
                << " ZMSK=" << zbuf[ctx].zmsk << std::endl;
  }
}




void GSState::SetTex0(int ctx, uint64_t value) {
  tex0[ctx].tbp  = (value & 0x3FFF) * 256;          // Convert to byte address
  tex0[ctx].tbw  = ((value >> 14) & 0x3F) * 64;     // Convert to pixels
  tex0[ctx].psm  = (value >> 20) & 0x3F;
  tex0[ctx].tw   = (value >> 26) & 0xF;             // Log2 of width
  tex0[ctx].th   = (value >> 30) & 0xF;             // Log2 of height
  tex0[ctx].tcc  = (value >> 34) & 1;
  tex0[ctx].tfx  = (value >> 35) & 3;
  tex0[ctx].cbp  = ((value >> 37) & 0x3FFF) * 256;
  tex0[ctx].cpsm = (value >> 51) & 0xF;
  tex0[ctx].csm  = (value >> 55) & 1;
   if (g_logFile.is_open()) {
      g_logFile << "[GS] TEX0[" << ctx << "]: TBP=0x" << std::hex << tex0[ctx].tbp
                << " TBW=" << std::dec << tex0[ctx].tbw
                << " Size=" << (1 << tex0[ctx].tw) << "x" << (1 << tex0[ctx].th)
                << " PSM=" << tex0[ctx].psm
                << " TCC=" << tex0[ctx].tcc
                << " TFX=" << tex0[ctx].tfx << std::endl;
  }
}




void GSState::SetTex1(int ctx, uint64_t value) {
  tex1[ctx].lcm  = value & 1;
  tex1[ctx].mxl  = (value >> 2) & 7;
  tex1[ctx].mmag = (value >> 5) & 1;
  tex1[ctx].mmin = (value >> 6) & 7;
  tex1[ctx].mtba = (value >> 9) & 1;
  tex1[ctx].l    = (value >> 19) & 3;
  tex1[ctx].k    = (value >> 32) & 0xFFF;
   if (g_logFile.is_open()) {
      g_logFile << "[GS] TEX1[" << ctx << "]: MMAG=" << (tex1[ctx].mmag ? "LINEAR" : "NEAREST")
                << " MMIN=" << (int)tex1[ctx].mmin << std::endl;
  }
}




void GSState::SetTest(int ctx, uint64_t value) {
  test[ctx].ate   = value & 1;
  test[ctx].atst  = (value >> 1) & 7;
  test[ctx].aref  = (value >> 4) & 0xFF;
  test[ctx].afail = (value >> 12) & 3;
  test[ctx].date  = (value >> 14) & 1;
  test[ctx].datm  = (value >> 15) & 1;
  test[ctx].zte   = (value >> 16) & 1;
  test[ctx].ztst  = (value >> 17) & 3;
   if (g_logFile.is_open()) {
      g_logFile << "[GS] TEST[" << ctx << "]:"
                << " ATE=" << test[ctx].ate;
      if (test[ctx].ate) {
          g_logFile << " ATST=" << (int)test[ctx].atst
                    << " AREF=" << (int)test[ctx].aref
                    << " AFAIL=" << (int)test[ctx].afail;
      }
      g_logFile << " ZTE=" << test[ctx].zte
                << " ZTST=" << (int)test[ctx].ztst << std::endl;
  }
}








void GSState::SetAlpha(int ctx, uint64_t value) {
  alpha[ctx].a   = value & 3;
  alpha[ctx].b   = (value >> 2) & 3;
  alpha[ctx].c   = (value >> 4) & 3;
  alpha[ctx].d   = (value >> 6) & 3;
  alpha[ctx].fix = (value >> 32) & 0xFF;
   if (g_logFile.is_open()) {
      g_logFile << "[GS] ALPHA[" << ctx << "]: A=" << (int)alpha[ctx].a
                << " B=" << (int)alpha[ctx].b
                << " C=" << (int)alpha[ctx].c
                << " D=" << (int)alpha[ctx].d
                << " FIX=" << (int)alpha[ctx].fix << std::endl;
  }
}




void GSState::SetClamp(int ctx, uint64_t value) {
  clamp[ctx].wms  = value & 3;
  clamp[ctx].wmt  = (value >> 2) & 3;
  clamp[ctx].minu = (value >> 4) & 0x3FF;
  clamp[ctx].maxu = (value >> 14) & 0x3FF;
  clamp[ctx].minv = (value >> 24) & 0x3FF;
  clamp[ctx].maxv = (value >> 34) & 0x3FF;
   if (g_logFile.is_open()) {
      g_logFile << "[GS] CLAMP[" << ctx << "]: WMS=" << (int)clamp[ctx].wms
                << " WMT=" << (int)clamp[ctx].wmt << std::endl;
  }
}




void GSState::SetTexa(uint64_t value) {
  texa.ta0 = value & 0xFF;
  texa.aem = (value >> 15) & 1;
  texa.ta1 = (value >> 32) & 0xFF;




  if (g_logFile.is_open()) {
      g_logFile << "[GS] TEXA: TA0=0x" << std::hex << (int)texa.ta0
                << " AEM=" << texa.aem
                << " TA1=0x" << (int)texa.ta1 << std::dec << std::endl;
  }
}




void GSState::SetColclamp(uint64_t value) {
  colclamp = value & 1;




  if (g_logFile.is_open()) {
      g_logFile << "[GS] COLCLAMP: " << (colclamp ? "CLAMP" : "MASK") << std::endl;
  }
}




void GSState::SetPrmodecont(uint64_t value) {
  bool old_val = prmodecont;
  prmodecont = value & 1;




  // On true→false transition, apply stored PRMODE attributes
  if (old_val && !prmodecont) {
      gouraud      = (prmode_raw >> 3) & 1;
      texture      = (prmode_raw >> 4) & 1;
      fog          = (prmode_raw >> 5) & 1;
      alpha_blend  = (prmode_raw >> 6) & 1;
      antialiasing = (prmode_raw >> 7) & 1;
      use_uv       = (prmode_raw >> 8) & 1;
      context      = (prmode_raw >> 9) & 1;
  }




  if (g_logFile.is_open()) {
      g_logFile << "[GS] PRMODECONT: " << (prmodecont ? "PRIM" : "PRMODE")
                << " (was " << (old_val ? "PRIM" : "PRMODE") << ")" << std::endl;
  }
}




void GSState::SetPrmode(uint64_t value) {
  prmode_raw = value;




  // Apply attribute flags only when PRMODE is the active source
  if (!prmodecont) {
      gouraud      = (value >> 3) & 1;
      texture      = (value >> 4) & 1;
      fog          = (value >> 5) & 1;
      alpha_blend  = (value >> 6) & 1;
      antialiasing = (value >> 7) & 1;
      use_uv       = (value >> 8) & 1;
      context      = (value >> 9) & 1;
  }




  if (g_logFile.is_open()) {
      g_logFile << "[GS] PRMODE: Raw=0x" << std::hex << value << std::dec
                << " Applied=" << (!prmodecont) << std::endl;
  }
}




void GSState::SetTex2(int ctx, uint64_t value) {
  // Partial update — only modify PSM, CBP, CPSM, CSM of existing TEX0
  tex0[ctx].psm  = (value >> 20) & 0x3F;
  tex0[ctx].cbp  = ((value >> 37) & 0x3FFF) * 256;
  tex0[ctx].cpsm = (value >> 51) & 0xF;
  tex0[ctx].csm  = (value >> 55) & 1;




  if (g_logFile.is_open()) {
      g_logFile << "[GS] TEX2[" << ctx << "]: PSM=" << tex0[ctx].psm
                << " CBP=0x" << std::hex << tex0[ctx].cbp << std::dec
                << " CPSM=" << tex0[ctx].cpsm
                << " CSM=" << tex0[ctx].csm << std::endl;
  }
}




void GSState::SetTexflush(uint64_t /*value*/) {
  texture_dirty = true;




  if (g_logFile.is_open()) {
      g_logFile << "[GS] TEXFLUSH: texture_dirty=true" << std::endl;
  }
}




void GSState::SetFba(int ctx, uint64_t value) {
  fba[ctx] = value & 1;




  if (g_logFile.is_open()) {
      g_logFile << "[GS] FBA[" << ctx << "]: " << fba[ctx] << std::endl;
  }
}




void GSState::SetPabe(uint64_t value) {
  pabe = value & 1;




  if (g_logFile.is_open()) {
      g_logFile << "[GS] PABE: " << pabe << std::endl;
  }
}




void GSState::SetFogcol(uint64_t value) {
  fogcol.r = value & 0xFF;
  fogcol.g = (value >> 8) & 0xFF;
  fogcol.b = (value >> 16) & 0xFF;




  if (g_logFile.is_open()) {
      g_logFile << "[GS] FOGCOL: RGB=(" << (int)fogcol.r << ","
                << (int)fogcol.g << "," << (int)fogcol.b << ")" << std::endl;
  }
}




void GSState::DumpState(std::ostream& out) {
  int ctx = context;
   out << "\n╔══════════════════ GS STATE DUMP ══════════════════╗\n";
  out << "║ Context: " << ctx << "\n";
  out << "╠═══════════════════════════════════════════════════╣\n";
   // Primitive info
  out << "║ PRIMITIVE\n";
  out << "║   Type: " << (int)prim_type << " (";
  switch(prim_type) {
      case GSPrimType::Point:         out << "POINT"; break;
      case GSPrimType::Line:          out << "LINE"; break;
      case GSPrimType::LineStrip:     out << "LINE_STRIP"; break;
      case GSPrimType::Triangle:      out << "TRIANGLE"; break;
      case GSPrimType::TriangleStrip: out << "TRI_STRIP"; break;
      case GSPrimType::TriangleFan:   out << "TRI_FAN"; break;
      case GSPrimType::Sprite:        out << "SPRITE"; break;
  }
  out << ")\n";
  out << "║   Gouraud=" << gouraud << " Texture=" << texture
      << " Fog=" << fog << " AlphaBlend=" << alpha_blend << "\n";
  out << "║   UseUV=" << use_uv << " (0=STQ, 1=UV)\n";
   // Current color
  out << "╠═══════════════════════════════════════════════════╣\n";
  out << "║ CURRENT COLOR: RGBA=(" << (int)r << "," << (int)g << ","
      << (int)b << "," << (int)a << ")\n";
  if (a == 0) {
      out << "║   ⚠️  ALPHA IS ZERO!\n";
  }
   // Texture coords
  out << "║ TEX COORDS: ST=(" << s << "," << t << ") Q=" << q << "\n";
  out << "║             UV=(" << u << "," << v << ")\n";
   // Framebuffer
  out << "╠═══════════════════════════════════════════════════╣\n";
  out << "║ FRAME[" << ctx << "]: VRAM=0x" << std::hex << frame[ctx].fbp
      << " W=" << std::dec << frame[ctx].fbw << " PSM=" << frame[ctx].psm << "\n";
   // Z Buffer
  out << "║ ZBUF[" << ctx << "]: VRAM=0x" << std::hex << zbuf[ctx].zbp
      << std::dec << " ZMSK=" << zbuf[ctx].zmsk << "\n";
   // Texture
  out << "║ TEX0[" << ctx << "]: ";
  if (texture) {
      out << "TBP=0x" << std::hex << tex0[ctx].tbp << std::dec
          << " Size=" << (1 << tex0[ctx].tw) << "x" << (1 << tex0[ctx].th)
          << " TFX=" << tex0[ctx].tfx << "\n";
  } else {
      out << "(disabled)\n";
  }
   // Scissor
  out << "║ SCISSOR[" << ctx << "]: (" << scissor[ctx].x0 << "," << scissor[ctx].y0
      << ") to (" << scissor[ctx].x1 << "," << scissor[ctx].y1 << ")\n";
   // Offset
  out << "║ XYOFFSET[" << ctx << "]: (" << offset[ctx].x << "," << offset[ctx].y << ")\n";
   // Test
  out << "╠═══════════════════════════════════════════════════╣\n";
  out << "║ TEST[" << ctx << "]:\n";
  out << "║   Alpha Test: " << (test[ctx].ate ? "ON" : "OFF");
  if (test[ctx].ate) {
      out << " Method=" << (int)test[ctx].atst << " Ref=" << (int)test[ctx].aref;
      out << " (";
      switch(test[ctx].atst) {
          case 0: out << "NEVER"; break;
          case 1: out << "ALWAYS"; break;
          case 2: out << "LESS"; break;
          case 3: out << "LEQUAL"; break;
          case 4: out << "EQUAL"; break;
          case 5: out << "GEQUAL"; break;
          case 6: out << "GREATER"; break;
          case 7: out << "NOTEQUAL"; break;
      }
      out << ")";
  }
  out << "\n";
  out << "║   Z Test: " << (test[ctx].zte ? "ON" : "OFF")
      << " Method=" << (int)test[ctx].ztst << "\n";
   // Alpha blending
  out << "║ ALPHA[" << ctx << "]: ";
  if (alpha_blend) {
      out << "A=" << (int)alpha[ctx].a << " B=" << (int)alpha[ctx].b
          << " C=" << (int)alpha[ctx].c << " D=" << (int)alpha[ctx].d
          << " FIX=" << (int)alpha[ctx].fix << "\n";
  } else {
      out << "(disabled)\n";
  }
   // Vertex buffer status
  out << "╠═══════════════════════════════════════════════════╣\n";
  out << "║ BUFFERS: Queue=" << vertex_queue.size()
      << " DrawBuffer=" << draw_buffer.size() << "\n";
   out << "╚═══════════════════════════════════════════════════╝\n\n";
}




void GSState::DiagnoseBatch(std::ostream& out) {
  if (draw_buffer.empty()) {
      out << "[DIAG] No vertices in draw buffer!\n";
      return;
  }
   // Analyze vertices
  float minX = FLT_MAX, maxX = -FLT_MAX;
  float minY = FLT_MAX, maxY = -FLT_MAX;
  int zeroAlphaCount = 0;
   for (const auto& v : draw_buffer) {
      minX = std::min(minX, v.x);
      maxX = std::max(maxX, v.x);
      minY = std::min(minY, v.y);
      maxY = std::max(maxY, v.y);
      if (v.a == 0) zeroAlphaCount++;
  }
   out << "╔══════════════════ BATCH ANALYSIS ═════════════════╗\n";
  out << "║ Vertex Count: " << draw_buffer.size() << "\n";
  out << "║ Bounds: X[" << minX << " to " << maxX << "] "
      << "Y[" << minY << " to " << maxY << "]\n";
   // Check for issues
  int ctx = context;
   // Scissor check
  if (maxX < scissor[ctx].x0 || minX > scissor[ctx].x1 ||
      maxY < scissor[ctx].y0 || minY > scissor[ctx].y1) {
      out << "║ ❌ ALL GEOMETRY OUTSIDE SCISSOR!\n";
  }
   // Alpha check
  if (zeroAlphaCount == (int)draw_buffer.size()) {
      out << "║ ⚠️  ALL VERTICES HAVE ALPHA=0\n";
      if (!alpha_blend) {
          out << "║    (Alpha blending OFF - vertex alpha may not matter)\n";
      }
      if (test[ctx].ate) {
          out << "║    Alpha test ON: ATST=" << (int)test[ctx].atst
              << " AREF=" << (int)test[ctx].aref << "\n";
          if (test[ctx].atst == 0) {
              out << "║    ❌ ATST=NEVER - ALL PIXELS WILL FAIL!\n";
          } else if (test[ctx].atst == 5 && test[ctx].aref > 0) {
              out << "║    ❌ ATST=GEQUAL with AREF>0 - Zero alpha will FAIL!\n";
          } else if (test[ctx].atst == 6 && test[ctx].aref == 0) {
              out << "║    ❌ ATST=GREATER with AREF=0 - Zero alpha will FAIL!\n";
          }
      }
  }
   // Texture check
  if (texture && tex0[ctx].tbp == 0 && tex0[ctx].tbw == 0) {
      out << "║ ⚠️  Texturing ON but TEX0 not configured!\n";
  }
   // Frame buffer check
  if (frame[ctx].fbw == 0) {
      out << "║ ❌ FRAME buffer width is 0!\n";
  }
   // Show first 3 vertices
  out << "╠═══════════════════════════════════════════════════╣\n";
  out << "║ First vertices:\n";
  for (size_t i = 0; i < std::min((size_t)3, draw_buffer.size()); i++) {
      const auto& v = draw_buffer[i];
      out << "║   V" << i << ": (" << v.x << "," << v.y << "," << v.z << ")"
          << " RGBA=(" << (int)v.r << "," << (int)v.g << "," << (int)v.b << "," << (int)v.a << ")";
      if (use_uv) {
          out << " UV=(" << v.s << "," << v.t << ")";
      } else {
          out << " STQ=(" << v.s << "," << v.t << "," << v.q << ")";
      }
      out << "\n";
  }
  out << "╚═══════════════════════════════════════════════════╝\n\n";
}




void GSState::SetBitBltBuf(uint64_t value) {
  transfer.sbp  = (value & 0x3FFF) * 256;           // Source base (byte addr)
  transfer.sbw  = ((value >> 16) & 0x3F) * 64;      // Source width (pixels)
  transfer.spsm = (value >> 24) & 0x3F;             // Source format
  transfer.dbp  = ((value >> 32) & 0x3FFF) * 256;   // Dest base (byte addr)
  transfer.dbw  = ((value >> 48) & 0x3F) * 64;      // Dest width (pixels)
  transfer.dpsm = (value >> 56) & 0x3F;             // Dest format
   if (g_logFile.is_open()) {
      g_logFile << "[GS] BITBLTBUF: SRC=0x" << std::hex << transfer.sbp
                << " (" << std::dec << transfer.sbw << "px, PSM=" << transfer.spsm << ")"
                << " → DST=0x" << std::hex << transfer.dbp
                << " (" << std::dec << transfer.dbw << "px, PSM=" << transfer.dpsm << ")"
                << std::endl;
  }
}




void GSState::SetTrxPos(uint64_t value) {
  transfer.ssax = value & 0x7FF;
  transfer.ssay = (value >> 16) & 0x7FF;
  transfer.dsax = (value >> 32) & 0x7FF;
  transfer.dsay = (value >> 48) & 0x7FF;
  transfer.dir  = (value >> 59) & 0x3;
   if (g_logFile.is_open()) {
      g_logFile << "[GS] TRXPOS: SRC(" << transfer.ssax << "," << transfer.ssay << ")"
                << " → DST(" << transfer.dsax << "," << transfer.dsay << ")"
                << " DIR=" << (int)transfer.dir << std::endl;
  }
}




void GSState::SetTrxReg(uint64_t value) {
  transfer.rrw = value & 0xFFF;
  transfer.rrh = (value >> 32) & 0xFFF;
   if (g_logFile.is_open()) {
      g_logFile << "[GS] TRXREG: " << transfer.rrw << "x" << transfer.rrh
                << " (" << (transfer.rrw * transfer.rrh) << " pixels)" << std::endl;
  }
}




void GSState::SetTrxDir(uint64_t value) {
  transfer.xdir = value & 0x3;
   const char* dir_names[] = {"Host→VRAM", "VRAM→Host", "VRAM→VRAM", "Deactivated"};
   if (g_logFile.is_open()) {
      g_logFile << "[GS] TRXDIR: " << dir_names[transfer.xdir] << std::endl;
    
      if (transfer.xdir == 0) {
          g_logFile << "[GS] *** TEXTURE UPLOAD STARTED ***" << std::endl;
          g_logFile << "[GS]     Dest VRAM: 0x" << std::hex << transfer.dbp << std::dec << std::endl;
          g_logFile << "[GS]     Position: (" << transfer.dsax << "," << transfer.dsay << ")" << std::endl;
          g_logFile << "[GS]     Size: " << transfer.rrw << "x" << transfer.rrh << std::endl;
          g_logFile << "[GS]     Format: PSM=" << transfer.dpsm << std::endl;
      }
  }
   // Initialize transfer position
  if (transfer.xdir == 0) {
      transfer.cur_x = transfer.dsax;
      transfer.cur_y = transfer.dsay;
      transfer.active = true;
      transfer.pixels_written = 0;
  }
}




int GSState::GetPixelSize(uint8_t psm) {
  switch (psm) {
      case 0x00: return 4;  // PSMCT32
      case 0x01: return 4;  // PSMCT24 (stored as 32-bit)
      case 0x02: return 2;  // PSMCT16
      case 0x0A: return 2;  // PSMCT16S
      case 0x13: return 1;  // PSMT8
      case 0x14: return 1;  // PSMT4
      case 0x30: return 4;  // PSMZ32
      case 0x31: return 4;  // PSMZ24
      case 0x32: return 2;  // PSMZ16
      case 0x3A: return 2;  // PSMZ16S
      default: return 4;
  }
}




void GSState::WriteHWReg(uint64_t value) {
  if (!transfer.active || transfer.xdir != 0) return;
   int pixels_per_write;
  switch (transfer.dpsm) {
      case 0x00: case 0x01: pixels_per_write = 2; break;  // 32-bit: 2 pixels per 64-bit
      case 0x02: case 0x0A: pixels_per_write = 4; break;  // 16-bit: 4 pixels per 64-bit
      case 0x13: pixels_per_write = 8; break;              // 8-bit: 8 pixels per 64-bit
      case 0x14: pixels_per_write = 16; break;             // 4-bit: 16 pixels per 64-bit
      default: pixels_per_write = 2; break;
  }
   for (int p = 0; p < pixels_per_write; p++) {
      // Wrap X coordinate
      if (transfer.cur_x >= transfer.dsax + transfer.rrw) {
          transfer.cur_x = transfer.dsax;
          transfer.cur_y++;
      }
    
      // Check if transfer complete
      if (transfer.cur_y >= transfer.dsay + transfer.rrh) {
          if (transfer.active) {
              g_logFile << "[GS] *** TEXTURE UPLOAD COMPLETE ***" << std::endl;
              g_logFile << "[GS]     Pixels written: " << transfer.pixels_written << std::endl;
              transfer.active = false;


              // Task 9.2: Push InvalidateTextures job for the destination VRAM range
              uint32_t dest_base = transfer.dbp;
              uint32_t dest_size = (uint32_t)transfer.rrw * transfer.rrh * GetPixelSize(transfer.dpsm);
              RenderJob inv_job;
              inv_job.type = RenderCommandType::InvalidateTextures;
              inv_job.args.arg1 = dest_base;
              inv_job.args.arg2 = dest_size;
              g_renderQueue.Push(std::move(inv_job));


              // Task 9.3: Track written VRAM pages
              {
                  std::lock_guard<std::mutex> lock(g_vram_pages_mutex);
                  uint32_t page_start = (dest_base / PAGE_SIZE) * PAGE_SIZE;
                  uint32_t page_end = dest_base + dest_size;
                  for (uint32_t page = page_start; page < page_end; page += PAGE_SIZE) {
                      g_vram_written_pages.insert(page % (uint32_t)VRAM_SIZE);
                  }
              }
          }
          return;
      }
    
      // Calculate linear VRAM address (simplified - real PS2 uses block swizzling)
      uint32_t addr = transfer.dbp + (transfer.cur_y * transfer.dbw + transfer.cur_x) * GetPixelSize(transfer.dpsm);
    
      if (addr >= VRAM_SIZE) {
          addr %= VRAM_SIZE;
      }
    
      // Write pixel based on format
      switch (transfer.dpsm) {
          case 0x00: { // PSMCT32
              uint32_t pixel = (value >> (p * 32)) & 0xFFFFFFFF;
              if (addr + 4 <= VRAM_SIZE) {
                  gs_vram[addr + 0] = (pixel >> 0) & 0xFF;
                  gs_vram[addr + 1] = (pixel >> 8) & 0xFF;
                  gs_vram[addr + 2] = (pixel >> 16) & 0xFF;
                  gs_vram[addr + 3] = (pixel >> 24) & 0xFF;
              }
              break;
          }
          case 0x01: { // PSMCT24
              uint32_t pixel = (value >> (p * 32)) & 0xFFFFFF;
              if (addr + 4 <= VRAM_SIZE) {
                  gs_vram[addr + 0] = (pixel >> 0) & 0xFF;
                  gs_vram[addr + 1] = (pixel >> 8) & 0xFF;
                  gs_vram[addr + 2] = (pixel >> 16) & 0xFF;
                  gs_vram[addr + 3] = 0x80;
              }
              break;
          }
          case 0x02: case 0x0A: { // PSMCT16/16S
              uint16_t pixel = (value >> (p * 16)) & 0xFFFF;
              if (addr + 2 <= VRAM_SIZE) {
                  gs_vram[addr + 0] = pixel & 0xFF;
                  gs_vram[addr + 1] = (pixel >> 8) & 0xFF;
              }
              break;
          }
          case 0x13: { // PSMT8
              uint8_t pixel = (value >> (p * 8)) & 0xFF;
              if (addr < VRAM_SIZE) {
                  gs_vram[addr] = pixel;
              }
              break;
          }
          default:
              break;
      }
    
      transfer.cur_x++;
      transfer.pixels_written++;
  }
   // Log progress periodically
  static size_t last_log = 0;
  if (transfer.pixels_written - last_log >= 50000) {
      g_logFile << "[GS] Transfer progress: " << transfer.pixels_written
                << "/" << (transfer.rrw * transfer.rrh) << std::endl;
      last_log = transfer.pixels_written;
  }
}




void GSState::DumpVRAMRegion(uint32_t base, int width, int height, uint8_t psm) {
  g_logFile << "[GS] VRAM Check @ 0x" << std::hex << base << std::dec << std::endl;
   int pixel_size = GetPixelSize(psm);
  int non_zero = 0;
  int sample_count = std::min(256, width * height);
   for (int i = 0; i < sample_count; i++) {
      uint32_t addr = base + i * pixel_size;
      if (addr < VRAM_SIZE) {
          for (int b = 0; b < pixel_size; b++) {
              if (gs_vram[addr + b] != 0) {
                  non_zero++;
                  break;
              }
          }
      }
  }
   g_logFile << "[GS]   Non-zero pixels in first " << sample_count << ": " << non_zero << std::endl;
   // Show first pixel
  if (base + 4 <= VRAM_SIZE) {
      g_logFile << "[GS]   First pixel: RGBA=("
                << (int)gs_vram[base] << ","
                << (int)gs_vram[base+1] << ","
                << (int)gs_vram[base+2] << ","
                << (int)gs_vram[base+3] << ")" << std::endl;
  }
}