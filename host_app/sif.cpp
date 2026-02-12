#include "ps2_scheduler.h"
#include <cstring>
#include <iostream>
#include "memory.h"
#include "sif.h"

void SifCompletePendingRpcs() {
    // Signal all pending RPC completion semaphores
    for (auto& rpc : g_sif.pending_rpcs) {
        if (rpc.active && rpc.completion_sema > 0) {
            g_logFile << "SIF HLE: Auto-signaling completion sema " << rpc.completion_sema << std::endl;
            g_scheduler.iSignalSema(rpc.completion_sema);
            rpc.active = false;
        }
    }
    g_sif.pending_rpcs.clear();
}