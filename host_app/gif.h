// gif.h
#pragma once
#include <cstdint>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <array>


enum class GIFPath : uint8_t {
    PATH1 = 1,  // VU1 XGKICK
    PATH2 = 2,  // VIF1 DIRECT
    PATH3 = 3   // GIF DMA
};

enum class GIFFormat : uint8_t {
    PACKED   = 0,
    REGLIST  = 1,
    IMAGE    = 2,
    DISABLED = 3
};

// GIF Register addresses (internal, not memory-mapped directly)
enum GSInternalReg : uint8_t {
    GS_PRIM      = 0x00,
    GS_RGBAQ     = 0x01,
    GS_ST        = 0x02,
    GS_UV        = 0x03,
    GS_XYZF2     = 0x04,  // Vertex kick WITH fog
    GS_XYZ2      = 0x05,  // Vertex kick
    GS_TEX0_1    = 0x06,
    GS_TEX0_2    = 0x07,
    GS_CLAMP_1   = 0x08,
    GS_CLAMP_2   = 0x09,
    GS_FOG       = 0x0A,
    GS_XYZF3     = 0x0C,  // Vertex NO draw
    GS_XYZ3      = 0x0D,  // Vertex NO draw
    GS_AD        = 0x0E,  // Address + Data (write to any register)
    GS_NOP       = 0x0F,
    // ... many more
    GS_TEX1_1    = 0x14,
    GS_TEX1_2    = 0x15,
    GS_TEX2_1    = 0x16,
    GS_TEX2_2    = 0x17,
    GS_XYOFFSET_1= 0x18,
    GS_XYOFFSET_2= 0x19,
    GS_PRMODECONT= 0x1A,
    GS_PRMODE    = 0x1B,
    GS_TEXCLUT   = 0x1C,
    GS_SCANMSK   = 0x22,
    GS_MIPTBP1_1 = 0x34,
    GS_MIPTBP1_2 = 0x35,
    GS_MIPTBP2_1 = 0x36,
    GS_MIPTBP2_2 = 0x37,
    GS_TEXA      = 0x3B,
    GS_FOGCOL    = 0x3D,
    GS_TEXFLUSH  = 0x3F,
    GS_SCISSOR_1 = 0x40,
    GS_SCISSOR_2 = 0x41,
    GS_ALPHA_1   = 0x42,
    GS_ALPHA_2   = 0x43,
    GS_DIMX      = 0x44,
    GS_DTHE      = 0x45,
    GS_COLCLAMP  = 0x46,
    GS_TEST_1    = 0x47,
    GS_TEST_2    = 0x48,
    GS_PABE      = 0x49,
    GS_FBA_1     = 0x4A,
    GS_FBA_2     = 0x4B,
    GS_FRAME_1   = 0x4C,
    GS_FRAME_2   = 0x4D,
    GS_ZBUF_1    = 0x4E,
    GS_ZBUF_2    = 0x4F,
    GS_BITBLTBUF = 0x50,
    GS_TRXPOS    = 0x51,
    GS_TRXREG    = 0x52,
    GS_TRXDIR    = 0x53,
    GS_HWREG     = 0x54,
    GS_SIGNAL    = 0x60,
    GS_FINISH    = 0x61,
    GS_LABEL     = 0x62,
};

struct GIFTag {
    uint16_t nloop;      // Number of loop iterations
    bool     eop;        // End of packet
    bool     pre;        // PRIM field enable
    uint16_t prim;       // PRIM register data (11 bits)
    GIFFormat flg;       // Data format
    uint8_t  nregs;      // Number of registers (0 = 16)
    uint8_t  regs[16];   // Register descriptors
    
    void Parse(uint64_t lo, uint64_t hi) {
        nloop = lo & 0x7FFF;
        eop   = (lo >> 15) & 1;
        pre   = (lo >> 46) & 1;
        prim  = (lo >> 47) & 0x7FF;
        flg   = static_cast<GIFFormat>((lo >> 58) & 0x3);
        nregs = (lo >> 60) & 0xF;
        if (nregs == 0) nregs = 16;
        
        // Unpack register descriptors from hi
        for (int i = 0; i < 16; i++) {
            regs[i] = (hi >> (i * 4)) & 0xF;
        }
    }
};

// What gets queued for the graphics thread
struct GIFPacket {
    GIFPath path;
    std::vector<uint8_t> data;  // Raw packet data including GIFtag
};

class GIF {
public:
    // I/O Registers
    uint32_t ctrl = 0;   // 0x10003000 - Control
    uint32_t mode = 0;   // 0x10003010 - Mode
    uint32_t stat = 0;   // 0x10003020 - Status (read-only mostly)
    

    std::vector<uint8_t> dma_buffer;
    // FIFO (16 QW = 256 bytes)
    std::array<uint8_t, 256> fifo;
    int fifo_count = 0;
    
    // PATH3 masking
    bool path3_masked = false;
    
    // Current transfer state
    GIFTag current_tag;
    int current_loop = 0;
    int current_reg = 0;
    bool in_transfer = false;
    
    // Thread-safe queue to graphics thread
    std::queue<GIFPacket> packet_queue;
    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    
    void Reset();
    uint32_t Read(uint32_t addr);
    void Write(uint32_t addr, uint32_t value);
    
    // Called by DMA/VIF
    void ReceiveData(GIFPath path, const uint8_t* data, size_t size);
    
    // Process a complete GIF packet
    void ProcessPacket(GIFPath path, const uint8_t* data, size_t size);
    void ProcessBuffer();
    void FinishDMA();
    void FlushBatch();
    
private:
    void ProcessPacked(uint8_t reg, const uint8_t* data);
    void ProcessReglist(uint8_t reg, const uint8_t* data);
    void ProcessImage(const uint8_t* data, size_t qwords);
};

extern GIF g_gif;