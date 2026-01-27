#include "cpu_state.h"
#include "hle_heap.h"

HLEHeap g_heap;

void HLE_001815c0(CpuContext& ctx) {  // malloc
    uint32_t size = ctx.cpuRegs.GPR.r[4].UL[0];
    uint32_t ptr = g_heap.alloc(size);
    ctx.cpuRegs.GPR.r[2].UL[0] = ptr;
}

void HLE_001815f0(CpuContext& ctx) {  // free
    uint32_t ptr = ctx.cpuRegs.GPR.r[4].UL[0];
    g_heap.free(ptr);
}