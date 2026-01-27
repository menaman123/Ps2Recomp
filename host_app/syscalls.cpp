#include "syscalls.h"
#include "gs.h"
#include "dmac.h"
#include "memory.h"
#include <iostream>
#include <fstream>
#include <cstring>
#include "ps2_scheduler.h"
#include "sema.h"
#include "recompiled.h"
#include "rabbitizer.hpp"
#include <iomanip>
#include "intc.h"
#include "instructions/InstructionR5900.hpp"
#include "sif.h"
#include <queue>
#include <cstdio>
#include <atomic>

#ifdef _WIN32
#include <io.h>
#include <process.h>
#else
#include <unistd.h>
#endif

extern std::ofstream g_logFile;
extern FILE* g_isoFile;
uint32_t custom_syscall_addresses[256] = {0};
std::map<int, EventFlag> g_eventFlags;
int g_nextEventFlagId = 1; // Start from ID 1
SifState g_sif;
GsRegs g_gs_regs;
VideoState g_video_state;
std::map<int, FILE*> g_file_io_handles;
extern std::atomic<bool> g_window_resize_pending;
extern std::atomic<int>  g_new_window_width;
extern std::atomic<int>  g_new_window_height;

constexpr int THREAD_STATUS_RUNNING = 0x01; // THS_RUN
constexpr int THREAD_STATUS_READY   = 0x02; // THS_READY
constexpr int THREAD_STATUS_WAIT    = 0x04; // THS_WAIT
constexpr int THREAD_STATUS_DORMANT = 0x10; // THS_DORMANT


const int GS_MODE_NTSC = 0x02;
const int GS_MODE_PAL  = 0x03;
const int GS_MODE_VGA  = 0x50; // 640x480p
const int GS_MODE_480P = 0x53; // DTV 480p

struct OsdConfigParam {
    int32_t spdifMode;      // 0 = Off, 1 = On
    int32_t screenType;     // 0 = 4:3, 1 = Full, 2 = 16:9
    int32_t videoOutput;    // 0 = RGB, 1 = Component (YCrCb)
    int32_t language;       // 0=Jap, 1=Eng, 2=Fre, 3=Spa, 4=Ger, 5=Ita, 6=Dut, 7=Por
    int32_t timezoneOffset; // Minutes from GMT
    int32_t timeZoneFlag;   // Daylight savings flags
    int32_t ps1DrvConfig;   // PS1 Driver options (Disk Speed, Texture Mapping)
    int32_t version;        // Structure version
};

struct Thread {
    int id;
    int status;
    int initial_priority;
    int current_priority;
    
    // Each thread needs its own CPU state (Registers, PC, etc.)
    CpuContext ctx; 
    
    uint32_t stack_base;
    uint32_t stack_size;
    uint32_t gp_reg;
    uint32_t func_pc;
};

// Global container for threads
std::map<int, Thread> g_threads;
int g_nextThreadId = 1;
int g_currentThreadId = 1;

const uint32_t KERNEL_SYSCALL_TABLE_BASE = 0x800002E0;

// A helper function to read a string from guest memory
std::string read_string_from_guest(uint32_t address) {
    return std::string(reinterpret_cast<char*>(memory::get_pointer(address)));
}

void NotImplemented_Syscall(const char* name, CpuContext& ctx) {
    g_logFile << "Syscall: " << name << " (Not Implemented)" << std::endl;
    return;
    exit(1);
}

enum SifRpcServerId : uint32_t {
    // Basic I/O and System
    SIF_IOP_FILEIO      = 0x80000001, // File I/O (FILEIO)
    SIF_IOP_HEAP        = 0x80000003, // IOP Heap Allocation (FILEIO)
    SIF_IOP_LOADFILE    = 0x80000006, // Module/ELF Loader (LOADFILE)

    // Controller (PADMAN)
    SIF_PAD_BIND        = 0x80000100, // Pad (PADMAN)
    SIF_PAD_EXT         = 0x80000101, // Pad extension (PADMAN)

    // Memory Card (MCSERV)
    SIF_MC_SERV         = 0x80000400, // Memory cards (MCSERV)

    // CD/DVD Drive (CDVDFSV)
    SIF_CD_INIT         = 0x80000592, // CDVD Init
    SIF_CD_SCMD         = 0x80000593, // CDVD S commands (Synchronous)
    SIF_CD_NCMD         = 0x80000595, // CDVD N commands (Asynchronous)
    SIF_CD_SEARCHFILE   = 0x80000597, // CDVD SearchFile
    SIF_CD_DISKREADY    = 0x8000059A, // CDVD Disk Ready

    // Sound (LIBSD/SDRDRV)
    SIF_LIBSD_REMOTE    = 0x80000701, // LIBSD Remote (SDRDRV)

    // Multitap (MTAPMAN)
    SIF_MTAP_OPEN       = 0x80000901, // MTAP Port Open
    SIF_MTAP_CLOSE      = 0x80000902, // MTAP Port Close
    SIF_MTAP_GETCONN    = 0x80000903, // MTAP Get Connection
    SIF_MTAP_UNK4       = 0x80000904, // MTAP Unknown 4
    SIF_MTAP_UNK5       = 0x80000905, // MTAP Unknown 5

    // Peripherals
    SIF_EYETOY          = 0x80001400  // EyeToy (EYETOY)
};

std::string GetSifServerName(uint32_t server_id) {
    switch (server_id) {
        // Basic I/O
        case SIF_IOP_FILEIO:      return "FILEIO (File I/O)";
        case SIF_IOP_HEAP:        return "IOPHEAP (Heap Allocation)";
        case SIF_IOP_LOADFILE:    return "LOADFILE (Module Loader)";
        
        // Controllers
        case SIF_PAD_BIND:        return "PADMAN (Controller)";
        case SIF_PAD_EXT:         return "PADMAN (Extension)";
        
        // Memory Card
        case SIF_MC_SERV:         return "MCSERV (Memory Card)";
        
        // CDVD Drive
        case SIF_CD_INIT:         return "CDVDFSV (CDVD Init)";
        case SIF_CD_SCMD:         return "CDVDFSV (S-Command)";
        case SIF_CD_NCMD:         return "CDVDFSV (N-Command)";
        case SIF_CD_SEARCHFILE:   return "CDVDFSV (Search File)";
        case SIF_CD_DISKREADY:    return "CDVDFSV (Disk Ready)";
        
        // Sound
        case SIF_LIBSD_REMOTE:    return "LIBSD (Sound Driver)";
        
        // Multitap
        case SIF_MTAP_OPEN:       return "MTAPMAN (Port Open)";
        case SIF_MTAP_CLOSE:      return "MTAPMAN (Port Close)";
        case SIF_MTAP_GETCONN:    return "MTAPMAN (Get Connection)";
        
        default: return "Unknown Server ID";
    }
}

void handle_mtc0_write(CpuContext& ctx, uint8_t rd, uint32_t value) {
    switch (rd) {
        case 12: // Status register
            // Mask writable bits (IEc, KUc, IEp, KUp, IEo, KUo, IM, BEV, etc.)
            // Bits that are typically writable on PS2:
            // 28-31: CU (Coprocessor Usable) - 0xF0000000
            // 22:    BEV (Bootstrap Exception Vector) - 0x00400000
            // 16-23: IM (Interrupt Mask) - 0x00FF0000
            // 1-5:   KUo, IEo, KUp, IEp, KUc, IEc - 0x0000003E
            ctx.cpuRegs.CP0.n.Status = (value & 0xF0FF003F) | (ctx.cpuRegs.CP0.n.Status & ~0xF0FF003F);
            
            // Check if interrupts should be processed
            if ((ctx.cpuRegs.CP0.n.Status & 0x1) && 
                (ctx.cpuRegs.CP0.n.Status & ctx.cpuRegs.CP0.n.Cause & 0xFF00)) {
                g_logFile << "MTC0: Interrupts enabled, pending interrupt detected" << std::endl;
            }
            break;
            
        case 13: // Cause register
            // Only bits 8-9 (IP0-IP1, software interrupt bits) are writable
            ctx.cpuRegs.CP0.n.Cause = (ctx.cpuRegs.CP0.n.Cause & ~0x300) | (value & 0x300);
            break;
            
        case 9:  // Count register (free-running counter)
            ctx.cpuRegs.CP0.r[9] = value;
            break;
            
        case 11: // Compare register (clears timer interrupt when written)
            ctx.cpuRegs.CP0.r[11] = value;
            ctx.cpuRegs.CP0.n.Cause &= ~0x8000; // Clear IP7 (timer interrupt bit)
            break;
            
        case 14: // EPC (Exception Program Counter)
            ctx.cpuRegs.CP0.r[14] = value;
            break;
            
        case 30: // ErrorEPC
            ctx.cpuRegs.CP0.r[30] = value;
            break;
            
        case 0:  // Index (TLB)
        case 2:  // EntryLo0 (TLB)
        case 3:  // EntryLo1 (TLB)
        case 5:  // PageMask (TLB)
        case 10: // EntryHi (TLB)
            ctx.cpuRegs.CP0.r[rd] = value;
            g_logFile << "MTC0: TLB register " << (int)rd << " write = 0x" 
                      << std::hex << value << std::endl;
            break;
            
        case 24: // PerfCnt (Performance counter control)
        case 25: // ErrCtl (Error control)
            ctx.cpuRegs.CP0.r[rd] = value;
            g_logFile << "MTC0: Performance/Debug register " << (int)rd << " write = 0x" 
                      << std::hex << value << std::endl;
            break;
            
        default:
            // For other registers, just write directly
            ctx.cpuRegs.CP0.r[rd] = value;
            g_logFile << "MTC0: Write to COP0 register " << (int)rd << " = 0x" 
                      << std::hex << value << std::endl;
            break;
    }
}

void GsPutIMR(CpuContext& ctx) {
    g_gs_regs.GS_IMR = ctx.cpuRegs.GPR.r[4].UD[0];  // a0
}

// Syscall 70h - GsGetIMR() -> uint64_t  (for completeness)
void GsGetIMR(CpuContext& ctx) {
    ctx.cpuRegs.GPR.r[2].UD[0] = g_gs_regs.GS_IMR;  // return in v0
}

void SetGsCrt(CpuContext& ctx) {
    g_logFile << "Syscall: SetGsCrt() called!" << std::endl;

    // 1. Read Registers
    // a0 ($4): Interlace (0=Non-Interlaced, 1=Interlaced)
    bool interlaced  = ctx.cpuRegs.GPR.r[4].SL[0] != 0; 
    
    // a1 ($5): Display Mode (NTSC, PAL, VGA, etc.)
    int display_mode = ctx.cpuRegs.GPR.r[5].SL[0];     
    
    // a2 ($6): Frame Mode (0=Field, 1=Frame)
    bool frame_mode  = ctx.cpuRegs.GPR.r[6].SL[0] != 0; 

    // 2. Update Low-Level Emulation (LLE) State
    g_video_state.interlaced   = interlaced;
    g_video_state.display_mode = display_mode;
    g_video_state.frame_mode   = frame_mode;

    // 3. Determine Window Resolution
    // Default to NTSC standard
    int width = 640;  
    int height = 448; 
    const char* mode_name = "NTSC";

    switch (display_mode) {
        case GS_MODE_NTSC:
            // NTSC is technically 448 lines in Interlaced mode.
            width = 640;
            height = 448;
            mode_name = "NTSC";
            break;

        case GS_MODE_PAL:
            // PAL is higher resolution (512 lines)
            width = 640;
            height = 512;
            mode_name = "PAL";
            break;

        case GS_MODE_VGA:
            width = 640;
            height = 480;
            mode_name = "VGA";
            break;
            
        case GS_MODE_480P:
            width = 640;
            height = 480;
            mode_name = "480p";
            break;

        default:
            g_logFile << "  [HLE] Warning: Unknown video mode 0x" << std::hex << display_mode 
                      << ". Defaulting to NTSC." << std::dec << std::endl;
            break;
    }

    g_logFile << "  [HLE] SetGsCrt: Mode=" << mode_name 
              << " (" << width << "x" << height << ")"
              << " Interlaced=" << interlaced << std::endl;

    // 4. Signal Main Thread to Resize Window
    // We update the dimensions first, then flip the flag.
    g_new_window_width = width;
    g_new_window_height = height;
    g_window_resize_pending = true;

    // 5. Return Success
    ctx.cpuRegs.GPR.r[2].UL[0] = 0;
}

void SetOsdConfigParam(CpuContext& ctx) {
    g_logFile << "Syscall: SetOsdConfigParam() called!" << std::endl;
    
    uint32_t param_ptr = ctx.cpuRegs.GPR.r[4].UL[0];
    
    g_logFile << "Syscall: SetOsdConfigParam(param: 0x" 
              << std::hex << param_ptr << ")" << std::endl;
    /*
    
    if (param_ptr != 0) {
        // Read the config structure from guest memory
        uint8_t* config_data = reinterpret_cast<uint8_t*>(
            memory::get_pointer(param_ptr)
        );
        
        // Store it in kernel memory or global state
        // In real PS2, this gets written to specific kernel addresses
        // For emulation, you might just acknowledge it:
        
        // Example of what you might extract:
        // uint8_t language = config_data[0];
        // uint8_t screen_type = (config_data[1] >> 1) & 0x03;
        // int8_t timezone = config_data[2];
        
        g_logFile << "  Language: " << (int)config_data[0] << std::endl;
        g_logFile << "  Screen Type: " << ((config_data[1] >> 1) & 0x03) << std::endl;
    }
    
    */
    ctx.cpuRegs.GPR.r[2].SL[0] = 0; // Success
}

