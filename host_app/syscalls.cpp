#include "syscalls.h"
#include "memory.h"
#include <iostream>
#include <fstream>
#include <cstring>

#ifdef _WIN32
#include <io.h>
#include <process.h>
#define write _write
#else
#include <unistd.h>
#endif

extern std::ofstream g_logFile;
std::map<int, EventFlag> g_eventFlags;
int g_nextEventFlagId = 1; // Start from ID 1

// A helper function to read a string from guest memory
std::string read_string_from_guest(uint32_t address) {
    return std::string(reinterpret_cast<char*>(memory::get_pointer(address)));
}

void NotImplemented_Syscall(const char* name, CpuContext& ctx) {
    g_logFile << "Syscall: " << name << " (Not Implemented)" << std::endl;
}

void CreateEventFlag(CpuContext& ctx) {
    // Need work on 
    uint32_t flag_params_addr = ctx.cpuRegs.GPR.r[4].UL[0]; // $a0: pointer to flag parameters
    
    int flag_id = g_nextEventFlagId++;
    EventFlag flag;
    flag.initial_bits = 0;
    flag.current_bits = 0;
    flag.mutex = SDL_CreateMutex();
    flag.condition = SDL_CreateCond();
    
    g_eventFlags[flag_id] = flag;
    
    g_logFile << "Syscall: CreateEventFlag() -> new id " << std::dec << flag_id << std::endl;
    ctx.cpuRegs.GPR.r[2].UD[0] = flag_id; // Return the new ID in $v0
}

void DeleteEventFlag(CpuContext& ctx) {
    int event_id = ctx.cpuRegs.GPR.r[4].SL[0]; // $a0
    g_logFile << "Syscall: DeleteEventFlag(id: " << std::dec << event_id << ")" << std::endl;

    auto it = g_eventFlags.find(event_id);
    if (it != g_eventFlags.end()) {
        SDL_DestroyMutex(it->second.mutex);
        SDL_DestroyCond(it->second.condition);
        g_eventFlags.erase(it);
        ctx.cpuRegs.GPR.r[2].UD[0] = 0; // Success
    } else {
        ctx.cpuRegs.GPR.r[2].UD[0] = -1; // Error
    }
}

void SetEventFlag(CpuContext& ctx) {
    int event_id = ctx.cpuRegs.GPR.r[4].SL[0];
    uint32_t bits_to_set = ctx.cpuRegs.GPR.r[5].UL[0];
    g_logFile << "Syscall: SetEventFlag(id: " << std::dec << event_id << ", bits: 0x" << std::hex << bits_to_set << ")" << std::endl;

    auto it = g_eventFlags.find(event_id);
    if (it != g_eventFlags.end()) {
        SDL_LockMutex(it->second.mutex);
        it->second.current_bits |= bits_to_set;
        SDL_CondBroadcast(it->second.condition); 
        SDL_UnlockMutex(it->second.mutex);
        ctx.cpuRegs.GPR.r[2].UD[0] = 0;
    } else {
        ctx.cpuRegs.GPR.r[2].UD[0] = -1;
    }
}

void ClearEventFlag(CpuContext& ctx) {
    int event_id = ctx.cpuRegs.GPR.r[4].SL[0];
    uint32_t bits_to_clear = ~ctx.cpuRegs.GPR.r[5].UL[0];
    g_logFile << "Syscall: ClearEventFlag(id: " << std::dec << event_id << ", bits: 0x" << std::hex << bits_to_clear << ")" << std::endl;
    
    auto it = g_eventFlags.find(event_id);
    if (it != g_eventFlags.end()) {
        SDL_LockMutex(it->second.mutex);
        it->second.current_bits &= bits_to_clear;
        SDL_UnlockMutex(it->second.mutex);
        ctx.cpuRegs.GPR.r[2].UD[0] = 0;
    } else {
        ctx.cpuRegs.GPR.r[2].UD[0] = -1;
    }
}

