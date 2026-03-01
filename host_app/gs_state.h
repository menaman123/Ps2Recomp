#pragma once
#include <vector>
#include <cstdint>
#include <cstring>
#include <ostream>




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
  bool use_uv = false;  // FST: 0=STQ, 1=UV
  int strip_count = 0;
   // CONTEXT (0 or 1)
  int context = 0;




  // XYOFFSET - decoded 12.4 to float
  struct { float x = 0.0f, y = 0.0f; } offset[2];
   // SCISSOR
  struct { uint16_t x0 = 0, x1 = 0, y0 = 0, y1 = 0; } scissor[2];
   // FRAME register (where we draw)
  struct {
      uint32_t fbp = 0;    // Frame buffer pointer (byte address)
      uint32_t fbw = 0;    // Frame buffer width (pixels)
      uint32_t psm = 0;    // Pixel storage mode
      uint32_t fbmsk = 0;  // Write mask
  } frame[2];
   // ZBUF register
  struct {
      uint32_t zbp = 0;    // Z buffer pointer
      uint32_t psm = 0;    // Z format
      bool zmsk = false;   // Z write mask (1 = disabled)
  } zbuf[2];
   // TEX0 register (texture source)
  struct {
      uint32_t tbp = 0;    // Texture base pointer (byte address)
      uint32_t tbw = 0;    // Texture buffer width
      uint32_t psm = 0;    // Texture pixel format
      uint32_t tw = 0;     // Texture width (log2)
      uint32_t th = 0;     // Texture height (log2)
      uint32_t tcc = 0;    // Texture color component (0=RGB, 1=RGBA)
      uint32_t tfx = 0;    // Texture function
      uint32_t cbp = 0;    // CLUT base pointer
      uint32_t cpsm = 0;   // CLUT pixel format
      uint32_t csm = 0;    // CLUT storage mode
  } tex0[2];
   // TEX1 register (texture filtering)
  struct {
      bool lcm = false;    // LOD calculation method
      uint8_t mxl = 0;     // Maximum MIP level
      bool mmag = false;   // Magnification filter (0=nearest, 1=linear)
      uint8_t mmin = 0;    // Minification filter
      bool mtba = false;   // MIP base address specification
      uint8_t l = 0;       // LOD parameter L
      uint8_t k = 0;       // LOD parameter K
  } tex1[2];
   // TEST register
  struct {
      bool ate = false;    // Alpha test enable
      uint8_t atst = 0;    // Alpha test method
      uint8_t aref = 0;    // Alpha reference
      uint8_t afail = 0;   // Alpha fail action
      bool date = false;   // Destination alpha test
      bool datm = false;   // Destination alpha test mode
      bool zte = true;     // Z test enable
      uint8_t ztst = 1;    // Z test method (1 = GEQUAL default)
  } test[2];
   // ALPHA register (blending equation)
  struct {
      uint8_t a = 0, b = 1, c = 0, d = 1;
      uint8_t fix = 0;
  } alpha[2];
   // CLAMP register (texture wrapping)
  struct {
      uint8_t wms = 0;     // Wrap mode S
      uint8_t wmt = 0;     // Wrap mode T
      uint16_t minu = 0;   // Min U clamp
      uint16_t maxu = 0;   // Max U clamp
      uint16_t minv = 0;   // Min V clamp
      uint16_t maxv = 0;   // Max V clamp
  } clamp[2];




  // TEXA register (0x3B) — Texture alpha expansion
  struct {
      uint8_t ta0 = 0x00;   // Alpha value when alpha=0 in 16-bit textures
      bool aem = false;      // Alpha Expand Method (true = ta0 used only when RGB!=0)
      uint8_t ta1 = 0x80;   // Alpha value when alpha=1 in 16-bit textures
  } texa;




  // COLCLAMP register (0x46)
  bool colclamp = true;      // true=clamp, false=mask/wrap




  // PRMODECONT register (0x1A)
  bool prmodecont = true;    // true=use PRIM, false=use PRMODE




  // PRMODE register (0x1B) — stored raw value
  uint64_t prmode_raw = 0;




  // TEXFLUSH dirty flag
  bool texture_dirty = true; // true=re-snapshot VRAM on next flush




  // FBA register (0x4A/0x4B) — per-context
  bool fba[2] = {false, false};




  // PABE register (0x49)
  bool pabe = false;




  // FOGCOL register (0x3D)
  struct {
      uint8_t r = 0, g = 0, b = 0;
  } fogcol;




  struct {
      // BITBLTBUF
      uint32_t sbp = 0;    // Source base pointer
      uint32_t sbw = 0;    // Source buffer width
      uint32_t spsm = 0;   // Source pixel format
      uint32_t dbp = 0;    // Destination base pointer
      uint32_t dbw = 0;    // Destination buffer width
      uint32_t dpsm = 0;   // Destination pixel format
    
      // TRXPOS
      uint16_t ssax = 0;   // Source start X
      uint16_t ssay = 0;   // Source start Y
      uint16_t dsax = 0;   // Dest start X
      uint16_t dsay = 0;   // Dest start Y
      uint8_t dir = 0;     // Pixel order direction
    
      // TRXREG
      uint16_t rrw = 0;    // Transfer width
      uint16_t rrh = 0;    // Transfer height
    
      // TRXDIR
      uint8_t xdir = 3;    // 0=Host→Local, 1=Local→Host, 2=Local→Local, 3=Deactivated
    
      // Current position for HWREG writes
      uint16_t cur_x = 0;
      uint16_t cur_y = 0;
      bool active = false;
      size_t pixels_written = 0;
  } transfer;
   // Current vertex attributes
  uint8_t r = 0x80, g = 0x80, b = 0x80, a = 0x80;
  float s = 0.0f, t = 0.0f, q = 1.0f;
  uint16_t u = 0, v = 0;
  uint8_t fog_coef = 0;
   std::vector<GSVertex> vertex_queue;
  std::vector<GSVertex> draw_buffer;
   // Register setters
  void SetPrim(uint64_t value);
  void SetRGBAQ(uint64_t value);
  void SetST(uint64_t lo, uint64_t hi);
  void KickVertex(float x, float y, float z, uint8_t fog_val, bool draw);
  void FlushPrimitive();
  void SetScissor(int ctx, uint64_t value);
  void SetXYOffset(int ctx, uint64_t value);
  void SetFrame(int ctx, uint64_t value);
  void SetZBuf(int ctx, uint64_t value);
  void SetTex0(int ctx, uint64_t value);
  void SetTex1(int ctx, uint64_t value);
  void SetTest(int ctx, uint64_t value);
  void SetAlpha(int ctx, uint64_t value);
  void SetClamp(int ctx, uint64_t value);
  void SetTexa(uint64_t value);
  void SetColclamp(uint64_t value);
  void SetPrmodecont(uint64_t value);
  void SetPrmode(uint64_t value);
  void SetTex2(int ctx, uint64_t value);
  void SetTexflush(uint64_t value);
  void SetFba(int ctx, uint64_t value);
  void SetPabe(uint64_t value);
  void SetFogcol(uint64_t value);
  void SetBitBltBuf(uint64_t value);
  void SetTrxPos(uint64_t value);
  void SetTrxReg(uint64_t value);
  void SetTrxDir(uint64_t value);
  void WriteHWReg(uint64_t value);
  int GetPixelSize(uint8_t psm);
  void Flush();
   // Diagnostic helper
  void DumpState(std::ostream& out);
  void DiagnoseBatch(std::ostream& out);
  void DumpVRAMRegion(uint32_t base, int width, int height, uint8_t psm);
};




extern GSState g_gs_state;
extern std::vector<uint8_t> gs_vram; // 4MB GS VRAM


// [GS-XFER] Session-wide transfer counters (defined in gs_state.cpp)
extern uint32_t g_bitbltbuf_count;
extern uint32_t g_trxdir_count;
extern uint32_t g_hwreg_count;





