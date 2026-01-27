#pragma once
#include "cpu_state.h"
#include <cstdint>
#include <map>

class HLEHeap {
private:
    uint32_t heap_start;
    uint32_t heap_end;
    uint32_t current_ptr;
    std::map<uint32_t, uint32_t> allocations;  // ptr -> size

public:
    void initialize(uint32_t start, uint32_t size) {
        heap_start = start;
        heap_end = start + size;
        current_ptr = start;
        allocations.clear();
    }

    uint32_t alloc(uint32_t size) {
        // Align to 16 bytes (PS2 quadword alignment)
        size = (size + 15) & ~15;
        
        if (current_ptr + size > heap_end) {
            return 0;  // Out of memory
        }
        
        uint32_t ptr = current_ptr;
        allocations[ptr] = size;
        current_ptr += size;
        return ptr;  // This is an address within main_memory
    }

    void free(uint32_t ptr) {
        // Simple implementation - doesn't reclaim memory
        // (Fine for most games that don't heavily free)
        allocations.erase(ptr);
    }
};

void HLE_001815c0(CpuContext& ctx);
void HLE_001815f0(CpuContext& ctx);
extern HLEHeap g_heap;