void Syscall_GetOsdConfigParam(CpuContext& ctx) {
    g_logFile << "Syscall: Syscall_GetOsdConfigParam() called!" << std::endl;
    // 1. Get the destination address from register $a0 (GPR 4)
    uint32_t config_ptr = ctx.cpuRegs.GPR.r[4].UL[0];

    g_logFile << "Syscall: GetOsdConfigParam(out_ptr: 0x" 
              << std::hex << config_ptr << ")" << std::endl;

    // 2. Define safe defaults (Standard English, 4:3, RGB, Standard PS1 settings)
    // Writing word-by-word (32-bit) to guest memory matches the 'int' fields.
    
    // Offset 0x00: spdifMode (0 = OFF)
    memory::write<int32_t>(config_ptr + 0x00, 0); 

    // Offset 0x04: screenType (0 = 4:3)
    memory::write<int32_t>(config_ptr + 0x04, 0); 

    // Offset 0x08: videoOutput (1 = Component/YCrCb is safer for modern displays)
    memory::write<int32_t>(config_ptr + 0x08, 1); 

    // Offset 0x0C: language (1 = English)
    memory::write<int32_t>(config_ptr + 0x0C, 1); 

    // Offset 0x10: timezoneOffset (0 = GMT)
    memory::write<int32_t>(config_ptr + 0x10, 0); 

    // Offset 0x14: timeZoneFlag (0 = Standard)
    memory::write<int32_t>(config_ptr + 0x14, 0); 

    // Offset 0x18: ps1DrvConfig
    // CRITICAL for PS1DRV: Set to 0 to disable "Fast Loading" and "Texture Smoothing".
    // This ensures PS1DRV uses the most compatible timing paths.
    memory::write<int32_t>(config_ptr + 0x18, 0); 

    // Offset 0x1C: version (1)
    memory::write<int32_t>(config_ptr + 0x1C, 1); 

    // 3. Return Success (0) in $v0
    ctx.cpuRegs.GPR.r[2].SL[0] = 0;
}


void CreateEventFlag(CpuContext& ctx) {
    g_logFile << "Syscall: CreateEventFlag() called!" << std::endl;
    
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
    g_logFile << "Syscall: DeleteEventFlag() called!" << std::endl;
    
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
    g_logFile << "Syscall: SetEventFlag() called!" << std::endl;
    
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
    g_logFile << "Syscall: ClearEventFlag() called!" << std::endl;
    
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
    g_logFile << "Syscall: WaitEventFlag() called!" << std::endl;
    
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
    g_logFile << "Syscall: iSetEventFlag() called!" << std::endl;
    
    SetEventFlag(ctx);
}

void sifRpcBind(CpuContext& ctx) {
    g_logFile << "Syscall: sifRpcBind() called!" << std::endl;

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
    g_logFile << "Syscall: sifRpcCall() called!" << std::endl;

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
    g_logFile << "Syscall: sifSetRpcQueue() called!" << std::endl;

    uint32_t qd_addr = ctx.cpuRegs.GPR.r[4].UL[0];
    uint32_t thread_id = ctx.cpuRegs.GPR.r[5].UL[0];

    g_logFile << "Syscall: sifSetRpcQueue(qd: 0x" << std::hex << qd_addr
              << ", thread_id: " << std::dec << thread_id << ") called!" << std::endl;
}

// Exit syscall
void sceExit(CpuContext& ctx) {
    g_logFile << "Syscall: sceExit() called!" << std::endl;
    
    int exit_code = static_cast<int>(ctx.cpuRegs.GPR.r[4].SL[0]); // $a0
    std::cout << "Syscall: exit(" << exit_code << ")" << std::endl;
    exit(exit_code);
}

// Write syscall
void sceWrite(CpuContext& ctx) {
    g_logFile << "Syscall: sceWrite() called!" << std::endl;

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
    g_logFile << "Syscall: sceOpen() called!" << std::endl;
    
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
    g_logFile << "Syscall: _Exit() called!" << std::endl;
    // The exit code is typically passed in register $a0, which is GPR 4.
    int exit_code = ctx.cpuRegs.GPR.r[4].SL[0];
    std::cout << "Syscall: _Exit called with code " << exit_code << std::endl;
    exit(exit_code);
}

#define SIF_CMD_RPC_END     0x80000008
#define SIF_CMD_RPC_BIND    0x80000009
#define SIF_CMD_RPC_CALL    0x8000000A
#define SIF_CMD_RPC_RDATA   0x8000000C