void WaitEventFlag(CpuContext& ctx) {
    int event_id = ctx.cpuRegs.GPR.r[4].SL[0];
    uint32_t wait_bits = ctx.cpuRegs.GPR.r[5].UL[0];
    int wait_mode = ctx.cpuRegs.GPR.r[6].SL[0];
    g_logFile << "Syscall: WaitEventFlag(id: " << std::dec << event_id << ", bits: 0x" << std::hex << wait_bits << ", mode: " << wait_mode << ")" << std::endl;
    
    auto it = g_eventFlags.find(event_id);
    if (it != g_eventFlags.end()) {
        SDL_LockMutex(it->second.mutex);
        while (true) {
            bool condition_met = false;
            if (wait_mode == 0) { // WEF_AND
                condition_met = (it->second.current_bits & wait_bits) == wait_bits;
            } else { // WEF_OR
                condition_met = (it->second.current_bits & wait_bits) != 0;
            }

            if (condition_met) {
                break; // Condition is met, exit the loop
            }

            // Condition not met, wait until another thread signals.
            SDL_CondWait(it->second.condition, it->second.mutex);
        }
        SDL_UnlockMutex(it->second.mutex);
        ctx.cpuRegs.GPR.r[2].UD[0] = 0; // Success
    } else {
        ctx.cpuRegs.GPR.r[2].UD[0] = -1; // Error
    }
}

void iSetEventFlag(CpuContext& ctx) {
    SetEventFlag(ctx);
}

void sifRpcBind(CpuContext& ctx) {
    uint32_t bd_addr = ctx.cpuRegs.GPR.r[4].UL[0];      // $a0: pointer to sceSifRpcData
    uint32_t rpc_number = ctx.cpuRegs.GPR.r[5].UL[0]; // $a1: RPC number
    uint32_t mode = ctx.cpuRegs.GPR.r[6].UL[0];         // $a2: mode

    g_logFile << "Syscall: sifRpcBind(bd: 0x" << std::hex << bd_addr
              << ", rpc_number: 0x" << rpc_number
              << ", mode: 0x" << mode << ") called!" << std::endl;

    // Return a success code (0) in $v0
    ctx.cpuRegs.GPR.r[2].UD[0] = 0;
}

void sifRpcCall(CpuContext& ctx) {
    uint32_t bd_addr = ctx.cpuRegs.GPR.r[4].UL[0];      // $a0
    uint32_t rpc_number = ctx.cpuRegs.GPR.r[5].UL[0]; // $a1
    uint32_t mode = ctx.cpuRegs.GPR.r[6].UL[0];         // $a2
    uint32_t send_addr = ctx.cpuRegs.GPR.r[7].UL[0];    // $a3

    g_logFile << "Syscall: sifRpcCall(bd: 0x" << std::hex << bd_addr
              << ", rpc_number: 0x" << rpc_number
              << ", mode: 0x" << mode
              << ", send: 0x" << send_addr << ") called!" << std::endl;

    // A real implementation would handle the RPC logic here.
    // For now, we just return a success code (0) in $v0.
    ctx.cpuRegs.GPR.r[2].UD[0] = 0;
}

void sifSetRpcQueue(CpuContext& ctx) {
    uint32_t qd_addr = ctx.cpuRegs.GPR.r[4].UL[0];
    uint32_t thread_id = ctx.cpuRegs.GPR.r[5].UL[0];

    g_logFile << "Syscall: sifSetRpcQueue(qd: 0x" << std::hex << qd_addr
              << ", thread_id: " << std::dec << thread_id << ") called!" << std::endl;
}

// Exit syscall
void sceExit(CpuContext& ctx) {
    int exit_code = static_cast<int>(ctx.cpuRegs.GPR.r[4].SL[0]); // $a0
    std::cout << "Syscall: exit(" << exit_code << ")" << std::endl;
    exit(exit_code);
}

// Write syscall
void sceWrite(CpuContext& ctx) {
    int fd = static_cast<int>(ctx.cpuRegs.GPR.r[4].SL[0]); // $a0
    uint32_t ptr = ctx.cpuRegs.GPR.r[5].UL[0]; // $a1
    int len = static_cast<int>(ctx.cpuRegs.GPR.r[6].SL[0]); // $a2

    std::string str = read_string_from_guest(ptr);
    std::cout << "Syscall: write(" << fd << ", \"" << str << "\", " << len << ")" << std::endl;

    if (fd == 1 || fd == 2) { // stdout or stderr
        write(fd, str.c_str(), len);
    }
    
    // Return the number of bytes written
    ctx.cpuRegs.GPR.r[2].SL[0] = len; // $v0
}


