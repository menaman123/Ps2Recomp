#pragma once
#include "cpu_state.h"
#include <cstdint>
#include <map>
#include <iostream>

class HLEHeap {
private:
    uint32_t heap_start;
    uint32_t heap_end;
    uint32_t current_top; // The highest point the heap has ever reached
    uint32_t current_bottom; // For high allocations (if needed)
    
    // Map of Free Blocks: Address -> Size
    std::map<uint32_t, uint32_t> free_blocks;
    
    // Map of Active Allocations: Address -> Size (Used for free() lookups)
    std::map<uint32_t, uint32_t> allocations; 

public:
    void initialize(uint32_t start, uint32_t size) {
        heap_start = start;
        heap_end = start + size;
        current_top = start;
        current_bottom = heap_end; // Start at the very end of RAM
        
        // alignment for the top
        current_bottom = current_bottom & ~0xF; 

        allocations.clear();
        free_blocks.clear();
        printf("Heap Initialized: Bottom=0x%X - Top=0x%X\n", heap_start, heap_end);
    }

    uint32_t alloc(uint32_t size) {
        // 1. Align size to 16 bytes (PS2 requirement)
        size = (size + 15) & ~15;

        // 2. Check Free List (First-Fit Strategy)
        // We look for the first gap that is big enough.
        for (auto it = free_blocks.begin(); it != free_blocks.end(); ++it) {
            uint32_t block_addr = it->first;
            uint32_t block_size = it->second;

            if (block_size >= size) {
                // Found a gap!
                
                // Remove this free block record
                free_blocks.erase(it);

                // If the gap is bigger than we need, split it
                if (block_size > size) {
                    uint32_t remainder_addr = block_addr + size;
                    uint32_t remainder_size = block_size - size;
                    free_blocks[remainder_addr] = remainder_size;
                }

                // Record allocation
                allocations[block_addr] = size;
                return block_addr;
            }
        }

        // 3. No gap found, expand the heap (Bump Pointer)
        if (current_top + size > current_bottom) {
            printf("[HLE Heap] CRITICAL: Out of Memory! Requested %u, Gap=%u (top=0x%X, bottom=0x%X)\n", 
                size, current_bottom - current_top, current_top, current_bottom);
            return 0;
        }

        uint32_t ptr = current_top;
        allocations[ptr] = size;
        current_top += size;
        
        return ptr;
    }
    uint32_t alloc_high(uint32_t size) {
        // 1. Align size
        size = (size + 15) & ~15;

        // 2. Check for collision with the Low Heap
        if (current_bottom - size < current_top) {
             printf("[HLE Heap] CRITICAL: Heap Collision! Bottom meeting Top.\n");
             return 0;
        }

        // 3. Allocate downwards
        current_bottom -= size;
        uint32_t ptr = current_bottom;
        
        // 4. Record it so Free() works
        allocations[ptr] = size;
        
        printf("[HLE Heap] High Alloc: %d bytes at 0x%X\n", size, ptr);
        return ptr;
    }
    void free(uint32_t ptr) {
        // Find the allocation size
        auto it = allocations.find(ptr);
        if (it == allocations.end()) {
            printf("[HLE Heap] Warning: Attempted to free invalid pointer 0x%X\n", ptr);
            return;
        }

        uint32_t size = it->second;
        allocations.erase(it);

        // --- TOP COALESCING LOGIC ---
        // Check if the block being freed is at the very top of the heap.
        if (ptr + size == current_top) {
            // It is! We can just lower the ceiling directly.
            current_top = ptr;
            
            // Check if the NEW top is adjacent to another free block below it.
            // This handles the case where we free the top, and the block underneath was already free.
            trim_top(); 
        } else {
            // Not at the top, just add to the free list hole
            free_blocks[ptr] = size;

            // Merge neighbors to prevent fragmentation
            coalesce();
        }
    }

private:
    void trim_top() {
        // Look for a free block that ends exactly at the current_top.
        // If found, we can lower the current_top further and remove that free block record.
        
        if (free_blocks.empty()) return;

        // Since map is sorted by address, the highest address is at the end (rbegin).
        auto last_free = free_blocks.rbegin();
        uint32_t block_addr = last_free->first;
        uint32_t block_size = last_free->second;

        if (block_addr + block_size == current_top) {
            // Found one! Lower the top further.
            current_top = block_addr;
            
            // Remove from map (convert reverse_iterator to iterator for erase)
            // std::next(last_free).base() converts the reverse iterator correctly
            free_blocks.erase(std::next(last_free).base());
            
            // Recurse: See if we can trim even more (e.g., stacked free blocks)
            trim_top();
        }
    }

    void coalesce() {
        // Iterate through free blocks and merge adjacent ones
        for (auto it = free_blocks.begin(); it != free_blocks.end(); ) {
            auto next = std::next(it);
            if (next == free_blocks.end()) break;

            uint32_t current_addr = it->first;
            uint32_t current_size = it->second;
            uint32_t next_addr = next->first;
            uint32_t next_size = next->second;

            // Check if current block ends exactly where next block starts
            if (current_addr + current_size == next_addr) {
                // Merge them: Extend current block
                it->second += next_size;
                // Remove the next block
                free_blocks.erase(next);
                // Do not increment 'it' so we can check if the *new* big block merges with the *next* one
            } else {
                ++it;
            }
        }
    }
};

void HLE_001815c0(CpuContext& ctx);
void HLE_001815f0(CpuContext& ctx);
void HLE_002cf930(CpuContext& ctx);
void HLE_002c6ce0(CpuContext& ctx);
extern HLEHeap g_heap;