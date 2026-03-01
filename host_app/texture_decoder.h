#pragma once
#include "render.h"
#include <cstdint>
#include <vector>


namespace TextureDecoder{
   std::vector<uint8_t> Decode(const BatchTextureInfo& info);


   std::vector<uint8_t> DecodePSMCT32(const uint8_t* vram, uint32_t vram_base, uint32_t tbp, uint32_t tbw, int width, int height);
   std::vector<uint8_t> DecodePSMCT16(const uint8_t* vram, uint32_t vram_base, uint32_t tbp, uint32_t tbw, int width, int height);
   std::vector<uint8_t> DecodePSMT8(const uint8_t* vram, uint32_t vram_base, uint32_t tbp, uint32_t tbw, int width, int height,
       const uint8_t* clut, uint32_t clut_base, uint32_t cbp, uint32_t cpsm, uint32_t csm);
   std::vector<uint8_t> DecodePSMT4(const uint8_t* vram, uint32_t vram_base, uint32_t tbp, uint32_t tbw, int width, int height,
       const uint8_t* clut, uint32_t clut_base, uint32_t cbp, uint32_t cpsm, uint32_t csm);


   uint32_t GetAddressPSMCT32(int x, int y, uint32_t tbp, uint32_t tbw);
   uint32_t GetAddressPSMCT16(int x, int y, uint32_t tbp, uint32_t tbw);
   uint32_t GetAddressPSMT8(int x, int y, uint32_t tbp, uint32_t tbw);
   uint32_t GetAddressPSMT4(int x, int y, uint32_t tbp, uint32_t tbw);


   void ReadCLUT32(const uint8_t* vram, uint32_t vram_base, uint32_t cbp, uint32_t csm, int count, uint32_t* out_rgba);
   void ReadCLUT16(const uint8_t* vram, uint32_t vram_base, uint32_t cbp, uint32_t csm, int count, uint32_t* out_rgba);


   uint32_t Expand16to32(uint16_t pixel);
}