// Placeholder for open syscall
void sceOpen(CpuContext& ctx) {
    // Arguments: const char* filename, int flags
    uint32_t filename_ptr = ctx.cpuRegs.GPR.r[4].UL[0]; // $a0
    int flags = static_cast<int>(ctx.cpuRegs.GPR.r[5].SL[0]); // $a1
    
    std::string filename = read_string_from_guest(filename_ptr);
    std::cout << "System Call: sceOpen(filename: \"" << filename << "\", flags: " << flags << ") called!" << std::endl;
    
    // The return value (file descriptor) is placed in $v0
    ctx.cpuRegs.GPR.r[2].SL[0] = 0; // Return a dummy file descriptor for now
}

void _Exit(CpuContext& ctx)
{
    // The exit code is typically passed in register $a0, which is GPR 4.
    int exit_code = ctx.cpuRegs.GPR.r[4].SL[0];
    std::cout << "Syscall: _Exit called with code " << exit_code << std::endl;
    exit(exit_code);
}

void sceSifSetDma(CpuContext& ctx) {
    uint32_t sdt_addr = ctx.cpuRegs.GPR.r[4].UL[0];
    uint32_t count = ctx.cpuRegs.GPR.r[5].UL[0];
    g_logFile << "Syscall: sceSifSetDma(sdt_addr: 0x" << std::hex << sdt_addr << ", count: " << std::dec << count << ") called!" << std::endl;
    SifDmaTransfer_t* transfers = reinterpret_cast<SifDmaTransfer_t*>(memory::get_pointer(sdt_addr));
    for (uint32_t i = 0; i < count; ++i) {
        SifDmaTransfer_t& transfer = transfers[i];
        memcpy(memory::get_pointer(reinterpret_cast<uint32_t>(transfer.dest)), 
               memory::get_pointer(reinterpret_cast<uint32_t>(transfer.src)), 
               transfer.size);
    }
    ctx.cpuRegs.GPR.r[2].UD[0] = 1;
}

void enableDispatchThread(CpuContext& ctx) {
    // Arguments are typically passed in registers $a0, $a1, etc.
    // For EnableDispatchThread, check PS2 SDK documentation for exact parameters
    
    g_logFile << "Syscall: EnableDispatchThread() called!" << std::endl;
    std::cout << "Syscall: EnableDispatchThread() called!" << std::endl;
    
    // On PS2, this syscall typically enables thread dispatching in the kernel
    // For emulation purposes, we can just acknowledge it and return success
    
    // Return success (0) in $v0
    ctx.cpuRegs.GPR.r[2].SL[0] = 0;
}


