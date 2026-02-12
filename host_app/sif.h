#ifndef SIF_H
#define SIF_H

#include <cstdint>
#include <vector>

struct SifState{
    uint32_t mscom;
    uint32_t smcom;
    uint32_t msflag;
    uint32_t smflag;
    uint32_t ctl_reg;

    uint32_t sys_subaddr;   // 0x80000000
    uint32_t sys_mainaddr;  // 0x80000001
    uint32_t sys_rpcinit;   // 0x80000002

    struct PendingRpc {
        uint32_t client_data_addr;  // Address of client data structure
        int completion_sema;         // Semaphore to signal on completion
        bool active;
    };
    std::vector<PendingRpc> pending_rpcs;
    
    // Last DMA ID for sceSifDmaStat
    uint32_t last_dma_id;
};

extern SifState g_sif;
void SifCompletePendingRpcs();

#endif // SIF_H