void sceSifSetDma(CpuContext& ctx) {
    uint32_t sdt_addr = ctx.cpuRegs.GPR.r[4].UL[0];
    int32_t count = ctx.cpuRegs.GPR.r[5].SL[0];
    
    g_logFile << "Syscall: sceSifSetDma(sdt_addr: 0x" << std::hex << sdt_addr 
              << ", count: " << std::dec << count << ")" << std::endl;
    
    for (int i = 0; i < count; i++) {
        SifDmaTransfer_t transfer;
        uint32_t entry_addr = sdt_addr + (i * sizeof(SifDmaTransfer_t));
        
        transfer.src = memory::read<uint32_t>(entry_addr + 0);
        transfer.dest = memory::read<uint32_t>(entry_addr + 4);
        transfer.size = memory::read<int32_t>(entry_addr + 8);
        transfer.attr = memory::read<int32_t>(entry_addr + 12);
        
        g_logFile << "  Transfer " << i << ": src=0x" << std::hex << transfer.src
                  << " dest=0x" << transfer.dest << " size=" << std::dec << transfer.size
                  << " attr=0x" << std::hex << transfer.attr << std::dec << std::endl;
        
        // Perform the transfer (EE → IOP, so this is SIF1)
        // In HLE mode, we just acknowledge the transfer immediately
        // The actual data doesn't go anywhere meaningful for HLE'd IOP modules
        uint32_t packet_addr = transfer.src;
        
        // Read SifCmdHeader (16 bytes)
        // [0x0] psize/dsize
        // [0x4] dest
        // [0x8] cid (Command ID)
        // [0xC] opt
        uint32_t command_id = memory::read<uint32_t>(packet_addr + 8);
        uint32_t opt        = memory::read<uint32_t>(packet_addr + 12);
        
        g_logFile << "    [SIF Packet] CID: 0x" << std::hex << command_id 
                  << " Opt: " << opt << std::dec << std::endl;

        // Handle specific commands
        if (command_id == 0x80000000) { // Change SADDR
        }
        else if (command_id == 0x80000001) { // Set SREG
        }
        else if (command_id == 0x80000002) { // SIF_INIT
            g_logFile << "    [SIF Action] Init request detected." << std::endl;
            if (opt == 0) {
                // IOP sets SMFLG to 0x20000 to acknowledge init
                memory::write<uint32_t>(0x1000F230, 0x20000);
                g_logFile << "    [SIF Action] Wrote 0x20000 to SMFLAG." << std::endl;
            }
        }
        else if (command_id == 0x80000003) { // SIF_CMD_RESET (IOP Reboot)
            g_logFile << "    [SIF Action] Reset Request (IOP Reboot) detected." << std::endl;
            
            // 1. Reset SIF flags to simulate IOP shutting down
            g_sif.smflag = 0; 
            g_sif.msflag = 0;
            memory::write<uint32_t>(0x1000F230, 0); // SMFLAG
            memory::write<uint32_t>(0x1000F220, 0); // MSFLAG

            // 2. Set the "Boot End" flag (0x40000)
            // This signals to the EE that the "new" IOP kernel is ready.
            g_sif.smflag = 0x40000; 
            memory::write<uint32_t>(0x1000F230, g_sif.smflag);
            
            g_logFile << "    [SIF Action] HLE Reboot: Wrote 0x40000 to SMFLAG." << std::endl;
        }
        else if(command_id == 0x80000008){ // Request End

        }
        else if (command_id == 0x80000009) { // SIF_BIND
            // Packet structure for BIND:
            // [0x00] Header
            // [0x1C] client_data_pointer (The struct the game checks!)
            // [0x20] server_id_requested
            
            uint32_t client_data_addr = memory::read<uint32_t>(packet_addr + 0x1C);
            uint32_t server_id        = memory::read<uint32_t>(packet_addr + 0x20);
            
            g_logFile << "    [SIF Action] Bind Request:" << std::endl;
            g_logFile << "      Server ID: 0x" << std::hex << server_id 
                      << " -> " << GetSifServerName(server_id) << std::dec << std::endl;
            g_logFile << "      Client Struct At: 0x" << std::hex << client_data_addr << std::dec << std::endl;
            
            // FIX THE INFINITE LOOP:
            // The game is waiting for 'client_data->server' to become non-zero.
            // We write a dummy handle (0x1) to tell the game "Connection Successful".
            if (client_data_addr != 0) {
                // Offset 0x24 in SifRpcClientData is the 'server' handle
                memory::write<uint32_t>(client_data_addr + 0x24, 0x1);
                g_logFile << "    [SIF Action] Wrote Success Handle (0x1) to client->server" << std::endl;
            }
        }
        else if (command_id == 0x8000000A){  // SIF_RPC_CALL
            // 1. Read RPC details from the packet struct
            uint32_t rpc_func_num = memory::read<uint32_t>(packet_addr + 0x20); // Offset 32
            uint32_t server_handle = memory::read<uint32_t>(packet_addr + 0x34); // Offset 52
            uint32_t recv_buffer  = memory::read<uint32_t>(packet_addr + 0x28); // Offset 40

            g_logFile << "    [SIF Action] RPC Call Detected!" << std::endl;
            g_logFile << "      Server Handle: 0x" << std::hex << server_handle << std::endl;
            g_logFile << "      Function ID:   0x" << rpc_func_num << std::dec << std::endl;

            // 2. Identify the Server (Based on your Bind implementation)
            //    If you used 0x1 for ALL servers, this logic is tricky. 
            //    Ideally, use unique handles in Bind (e.g., 0xDEAD0006 for LOADFILE).
            //    Assuming 0xDEAD0006 for LOADFILE based on previous advice:
            bool is_file_io = (server_handle == 0xDEAD0001);
            bool is_iop_heap_allocation = (server_handle == 0xDEAD0003);
            bool is_loadfile = (server_handle == 0xDEAD0006);

            bool is_pad = (server_handle == 0xDEAD0100);
            bool is_pad_extension = (server_handle == 0xDEAD0101);
            bool is_memory_card = (server_handle == 0xDEAD0400);
            bool is_cdvd_init = (server_handle == 0xDEAD0592);
            bool is_cdvd_s_commands = (server_handle == 0xDEAD0593);
            bool is_cdvd_n_commands = (server_handle == 0xDEAD0595);
            bool is_cdvd_search_file = (server_handle == 0xDEAD0597);
            bool is_cdvd_disk_ready = (server_handle == 0xDEAD059A);
            bool is_libsd_remote = (server_handle == 0xDEAD0701);
            bool is_mtap_port_open = (server_handle == 0xDEAD0901);
            bool is_mtap_port_close = (server_handle == 0xDEAD0902);
            bool is_mtap_get_connections = (server_handle == 0xDEAD0903);
            bool is_mtap_unknown_1 = (server_handle == 0xDEAD0904);
            bool is_mtap_unknown_2 = (server_handle == 0xDEAD0905);
            bool is_eye_toy = (server_handle == 0xDEAD1400);


            // 3. Handle SifLoadElf (Func 0x01 on LOADFILE)
            if (is_loadfile){
                g_logFile << "    [HLE] LOAD FILE Call Detected (Func: " << std::hex << rpc_func_num << ")" << std::endl;
                int32_t result = 0;
                if (rpc_func_num == 0xFF) { 
                        // Init/Handshake
                        // Just return 0 (Success) to allow the game to proceed to 'open' calls.
                        if (recv_buffer != 0) memory::write<int32_t>(recv_buffer, 0);
                }
                else if (rpc_func_num == 0x01) {
                    // The filename is usually located immediately after the struct (Offset 0x38)
                    // or pointed to by a second DMA transfer. 
                    // In SifLoadElf packets, the path string is often embedded at +0x40.
                    char filename[256];
                    uint32_t payload_addr = packet_addr + 0x40; // Approx offset, verify with hexdump
                    
                    for(int j=0; j<256; j++) {
                        filename[j] = memory::read<uint8_t>(payload_addr + j);
                        if(filename[j] == 0) break;
                    }

                    g_logFile << "    [HLE] INTERCEPTED SifLoadElf: " << filename << std::endl;

                    // 4. EXECUTE THE LOAD
                    // This function must read the ELF from your ISO and write it to EE RAM.
                    // It should inspect the ELF Program Header to find the destination (e.g. 0x2ec190).
                    //bool success = LoadElfToRAM(filename, ctx); 

                    // 5. Write Reply (Critical!)
                    // If we don't write 0 to the reply buffer, the game thinks it failed.
                    if (recv_buffer != 0) {
                        memory::write<int32_t>(recv_buffer, false ? 0 : -1);
                    }
                    g_logFile << " Need to implement LoadElfToRAM()" << std::endl;

                }
                else if (rpc_func_num == 0x04) {
                    char filename[256];
                    uint32_t name_addr = packet_addr + 0x40; // Filename is payload
                    
                    for(int k=0; k<255; k++) {
                        filename[k] = memory::read<uint8_t>(name_addr + k);
                        if(filename[k] == 0) break;
                    }
                    
                    g_logFile << "    [FILEIO] Open Request: " << filename << std::endl;

                    // Strip "cdrom0:\" or "host:\" to look in local folder
                    std::string path = filename;
                    size_t pos = path.find(':');
                    if (pos != std::string::npos) path = path.substr(pos + 1); 
                    if (!path.empty() && (path[0] == '\\' || path[0] == '/')) path = path.substr(1);

                    FILE* f = fopen(path.c_str(), "rb");
                    if (f) {
                        // We use a static counter for FDs to avoid conflicts
                        static int g_next_fio_fd = 100; 
                        int fd = g_next_fio_fd++;
                        g_file_io_handles[fd] = f;
                        result = fd; 
                        g_logFile << "    [FILEIO] Opened '" << path << "' as FD " << fd << std::endl;
                    } else {
                        g_logFile << "    [FILEIO] Failed to open '" << path << "'" << std::endl;
                        result = -1;
                    }
                }
                
                // Function 5: fioClose(fd)
                else if (rpc_func_num == 0x05) {
                    int fd = memory::read<int32_t>(packet_addr + 0x40);
                    if (g_file_io_handles.count(fd)) {
                        fclose(g_file_io_handles[fd]);
                        g_file_io_handles.erase(fd);
                        result = 0;
                    } else {
                        result = -1;
                    }
                }

                // Function 6: fioRead(fd, buffer, size) - THE FIX
                else if (rpc_func_num == 0x06) {
                    int fd       = memory::read<int32_t>(packet_addr + 0x40);
                    uint32_t dst = memory::read<uint32_t>(packet_addr + 0x44); // This will be 0x30024000
                    int size     = memory::read<int32_t>(packet_addr + 0x48);
                    
                    g_logFile << "    [FILEIO] Read FD " << fd << " -> 0x" << std::hex << dst 
                              << " (" << std::dec << size << " bytes)" << std::endl;

                    if (g_file_io_handles.count(fd)) {
                        FILE* f = g_file_io_handles[fd];
                        
                        // 1. Handle Memory Mirror (0x30xxxxxx -> 0x00xxxxxx)
                        // If we don't do this, we write to an empty map region
                        uint32_t phys_addr = dst & 0x1FFFFFFF; 

                        // 2. Read from Host File directly into Emulated RAM
                        // We read in chunks to avoid large allocations
                        uint8_t chunk[4096];
                        int total_read = 0;
                        while(total_read < size) {
                            int to_read = std::min(4096, size - total_read);
                            int r = fread(chunk, 1, to_read, f);
                            if (r <= 0) break;
                            
                            for(int k=0; k<r; k++) {
                                memory::write<uint8_t>(phys_addr + total_read + k, chunk[k]);
                            }
                            total_read += r;
                        }
                        
                        result = total_read;
                        g_logFile << "    [FILEIO] Wrote " << total_read << " bytes to RAM." << std::endl;
                    } else {
                        g_logFile << "    [FILEIO] ERROR: Invalid FD " << fd << std::endl;
                        result = -1;
                        
                        // FAIL-SAFE: Write a "JR RA" stub so the game doesn't crash on bad address
                        uint32_t phys_addr = dst & 0x1FFFFFFF;
                        memory::write<uint32_t>(phys_addr, 0x03E00008); // jr ra
                        memory::write<uint32_t>(phys_addr + 4, 0);      // nop
                    }
                }




                else {
                    g_logFile << "    [HLE] Stubbing RPC Call (Server: 0x" << std::hex << server_handle 
                            << ", Func: 0x" << rpc_func_num << ")" << std::endl;

                    // CRITICAL: We must reply "Success" so the game continues!
                    if (recv_buffer != 0) {
                        memory::write<int32_t>(recv_buffer, 0); 
                    }
                }
            
            }
else if (is_file_io) { // Server 0x80000001
                g_logFile << "    [HLE] FILEIO Call Detected (Func: 0x" << std::hex << rpc_func_num << ")" << std::endl;

                int32_t result = 0;

                // Function 4: fioOpen
                if (rpc_func_num == 0x04) {
                    char filename[256];
                    uint32_t name_addr = packet_addr + 0x40;
                    for(int k=0; k<255; k++) {
                        filename[k] = memory::read<uint8_t>(name_addr + k);
                        if(filename[k] == 0) break;
                    }
                    g_logFile << "    [FILEIO] Open Request: " << filename << std::endl;

                    std::string path = filename;
                    size_t pos = path.find(':');
                    if (pos != std::string::npos) path = path.substr(pos + 1); 
                    if (!path.empty() && (path[0] == '\\' || path[0] == '/')) path = path.substr(1);

                    FILE* f = fopen(path.c_str(), "rb");
                    if (f) {
                        static int g_next_fio_fd = 100; 
                        int fd = g_next_fio_fd++;
                        g_file_io_handles[fd] = f;
                        result = fd; 
                        g_logFile << "    [FILEIO] Opened '" << path << "' as FD " << fd << std::endl;
                    } else {
                        g_logFile << "    [FILEIO] Failed to open '" << path << "'" << std::endl;
                        result = -1;
                    }
                }
                // Function 5: fioClose
                else if (rpc_func_num == 0x05) {
                    int fd = memory::read<int32_t>(packet_addr + 0x40);
                    if (g_file_io_handles.count(fd)) {
                        fclose(g_file_io_handles[fd]);
                        g_file_io_handles.erase(fd);
                        result = 0;
                    } else {
                        result = -1;
                    }
                }
                // Function 6: fioRead - THE LOAD
                else if (rpc_func_num == 0x06) {
                    int fd       = memory::read<int32_t>(packet_addr + 0x40);
                    uint32_t dst = memory::read<uint32_t>(packet_addr + 0x44); // 0x30024000
                    int size     = memory::read<int32_t>(packet_addr + 0x48);
                    
                    if (g_file_io_handles.count(fd)) {
                        FILE* f = g_file_io_handles[fd];
                        
                        // FIX: Ensure Physical Address
                        uint32_t phys_addr = dst & 0x1FFFFFFF; 

                        std::vector<uint8_t> temp_buf(size);
                        size_t bytes = fread(temp_buf.data(), 1, size, f);
                        
                        for (size_t k = 0; k < bytes; k++) {
                            memory::write<uint8_t>(phys_addr + k, temp_buf[k]);
                        }
                        
                        result = (int32_t)bytes;
                        g_logFile << "    [FILEIO] Wrote " << bytes << " bytes to RAM 0x" << std::hex << phys_addr << std::endl;
                    } else {
                        g_logFile << "    [FILEIO] ERROR: Invalid FD " << fd << std::endl;
                        result = -1;
                        
                        // Fail-Safe Stub
                        uint32_t phys_addr = dst & 0x1FFFFFFF;
                        memory::write<uint32_t>(phys_addr, 0x03E00008); // jr ra
                        memory::write<uint32_t>(phys_addr + 4, 0);      // nop
                    }
                }

                if (recv_buffer != 0) {
                    // FIX: Mask address
                    memory::write<int32_t>(recv_buffer & 0x1FFFFFFF, result);
                }
            }
            else if (is_pad){
                g_logFile << "    [HLE] PADMAN Call Detected (Func: 0x" << std::hex << rpc_func_num << ")" << std::endl;
                    
                // Function 0x1 = PadOpen / PadInit
                // The game calls this to initialize the controller port.
                // We must return '1' (Success/Handle) or the game assumes no controller is connected.
                if (rpc_func_num == 0x1) {
                    if (recv_buffer != 0) {
                        memory::write<int32_t>(recv_buffer, 1); // Return Success (1)
                        g_logFile << "    [HLE] PADMAN: Returned Success (1) to PadOpen." << std::endl;
                    }
                }
                // Handle other Pad functions if they appear (often 0x6 for PadRead)
                else {
                    if (recv_buffer != 0) memory::write<int32_t>(recv_buffer, 1);
                }
            }

            else if (is_memory_card) { // Server 0xDEAD0400 (MCSERV)
                g_logFile << "    [HLE] MCSERV Call Detected (Func: 0x" << std::hex << rpc_func_num << ")" << std::endl;

                // Function 0xFE = McGetVersion / Init
                // The game checks if the MC library matches the kernel version.
                // Return 0 (Success) to pass the check.
                if (rpc_func_num == 0xFE) {
                    if (recv_buffer != 0) {
                        memory::write<int32_t>(recv_buffer, 0); // Return Success (0)
                        g_logFile << "    [HLE] MCSERV: Returned Success (0) to Init/VersionCheck." << std::endl;
                    }
                }
                // Function 0x5 = McDetectCard (Slot check)
                // Return 0 (Success/Card Present) or a negative error code if you want to simulate empty slot.
                else if (rpc_func_num == 0x5) {
                    if (recv_buffer != 0) {
                        memory::write<int32_t>(recv_buffer, 0); 
                        g_logFile << "    [HLE] MCSERV: Returned Success (0) to DetectCard." << std::endl;
                    }
                }
                else {
                    // Default stub for other MC functions to prevent deadlock
                    if (recv_buffer != 0) memory::write<int32_t>(recv_buffer, 0);
                }
            }
            else if (server_handle == 0xDEAD2345) { // Your Custom ID for 0x12345
                g_logFile << "    [Analysis] Custom Server 0x12345 Call Detected." << std::endl;

                if (rpc_func_num == 0x0) {
                    // 1. Parse the Command Struct from EE RAM
                    // We get the pointer to the command struct from Transfer 0
                    uint32_t transfer0_base = sdt_addr + (0 * 16); 
                    uint32_t cmd_ptr = memory::read<uint32_t>(transfer0_base + 0); // .src

                    // 2. Read the LBA and Size from the guest struct
                    // Note: These offsets (0x00 and 0x04) match the Hex Dump from your log
                    uint32_t lba  = memory::read<uint32_t>(cmd_ptr + 0x00); 
                    uint32_t size = memory::read<uint32_t>(cmd_ptr + 0x04); 
                    
                    // 3. Get Destination Address
                    // Based on your previous log, Offset 0x1C contained 0x2a0002.
                    // The '| 2' might be a flag (e.g., "Use DMA"), so we mask it out.
                    uint32_t raw_dest = memory::read<uint32_t>(cmd_ptr + 0x1C);
                    uint32_t dest_addr = raw_dest & 0xFFFFFFFC; // Align to 4 bytes

                    g_logFile << "    [HLE Loader] Request: LBA " << lba << " (" << size << " bytes) -> RAM 0x" << std::hex << dest_addr << std::dec << std::endl;

                    // 4. Perform the Read from ISO
                    if (g_isoFile) {
                        // PS2 Data Sectors are 2048 bytes
                        uint64_t iso_offset = (uint64_t)lba * 2048;
                        
                        // Safety Check: Ensure we don't read past the ISO end
                        fseek(g_isoFile, 0, SEEK_END);
                        uint64_t iso_size = ftell(g_isoFile);
                        
                        if (iso_offset + size <= iso_size) {
                            fseek(g_isoFile, iso_offset, SEEK_SET);

                            // Create a temp buffer to hold the data
                            std::vector<uint8_t> buffer(size);
                            size_t bytes_read = fread(buffer.data(), 1, size, g_isoFile);

                            // Copy data into EE RAM
                            // (In a real implementation, you might optimize this to avoid the vector copy)
                            for (size_t k = 0; k < bytes_read; k++) {
                                memory::write<uint8_t>(dest_addr + k, buffer[k]);
                            }
                            g_logFile << "    [HLE Loader] Successfully loaded " << bytes_read << " bytes." << std::endl;
                        } else {
                            g_logFile << "    [HLE Loader] ERROR: Read request out of bounds!" << std::endl;
                        }
                    } else {
                        g_logFile << "    [HLE Loader] ERROR: ISO file not open!" << std::endl;
                    }

                    // 5. Signal Success to the Game
                    // Writing 1 to the recv_buffer tells the game the operation finished.
                    if (recv_buffer != 0) {
                        memory::write<int32_t>(recv_buffer, 1);
                    }
                }
            }
            }

            /*
                        else if (is_file_io){
                g_logFile << "" << std::endl;
            }
            else if (is_iop_heap_allocation){
                g_logFile << "" << std::endl;
            }
            else if (is_pad){
                g_logFile << "" << std::endl;
            }
            else if (is_pad_extension){
                g_logFile << "" << std::endl;
            }
            else if (is_memory_card){
                g_logFile << "" << std::endl;
            }
            else if (is_cdvd_init){
                g_logFile << "" << std::endl;
            }
            else if (is_cdvd_s_commands){
                g_logFile << "" << std::endl;
            }
            else if (is_cdvd_n_commands){
                g_logFile << "" << std::endl;
            }
            else if (is_cdvd_search_file){
                g_logFile << "" << std::endl;
            }
            else if (is_cdvd_disk_ready){
                g_logFile << "" << std::endl;
            }
            else if (is_libsd_remote){
                g_logFile << "" << std::endl;
            }
            else if (is_mtap_port_open){
                g_logFile << "" << std::endl;
            }
            else if (is_mtap_port_close){
                g_logFile << "" << std::endl;
            }
            else if (is_mtap_get_connections){
                g_logFile << "" << std::endl;
            }
            else if (is_mtap_unknown_1){
                g_logFile << "" << std::endl;
            }
            else if (is_mtap_unknown_2){
                g_logFile << "" << std::endl;
            }
            else if (is_eye_toy){
                g_logFile << "" << std::endl;
            }
            
            
            
            */

        




        else if (command_id == 0x8000000C){ // Get other data

        }
    }
    
    // Generate unique DMA ID
    static uint32_t dma_id = 0;
    dma_id++;
    g_sif.last_dma_id = dma_id;
    
    g_logFile << "  Returning DMA ID: " << dma_id << std::endl;
    
    // Return the DMA ID
    ctx.cpuRegs.GPR.r[2].UL[0] = dma_id;
    
    // CRITICAL: For SIF DMA in HLE mode, we need to trigger completion
    // This simulates the IOP receiving and responding
    
    // Set the SIF1 (channel 6) interrupt to indicate completion
    g_dmac.stat |= DSTAT_CIS(DMA_SIF1);
    
    // Check if this triggers an enabled interrupt
    if (g_dmac.CheckInterrupt()) {
        g_logFile << "  DMA complete - dispatching interrupt" << std::endl;
        g_dmac.DispatchInterrupt(ctx);
    }
}