void runtime_syscall_dispatcher(uint32_t syscall_num, CpuContext& ctx) {
    switch (syscall_num) {
        case 0x00: NotImplemented_Syscall("RFU000_FullReset", ctx); break;
        case 0x01: NotImplemented_Syscall("ResetEE", ctx); break;
        case 0x02: NotImplemented_Syscall("SetGsCrt", ctx); break;
        case 0x04: sceExit(ctx); break;
        case 0x06: NotImplemented_Syscall("LoadPS2Exe", ctx); break;
        case 0x07: NotImplemented_Syscall("ExecPS2", ctx); break;
        case 0x10: NotImplemented_Syscall("AddSbusIntcHandler", ctx); break;
        case 0x11: NotImplemented_Syscall("RemoveSbusIntcHandler", ctx); break;
        case 0x12: NotImplemented_Syscall("Interrupt2Iop", ctx); break;
        case 0x13: NotImplemented_Syscall("SetVTLBRefillHandler", ctx); break;
        case 0x14: NotImplemented_Syscall("SetVCommonHandler", ctx); break;
        case 0x15: NotImplemented_Syscall("SetVInterruptHandler", ctx); break;
        case 0x16: NotImplemented_Syscall("AddIntcHandler", ctx); break;
        case 0x17: NotImplemented_Syscall("RemoveIntcHandler", ctx); break;
        case 0x18: NotImplemented_Syscall("AddDmacHandler", ctx); break;
        case 0x19: NotImplemented_Syscall("RemoveDmacHandler", ctx); break;
        case 0x20: NotImplemented_Syscall("_EnableIntc", ctx); break;
        case 0x21: NotImplemented_Syscall("_DisableIntc", ctx); break;
        case 0x22: NotImplemented_Syscall("_EnableDmac", ctx); break;
        case 0x23: NotImplemented_Syscall("_DisableDmac", ctx); break;
        case 0x24: NotImplemented_Syscall("_SetAlarm", ctx); break;
        case 0x25: NotImplemented_Syscall("_ReleaseAlarm", ctx); break;
        case 0x26: NotImplemented_Syscall("_iEnableIntc", ctx); break;
        case 0x27: NotImplemented_Syscall("_iDisableIntc", ctx); break;
        case 0x28: NotImplemented_Syscall("_iEnableDmac", ctx); break;
        case 0x29: NotImplemented_Syscall("_iDisableDmac", ctx); break;
        case 0x30: NotImplemented_Syscall("_iSetAlarm", ctx); break;
        case 0x31: NotImplemented_Syscall("_iReleaseAlarm", ctx); break;
        case 0x32: NotImplemented_Syscall("CreateThread", ctx); break;
        case 0x33: NotImplemented_Syscall("DeleteThread", ctx); break;
        case 0x34: NotImplemented_Syscall("StartThread", ctx); break;
        case 0x35: NotImplemented_Syscall("ExitThread", ctx); break;
        case 0x36: NotImplemented_Syscall("ExitDeleteThread", ctx); break;
        case 0x37: NotImplemented_Syscall("TerminateThread", ctx); break;
        case 0x38: NotImplemented_Syscall("iTerminateThread", ctx); break;
        case 0x39: NotImplemented_Syscall("DisableDispatchThread", ctx); break;
        case 0x40: enableDispatchThread(ctx); break;
        case 0x41: NotImplemented_Syscall("ChangeThreadPriority", ctx); break;
        case 0x42: NotImplemented_Syscall("iChangeThreadPriority", ctx); break;
        case 0x43: NotImplemented_Syscall("RotateThreadReadyQueue", ctx); break;
        case 0x44: NotImplemented_Syscall("iRotateThreadReadyQueue", ctx); break;
        case 0x45: NotImplemented_Syscall("ReleaseWaitThread", ctx); break;
        case 0x46: NotImplemented_Syscall("iReleaseWaitThread", ctx); break;
        case 0x47: NotImplemented_Syscall("GetThreadId", ctx); break;
        case 0x48: NotImplemented_Syscall("ReferThreadStatus", ctx); break;
        case 0x49: NotImplemented_Syscall("iReferThreadStatus", ctx); break;
        case 0x50: NotImplemented_Syscall("SleepThread", ctx); break;
        case 0x51: NotImplemented_Syscall("WakeupThread", ctx); break;
        case 0x52: NotImplemented_Syscall("iWakeupThread", ctx); break;
        case 0x53: NotImplemented_Syscall("CancelWakeupThread", ctx); break;
        case 0x54: NotImplemented_Syscall("iCancelWakeupThread", ctx); break;
        case 0x55: NotImplemented_Syscall("SuspendThread", ctx); break;
        case 0x56: NotImplemented_Syscall("iSuspendThread", ctx); break;
        case 0x57: NotImplemented_Syscall("ResumeThread", ctx); break;
        case 0x58: NotImplemented_Syscall("iResumeThread", ctx); break;
        case 0x59: NotImplemented_Syscall("JoinThread", ctx); break;
        case 0x62: NotImplemented_Syscall("EndOfHeap", ctx); break;
        case 0x64: NotImplemented_Syscall("CreateSema", ctx); break;
        case 0x65: NotImplemented_Syscall("DeleteSema", ctx); break;
        case 0x66: NotImplemented_Syscall("SignalSema", ctx); break;
        case 0x67: NotImplemented_Syscall("iSignalSema", ctx); break;
        case 0x68: NotImplemented_Syscall("WaitSema", ctx); break;
        case 0x69: NotImplemented_Syscall("PollSema", ctx); break;
        case 0x70: NotImplemented_Syscall("iPollSema", ctx); break;
        case 0x71: NotImplemented_Syscall("ReferSemaStatus", ctx); break;
        case 0x72: NotImplemented_Syscall("iReferSemaStatus", ctx); break;
        case 0x74: NotImplemented_Syscall("SetOsdConfigParam", ctx); break;
        case 0x75: NotImplemented_Syscall("GetOsdConfigParam", ctx); break;
        case 0x76: NotImplemented_Syscall("GetGsHParam", ctx); break;
        case 0x77: NotImplemented_Syscall("GetGsVParam", ctx); break;
        case 0x78: NotImplemented_Syscall("SetGsHParam", ctx); break;
        case 0x79: NotImplemented_Syscall("SetGsVParam", ctx); break;
        case 0x80: NotImplemented_Syscall("CreateEventFlag", ctx); break;
        case 0x81: NotImplemented_Syscall("DeleteEventFlag", ctx); break;
        case 0x82: NotImplemented_Syscall("SetEventFlag", ctx); break;
        case 0x83: iSetEventFlag(ctx); break;
        case 0x84: NotImplemented_Syscall("ClearEventFlag", ctx); break;
        case 0x85: NotImplemented_Syscall("iClearEventFlag", ctx); break;
        case 0x86: NotImplemented_Syscall("WaitEventFlag", ctx); break;
        case 0x87: NotImplemented_Syscall("PollEventFlag", ctx); break;
        case 0x88: NotImplemented_Syscall("iPollEventFlag", ctx); break;
        case 0x89: NotImplemented_Syscall("ReferEventFlagStatus", ctx); break;
        case 0x90: NotImplemented_Syscall("iReferEventFlagStatus", ctx); break;
        case 0x92: NotImplemented_Syscall("EnableIntcHandler", ctx); break;
        case 0x93: NotImplemented_Syscall("DisableIntcHandler", ctx); break;
        case 0x94: NotImplemented_Syscall("EnableDmacHandler", ctx); break;
        case 0x95: NotImplemented_Syscall("DisableDmacHandler", ctx); break;
        case 0x96: NotImplemented_Syscall("KSeg0", ctx); break;
        case 0x97: NotImplemented_Syscall("EnableCache", ctx); break;
        case 0x98: NotImplemented_Syscall("DisableCache", ctx); break;
        case 0x99: NotImplemented_Syscall("GetCop0", ctx); break;
        case 0x100: NotImplemented_Syscall("FlushCache", ctx); break;
        case 0x102: NotImplemented_Syscall("CpuConfig", ctx); break;
        case 0x103: NotImplemented_Syscall("iGetCop0", ctx); break;
        case 0x104: NotImplemented_Syscall("iFlushCache", ctx); break;
        case 0x106: NotImplemented_Syscall("iCpuConfig", ctx); break;
        case 0x107: NotImplemented_Syscall("sceSifStopDma", ctx); break;
        case 0x108: NotImplemented_Syscall("SetCPUTimerHandler", ctx); break;
        case 0x109: NotImplemented_Syscall("SetCPUTimer", ctx); break;
        case 0x110: NotImplemented_Syscall("ForceRead", ctx); break;
        case 0x111: NotImplemented_Syscall("ForceWrite", ctx); break;
        case 0x112: NotImplemented_Syscall("GsGetIMR", ctx); break;
        case 0x113: NotImplemented_Syscall("GsPutIMR", ctx); break;
        case 0x114: NotImplemented_Syscall("SetPgifHandler", ctx); break;
        case 0x115: NotImplemented_Syscall("SetVSyncFlag", ctx); break;
        case 0x117: NotImplemented_Syscall("print", ctx); break;
        case 0x118: NotImplemented_Syscall("sceSifDmaStat", ctx); break;
        case 0x119: sceSifSetDma(ctx); break;
        case 0x120: NotImplemented_Syscall("sceSifSetDChain", ctx); break;
        case 0x121: NotImplemented_Syscall("sceSifSetReg", ctx); break;
        case 0x122: NotImplemented_Syscall("sceSifGetReg", ctx); break;
        case 0x123: NotImplemented_Syscall("ExecOSD", ctx); break;
        case 0x124: NotImplemented_Syscall("Deci2Call", ctx); break;
        case 0x125: NotImplemented_Syscall("PSMode", ctx); break;
        case 0x126: NotImplemented_Syscall("MachineType", ctx); break;
        case 0x127: NotImplemented_Syscall("GetMemorySize", ctx); break;
        
        // Your previously implemented syscalls
        case 0x3c: sifRpcBind(ctx); break;
        case 0x3d: sifRpcCall(ctx); break;
        // The list has 0x74 as SetOsdConfigParam, but you implemented sceSifSetDma as 0x74
        // I will keep your implementation for now.
        // case 0x74: sceSifSetDma(ctx); break; 

        default:
            g_logFile << "Unhandled syscall: 0x" << std::hex << syscall_num << std::endl;
            break;
    }
}