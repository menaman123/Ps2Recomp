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
// DecodePSMCT24 — same swizzle as PSMCT32, but alpha is forced to 0x80
// ---------------------------------------------------------------------------
std::vector<uint8_t> DecodePSMCT24(
   const uint8_t* vram, uint32_t vram_base,
   uint32_t tbp, uint32_t tbw, int width, int height)
{
   std::vector<uint8_t> output(width * height * 4);


   for (int y = 0; y < height; y++) {
       for (int x = 0; x < width; x++) {
           // PSMCT24 uses the same block layout as PSMCT32
           uint32_t addr = GetAddressPSMCT32(x, y, tbp, tbw);


           uint32_t offset;
           if (addr >= vram_base) {
               offset = addr - vram_base;
           } else {
               offset = (VRAM_SIZE - vram_base) + addr;
           }


           int dst = (y * width + x) * 4;


           // Read RGB from the 32-bit slot, ignore the stored alpha byte
           output[dst + 0] = vram[offset + 0]; // R
           output[dst + 1] = vram[offset + 1]; // G
           output[dst + 2] = vram[offset + 2]; // B
           output[dst + 3] = 0x80;             // Force opaque (PS2: 0x80 = 1.0)
       }
   }


   return output;
}










// PSMCT16 block arrangement table (4 columns x 8 rows within a page)
static constexpr int block_table_PSMCT16[8][4] = {
 {  0,  2,  8, 10 },
 {  1,  3,  9, 11 },
 {  4,  6, 12, 14 },
 {  5,  7, 13, 15 },
 { 16, 18, 24, 26 },
 { 17, 19, 25, 27 },
 { 20, 22, 28, 30 },
 { 21, 23, 29, 31 }
};








// ---------------------------------------------------------------------------
// GetAddressPSMCT16
// ---------------------------------------------------------------------------
uint32_t GetAddressPSMCT16(int x, int y, uint32_t tbp, uint32_t tbw) {
 // PSMCT16: page = 64x64 pixels (8192 bytes)
 //          block = 16x8 pixels (256 bytes)
 //          column = 16x2 pixels (64 bytes)
 int page_x = x / 64;
 int page_y = y / 64;
 int pages_per_row = (tbw > 0) ? (tbw / 64) : 1;
 int page = page_y * pages_per_row + page_x;




 int local_x = x % 64;
 int local_y = y % 64;
 int block = block_table_PSMCT16[local_y / 8][local_x / 16];




 int column = (y % 8) / 2;
 int pixel_in_column = (x % 16) + ((y & 1) * 16);




 uint32_t addr = page * 8192 + block * 256 + column * 64 + pixel_in_column * 2;
 return (tbp + addr) % VRAM_SIZE;
}








// ---------------------------------------------------------------------------
// DecodePSMCT16
// ---------------------------------------------------------------------------
std::vector<uint8_t> DecodePSMCT16(
 const uint8_t* vram, uint32_t vram_base,
 uint32_t tbp, uint32_t tbw, int width, int height)
{
 std::vector<uint8_t> output(width * height * 4);




 for (int y = 0; y < height; y++) {
     for (int x = 0; x < width; x++) {
         uint32_t addr = GetAddressPSMCT16(x, y, tbp, tbw);




         uint32_t offset;
         if (addr >= vram_base) {
             offset = addr - vram_base;
         } else {
             offset = (VRAM_SIZE - vram_base) + addr;
         }




         // Read 16-bit pixel (little-endian)
         uint16_t pixel = vram[offset] | (vram[offset + 1] << 8);
         uint32_t rgba = Expand16to32(pixel);




         int dst = (y * width + x) * 4;
         output[dst + 0] = rgba & 0xFF;         // R
         output[dst + 1] = (rgba >> 8) & 0xFF;  // G
         output[dst + 2] = (rgba >> 16) & 0xFF;  // B
         output[dst + 3] = (rgba >> 24) & 0xFF;  // A
     }
 }




 return output;
}








// PSMT8 block arrangement table (8 columns x 4 rows within a page)
static constexpr int block_table_PSMT8[4][8] = {
 {  0,  1,  4,  5, 16, 17, 20, 21 },
 {  2,  3,  6,  7, 18, 19, 22, 23 },
 {  8,  9, 12, 13, 24, 25, 28, 29 },
 { 10, 11, 14, 15, 26, 27, 30, 31 }
};




