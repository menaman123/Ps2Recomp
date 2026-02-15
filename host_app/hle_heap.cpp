#include "cpu_state.h"
#include "hle_heap.h"
#include "memory.h"
#include "gif.h"
#include <fstream>

HLEHeap g_heap;

extern std::ofstream g_logFile;

void HLE_001815c0(CpuContext& ctx) {
    uint32_t size = ctx.cpuRegs.GPR.r[4].UL[0]; // a0 is the ONLY parameter

    uint32_t ptr = 0;

    if (size < 0x10000) {
        ptr = g_heap.alloc_high(size);
    } else {
        ptr = g_heap.alloc(size);
    }

    if (ptr == 0) {
        printf("Malloc Failed! Size: %u\n", size);
        g_logFile << "Malloc Failed! Size: " << size << std::endl;
    }

    ctx.cpuRegs.GPR.r[2].UL[0] = ptr;
}

void HLE_001815f0(CpuContext& ctx) { // free
    uint32_t ptr = ctx.cpuRegs.GPR.r[4].UL[0];
    
    // Log the free
    printf("Free: freeing ptr 0x%X\n", ptr);
    g_logFile << "Free: freeing ptr 0x" << std::hex << ptr << std::endl;
    
    g_heap.free(ptr);
}

void HLE_002cf930(CpuContext& ctx) {
    uint32_t caller = ctx.cpuRegs.GPR.r[31].UL[0];  // return address
    
  
    uint32_t size_needed = ctx.cpuRegs.GPR.r[4].UL[0];
    printf("SysAlloc: size=%u (0x%X) caller=0x%X\n", size_needed, size_needed, caller);
    g_logFile << "SysAlloc: size=" << size_needed << " (0x" << std::hex << size_needed << ") caller=0x" << caller << std::endl;

    
    // Use the actual heap manager instead of a static counter
    // This allows freed blocks to be reused.
    uint32_t ptr = g_heap.alloc(size_needed);

    static int call_count = 0;
    call_count++;

    if (ptr == 0) { // Assuming g_heap.alloc returns 0 on failure
        printf("SysAlloc #%d: Out of Memory! Requested %d bytes\n", call_count, size_needed);
        g_logFile << "SysAlloc #" << call_count << ": Out of Memory! Requested " << size_needed << " bytes" << std::endl;
        ctx.cpuRegs.GPR.r[2].UL[0] = 0xFFFFFFFF; // Return -1 (Failure)
    } else {
        printf("SysAlloc #%d: Allocated %d bytes at 0x%X\n", call_count, size_needed, ptr);
        g_logFile << "SysAlloc #" << call_count << ": Allocated " << size_needed << " bytes at 0x" << std::hex << ptr << std::endl;
        ctx.cpuRegs.GPR.r[2].UL[0] = ptr;
    }
}


void HLE_002c6ce0(CpuContext& ctx) {
    uint32_t dest = ctx.cpuRegs.GPR.r[4].UL[0];
    uint32_t src  = ctx.cpuRegs.GPR.r[5].UL[0];
    uint32_t size = ctx.cpuRegs.GPR.r[6].UL[0];

    // Guard: block copies into IO register space
    if (dest >= 0x10000000 && dest < 0x10010000) {
        g_logFile << "[memcpy] BLOCKED IO dest=0x" << std::hex << dest
                  << " src=0x" << src << " size=0x" << size
                  << " ra=0x" << ctx.cpuRegs.GPR.r[31].UL[0] << std::endl;
        // Return dest (standard memcpy return)
        ctx.cpuRegs.GPR.r[2].UD[0] = static_cast<uint64_t>(dest);
        return;
    }

    if (size > 0) {
        uint8_t* dest_ptr = memory::translate_address(dest, size);
        uint8_t* src_ptr  = memory::translate_address(src, size);

        if (dest_ptr && src_ptr) {
            // Fast path: both pointers resolved, use native memcpy
            std::memmove(dest_ptr, src_ptr, size);  // memmove handles overlap safely
        } else {
            // Fallback: byte-by-byte through memory system
            g_logFile << "[memcpy] Slow path: dest=0x" << std::hex << dest
                      << " src=0x" << src << " size=0x" << size 
                      << " ra=0x" << ctx.cpuRegs.GPR.r[31].UL[0] << std::endl;
            for (uint32_t i = 0; i < size; i++) {
                uint8_t byte = memory::read<uint8_t>(src + i);
                memory::write<uint8_t>(dest + i, byte);
            }
        }
    }

    // Return dest pointer (standard memcpy behavior)
    ctx.cpuRegs.GPR.r[2].UD[0] = static_cast<uint64_t>(dest);
}