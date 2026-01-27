// ee_timers.h
#pragma once
#include <cstdint>

struct EETimer {
    uint16_t count;   // TN_COUNT
    uint16_t mode;    // TN_MODE
    uint16_t comp;    // TN_COMP
    uint16_t hold;    // TN_HOLD (T0/T1 only)
    
    // Internal state
    uint64_t last_update_cycle;
    bool compare_flag;
    bool overflow_flag;
};

class EETimers {
public:
    EETimer timers[4];
    
    void Reset();
    
    // Register access
    uint32_t Read(uint32_t addr);
    void Write(uint32_t addr, uint32_t value);
    
    // Called every N cycles or on read
    void Update(uint64_t current_cycle);
    
    // Check for interrupts
    bool CheckInterrupt(int timer);
    
private:
    uint16_t GetClockDivisor(int timer);
};

extern EETimers g_ee_timers;