// PSMT4 block arrangement table (8 columns x 4 rows within a page)
static constexpr int block_table_PSMT4[8][4] = {
 {  0,  2,  8, 10 },
 {  1,  3,  9, 11 },
 {  4,  6, 12, 14 },
 {  5,  7, 13, 15 },
 { 16, 18, 24, 26 },
 { 17, 19, 25, 27 },
 { 20, 22, 28, 30 },
 { 21, 23, 29, 31 }
};








// ---------------------------------------------------------------------------
// CSM1 CLUT index swizzle
// In each group of 32 entries, indices 8-15 swap with 16-23
// ---------------------------------------------------------------------------
static uint32_t CSM1Swizzle(uint32_t index) {
 uint32_t within_group = index % 32;
 if (within_group >= 8 && within_group < 16) {
     return index + 8;
 } else if (within_group >= 16 && within_group < 24) {
     return index - 8;
 }
 return index;
}








// ---------------------------------------------------------------------------
// GetAddressPSMT8
// ---------------------------------------------------------------------------
uint32_t GetAddressPSMT8(int x, int y, uint32_t tbp, uint32_t tbw) {
 // PSMT8: page = 128x64 pixels (8192 bytes)
 //        block = 16x16 pixels (256 bytes)
 //        column = 16x4 pixels (64 bytes)
 int page_x = x / 128;
 int page_y = y / 64;
 int pages_per_row = (tbw > 0) ? (tbw / 128) : 1;
 int page = page_y * pages_per_row + page_x;




 int local_x = x % 128;
 int local_y = y % 64;
 int block = block_table_PSMT8[local_y / 16][local_x / 16];




 int column = (y % 16) / 4;
 int pixel_in_column = (x % 16) + ((y % 4) * 16);




 uint32_t addr = page * 8192 + block * 256 + column * 64 + pixel_in_column;
 return (tbp + addr) % VRAM_SIZE;
}








// ---------------------------------------------------------------------------
// GetAddressPSMT4
// ---------------------------------------------------------------------------
uint32_t GetAddressPSMT4(int x, int y, uint32_t tbp, uint32_t tbw) {
 // PSMT4: page = 128x128 pixels (8192 bytes)
 //        block = 32x16 pixels (256 bytes)
 //        column = 32x4 pixels (64 bytes)
 int page_x = x / 128;
 int page_y = y / 128;
 int pages_per_row = (tbw > 0) ? (tbw / 128) : 1;
 int page = page_y * pages_per_row + page_x;




 int local_x = x % 128;
 int local_y = y % 128;
 int block = block_table_PSMT4[local_y / 16][local_x / 32];




 int column = (y % 16) / 4;
 int pixel_in_column = (x % 32) + ((y % 4) * 32);




 // Each byte holds 2 pixels (4 bits each)
 uint32_t addr = page * 8192 + block * 256 + column * 64 + pixel_in_column / 2;
 return (tbp + addr) % VRAM_SIZE;
}








// ---------------------------------------------------------------------------
// ReadCLUT32 — read CLUT entries in PSMCT32 format with CSM1 swizzle
// ---------------------------------------------------------------------------
void ReadCLUT32(const uint8_t* vram, uint32_t vram_base,
             uint32_t cbp, uint32_t csm, int count,
             uint32_t* out_rgba)
{
 for (int i = 0; i < count; i++) {
     uint32_t idx = (csm == 0) ? CSM1Swizzle(i) : (uint32_t)i;
     uint32_t addr = (cbp + idx * 4) % VRAM_SIZE;




     uint32_t offset;
     if (addr >= vram_base) {
         offset = addr - vram_base;
     } else {
         offset = (VRAM_SIZE - vram_base) + addr;
     }




     out_rgba[i] = vram[offset]
                 | (vram[offset + 1] << 8)
                 | (vram[offset + 2] << 16)
                 | (vram[offset + 3] << 24);
 }
}








// ---------------------------------------------------------------------------
// ReadCLUT16 — read CLUT entries in PSMCT16 format with CSM1 swizzle
// ---------------------------------------------------------------------------
void ReadCLUT16(const uint8_t* vram, uint32_t vram_base,
             uint32_t cbp, uint32_t csm, int count,
             uint32_t* out_rgba)
{
 for (int i = 0; i < count; i++) {
     uint32_t idx = (csm == 0) ? CSM1Swizzle(i) : (uint32_t)i;
     uint32_t addr = (cbp + idx * 2) % VRAM_SIZE;




     uint32_t offset;
     if (addr >= vram_base) {
         offset = addr - vram_base;
     } else {
         offset = (VRAM_SIZE - vram_base) + addr;
     }




     uint16_t pixel = vram[offset] | (vram[offset + 1] << 8);
     out_rgba[i] = Expand16to32(pixel);
 }
}








