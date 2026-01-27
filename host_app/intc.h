#ifndef INTC_H
#define INTC_H

#include "cpu_state.h"
#include <vector>
#include <map>
#include <cstdint>

// INTC interrupt causes (bits in INTC_STAT/INTC_MASK)
enum IntcCause {
    INTC_GS      = 0,   // GS interrupt
    INTC_SBUS    = 1,   // SBUS
    INTC_VBON    = 2,   // VBLANK start
    INTC_VBOF    = 3,   // VBLANK end
    INTC_VIF0    = 4,
    INTC_VIF1    = 5,
    INTC_VU0     = 6,
    INTC_VU1     = 7,
    INTC_IPU     = 8,
    INTC_TIM0    = 9,   // Timer 0
    INTC_TIM1    = 10,  // Timer 1
    INTC_TIM2    = 11,  // Timer 2
    INTC_TIM3    = 12,  // Timer 3
    INTC_SFIFO   = 13,
    INTC_VU0WD   = 14,  // VU0 Watchdog
    INTC_COUNT   = 15
};

struct IntcHandler {
    int id;
    int cause;
    uint32_t handler_pc;
    uint32_t gp;
    uint32_t arg;
    int flag;
    bool active;
};

class INTC {
public:
    uint32_t stat = 0;  // INTC_STAT @ 0x1000F000
    uint32_t mask = 0;  // INTC_MASK @ 0x1000F010
    
    void Reset();
    
    // Register access (for memory-mapped I/O)
    uint32_t Read(uint32_t addr);
    void Write(uint32_t addr, uint32_t value);
    
    // Syscall helpers
    bool EnableIntc(int cause_bit);   // Returns old state
    bool DisableIntc(int cause_bit);  // Returns old state
    
    // Raise an interrupt (set bit in STAT)
    void RaiseInterrupt(int cause);
    
    // Check if INT0 should fire: (stat & mask) != 0
    bool CheckInterrupt();
    
    // Dispatch to registered handlers
    void DispatchInterrupt(CpuContext& ctx);
};

extern INTC g_intc;
extern std::map<int, std::vector<IntcHandler>> g_intc_queues;
extern int g_nextIntcId;

#endif // INTC_H