void sceSifDmaStat(CpuContext& ctx) {
    uint32_t dma_id = ctx.cpuRegs.GPR.r[4].UL[0];
    g_logFile << "Syscall: sceSifDmaStat(id=" << std::dec << dma_id << ") = -1 (complete)" << std::endl;
    ctx.cpuRegs.GPR.r[2].SL[0] = -1;  // -1 = completed
} 
void GetMemorySize(CpuContext& ctx) {
    g_logFile << "Syscall: GetMemorySize() called" << std::endl;

    // The PS2 standard main memory size is 32MB (0x02000000 bytes).
    // While debug units (TOOL) had 128MB, retail games expect 32MB.
    ctx.cpuRegs.GPR.r[2].SL[0] = 0x02000000;
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

void SetSysCall(CpuContext& ctx) {

    g_logFile << "Syscall: SetSysCall() called!" << std::endl;

    int index = ctx.cpuRegs.GPR.r[4].SL[0];      // $a0: The Syscall Index
    uint32_t new_addr = ctx.cpuRegs.GPR.r[5].UL[0]; // $a1: The Guest Address

    if (index >= 0 && index < 256) {
        // 1. Update your C++ internal tracking array for the dispatcher
        custom_syscall_addresses[index] = new_addr;

        // 2. Calculate the physical/virtual address in the kernel table
        uint32_t table_entry_ptr = KERNEL_SYSCALL_TABLE_BASE + (index * 4);

        // 3. Write the new address into emulated RAM
        // This allows other guest functions to see the change in memory
        memory::write<uint32_t>(table_entry_ptr, new_addr);

        g_logFile << "SetSyscall: Index " << std::dec << index 
                  << " redirected to Guest Address 0x" << std::hex << new_addr 
                  << " (Table Entry at 0x" << table_entry_ptr << ")" << std::endl;
    }
}

void sceSifSetDChain(CpuContext& ctx) {
    g_logFile << "Syscall: sceSifSetDChain() called!" << std::endl;

    // SIF1 is DMA Channel 6.
    // The TADR (Tag Address Register) for Ch6 is located at 0x1000806C.
    // We read this from emulated memory to see where the DMA chain starts.
    uint32_t tadr = memory::read<uint32_t>(0x1000806C);
    
    // The QWC (Quadword Count) register is at 0x10008068
    uint32_t qwc = memory::read<uint32_t>(0x10008068);

    g_logFile << "  [SIF1] Starting DMA Chain Transfer" << std::endl;
    g_logFile << "  TADR (Chain Start): 0x" << std::hex << tadr << std::endl;
    g_logFile << "  QWC  (Initial Count): " << std::dec << qwc << std::endl;

    // LOGIC EXPLANATION:
    // In a LLE (Low Level) emulator, we would now parse the DMAtag at 'tadr',
    // reading data from main RAM and pushing it into the SIF FIFO.
    // The IOP would then read from that FIFO.
    //
    // Since we are HLE-ing the SIF (see sifRpcCall), we assume this transfer
    // is either:
    // 1. Handled by our other HLE replacements.
    // 2. A low-level command we can't easily process without an IOP emulator.
    //
    // We return 1 (Success) to tell the game the transfer "started".
    // If the game waits for a specific response packet from the IOP, 
    // further specific HLE logic would be needed here.

    // Return 1 to indicate the DMA channel was successfully configured/started.
    ctx.cpuRegs.GPR.r[2].UD[0] = 1;
}

void FlushCache(CpuContext& ctx) {
    uint32_t mode = ctx.cpuRegs.GPR.r[4].UL[0];

    g_logFile << "Syscall: FlushCache(mode: " << std::dec << mode << ")" << std::endl;

    // --- DIAGNOSTIC PROBE START ---
    // We are checking the address that failed in your previous log (0x2dad60)
    // to see what the hardware (DMAC) is seeing right now.
    
    // NOTE: Replace 'ctx.memory.read32' with your actual RAM read function.
    // We want to read from PHYSICAL RAM, bypassing any cache logic you might have.
    uint32_t debug_addr = 0x2dad60; 
    uint32_t val1 = memory::read<uint32_t>(debug_addr);
    uint32_t val2 = memory::read<uint32_t>(debug_addr + 4);
    uint32_t val3 = memory::read<uint32_t>(debug_addr + 8);
    uint32_t val4 = memory::read<uint32_t>(debug_addr + 12);

    g_logFile << "[DEBUG Probe] RAM content at VIF Packet Start (0x2dad60):" << std::endl;
    g_logFile << "  " << std::hex << val1 << " " << val2 << " " << val3 << " " << val4 << std::endl;

    // The log showed this garbage data previously: 03000e09 ...
    // If we see that again here, we confirm the Data Cache was never flushed to RAM.
    // -----------------------------

    // Standard PS2 return behavior
    ctx.cpuRegs.GPR.r[2].SL[0] = 0;
}

void handle_branch_logic(const rabbitizer::InstructionR5900& instr, 
                        CpuContext& ctx, 
                        bool& exit_interpreter) {
    
    uint32_t current_pc = ctx.cpuRegs.pc;
    uint32_t delay_slot_pc = current_pc + 4;
    bool is_likely = false;
    bool branch_taken = false;
    uint32_t target_addr = 0;
    
    // Determine instruction type and calculate target
    switch (instr.getUniqueId()) {
        // ============================================
        // CONDITIONAL BRANCHES
        // ============================================
        case RABBITIZER_INSTR_ID_cpu_beq:
            branch_taken = (ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].UL[0] == 
                           ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())].UL[0]);
            target_addr = current_pc + 4 + (static_cast<int16_t>(instr.Get_immediate()) << 2);
            break;
            
        case RABBITIZER_INSTR_ID_cpu_bne:
            branch_taken = (ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].UL[0] != 
                           ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())].UL[0]);
            target_addr = current_pc + 4 + (static_cast<int16_t>(instr.Get_immediate()) << 2);
            break;
            
        case RABBITIZER_INSTR_ID_cpu_blez:
            branch_taken = (ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].SL[0] <= 0);
            target_addr = current_pc + 4 + (static_cast<int16_t>(instr.Get_immediate()) << 2);
            break;
            
        case RABBITIZER_INSTR_ID_cpu_bgtz:
            branch_taken = (ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].SL[0] > 0);
            target_addr = current_pc + 4 + (static_cast<int16_t>(instr.Get_immediate()) << 2);
            break;
            
        case RABBITIZER_INSTR_ID_cpu_bltz:
            branch_taken = (ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].SL[0] < 0);
            target_addr = current_pc + 4 + (static_cast<int16_t>(instr.Get_immediate()) << 2);
            break;
            
        case RABBITIZER_INSTR_ID_cpu_bgez:
            branch_taken = (ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].SL[0] >= 0);
            target_addr = current_pc + 4 + (static_cast<int16_t>(instr.Get_immediate()) << 2);
            break;
            
        case RABBITIZER_INSTR_ID_cpu_beqz:
            branch_taken = (ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].UL[0] == 0);
            target_addr = current_pc + 4 + (static_cast<int16_t>(instr.Get_immediate()) << 2);
            break;
            
        case RABBITIZER_INSTR_ID_cpu_bnez:
            branch_taken = (ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].UL[0] != 0);
            target_addr = current_pc + 4 + (static_cast<int16_t>(instr.Get_immediate()) << 2);
            break;
            
        // ============================================
        // LIKELY BRANCHES (delay slot only executes if branch taken)
        // ============================================
        case RABBITIZER_INSTR_ID_cpu_beql:
            is_likely = true;
            branch_taken = (ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].UL[0] == 
                           ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())].UL[0]);
            target_addr = current_pc + 4 + (static_cast<int16_t>(instr.Get_immediate()) << 2);
            break;
            
        case RABBITIZER_INSTR_ID_cpu_bnel:
            is_likely = true;
            branch_taken = (ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].UL[0] != 
                           ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())].UL[0]);
            target_addr = current_pc + 4 + (static_cast<int16_t>(instr.Get_immediate()) << 2);
            break;
            
        case RABBITIZER_INSTR_ID_cpu_blezl:
            is_likely = true;
            branch_taken = (ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].SL[0] <= 0);
            target_addr = current_pc + 4 + (static_cast<int16_t>(instr.Get_immediate()) << 2);
            break;
            
        case RABBITIZER_INSTR_ID_cpu_bgtzl:
            is_likely = true;
            branch_taken = (ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].SL[0] > 0);
            target_addr = current_pc + 4 + (static_cast<int16_t>(instr.Get_immediate()) << 2);
            break;
            
        case RABBITIZER_INSTR_ID_cpu_bltzl:
            is_likely = true;
            branch_taken = (ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].SL[0] < 0);
            target_addr = current_pc + 4 + (static_cast<int16_t>(instr.Get_immediate()) << 2);
            break;
            
        case RABBITIZER_INSTR_ID_cpu_bgezl:
            is_likely = true;
            branch_taken = (ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].SL[0] >= 0);
            target_addr = current_pc + 4 + (static_cast<int16_t>(instr.Get_immediate()) << 2);
            break;
            
        // ============================================
        // UNCONDITIONAL JUMPS
        // ============================================
        case RABBITIZER_INSTR_ID_cpu_j:
            branch_taken = true;
            target_addr = (current_pc & 0xF0000000) | (instr.Get_instr_index() << 2);
            break;
            
        case RABBITIZER_INSTR_ID_cpu_b:
            branch_taken = true;
            target_addr = current_pc + 4 + (static_cast<int16_t>(instr.Get_immediate()) << 2);
            break;
            
        // ============================================
        // JUMP AND LINK (Function calls)
        // ============================================
        case RABBITIZER_INSTR_ID_cpu_jal:
            branch_taken = true;
            target_addr = (current_pc & 0xF0000000) | (instr.Get_instr_index() << 2);
            // Save return address BEFORE delay slot executes
            ctx.cpuRegs.GPR.r[31].UL[0] = current_pc + 8;
            break;
            
        case RABBITIZER_INSTR_ID_cpu_jalr:
            branch_taken = true;
            target_addr = ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].UL[0];
            // Save return address BEFORE delay slot executes
            ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rd())].UL[0] = current_pc + 8;
            break;
            
        // ============================================
        // JUMP REGISTER (indirect jump, often for returns)
        // ============================================
        case RABBITIZER_INSTR_ID_cpu_jr:
            branch_taken = true;
            target_addr = ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].UL[0];
            // If jumping to $ra, this is likely a function return
            if (static_cast<uint8_t>(instr.GetO32_rs()) == 31) {
                exit_interpreter = true;  // Signal to exit after delay slot
            }
            break;
            
        default:
            g_logFile << "Dynamic interpreter: Unknown branch/jump instruction " 
                      << instr.getOpcodeName() << " at 0x" 
                      << std::hex << instr.getVram() << std::endl;
            exit(1);
            return;
    }
    
    // ============================================
    // DELAY SLOT HANDLING
    // ============================================
    
    // For likely branches, only execute delay slot if branch is taken
    if (is_likely && !branch_taken) {
        // Skip delay slot (nullify it)
        ctx.cpuRegs.pc = current_pc + 8;  // PC after delay slot
        return;
    }
    
    // Execute delay slot instruction
    uint32_t delay_slot_word = memory::read<uint32_t>(delay_slot_pc);
    rabbitizer::InstructionR5900 delay_slot_instr(delay_slot_word, delay_slot_pc);
    
    // Sanity check: delay slot shouldn't be a branch/jump
    if (delay_slot_instr.isBranch() || delay_slot_instr.isJump()) {
        g_logFile << "WARNING: Branch in delay slot at 0x" 
                  << std::hex << delay_slot_pc << std::endl;
    }
    
    // Execute the delay slot
    execute_single_instruction(delay_slot_instr, ctx);
    
    // ============================================
    // UPDATE PC BASED ON BRANCH OUTCOME
    // ============================================
    
    if (branch_taken) {
        ctx.cpuRegs.pc = target_addr;
        
        g_logFile << "Dynamic interpreter: Branch taken to 0x" 
                  << std::hex << target_addr << std::endl;
        
        // Check if target is in recompiled code
        auto it = recompiled_functions.find(target_addr);
        if (it != recompiled_functions.end()) {
            g_logFile << "Dynamic interpreter: Jumping to recompiled function at 0x" 
                      << std::hex << target_addr << std::endl;
            it->second(ctx, target_addr);
        }
    } else {
        // Branch not taken, continue to instruction after delay slot
        ctx.cpuRegs.pc = current_pc + 8;
    }
}

