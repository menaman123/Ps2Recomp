#include "texture_decoder.h"
#include <cstdio>
#include <cstring>


namespace TextureDecoder {


// PS2 PSM format constants
static constexpr uint32_t PSM_PSMCT32 = 0x00;
static constexpr uint32_t PSM_PSMCT24 = 0x01;
static constexpr uint32_t PSM_PSMCT16 = 0x02;
static constexpr uint32_t PSM_PSMT8   = 0x13;
static constexpr uint32_t PSM_PSMT4   = 0x14;


// VRAM size: 4MB
static constexpr uint32_t VRAM_SIZE = 4 * 1024 * 1024;


// PSMCT32 block arrangement table (8 columns x 4 rows within a page)
static constexpr int block_table_PSMCT32[4][8] = {
   {  0,  1,  4,  5, 16, 17, 20, 21 },
   {  2,  3,  6,  7, 18, 19, 22, 23 },
   {  8,  9, 12, 13, 24, 25, 28, 29 },
   { 10, 11, 14, 15, 26, 27, 30, 31 }
};


// ---------------------------------------------------------------------------
// GetAddressPSMCT32
// ---------------------------------------------------------------------------
uint32_t GetAddressPSMCT32(int x, int y, uint32_t tbp, uint32_t tbw) {
   // PSMCT32: page = 64x32 pixels (8192 bytes)
   //          block = 8x8 pixels (256 bytes)
   //          column = 8x2 pixels (64 bytes)
   int page_x = x / 64;
   int page_y = y / 32;
   int pages_per_row = (tbw > 0) ? (tbw / 64) : 1;
   int page = page_y * pages_per_row + page_x;


   int local_x = x % 64;
   int local_y = y % 32;
   int block = block_table_PSMCT32[local_y / 8][local_x / 8];


   int column = (y % 8) / 2;
   int pixel_in_column = (x % 8) + ((y & 1) * 8);


   uint32_t addr = page * 8192 + block * 256 + column * 64 + pixel_in_column * 4;
   return (tbp + addr) % VRAM_SIZE;
}


// ---------------------------------------------------------------------------
// DecodePSMCT32
// ---------------------------------------------------------------------------
std::vector<uint8_t> DecodePSMCT32(
   const uint8_t* vram, uint32_t vram_base,
   uint32_t tbp, uint32_t tbw, int width, int height)
{
   std::vector<uint8_t> output(width * height * 4);


   for (int y = 0; y < height; y++) {
       for (int x = 0; x < width; x++) {
           uint32_t addr = GetAddressPSMCT32(x, y, tbp, tbw);


           // Compute offset within the snapshot
           // addr is absolute in VRAM space; snapshot starts at vram_base
           uint32_t offset;
           if (addr >= vram_base) {
               offset = addr - vram_base;
           } else {
               // Handle wrap-around: addr wrapped past 4MB boundary
               offset = (VRAM_SIZE - vram_base) + addr;
           }


           int dst = (y * width + x) * 4;


           // Read 32-bit RGBA pixel (little-endian: R, G, B, A)
           output[dst + 0] = vram[offset + 0]; // R
           output[dst + 1] = vram[offset + 1]; // G
           output[dst + 2] = vram[offset + 2]; // B
           output[dst + 3] = vram[offset + 3]; // A
       }
   }


   return output;
}


// ---------------------------------------------------------------------------
// Stub decoders for formats implemented in later tasks
// ---------------------------------------------------------------------------
std::vector<uint8_t> DecodePSMCT16(
   const uint8_t* vram, uint32_t vram_base,
   uint32_t tbp, uint32_t tbw, int width, int height)
{
   printf("[TextureDecoder] PSMCT16 decode not yet implemented\n");
   return {};
}


std::vector<uint8_t> DecodePSMT8(
   const uint8_t* vram, uint32_t vram_base,
   uint32_t tbp, uint32_t tbw, int width, int height,
   const uint8_t* clut, uint32_t clut_base,
   uint32_t cbp, uint32_t cpsm, uint32_t csm)
{
   printf("[TextureDecoder] PSMT8 decode not yet implemented\n");
   return {};
}


std::vector<uint8_t> DecodePSMT4(
   const uint8_t* vram, uint32_t vram_base,
   uint32_t tbp, uint32_t tbw, int width, int height,
   const uint8_t* clut, uint32_t clut_base,
   uint32_t cbp, uint32_t cpsm, uint32_t csm)
{
   printf("[TextureDecoder] PSMT4 decode not yet implemented\n");
   return {};
}


// ---------------------------------------------------------------------------
// Swizzle stubs for formats implemented in later tasks
// ---------------------------------------------------------------------------
uint32_t GetAddressPSMCT16(int x, int y, uint32_t tbp, uint32_t tbw) {
   // Stub — implemented in task 8.1
   return (tbp + (y * tbw + x) * 2) % VRAM_SIZE;
}


uint32_t GetAddressPSMT8(int x, int y, uint32_t tbp, uint32_t tbw) {
   // Stub — implemented in task 8.4
   return (tbp + y * tbw + x) % VRAM_SIZE;
}


uint32_t GetAddressPSMT4(int x, int y, uint32_t tbp, uint32_t tbw) {
   // Stub — implemented in task 8.5
   return (tbp + (y * tbw + x) / 2) % VRAM_SIZE;
}


// ---------------------------------------------------------------------------
// CLUT helper stubs
// ---------------------------------------------------------------------------
void ReadCLUT32(const uint8_t* vram, uint32_t vram_base,
               uint32_t cbp, uint32_t csm, int count,
               uint32_t* out_rgba)
{
   printf("[TextureDecoder] ReadCLUT32 not yet implemented\n");
}


void ReadCLUT16(const uint8_t* vram, uint32_t vram_base,
               uint32_t cbp, uint32_t csm, int count,
               uint32_t* out_rgba)
{
   printf("[TextureDecoder] ReadCLUT16 not yet implemented\n");
}


// ---------------------------------------------------------------------------
// Expand16to32 stub
// ---------------------------------------------------------------------------
uint32_t Expand16to32(uint16_t pixel) {
   // Stub — implemented in task 8.1
   uint8_t r = (pixel & 0x1F);
   uint8_t g = ((pixel >> 5) & 0x1F);
   uint8_t b = ((pixel >> 10) & 0x1F);
   uint8_t a = (pixel >> 15) & 1;


   r = (r << 3) | (r >> 2);
   g = (g << 3) | (g >> 2);
   b = (b << 3) | (b >> 2);
   a = a ? 255 : 0;


   return r | (g << 8) | (b << 16) | (a << 24);
}


// ---------------------------------------------------------------------------
// Decode — main dispatcher
// ---------------------------------------------------------------------------
std::vector<uint8_t> Decode(const BatchTextureInfo& info) {
   if (!info.enabled) {
       return {};
   }


   int width  = 1 << info.tw;
   int height = 1 << info.th;


   if (info.vram_snapshot.empty()) {
       printf("[TextureDecoder] Warning: empty VRAM snapshot for TBP=0x%X PSM=0x%X\n",
              info.tbp, info.psm);
       return {};
   }


   const uint8_t* vram = info.vram_snapshot.data();
   uint32_t vram_base = info.vram_snapshot_base;


   switch (info.psm) {
       case PSM_PSMCT32:
           return DecodePSMCT32(vram, vram_base,
                                info.tbp, info.tbw, width, height);


       case PSM_PSMCT16:
           return DecodePSMCT16(vram, vram_base,
                                info.tbp, info.tbw, width, height);


       case PSM_PSMT8:
           return DecodePSMT8(vram, vram_base,
                              info.tbp, info.tbw, width, height,
                              info.clut_snapshot.data(), info.clut_snapshot_base,
                              info.cbp, info.cpsm, info.csm);


       case PSM_PSMT4:
           return DecodePSMT4(vram, vram_base,
                              info.tbp, info.tbw, width, height,
                              info.clut_snapshot.data(), info.clut_snapshot_base,
                              info.cbp, info.cpsm, info.csm);


       default:
           printf("[TextureDecoder] Warning: unsupported PSM format 0x%02X\n", info.psm);
           return {};
   }
}


} // namespace TextureDecoder