// ---------------------------------------------------------------------------
// DecodePSMT8
// ---------------------------------------------------------------------------
std::vector<uint8_t> DecodePSMT8(
 const uint8_t* vram, uint32_t vram_base,
 uint32_t tbp, uint32_t tbw, int width, int height,
 const uint8_t* clut, uint32_t clut_base,
 uint32_t cbp, uint32_t cpsm, uint32_t csm)
{
 // Build CLUT lookup table (256 entries)
 uint32_t clut_rgba[256] = {};
 if (cpsm == PSM_PSMCT32) {
     ReadCLUT32(clut, clut_base, cbp, csm, 256, clut_rgba);
 } else if (cpsm == PSM_PSMCT16) {
     ReadCLUT16(clut, clut_base, cbp, csm, 256, clut_rgba);
 } else {
     printf("[TextureDecoder] Warning: unsupported CLUT format 0x%02X for PSMT8\n", cpsm);
     return {};
 }




 std::vector<uint8_t> output(width * height * 4);




 for (int y = 0; y < height; y++) {
     for (int x = 0; x < width; x++) {
         uint32_t addr = GetAddressPSMT8(x, y, tbp, tbw);




         uint32_t offset;
         if (addr >= vram_base) {
             offset = addr - vram_base;
         } else {
             offset = (VRAM_SIZE - vram_base) + addr;
         }




         uint8_t index = vram[offset];
         uint32_t rgba = clut_rgba[index];




         int dst = (y * width + x) * 4;
         output[dst + 0] = rgba & 0xFF;
         output[dst + 1] = (rgba >> 8) & 0xFF;
         output[dst + 2] = (rgba >> 16) & 0xFF;
         output[dst + 3] = (rgba >> 24) & 0xFF;
     }
 }




 return output;
}








// ---------------------------------------------------------------------------
// DecodePSMT4
// ---------------------------------------------------------------------------
std::vector<uint8_t> DecodePSMT4(
 const uint8_t* vram, uint32_t vram_base,
 uint32_t tbp, uint32_t tbw, int width, int height,
 const uint8_t* clut, uint32_t clut_base,
 uint32_t cbp, uint32_t cpsm, uint32_t csm)
{
 // Build CLUT lookup table (16 entries)
 uint32_t clut_rgba[16] = {};
 if (cpsm == PSM_PSMCT32) {
     ReadCLUT32(clut, clut_base, cbp, csm, 16, clut_rgba);
 } else if (cpsm == PSM_PSMCT16) {
     ReadCLUT16(clut, clut_base, cbp, csm, 16, clut_rgba);
 } else {
     printf("[TextureDecoder] Warning: unsupported CLUT format 0x%02X for PSMT4\n", cpsm);
     return {};
 }




 std::vector<uint8_t> output(width * height * 4);




 for (int y = 0; y < height; y++) {
     for (int x = 0; x < width; x++) {
         uint32_t addr = GetAddressPSMT4(x, y, tbp, tbw);




         uint32_t offset;
         if (addr >= vram_base) {
             offset = addr - vram_base;
         } else {
             offset = (VRAM_SIZE - vram_base) + addr;
         }




         // Two 4-bit indices per byte; low nibble is even pixel, high nibble is odd
         uint8_t byte_val = vram[offset];
         uint8_t index;
         int pixel_in_byte = (x % 32) + ((y % 4) * 32);
         if (pixel_in_byte & 1) {
             index = (byte_val >> 4) & 0x0F;
         } else {
             index = byte_val & 0x0F;
         }




         uint32_t rgba = clut_rgba[index];




         int dst = (y * width + x) * 4;
         output[dst + 0] = rgba & 0xFF;
         output[dst + 1] = (rgba >> 8) & 0xFF;
         output[dst + 2] = (rgba >> 16) & 0xFF;
         output[dst + 3] = (rgba >> 24) & 0xFF;
     }
 }




 return output;
}








// ---------------------------------------------------------------------------
// Expand16to32 — R5G5B5A1 to RGBA8888
// ---------------------------------------------------------------------------
uint32_t Expand16to32(uint16_t pixel) {
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


     case PSM_PSMCT24:
         return DecodePSMCT24(vram, vram_base,
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

