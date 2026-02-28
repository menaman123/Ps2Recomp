#pragma once
#include "render.h"
#include <cstdint>
#include <vector>

namespace TextureDecoder{
    std::vector<uint8_t> Decode(const BatchTextureInfo& info);

    std::vector<uint8_t> DecodePSMCT32(const uint8_t* vram, uint32_t vram_base, uint32_t tbp, uint32_t tbw, uint32_t width, uint32_t height);
    std::vector<uint8_t> DecodePSMCT16(const uint8_t* vram, uint32_t vram_base, uint32_t tbp, uint32_t tbw, uint32_t width, uint32_t height);
    std::vector<uint8_t> DecodePSMCT8(const uint8_t* vram, uint32_t vram_base, uint32_t tbp, uint32_t tbw, uint32_t width, uint32_t height);
    std::vector<uint8_t> DecodePSMCT4(const uint8_t* vram, uint32_t vram_base, uint32_t tbp, uint32_t tbw, uint32_t width, uint32_t height);

    uint32_t GetAddressPSMCT32(int x, int y, uint32_t tbp, uint32_t tbw);
    uint32_t GetAddressPSMCT16(int x, int y, uint32_t tbp, uint32_t tbw);
    uint32_t GetAddressPSMCT8(int x, int y, uint32_t tbp, uint32_t tbw);
    uint32_t GetAddressPSMCT4(int x, int y, uint32_t tbp, uint32_t tbw);

    void ReadCLUT32(const uint8_t* vram, uint32_t vram_base, uint32_t cbp, uint32_t csm, int count, uint32_t* out_rgb);
    void ReadCLUT16(const uint8_t* vram, uint32_t vram_base, uint32_t cbp, uint32_t csm, int count, uint32_t* out_rgb);

    uint32_t Convert16To32(uint16_t pixel);
}



