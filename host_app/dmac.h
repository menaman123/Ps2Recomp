#pragma once
#include "cpu_state.h"
#include <cstdint>
#include <functional>
#include <vector>
#include <map>

struct DmacHandler {
    int id;              // Unique ID
    uint32_t handler_pc; // Handler function address
    uint32_t gp;         // Global Pointer
    uint32_t arg;        // Argument passed to handler (NEW)
    int flag;            // Options flag (NEW)
    bool active;
};

// Global queue of handlers, indexed by Channel ID (0-9)

// DMA Channel IDs
enum DMAChannelID {
    DMA_VIF0 = 0,
    DMA_VIF1 = 1,
    DMA_GIF  = 2,
    DMA_IPU0 = 3,
    DMA_IPU1 = 4,
    DMA_SIF0 = 5,  // IOP → EE (receive from IOP)
    DMA_SIF1 = 6,  // EE → IOP (send to IOP)
    DMA_SIF2 = 7,
    DMA_SPR0 = 8,
    DMA_SPR1 = 9,
    DMA_COUNT = 10
};

// D_CHCR bits
#define CHCR_DIR    (1 << 0)   // Direction: 0=to mem, 1=from mem
#define CHCR_MOD    (3 << 2)   // Mode: 0=normal, 1=chain, 2=interleave
#define CHCR_ASP    (3 << 4)   // Address stack pointer
#define CHCR_TTE    (1 << 6)   // Tag transfer enable
#define CHCR_TIE    (1 << 7)   // Tag interrupt enable
#define CHCR_STR    (1 << 8)   // Start/busy

// D_STAT bits - lower 16 bits are channel interrupt status
// Upper 16 bits are channel interrupt masks
#define DSTAT_CIS(ch)  (1 << (ch))        // Channel interrupt status
#define DSTAT_CIM(ch)  (1 << (16 + (ch))) // Channel interrupt mask

struct DMAChannelRegs {
    uint32_t chcr = 0;   // Channel control
    uint32_t madr = 0;   // Memory address
    uint32_t qwc = 0;    // Quadword count
    uint32_t tadr = 0;   // Tag address
    uint32_t asr0 = 0;   // Address stack 0
    uint32_t asr1 = 0;   // Address stack 1
    uint32_t sadr = 0;   // Scratchpad address
};

class DMAC {
public:
    DMAChannelRegs channels[DMA_COUNT];
    
    uint32_t ctrl = 0;    // D_CTRL - Global DMA control
    uint32_t stat = 0;    // D_STAT - Interrupt status/mask
    uint32_t pcr = 0;     // D_PCR  - Priority control
    uint32_t sqwc = 0;    // D_SQWC - Interleave size
    uint32_t rbsr = 0;    // D_RBSR - Ring buffer size (MFIFO)
    uint32_t rbor = 0;    // D_RBOR - Ring buffer offset (MFIFO)
    uint32_t stadr = 0;   // D_STADR - Stall address
    uint32_t enabler = 0; // D_ENABLER
    uint32_t enablew = 0; // D_ENABLEW
    
    void Reset();
    
    // Register access
    uint32_t Read(uint32_t addr);
    void Write(uint32_t addr, uint32_t value);
    // Channel operations
    void ProcessSifDma(int ch);
    void StartChannel(int ch);
    void CompleteChannel(int ch);  // Mark channel as done, raise interrupt
    void ProcessVifDmaChain(int ch);
    void ProcessVifDmaNormal(int ch);
    void ProcessGifDmaChain();
    void ProcessGifDmaNormal();
    void ProcessSprDma(int ch);
    void TransferToGIF(uint32_t addr, uint32_t qwc);


    
    // Interrupt handling
    bool CheckInterrupt();
    void DispatchInterrupt(CpuContext& ctx);
};

extern DMAC g_dmac;
extern int g_next_dmac_handler_id;
extern std::map<int, std::vector<DmacHandler>> g_dmac_queues;
int RemoveDmacHandler(int dma_cause, int handler_id);