void execute_single_instruction(const rabbitizer::InstructionR5900& instr, CpuContext& ctx) {
    switch (instr.getUniqueId()) {
        // ========================================
        // INTEGER ALU OPERATIONS
        // ========================================
        case RABBITIZER_INSTR_ID_cpu_add:
        case RABBITIZER_INSTR_ID_cpu_addu:
            ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rd())].SL[0] = 
                ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].SL[0] + 
                ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())].SL[0];
            break;
            
        case RABBITIZER_INSTR_ID_cpu_sub:
        case RABBITIZER_INSTR_ID_cpu_subu:
            ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rd())].SL[0] = 
                ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].SL[0] - 
                ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())].SL[0];
            break;
            
        case RABBITIZER_INSTR_ID_cpu_addi:
        case RABBITIZER_INSTR_ID_cpu_addiu:
            ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())].SL[0] = 
                ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].SL[0] + 
                static_cast<int16_t>(instr.Get_immediate());
            break;
            
        // 64-bit operations
        case RABBITIZER_INSTR_ID_cpu_daddu:
            ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rd())].SD[0] = 
                ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].SD[0] + 
                ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())].SD[0];
            break;
            
        case RABBITIZER_INSTR_ID_cpu_daddiu:
            ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())].SD[0] = 
                ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].SD[0] + 
                static_cast<int16_t>(instr.Get_immediate());
            break;
            
        // ========================================
        // LOGICAL OPERATIONS
        // ========================================
        case RABBITIZER_INSTR_ID_cpu_and:
            ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rd())].UL[0] = 
                ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].UL[0] & 
                ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())].UL[0];
            break;
            
        case RABBITIZER_INSTR_ID_cpu_or:
            ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rd())].UL[0] = 
                ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].UL[0] | 
                ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())].UL[0];
            break;
            
        case RABBITIZER_INSTR_ID_cpu_xor:
            ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rd())].UL[0] = 
                ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].UL[0] ^ 
                ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())].UL[0];
            break;
            
        case RABBITIZER_INSTR_ID_cpu_nor:
            ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rd())].UL[0] = 
                ~(ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].UL[0] | 
                  ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())].UL[0]);
            break;
            
        case RABBITIZER_INSTR_ID_cpu_andi:
            ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())].UL[0] = 
                ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].UL[0] & 
                instr.Get_immediate();
            break;
            
        case RABBITIZER_INSTR_ID_cpu_ori:
            ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())].UL[0] = 
                ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].UL[0] | 
                instr.Get_immediate();
            break;
            
        case RABBITIZER_INSTR_ID_cpu_xori:
            ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())].UL[0] = 
                ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].UL[0] ^ 
                instr.Get_immediate();
            break;
            
        case RABBITIZER_INSTR_ID_cpu_lui:
            ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())].UL[0] = 
                instr.Get_immediate() << 16;
            break;
            
        // ========================================
        // SHIFT OPERATIONS
        // ========================================
        case RABBITIZER_INSTR_ID_cpu_sll:
            ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rd())].UL[0] = 
                ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())].UL[0] << 
                instr.Get_sa();
            break;
            
        case RABBITIZER_INSTR_ID_cpu_srl:
            ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rd())].UL[0] = 
                ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())].UL[0] >> 
                instr.Get_sa();
            break;
            
        case RABBITIZER_INSTR_ID_cpu_sra:
            ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rd())].SL[0] = 
                ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())].SL[0] >> 
                instr.Get_sa();
            break;
            
        case RABBITIZER_INSTR_ID_cpu_sllv:
            ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rd())].UL[0] = 
                ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())].UL[0] << 
                (ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].UL[0] & 0x1F);
            break;
            
        case RABBITIZER_INSTR_ID_cpu_srlv:
            ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rd())].UL[0] = 
                ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())].UL[0] >> 
                (ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].UL[0] & 0x1F);
            break;
            
        case RABBITIZER_INSTR_ID_cpu_srav:
            ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rd())].SL[0] = 
                ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())].SL[0] >> 
                (ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].UL[0] & 0x1F);
            break;
            
        // ========================================
        // MULTIPLY/DIVIDE
        // ========================================
        case RABBITIZER_INSTR_ID_cpu_mult: {
            int64_t result = static_cast<int64_t>(ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].SL[0]) * 
                            static_cast<int64_t>(ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())].SL[0]);
            ctx.cpuRegs.LO.UL[0] = static_cast<uint32_t>(result);
            ctx.cpuRegs.HI.UL[0] = static_cast<uint32_t>(result >> 32);
            break;
        }
        
        case RABBITIZER_INSTR_ID_cpu_multu: {
            uint64_t result = static_cast<uint64_t>(ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].UL[0]) * 
                             static_cast<uint64_t>(ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())].UL[0]);
            ctx.cpuRegs.LO.UL[0] = static_cast<uint32_t>(result);
            ctx.cpuRegs.HI.UL[0] = static_cast<uint32_t>(result >> 32);
            break;
        }
        
        case RABBITIZER_INSTR_ID_cpu_div:
        case RABBITIZER_INSTR_ID_cpu_divu:
            if (ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())].SL[0] != 0) {
                ctx.cpuRegs.LO.SL[0] = ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].SL[0] / 
                                       ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())].SL[0];
                ctx.cpuRegs.HI.SL[0] = ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].SL[0] % 
                                       ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())].SL[0];
            }
            break;
            
        case RABBITIZER_INSTR_ID_cpu_ddivu:
            if (ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())].UD[0] != 0) {
                ctx.cpuRegs.LO.UD[0] = ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].UD[0] / 
                                       ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())].UD[0];
                ctx.cpuRegs.HI.UD[0] = ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].UD[0] % 
                                       ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())].UD[0];
            }
            break;
            
        // ========================================
        // COMPARISON OPERATIONS
        // ========================================
        case RABBITIZER_INSTR_ID_cpu_slt:
            ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rd())].UL[0] = 
                (ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].SL[0] < 
                 ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())].SL[0]) ? 1 : 0;
            break;
            
        case RABBITIZER_INSTR_ID_cpu_sltu:
            ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rd())].UL[0] = 
                (ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].UL[0] < 
                 ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())].UL[0]) ? 1 : 0;
            break;
            
        case RABBITIZER_INSTR_ID_cpu_slti:
            ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())].UL[0] = 
                (ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].SL[0] < 
                 static_cast<int16_t>(instr.Get_immediate())) ? 1 : 0;
            break;
            
        case RABBITIZER_INSTR_ID_cpu_sltiu:
            ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())].UL[0] = 
                (ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].UL[0] < 
                 static_cast<uint16_t>(instr.Get_immediate())) ? 1 : 0;
            break;
            
        case RABBITIZER_INSTR_ID_cpu_movz:
            if (ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())].UL[0] == 0) {
                ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rd())].UL[0] = 
                    ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].UL[0];
            }
            break;
            
        case RABBITIZER_INSTR_ID_cpu_movn:
            if (ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())].UL[0] != 0) {
                ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rd())].UL[0] = 
                    ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].UL[0];
            }
            break;
            
        // ========================================
        // LOAD OPERATIONS
        // ========================================
        case RABBITIZER_INSTR_ID_cpu_lb: {
            uint32_t addr = ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].UL[0] + 
                           static_cast<int16_t>(instr.Get_immediate());
            ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())].SL[0] = 
                static_cast<int32_t>(static_cast<int8_t>(memory::read<uint8_t>(addr)));
            break;
        }
        
        case RABBITIZER_INSTR_ID_cpu_lbu: {
            uint32_t addr = ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].UL[0] + 
                           static_cast<int16_t>(instr.Get_immediate());
            ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())].UL[0] = 
                static_cast<uint32_t>(memory::read<uint8_t>(addr));
            break;
        }
        
        case RABBITIZER_INSTR_ID_cpu_lh: {
            uint32_t addr = ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].UL[0] + 
                           static_cast<int16_t>(instr.Get_immediate());
            ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())].SL[0] = 
                static_cast<int32_t>(static_cast<int16_t>(memory::read<uint16_t>(addr)));
            break;
        }
        
        case RABBITIZER_INSTR_ID_cpu_lhu: {
            uint32_t addr = ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].UL[0] + 
                           static_cast<int16_t>(instr.Get_immediate());
            ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())].UL[0] = 
                static_cast<uint32_t>(memory::read<uint16_t>(addr));
            break;
        }
        
        case RABBITIZER_INSTR_ID_cpu_lw: {
            uint32_t addr = ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].UL[0] + 
                           static_cast<int16_t>(instr.Get_immediate());
            ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())].UL[0] = memory::read<uint32_t>(addr);
            break;
        }
        case RABBITIZER_INSTR_ID_cpu_lwu: {
            uint32_t addr = ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].UL[0] + 
                           static_cast<int16_t>(instr.Get_immediate());
            // Read 32-bit value, cast to 64-bit (zero-extension), store in lower 64 bits (UD[0])
            ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())].UD[0] = 
                static_cast<uint64_t>(memory::read<uint32_t>(addr));
            break;
        }
        
        case RABBITIZER_INSTR_ID_cpu_ld: {
            uint32_t addr = ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].UL[0] + 
                           static_cast<int16_t>(instr.Get_immediate());
            ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())].UD[0] = memory::read<uint64_t>(addr);
            break;
        }
        
        case RABBITIZER_INSTR_ID_r5900_lq: {
            uint32_t addr = ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].UL[0] + 
                           static_cast<int16_t>(instr.Get_immediate());
            memory::read_quad(addr, *reinterpret_cast<QuadWord*>(&ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())]));
            break;
        }
        
        // ========================================
        // STORE OPERATIONS
        // ========================================
        case RABBITIZER_INSTR_ID_cpu_sb: {
            uint32_t addr = ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].UL[0] + 
                           static_cast<int16_t>(instr.Get_immediate());
            memory::write<uint8_t>(addr, ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())].UL[0]);
            break;
        }
        
        case RABBITIZER_INSTR_ID_cpu_sh: {
            uint32_t addr = ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].UL[0] + 
                           static_cast<int16_t>(instr.Get_immediate());
            memory::write<uint16_t>(addr, ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())].UL[0]);
            break;
        }
        
        case RABBITIZER_INSTR_ID_cpu_sw: {
            uint32_t addr = ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].UL[0] + 
                           static_cast<int16_t>(instr.Get_immediate());
            memory::write<uint32_t>(addr, ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())].UL[0]);
            break;
        }
        
        case RABBITIZER_INSTR_ID_cpu_sd: {
            uint32_t addr = ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].UL[0] + 
                           static_cast<int16_t>(instr.Get_immediate());
            memory::write<uint64_t>(addr, ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())].UD[0]);
            break;
        }
        
        case RABBITIZER_INSTR_ID_r5900_sq: {
            uint32_t addr = ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rs())].UL[0] + 
                           static_cast<int16_t>(instr.Get_immediate());
            memory::write_quad(addr, *reinterpret_cast<const QuadWord*>(&ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())]));
            break;
        }
        
        // ========================================
        // FLOATING POINT OPERATIONS
        // ========================================
        case RABBITIZER_INSTR_ID_cpu_add_s:
            ctx.fpuRegs.fpr[static_cast<uint8_t>(instr.GetO32_fd())].f = 
                ctx.fpuRegs.fpr[static_cast<uint8_t>(instr.GetO32_fs())].f + 
                ctx.fpuRegs.fpr[static_cast<uint8_t>(instr.GetO32_ft())].f;
            break;
            
        case RABBITIZER_INSTR_ID_cpu_sub_s:
            ctx.fpuRegs.fpr[static_cast<uint8_t>(instr.GetO32_fd())].f = 
                ctx.fpuRegs.fpr[static_cast<uint8_t>(instr.GetO32_fs())].f - 
                ctx.fpuRegs.fpr[static_cast<uint8_t>(instr.GetO32_ft())].f;
            break;
            
        case RABBITIZER_INSTR_ID_cpu_mul_s:
            ctx.fpuRegs.fpr[static_cast<uint8_t>(instr.GetO32_fd())].f = 
                ctx.fpuRegs.fpr[static_cast<uint8_t>(instr.GetO32_fs())].f * 
                ctx.fpuRegs.fpr[static_cast<uint8_t>(instr.GetO32_ft())].f;
            break;
            
        case RABBITIZER_INSTR_ID_cpu_div_s:
            ctx.fpuRegs.fpr[static_cast<uint8_t>(instr.GetO32_fd())].f = 
                ctx.fpuRegs.fpr[static_cast<uint8_t>(instr.GetO32_fs())].f / 
                ctx.fpuRegs.fpr[static_cast<uint8_t>(instr.GetO32_ft())].f;
            break;
            
        case RABBITIZER_INSTR_ID_cpu_mtc1:
            ctx.fpuRegs.fpr[static_cast<uint8_t>(instr.GetO32_fs())].UL = 
                ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())].UL[0];
            break;
            
        case RABBITIZER_INSTR_ID_cpu_mfc1:
            ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())].UL[0] = 
                ctx.fpuRegs.fpr[static_cast<uint8_t>(instr.GetO32_fs())].UL;
            break;
        case RABBITIZER_INSTR_ID_cpu_mtc0:
            handle_mtc0_write(ctx, 
                            static_cast<uint8_t>(instr.GetO32_rd()),
                            ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())].UL[0]);
            break;
        case RABBITIZER_INSTR_ID_cpu_mfc0:
            ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())].UL[0] = 
                ctx.cpuRegs.CP0.r[static_cast<uint8_t>(instr.GetO32_rd())];
            break;
            
        // ========================================
        // SYSTEM OPERATIONS
        // ========================================
        case RABBITIZER_INSTR_ID_cpu_syscall:
            runtime_syscall_dispatcher(ctx.cpuRegs.GPR.r[3].UL[0], ctx);
            break;
            
        case RABBITIZER_INSTR_ID_cpu_sync:
            // Memory barrier - no-op in interpreter
            break;
            
        case RABBITIZER_INSTR_ID_r5900_ei:
            ctx.cpuRegs.CP0.n.Status |= 0x1;  // Enable interrupts
            break;
            
        case RABBITIZER_INSTR_ID_r5900_di:
            ctx.cpuRegs.CP0.n.Status &= ~0x1;  // Disable interrupts
            break;
            
        case RABBITIZER_INSTR_ID_cpu_nop:
            // No operation
            break;
        case RABBITIZER_INSTR_ID_cpu_dsra32:
            // Doubleword Shift Right Arithmetic + 32
            // We use SD[0] (Signed 64-bit) to ensure C++ performs sign extension (arithmetic shift).
            // The shift amount is the instruction's 'sa' field plus 32.
            ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rd())].SD[0] = 
                ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())].SD[0] >> 
                (instr.Get_sa() + 32);
            break;
        case RABBITIZER_INSTR_ID_cpu_eret:
            if (ctx.cpuRegs.CP0.n.Status & 0x4) { // Check ERL
                 // CHANGED: ErrorPC -> ErrorEPC
                 ctx.cpuRegs.pc = ctx.cpuRegs.CP0.n.ErrorEPC;
                 ctx.cpuRegs.CP0.n.Status &= ~0x4;
            } else {
                 ctx.cpuRegs.pc = ctx.cpuRegs.CP0.n.EPC;
                 ctx.cpuRegs.CP0.n.Status &= ~0x2;
            }
            
            // The interpreter loop adds +4 after every instruction. 
            // We subtract 4 here so the next loop iteration lands exactly on the target PC.
            ctx.cpuRegs.pc -= 4; 
            break;


        case RABBITIZER_INSTR_ID_cpu_dsll:
            // 64-bit shift: writes to UD[0] (unsigned doubleword)
            ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rd())].UD[0] = 
                ctx.cpuRegs.GPR.r[static_cast<uint8_t>(instr.GetO32_rt())].UD[0] << 
                instr.Get_sa();
            break;


            
        // ========================================
        // UNHANDLED INSTRUCTIONS
        // ========================================
        default:
            g_logFile << "Dynamic interpreter: Unhandled instruction " 
                      << instr.getOpcodeName() << " at 0x" 
                      << std::hex << instr.getVram() 
                      << " (opcode: 0x" << static_cast<int>(instr.Get_opcode())
                      << ", function: 0x" << static_cast<int>(instr.Get_function()) << ")"
                      << std::endl;
            exit(1);
    }
}

