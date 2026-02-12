#pragma once
#include <cstdint>


struct GsRegs {
    uint64_t PMODE;
    uint64_t SMODE1;
    uint64_t SMODE2;
    uint64_t SRFSH;
    uint64_t SYNCH1;
    uint64_t SYNCH2;
    uint64_t SYNCV;
    uint64_t DISPFB1;
    uint64_t DISPLAY1;
    uint64_t DISPFB2;
    uint64_t DISPLAY2;
    uint64_t EXTBUF;
    uint64_t EXTDATA;
    uint64_t EXTWRITE;
    uint64_t BGCOLOR;
    uint64_t GS_CSR;
    uint64_t GS_IMR;
    uint64_t BUSDIR;
    uint64_t SIGLBLID;
};

struct VideoState {
    int display_mode = 0x02;  // Default NTSC
    bool interlaced = true;
    bool frame_mode = false;
    
    int GetWidth() const { return (display_mode == 0x03) ? 640 : 640; }
    int GetHeight() const { return (display_mode == 0x03) ? 512 : 448; }
    float GetRefreshRate() const { return (display_mode == 0x03) ? 50.0f : 59.94f; }
};

uint64_t ReadPrivilegedRegister(uint32_t address);
void WritePrivilegedRegister(uint32_t address, uint64_t value);
void WritePrivilegedUpper(uint32_t addr, uint32_t value);
void WritePrivilegedLower(uint32_t addr, uint32_t value);
void GS_Reset();

extern VideoState g_video_state;

extern GsRegs g_gs_regs;
extern VideoState g_video_state;