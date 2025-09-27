#pragma once

#include "cpu_state.h"
#include <cstdint>
#include <map>
#include <string>
#include <SDL.h> // Include the main SDL header



struct SifDmaTransfer_t {
    void* src;
    void* dest;
    int32_t size;
    int32_t attr;
};

struct EventFlag {
    uint32_t initial_bits;
    uint32_t current_bits;
    SDL_mutex* mutex;       // For thread-safe access to this struct
    SDL_cond* condition;    // For threads to wait on
};
extern std::map<int, EventFlag> g_eventFlags;
extern int g_nextEventFlagId;

void NotImplemented_Syscall(const char* name, CpuContext& ctx);

// Implemented Syscalls
void sceExit(CpuContext& ctx);
void sceWrite(CpuContext& ctx);
void sceOpen(CpuContext& ctx);
void sifRpcBind(CpuContext& ctx);
void sifRpcCall(CpuContext& ctx);
void sifSetRpcQueue(CpuContext& ctx);
void sceSifSetDma(CpuContext& ctx);
void iSetEventFlag(CpuContext& ctx);

void CreateEventFlag(CpuContext& ctx);
void DeleteEventFlag(CpuContext& ctx); 
void SetEventFlag(CpuContext& ctx);
void ClearEventFlag(CpuContext& ctx);
void WaitEventFlag(CpuContext& ctx);
void enableDispatchThread(CpuContext& ctx);

// The main dispatcher function
void runtime_syscall_dispatcher(uint32_t syscall_num, CpuContext& ctx);