void AddIntcHandler(CpuContext& ctx) {
    g_logFile << "Syscall: AddIntcHandler" << std::endl;
    
    // Extract parameters from registers
    int int_cause = ctx.cpuRegs.GPR.r[4].SL[0];      // $a0: interrupt cause
    uint32_t handler_pc = ctx.cpuRegs.GPR.r[5].UL[0]; // $a1: handler function address
    int next = ctx.cpuRegs.GPR.r[6].SL[0];            // $a2: position reference
    uint32_t arg = ctx.cpuRegs.GPR.r[7].UL[0];        // $a3: argument
    
    // The 5th parameter (flag) is passed on the stack
    // Stack layout: [arg4/flag] [arg5] ... at $sp + 16 (after 4 saved regs)
    uint32_t stack_ptr = ctx.cpuRegs.GPR.r[29].UL[0];
    int flag = static_cast<int>(memory::read<int32_t>(stack_ptr + 16));
    
    g_logFile << "Syscall: AddIntcHandler(cause: " << std::dec << int_cause 
              << ", handler: 0x" << std::hex << handler_pc 
              << ", next: " << std::dec << next
              << ", arg: 0x" << std::hex << arg
              << ", flag: " << std::dec << flag << ")" << std::endl;
    
    // Validate interrupt cause (PS2 has 16 interrupt sources: 0-15)
    if (int_cause < 0 || int_cause > 14) {
        g_logFile << "  ERROR: Invalid interrupt cause: " << int_cause << std::endl;
        ctx.cpuRegs.GPR.r[2].SL[0] = -1; // Return -1 for error
        return;
    }
    
    // Validate handler address (should be in valid memory range)
    if (handler_pc == 0) {
        g_logFile << "  ERROR: NULL handler address" << std::endl;
        ctx.cpuRegs.GPR.r[2].SL[0] = -1;
        return;
    }
    
    // Create new handler
    IntcHandler new_handler;
    new_handler.id = g_nextIntcId++;
    new_handler.cause = int_cause;
    new_handler.handler_pc = handler_pc;
    new_handler.gp = ctx.cpuRegs.GPR.r[28].UL[0]; // Capture current $gp
    new_handler.arg = arg;
    new_handler.flag = flag;
    new_handler.active = true;
    
    // Get or create the handler queue for this cause
    std::vector<IntcHandler>& queue = g_intc_queues[int_cause];
    
    // Determine insertion position
    if (next == 0) {
        // Insert at front of queue
        queue.insert(queue.begin(), new_handler);
        g_logFile << "  Inserted handler " << new_handler.id 
                  << " at FRONT of queue for cause " << int_cause << std::endl;
    }
    else if (next == -1) {
        // Insert at back of queue
        queue.push_back(new_handler);
        g_logFile << "  Inserted handler " << new_handler.id 
                  << " at BACK of queue for cause " << int_cause << std::endl;
    }
    else {
        // Insert before handler with ID 'next'
        bool found = false;
        for (auto it = queue.begin(); it != queue.end(); ++it) {
            if (it->id == next) {
                queue.insert(it, new_handler);
                found = true;
                g_logFile << "  Inserted handler " << new_handler.id 
                          << " BEFORE handler " << next 
                          << " in queue for cause " << int_cause << std::endl;
                break;
            }
        }
        
        if (!found) {
            g_logFile << "  WARNING: Handler " << next 
                      << " not found, inserting at back" << std::endl;
            queue.push_back(new_handler);
        }
    }
    
    g_logFile << "  Queue for cause " << int_cause << " now has " 
              << queue.size() << " handlers" << std::endl;
    
    // Return handler ID in $v0
    ctx.cpuRegs.GPR.r[2].SL[0] = new_handler.id;
}

void RemoveIntcHandler(CpuContext& ctx) {
    g_logFile << "Syscall: RemoveIntcHandler" << std::endl;
    
    // Extract parameters from registers
    int32_t int_cause = ctx.cpuRegs.GPR.r[4].SL[0];   // $a0: interrupt cause
    int32_t handler_id = ctx.cpuRegs.GPR.r[5].SL[0];  // $a1: handler ID (not address!)
    
    g_logFile << "  RemoveIntcHandler(cause: " << std::dec << int_cause 
              << ", handler_id: " << handler_id << ")" << std::endl;
    
    // Validate interrupt cause
    if (int_cause < 0 || int_cause > 14) {
        g_logFile << "  ERROR: Invalid interrupt cause: " << int_cause << std::endl;
        ctx.cpuRegs.GPR.r[2].SL[0] = -1;
        return;
    }
    
    // Get the handler queue for this cause
    std::vector<IntcHandler>& queue = g_intc_queues[int_cause];
    
    // Search for the handler with matching ID
    for (auto it = queue.begin(); it != queue.end(); ++it) {
        if (it->id == handler_id) {
            // Found it! Log details before removing
            g_logFile << "  Found handler ID " << handler_id 
                      << " at address 0x" << std::hex << it->handler_pc 
                      << std::dec << std::endl;
            
            // Remove from queue
            queue.erase(it);
            
            g_logFile << "  Removed handler. Queue for cause " << int_cause 
                      << " now has " << queue.size() << " handlers" << std::endl;
            
            // Return 0 for success
            ctx.cpuRegs.GPR.r[2].SL[0] = 0;
            return;
        }
    }
    
    // Handler not found in this cause's queue
    g_logFile << "  ERROR: Handler ID " << handler_id 
              << " not found in cause " << int_cause << " queue" << std::endl;
    
    // Debug: Check if it exists in a different queue
    for (int other_cause = 0; other_cause <= 14; other_cause++) {
        if (other_cause == int_cause) continue;
        
        for (const auto& h : g_intc_queues[other_cause]) {
            if (h.id == handler_id) {
                g_logFile << "  DEBUG: Handler ID " << handler_id 
                          << " exists in cause " << other_cause 
                          << " queue instead!" << std::endl;
                break;
            }
        }
    }
    
    // Return -1 for failure
    ctx.cpuRegs.GPR.r[2].SL[0] = -1;
}
void SysRemoveDmacHandler(CpuContext &ctx){
    g_logFile << "Syscall: RemoveDmacHandler" << std::endl;
    int dma_cause = ctx.cpuRegs.GPR.r[4].SL[0];       // $a0
    uint32_t handler_id = ctx.cpuRegs.GPR.r[5].UL[0]; // $a1

    switch(dma_cause){
        case 0:
            g_logFile << "  Syscall: RemoveDmacHandler CAUSE: Vector Interface 0" << std::endl;
        case 1:
            g_logFile << "  Syscall: RemoveDmacHandler CAUSE: Vector Interface 1" << std::endl;
        case 2:
            g_logFile << "  Syscall: RemoveDmacHandler CAUSE: Graphicsc Interface" << std::endl;
        case 3:
            g_logFile << "  Syscall: RemoveDmacHandler CAUSE: IPU output" << std::endl;
        case 4:
            g_logFile << "  Syscall: RemoveDmacHandler CAUSE: IPU input" << std::endl;
        case 5:
            g_logFile << "  Syscall: RemoveDmacHandler CAUSE: IOP→EE" << std::endl;
        case 6:
            g_logFile << "  Syscall: RemoveDmacHandler CAUSE: EE→IOP" << std::endl;
        case 7:
            g_logFile << "  Syscall: RemoveDmacHandlerr CAUSE: Bidirectional" << std::endl;
        case 8:
            g_logFile << "  Syscall: RemoveDmacHandler CAUSE: Scratchpad read" << std::endl;
        case 9:
            g_logFile << "  Syscall: RemoveDmacHandler CAUSE: Scratchpad write" << std::endl;
        default:
            {
                g_logFile << "  Syscall: RemoveDmacHandler CAUSE: IDK!!!"<< std::endl;
            }   
    }

    int return_val = RemoveDmacHandler(dma_cause, handler_id);
    ctx.cpuRegs.GPR.r[2].SL[0] = static_cast<int32_t>(return_val);

}
void AddDmacHandler(CpuContext& ctx) {
    g_logFile << "Syscall: AddDmacHandler" << std::endl;

    // 1. Extract parameters
    int dma_cause = ctx.cpuRegs.GPR.r[4].SL[0];       // $a0
    uint32_t handler_pc = ctx.cpuRegs.GPR.r[5].UL[0]; // $a1
    int next = ctx.cpuRegs.GPR.r[6].SL[0];            // $a2
    uint32_t arg = ctx.cpuRegs.GPR.r[7].UL[0];        // $a3

    switch(dma_cause){
        case 0:
            g_logFile << "  Syscall: AddDmacHandler CAUSE: Vector Interface 0" << std::endl;
        case 1:
            g_logFile << "  Syscall: AddDmacHandler CAUSE: Vector Interface 1" << std::endl;
        case 2:
            g_logFile << "  Syscall: AddDmacHandler CAUSE: Graphicsc Interface" << std::endl;
        case 3:
            g_logFile << "  Syscall: AddDmacHandler CAUSE: IPU output" << std::endl;
        case 4:
            g_logFile << "  Syscall: AddDmacHandler CAUSE: IPU input" << std::endl;
        case 5:
            g_logFile << "  Syscall: AddDmacHandler CAUSE: IOP→EE" << std::endl;
        case 6:
            g_logFile << "  Syscall: AddDmacHandler CAUSE: EE→IOP" << std::endl;
        case 7:
            g_logFile << "  Syscall: AddDmacHandler CAUSE: Bidirectional" << std::endl;
        case 8:
            g_logFile << "  Syscall: AddDmacHandler CAUSE: Scratchpad read" << std::endl;
        case 9:
            g_logFile << "  Syscall: AddDmacHandler CAUSE: Scratchpad write" << std::endl;
    }

    // The 5th parameter (flag) is on the stack at $sp + 16
    uint32_t stack_ptr = ctx.cpuRegs.GPR.r[29].UL[0];
    int flag = static_cast<int>(memory::read<int32_t>(stack_ptr + 16));

    // 2. Validate inputs
    if (dma_cause < 0 || dma_cause > 15) {
        ctx.cpuRegs.GPR.r[2].SL[0] = -1;
        return;
    }
    if (handler_pc == 0) {
        ctx.cpuRegs.GPR.r[2].SL[0] = -1;
        return;
    }


    DmacHandler new_handler;
    new_handler.id = g_next_dmac_handler_id++;
    new_handler.handler_pc = handler_pc;
    new_handler.gp = ctx.cpuRegs.GPR.r[28].UL[0]; // Capture current GP
    new_handler.arg = arg;
    new_handler.flag = flag;
    new_handler.active = true;

    std::vector<DmacHandler>& queue = g_dmac_queues[dma_cause];

    if (next == 0) {
        // Insert at front
        queue.insert(queue.begin(), new_handler);
    } 
    else if (next == -1) {
        // Insert at back
        queue.push_back(new_handler);
    } 
    else {
        // Insert before specific ID
        bool found = false;
        for (auto it = queue.begin(); it != queue.end(); ++it) {
            if (it->id == next) {
                queue.insert(it, new_handler);
                found = true;
                break;
            }
        }
        if (!found) queue.push_back(new_handler);
    }

    // 5. Return the new ID
    ctx.cpuRegs.GPR.r[2].SL[0] = new_handler.id;
} 

void sceSifStopDma(CpuContext& ctx) {
    g_logFile << "Syscall: sceSifStopDma() called!" << std::endl;

    // SIF0 is DMA Channel 5 (IOP -> EE).
    // This function halts the reception of data from the IOP by clearing the Start bit.
    
    // 1. Clear the Start/Busy bit (Bit 8) in the CHCR
    // CHCR_STR is defined as (1 << 8) in dmac.h
    g_dmac.channels[DMA_SIF0].chcr &= ~CHCR_STR;

    // 2. Log the state change
    g_logFile << "  [SIF0] DMA Channel Stopped (STR bit cleared)." << std::endl;
    g_logFile << "  CHCR Now: 0x" << std::hex << g_dmac.channels[DMA_SIF0].chcr << std::dec << std::endl;

    // 3. Return 0 (Standard success/void return for this syscall)
    ctx.cpuRegs.GPR.r[2].SL[0] = 0;
}

void syscall_EnableIntc(CpuContext& ctx) {
    int cause_bit = ctx.cpuRegs.GPR.r[4].SL[0];  // a0
    bool was_disabled = g_intc.EnableIntc(cause_bit);
    was_disabled ? 1 : 0;
    ctx.cpuRegs.GPR.r[2].UL[0] = was_disabled;
}

// Syscall 15h  
void syscall_DisableIntc(CpuContext& ctx) {
    int cause_bit = ctx.cpuRegs.GPR.r[4].SL[0];  // a0
    bool was_enabled = g_intc.DisableIntc(cause_bit);
    was_enabled ? 1 : 0;
    ctx.cpuRegs.GPR.r[2].UL[0] = was_enabled;
}

void _DisableDmac(CpuContext& ctx) {
    g_logFile << "Syscall: _DisableDmac() called!" << std::endl;

    int channel = ctx.cpuRegs.GPR.r[4].SL[0]; // $a0: DMAC Channel

    g_logFile << "Syscall: _DisableDmac(cause_bit: " << std::dec << channel << ")" << std::endl;

    if (channel < 0 || channel > 15) {
        g_logFile << "  Error: Invalid DMAC channel " << channel << std::endl;
        ctx.cpuRegs.GPR.r[2].SL[0] = 0; 
        return;
    }

    // 1. Read current state using your DMAC class
    // We access the public 'stat' member directly or use Read()
    uint32_t current_stat = g_dmac.Read(0x1000E010); 
    
    // Mask bits are in the upper 16 bits (offset 16)
    int mask_bit = 16 + channel;
    
    // Check if currently Enabled (1)
    bool is_enabled = (current_stat >> mask_bit) & 1;

    if (is_enabled) {
        // 2. Use DMAC::Write to toggle the bit
        // Your Write logic for 0xE010 handles the toggling correctly:
        // stat = (stat & ...) ^ (value & ...);
        g_dmac.Write(0x1000E010, (1 << mask_bit));
        
        g_logFile << "  Disabled DMAC channel " << channel << " mask" << std::endl;
    } else {
        g_logFile << "  DMA Interrupt was already disabled." << std::endl;
    }

    // 3. Return true (1) if it was previously enabled
    ctx.cpuRegs.GPR.r[2].SL[0] = is_enabled ? 1 : 0;
}

