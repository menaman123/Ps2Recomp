#pragma once

#include "cpu_state.h"
#include <cstdint>
#include <map>
#include <string>
#include <SDL.h> // Include the main SDL header
#include "rabbitizer.hpp"
#include "instructions/InstructionR5900.hpp"
#include "ps2_scheduler.h"
#include "sema.h"
#include "intc.h"



struct SifDmaTransfer_t {
    uint32_t src;   
    uint32_t dest;
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
void sceSifDmaStat(CpuContext& ctx);
void SetOsdConfigParam(CpuContext& ctx);
void CreateSema(CpuContext& ctx);
void DeleteSema(CpuContext& ctx);
void SignalSema(CpuContext& ctx);
void WaitSema(CpuContext& ctx);
void PollSema(CpuContext& ctx);
void ReferSemaStatus(CpuContext& ctx);
void InitMainThread(CpuContext& ctx);
void InitHeap(CpuContext& ctx);
void _Exit(CpuContext& ctx);
void SetSysCall(CpuContext& ctx);
void CreateEventFlag(CpuContext& ctx);
void DeleteEventFlag(CpuContext& ctx); 
void SetEventFlag(CpuContext& ctx);
void ClearEventFlag(CpuContext& ctx);
void WaitEventFlag(CpuContext& ctx);
void enableDispatchThread(CpuContext& ctx);
void FlushCache(CpuContext& ctx);
void dynamic_decode_and_execute(uint32_t address, CpuContext& ctx);
void handle_branch_logic(const rabbitizer::InstructionR5900& instr, CpuContext& ctx, bool& exit_interpreter);
void execute_single_instruction(const rabbitizer::InstructionR5900& instr, CpuContext& ctx);
void AddIntcHandler(CpuContext& ctx);
void handle_mtc0_write(CpuContext& ctx, uint8_t rd, uint32_t value);
void syscall_EnableIntc(CpuContext& ctx);
void syscall_DisableIntc(CpuContext& ctx);
void _DisableDmac(CpuContext& ctx);
void CreateThread(CpuContext& ctx);
void Syscall_GetOsdConfigParam(CpuContext& ctx);
void sceSifSetDChain(CpuContext& ctx);
void AddDmacHandler(CpuContext& ctx);
void SysRemoveDmacHandler(CpuContext& ctx);
void _EnableDmac(CpuContext& ctx);
void sceSifSetReg(CpuContext& ctx);
void sceSifGetReg(CpuContext& ctx);
void sceSifStopDma(CpuContext& ctx);
void GsPutIMR(CpuContext& ctx);
void GsGetIMR(CpuContext& ctx);
void SetGsCrt(CpuContext& ctx);
void GetMemorySize(CpuContext& ctx);
void HLE_Deci2Call(CpuContext& ctx);
// The main dispatcher function
void runtime_syscall_dispatcher(uint32_t syscall_num, CpuContext& ctx);