void _EnableDmac(CpuContext& ctx) {
    g_logFile << "Syscall: _EnableDmac" << std::endl;
    
    // 1. Get Argument: $a0 (Register 4) contains the DMA channel bit index
    int cause_bit = ctx.cpuRegs.GPR.r[4].SL[0];

    g_logFile << "Syscall: _EnableDmac(cause_bit: " << std::dec << cause_bit << ")" << std::endl;

    // 2. Validation: Mask supports up to 15 (bits 16-31 of D_STAT)
    if (cause_bit < 0 || cause_bit > 15) {
        g_logFile << "  Error: Invalid DMA cause bit " << cause_bit << std::endl;
        ctx.cpuRegs.GPR.r[2].SL[0] = 0; // Return False (0)
        return;
    }

    // 3. Read current state via DMAC class
    // We use Read() to get the actual value of 'stat'
    uint32_t d_stat = g_dmac.Read(0x1000E010);

    // 4. Check if the interrupt was previously disabled (0)
    // The mask bits are in the upper 16 bits (offset 16)
    int mask_bit = 16 + cause_bit;
    bool was_disabled = !((d_stat >> mask_bit) & 1);

    // 5. Update using DMAC::Write
    if (was_disabled) {
        // Your DMAC::Write logic for D_STAT (0xE010) is:
        // stat = (stat & ~(value & 0xFFFF)) ^ (value & 0xFFFF0000);
        //
        // Writing 1 to an upper bit performs an XOR (toggle).
        // Since the bit is currently 0, XORing with 1 sets it to 1 (Enabled).
        g_dmac.Write(0x1000E010, (1 << mask_bit));
        
        g_logFile << "  Enabled DMAC channel " << cause_bit << " mask (via DMAC::Write)" << std::endl;
    } else {
        g_logFile << "  DMA Interrupt was already enabled." << std::endl;
    }

    // 6. Return Result in $v0
    ctx.cpuRegs.GPR.r[2].SL[0] = was_disabled ? 1 : 0;
}

uint32_t GetSifRegPAddr(uint32_t index) {
    switch (index) {
        case 1: return 0x1000F200; // SIF_MSCOM  (Main -> Sub)
        case 2: return 0x1000F210; // SIF_SMCOM  (Sub -> Main)
        case 3: return 0x1000F220; // SIF_MSFLAG (Main -> Sub Flag)
        case 4: return 0x1000F230; // SIF_SMFLAG (Sub -> Main Flag)
        case 5: return 0x1000F240; // SIF_CTRL   (Control)
        case 6: return 0x1000F260; // SIF_BD6    (Debug/Unk)
        default: return 0; // Invalid
    }
}

void sceSifGetReg(CpuContext& ctx) {
    g_logFile << "========================================" << std::endl;
    g_logFile << "Syscall: sceSifGetReg" << std::endl;
    
    // Argument a0 ($4): Register Index
    uint32_t index = ctx.cpuRegs.GPR.r[4].UL[0]; // $a0
    uint32_t result = 0;

    g_logFile << "  Input index (raw): 0x" << std::hex << index << std::dec << std::endl;

    if (index & 0x80000000) {
        // --- SYSTEM REGISTERS ---
        g_logFile << "  Type: SYSTEM REGISTER (software-only)" << std::endl;
        
        switch (index) {
            case 0x80000000: 
                result = g_sif.sys_subaddr; 
                g_logFile << "  Register: SIF_SYSREG_SUBADDR (0x80000000)" << std::endl;
                g_logFile << "  Description: IOP-side SIF buffer address" << std::endl;
                break;
            case 0x80000001: 
                result = g_sif.sys_mainaddr; 
                g_logFile << "  Register: SIF_SYSREG_MAINADDR (0x80000001)" << std::endl;
                g_logFile << "  Description: EE-side SIF buffer address" << std::endl;
                break;
            case 0x80000002: 
                result = g_sif.sys_rpcinit; 
                g_logFile << "  Register: SIF_SYSREG_RPCINIT (0x80000002)" << std::endl;
                g_logFile << "  Description: RPC initialization status" << std::endl;
                g_logFile << "  Typical values: 0=not init, non-zero=initialized" << std::endl;
                break;
            default:
                g_logFile << "  Register: UNKNOWN SYSTEM REGISTER" << std::endl;
                g_logFile << "  WARNING: Unknown System SIF Register read: 0x" 
                          << std::hex << index << std::dec << std::endl;
        }
    } else {
        // --- PHYSICAL REGISTERS ---
        g_logFile << "  Type: PHYSICAL REGISTER (memory-mapped)" << std::endl;
        
        uint32_t paddr = GetSifRegPAddr(index);
        
        switch (index) {
            case 1:
                g_logFile << "  Register: SIF_MSCOM (index 1)" << std::endl;
                g_logFile << "  Address: 0x1000F200" << std::endl;
                g_logFile << "  Description: Main-to-Sub Command buffer" << std::endl;
                g_logFile << "  Direction: EE → IOP" << std::endl;
                break;
            case 2:
                g_logFile << "  Register: SIF_SMCOM (index 2)" << std::endl;
                g_logFile << "  Address: 0x1000F210" << std::endl;
                g_logFile << "  Description: Sub-to-Main Command buffer" << std::endl;
                g_logFile << "  Direction: IOP → EE" << std::endl;
                break;
            case 3:
                g_logFile << "  Register: SIF_MSFLAG (index 3)" << std::endl;
                g_logFile << "  Address: 0x1000F220" << std::endl;
                g_logFile << "  Description: Main-to-Sub Flag register" << std::endl;
                g_logFile << "  Direction: EE → IOP (flags set by EE)" << std::endl;
                g_logFile << "  Common bits:" << std::endl;
                g_logFile << "    0x10000 = SIF_STAT_SIFINIT" << std::endl;
                g_logFile << "    0x20000 = SIF_STAT_CMDINIT" << std::endl;
                break;
            case 4:
                g_logFile << "  Register: SIF_SMFLAG (index 4)" << std::endl;
                g_logFile << "  Address: 0x1000F230" << std::endl;
                g_logFile << "  Description: Sub-to-Main Flag register" << std::endl;
                g_logFile << "  Direction: IOP → EE (flags set by IOP)" << std::endl;
                g_logFile << "  Common bits:" << std::endl;
                g_logFile << "    0x10000 = SIF_STAT_SIFINIT (IOP ack)" << std::endl;
                g_logFile << "    0x20000 = SIF_STAT_CMDINIT (IOP ack)" << std::endl;
                g_logFile << "  ** IMPORTANT: Game polls this waiting for IOP response! **" << std::endl;
                break;
            case 5:
                g_logFile << "  Register: SIF_CTRL (index 5)" << std::endl;
                g_logFile << "  Address: 0x1000F240" << std::endl;
                g_logFile << "  Description: SIF Control register" << std::endl;
                break;
            case 6:
                g_logFile << "  Register: SIF_BD6 (index 6)" << std::endl;
                g_logFile << "  Address: 0x1000F260" << std::endl;
                g_logFile << "  Description: Debug/Unknown register" << std::endl;
                break;
            default:
                g_logFile << "  Register: INVALID INDEX " << index << std::endl;
                g_logFile << "  WARNING: Invalid SIF Register index!" << std::endl;
        }
        
        if (paddr != 0) {
            result = memory::read<uint32_t>(paddr);
            g_logFile << "  Physical Address: 0x" << std::hex << paddr << std::dec << std::endl;
        } else {
            g_logFile << "  ERROR: No physical address for index " << index << std::endl;
        }
    }

    g_logFile << "  ----------------------------------------" << std::endl;
    g_logFile << "  Result Value: 0x" << std::hex << result << std::dec << std::endl;
    
    // Decode result bits for flag registers
    if (index == 3 || index == 4) {
        g_logFile << "  Flag decode:" << std::endl;
        if (result & 0x10000) g_logFile << "    [SET] Bit 16: SIF_STAT_SIFINIT" << std::endl;
        if (result & 0x20000) g_logFile << "    [SET] Bit 17: SIF_STAT_CMDINIT" << std::endl;
        if (result & 0x40000) g_logFile << "    [SET] Bit 18: SIF_STAT_BOOTEND" << std::endl;
        if (result == 0) g_logFile << "    (no flags set - game may be waiting!)" << std::endl;
    }
    
    // Log current SIF state for context
    g_logFile << "  ----------------------------------------" << std::endl;
    g_logFile << "  Current g_sif state:" << std::endl;
    g_logFile << "    sys_subaddr:  0x" << std::hex << g_sif.sys_subaddr << std::endl;
    g_logFile << "    sys_mainaddr: 0x" << g_sif.sys_mainaddr << std::endl;
    g_logFile << "    sys_rpcinit:  0x" << g_sif.sys_rpcinit << std::endl;
    g_logFile << "    mscom:        0x" << g_sif.mscom << std::endl;
    g_logFile << "    smcom:        0x" << g_sif.smcom << std::endl;
    g_logFile << "    msflag:       0x" << g_sif.msflag << std::endl;
    g_logFile << "    smflag:       0x" << g_sif.smflag << std::endl;
    g_logFile << "    ctl_reg:      0x" << g_sif.ctl_reg << std::dec << std::endl;
    g_logFile << "========================================" << std::endl;

    ctx.cpuRegs.GPR.r[2].UL[0] = result;
}

void sceSifSetReg(CpuContext& ctx) {
    g_logFile << "Syscall: sceSifSetReg" << std::endl;
    
    // Argument a0 ($4): Register Index
    u32 index = ctx.cpuRegs.GPR.r[4].UL[0];
    
    // Argument a1 ($5): Value to write
    u32 value = ctx.cpuRegs.GPR.r[5].UL[0];

    u32 paddr = 0;

    if (index & 0x80000000) {
        // --- SYSTEM REGISTERS (Software Only) ---
        // These store state but do not touch physical memory
        switch (index) {
            case 0x80000000: 
                g_logFile << "  Register: SIF_SYSREG_SUBADDR (0x80000000)" << std::endl;
                g_sif.sys_subaddr = value; 
                break;
            case 0x80000001: 
                g_logFile << "  Register: SIF_SYSREG_MAINADDR (0x80000001)" << std::endl;
                g_sif.sys_mainaddr = value; 
                break;
            case 0x80000002: 
                g_logFile << "  Register: SIF_SYSREG_RPCINIT (0x80000002)" << std::endl;
                g_sif.sys_rpcinit = value; 
                break;
            default:
                g_logFile << "Warning: sceSifSetReg unknown system index: 0x" << std::hex << index << std::endl;
        }
    } else {
        // --- PHYSICAL REGISTERS (Memory Mapped) ---
        // Using 1-based indexing to match PS2SDK: 1=MSCOM, 2=SMCOM, 3=MSFLAG, 4=SMFLAG
        switch (index) {
            case 1: // SIF_MSCOM (Main -> Sub Buffer)
                g_logFile << "  Register: SIF_MSCOM (index 1)" << std::endl;
                g_logFile << "  Description: Main-to-Sub Command buffer" << std::endl;
                paddr = 0x1000F200;
                g_sif.mscom = value;
                break;
                
            case 2: // SIF_SMCOM (Sub -> Main Buffer)
                g_logFile << "  Register: SIF_SMCOM (index 2)" << std::endl;
                g_logFile << "  Description: Sub-to-Main Command buffer" << std::endl;
                paddr = 0x1000F210;
                g_sif.smcom = value;
                break;
                
            case 3: // SIF_MSFLAG (Main -> Sub Flags)
                g_logFile << "  Register: SIF_MSFLAG (index 3)" << std::endl;
                g_logFile << "  Description: Main-to-Sub Flag register (EE -> IOP)" << std::endl;
                paddr = 0x1000F220;
                g_sif.msflag = value;
                
                // --- HLE HANDSHAKE LOGIC ---
                // When EE writes "I'm initialized" (0x10000/0x20000) to MSFLAG,
                // we must pretend to be the IOP and write "I see you" back to SMFLAG.
                
                if (value & 0x20000) { // SIF_STAT_CMDINIT
                    g_logFile << "  [HLE SIF] EE sent CMDINIT (0x20000). Auto-acknowledging on SMFLAG." << std::endl;
                    g_sif.smflag |= 0x20000;
                    memory::write<uint32_t>(0x1000F230, g_sif.smflag); // Update physical SMFLAG
                }
                if (value & 0x10000) { // SIF_STAT_SIFINIT
                    g_logFile << "  [HLE SIF] EE sent SIFINIT (0x10000). Auto-acknowledging on SMFLAG." << std::endl;
                    g_sif.smflag |= 0x10000;
                    memory::write<uint32_t>(0x1000F230, g_sif.smflag); // Update physical SMFLAG
                }
                break;
                
            case 4: // SIF_SMFLAG (Sub -> Main Flags)
                g_logFile << "  Register: SIF_SMFLAG (index 4)" << std::endl;
                g_logFile << "  Description: Sub-to-Main Flag register (IOP -> EE)" << std::endl;
                paddr = 0x1000F230;
                g_sif.smflag = value;
                break;
                
            case 5: // SIF_CTRL (Control)
                g_logFile << "  Register: SIF_CTRL (index 5)" << std::endl;
                g_logFile << "  Description: SIF Control register" << std::endl;
                paddr = 0x1000F240;
                g_sif.ctl_reg = value;
                break;
                
            case 6: // SIF_BD6 (Debug)
                g_logFile << "  Register: SIF_BD6 (index 6)" << std::endl;
                g_logFile << "  Description: Debug/Unknown register" << std::endl;
                paddr = 0x1000F260; 
                break;
                
            default:
                g_logFile << "Warning: sceSifSetReg passed invalid index: " << std::dec << index << std::endl;
                // Return 0 early to avoid writing to address 0
                ctx.cpuRegs.GPR.r[2].UL[0] = 0;
                return;
        }
    }

    // Perform the physical write if a valid address was set
    if (paddr != 0) {
        g_logFile << "  [Memory Write] 0x" << std::hex << value 
                  << " -> PAddr: 0x" << paddr << std::endl;
        memory::write<uint32_t>(paddr, value);
    }

    // Return the value written (standard behavior)
    ctx.cpuRegs.GPR.r[2].UL[0] = value;
}

void HLE_Deci2Call(CpuContext& ctx) {
    int command = ctx.cpuRegs.GPR.r[4].SL[0];      // $a0
    uint32_t param_ptr = ctx.cpuRegs.GPR.r[5].UL[0]; // $a1

    if (command == 1) { // DECI2_OPEN
        ctx.cpuRegs.GPR.r[2].SL[0] = 1;
    }
    else if (command == 2 || command == 0x10) { // DECI2_SEND / PRINT
        g_logFile << "[DECI2] Ptr: 0x" << std::hex << param_ptr << std::dec << std::endl;
        
        // Dump the first 32 bytes of the message to see if it's a struct or string
        g_logFile << "[DECI2] Hex Dump: ";
        char debugText[256];
        bool isText = true;
        
        for(int i=0; i<32; i++) {
            uint8_t byte = memory::read<uint8_t>(param_ptr + i);
            g_logFile << std::hex << std::setw(2) << std::setfill('0') << (int)byte << " ";
            
            // Try to construct string, filtering non-printables
            if (byte >= 32 && byte <= 126) debugText[i] = byte;
            else if (byte == 0) debugText[i] = 0;
            else { debugText[i] = '.'; isText = false; }
        }
        debugText[32] = 0; // Safety null
        g_logFile << std::endl << "[DECI2] Text View: " << debugText << std::endl;

        ctx.cpuRegs.GPR.r[2].SL[0] = 1; 
    }
    else {
        ctx.cpuRegs.GPR.r[2].SL[0] = -1;
    }
}
void dynamic_decode_and_execute(uint32_t start_address, CpuContext& ctx) {
    ctx.cpuRegs.pc = start_address;
    bool exit_interpreter = false;
    uint32_t instruction_count = 0;
    const uint32_t MAX_INSTRUCTIONS = 100000;
    
    g_logFile << "╔════════════════════════════════════════════════╗" << std::endl;
    g_logFile << "║  DYNAMIC INTERPRETER START                    ║" << std::endl;
    g_logFile << "║  Entry Point: 0x" << std::hex << std::setw(8) 
              << std::setfill('0') << start_address << std::setfill(' ') << "                  ║" << std::endl;
    g_logFile << "╚════════════════════════════════════════════════╝" << std::endl;

    while (!exit_interpreter) {
        // Safety: prevent infinite loops
        if (++instruction_count > MAX_INSTRUCTIONS) {
            g_logFile << "ERROR: Dynamic interpreter exceeded " << std::dec 
                      << MAX_INSTRUCTIONS << " instructions" << std::endl;
            g_logFile << "  Last PC: 0x" << std::hex << ctx.cpuRegs.pc << std::endl;
            exit(1);
        }
        
        uint32_t current_pc = ctx.cpuRegs.pc;

        // Fetch instruction
        uint32_t instr_word = memory::read<uint32_t>(current_pc);
        rabbitizer::InstructionR5900 instr(instr_word, current_pc);
        
        // Log execution
        std::string disasm = instr.disassemble(0, "");
        g_logFile << "  [" << std::dec << std::setw(6) << instruction_count << "] "
                  << "0x" << std::hex << std::setw(8) << std::setfill('0') << current_pc 
                  << ": " << disasm << std::setfill(' ') << std::endl;

        // Execute
        if (instr.isBranch() || instr.isJump()) {
            handle_branch_logic(instr, ctx, exit_interpreter);
        } else {
            execute_single_instruction(instr, ctx);
            ctx.cpuRegs.pc += 4;
        }
    }

    g_logFile << "╔════════════════════════════════════════════════╗" << std::endl;
    g_logFile << "║  DYNAMIC INTERPRETER EXIT                     ║" << std::endl;
    g_logFile << "║  Executed: " << std::dec << std::setw(6) << instruction_count 
              << " instructions               ║" << std::endl;
    g_logFile << "║  Final PC: 0x" << std::hex << std::setw(8) << std::setfill('0') 
              << ctx.cpuRegs.pc << std::setfill(' ') << "                  ║" << std::endl;
    g_logFile << "╚════════════════════════════════════════════════╝" << std::endl;
}

void runtime_syscall_dispatcher(uint32_t syscall_num, CpuContext& ctx) {
    if (syscall_num < 256 && custom_syscall_addresses[syscall_num] != 0) {
        // Instead of running C++ code, we point the CPU PC to the custom code
        uint32_t target_pc = custom_syscall_addresses[syscall_num];
         
        ctx.cpuRegs.pc = target_pc;
        g_logFile << "Executing CUSTOM guest syscall at 0x" << std::hex << ctx.cpuRegs.pc << std::endl;
        auto it = recompiled_functions.find(target_pc);
        if (it != recompiled_functions.end()) {
            it->second(ctx, target_pc);
        } else {
            g_logFile << "  Executing via dynamic interpreter" << std::endl;            // Need to handle code that hasnt been created
            dynamic_decode_and_execute(target_pc, ctx);
        }
        return; 
    }
    int syscall_num_int = static_cast<int>(syscall_num);
    
    switch (syscall_num_int) {
        case 0: NotImplemented_Syscall("RFU000_FullReset", ctx); break;
        case 1: NotImplemented_Syscall("ResetEE", ctx); break;
        case 2: SetGsCrt(ctx); break;
        case 4: sceExit(ctx); break;
        case 6: NotImplemented_Syscall("LoadPS2Exe", ctx); break;
        case 7: NotImplemented_Syscall("ExecPS2", ctx); break;
        case 10: NotImplemented_Syscall("AddSbusIntcHandler", ctx); break;
        case 11: NotImplemented_Syscall("RemoveSbusIntcHandler", ctx); break;
        case 12: NotImplemented_Syscall("Interrupt2Iop", ctx); break;
        case 13: NotImplemented_Syscall("SetVTLBRefillHandler", ctx); break;
        case 14: NotImplemented_Syscall("SetVCommonHandler", ctx); break;
        case 15: NotImplemented_Syscall("SetVInterruptHandler", ctx); break;
        case 16: AddIntcHandler(ctx); break;
        case 17: NotImplemented_Syscall("RemoveIntcHandler", ctx); break;
        case 18: AddDmacHandler(ctx); break;
        case 19: SysRemoveDmacHandler(ctx); break;
        case 20: syscall_EnableIntc(ctx); break;
        case 21: syscall_DisableIntc(ctx); break;
        case 22: _EnableDmac(ctx); break;
        case 23: _DisableDmac(ctx); break;
        case 24: NotImplemented_Syscall("_SetAlarm", ctx); break;
        case 25: NotImplemented_Syscall("_ReleaseAlarm", ctx); break;
        case 26: NotImplemented_Syscall("_iEnableIntc", ctx); break;
        case 27: NotImplemented_Syscall("_iDisableIntc", ctx); break;
        case 28: NotImplemented_Syscall("_iEnableDmac", ctx); break;
        case 29: NotImplemented_Syscall("_iDisableDmac", ctx); break;
        case 30: NotImplemented_Syscall("_iSetAlarm", ctx); break;
        case 31: NotImplemented_Syscall("_iReleaseAlarm", ctx); break;

        // ===== THREADING - NOW USES PS2SCHEDULER =====
        case 32: Syscall_CreateThread(ctx); break;
        case 33: Syscall_DeleteThread(ctx); break;
        case 34: Syscall_StartThread(ctx); break;
        case 35: Syscall_ExitThread(ctx); break;
        case 36: Syscall_ExitDeleteThread(ctx); break;
        case 37: Syscall_TerminateThread(ctx); break;
        case 38: Syscall_iTerminateThread(ctx); break;
        case 39: Syscall_DisableDispatchThread(ctx); break;
        case 40: Syscall_EnableDispatchThread(ctx); break;
        case 41: Syscall_ChangeThreadPriority(ctx); break;
        case 42: Syscall_iChangeThreadPriority(ctx); break;
        case 43: Syscall_RotateThreadReadyQueue(ctx); break;
        case 44: Syscall_iRotateThreadReadyQueue(ctx); break;
        case 45: Syscall_ReleaseWaitThread(ctx); break;
        case 46: Syscall_iReleaseWaitThread(ctx); break;
        case 47: Syscall_GetThreadId(ctx); break;
        case 48: Syscall_ReferThreadStatus(ctx); break;
        case 49: NotImplemented_Syscall("SetVSyncFlag", ctx); break;
        case 50: Syscall_SleepThread(ctx); break;
        case 51: Syscall_WakeupThread(ctx); break;
        case 52: Syscall_iWakeupThread(ctx); break;
        case 53: Syscall_CancelWakeupThread(ctx); break;
        case 54: Syscall_iCancelWakeupThread(ctx); break;
        case 55: Syscall_SuspendThread(ctx); break;
        case 56: Syscall_iSuspendThread(ctx); break;
        case 57: Syscall_ResumeThread(ctx); break;
        case 58: Syscall_iResumeThread(ctx); break;
        case 59: NotImplemented_Syscall("JoinThread", ctx); break;
        
        // ===== Thread Init - NOW USES PS2SCHEDULER =====
        case 60: Syscall_InitMainThread(ctx); break;
        case 61: Syscall_InitHeap(ctx); break;

        case 62: Syscall_EndOfHeap(ctx); break;

        case 64: Syscall_CreateSema(ctx); break;
        case 65: Syscall_DeleteSema(ctx); break;
        case 66: Syscall_SignalSema(ctx); break;
        case 67: Syscall_iSignalSema(ctx); break;
        case 68: Syscall_WaitSema(ctx); break;
        case 69: Syscall_PollSema(ctx); break;
        case 70: Syscall_iPollSema(ctx); break;

        case 71: NotImplemented_Syscall("ReferSemaStatus", ctx); break;
        case 72: NotImplemented_Syscall("iReferSemaStatus", ctx); break;
        case 74: SetOsdConfigParam(ctx); break;
        case 75: Syscall_GetOsdConfigParam(ctx); break;
        case 76: NotImplemented_Syscall("GetGsHParam", ctx); break;
        case 77: NotImplemented_Syscall("GetGsVParam", ctx); break;
        case 78: NotImplemented_Syscall("SetGsHParam", ctx); break;
        case 79: NotImplemented_Syscall("SetGsVParam", ctx); break;
        case 80: NotImplemented_Syscall("CreateEventFlag", ctx); break;
        case 81: NotImplemented_Syscall("DeleteEventFlag", ctx); break;
        case 82: NotImplemented_Syscall("SetEventFlag", ctx); break;
        case 83: iSetEventFlag(ctx); break;
        case 84: NotImplemented_Syscall("ClearEventFlag", ctx); break;
        case 85: NotImplemented_Syscall("iClearEventFlag", ctx); break;
        case 86: NotImplemented_Syscall("WaitEventFlag", ctx); break;
        case 87: NotImplemented_Syscall("PollEventFlag", ctx); break;
        case 88: NotImplemented_Syscall("iPollEventFlag", ctx); break;
        case 89: NotImplemented_Syscall("ReferEventFlagStatus", ctx); break;
        case 90: NotImplemented_Syscall("iReferEventFlagStatus", ctx); break;
        case 92: NotImplemented_Syscall("EnableIntcHandler", ctx); break;
        case 93: NotImplemented_Syscall("DisableIntcHandler", ctx); break;
        case 94: NotImplemented_Syscall("EnableDmacHandler", ctx); break;
        case 95: NotImplemented_Syscall("DisableDmacHandler", ctx); break;
        case 96: NotImplemented_Syscall("KSeg0", ctx); break;
        case 97: NotImplemented_Syscall("EnableCache", ctx); break;
        case 98: NotImplemented_Syscall("DisableCache", ctx); break;
        case 99: NotImplemented_Syscall("GetCop0", ctx); break;
        case 100: FlushCache(ctx); break;
        case 102: NotImplemented_Syscall("CpuConfig", ctx); break;
        case 103: NotImplemented_Syscall("iGetCop0", ctx); break;
        case 104: NotImplemented_Syscall("iFlushCache", ctx); break;
        case 106: NotImplemented_Syscall("iCpuConfig", ctx); break;
        case 107: sceSifStopDma(ctx); break;
        case 108: NotImplemented_Syscall("SetCPUTimerHandler", ctx); break;
        case 109: NotImplemented_Syscall("SetCPUTimer", ctx); break;
        case 110: NotImplemented_Syscall("ForceRead", ctx); break;
        case 111: NotImplemented_Syscall("ForceWrite", ctx); break;
        case 112: GsGetIMR(ctx); break;
        case 113: GsPutIMR(ctx); break;
        case 114: NotImplemented_Syscall("SetPgifHandler", ctx); break;
        case 115: NotImplemented_Syscall("SetVSyncFlag", ctx); break;
        case 116: SetSysCall(ctx); break;
        case 117: NotImplemented_Syscall("print", ctx); break;
        case 118: sceSifDmaStat(ctx); break;
        case 119: sceSifSetDma(ctx); break;
        case 120: sceSifSetDChain(ctx); break;
        case 121: sceSifSetReg(ctx); break;
        case 122: sceSifGetReg(ctx); break;
        case 123: NotImplemented_Syscall("ExecOSD", ctx); break;
        case 124: HLE_Deci2Call(ctx); break;
        case 125: NotImplemented_Syscall("PSMode", ctx); break;
        case 126: NotImplemented_Syscall("MachineType", ctx); break;
        case 127: GetMemorySize(ctx); break;
        
        // Your previously implemented syscalls
        // The list has 0x74 as SetOsdConfigParam, but you implemented sceSifSetDma as 0x74
        // I will keep your implementation for now.
        // case 0x74: sceSifSetDma(ctx); break; 

        default:
            g_logFile << "Unhandled syscall: 0x" << std::hex << syscall_num << std::endl;
            exit(1);
    }
}