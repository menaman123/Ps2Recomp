#include "syscalls.h"
#include "memory.h"
#include <iostream>
#include <fstream>
#include <cstring>
#include "sema.h"
#include "recompiled.h"
#include "rabbitizer.hpp"
#include <iomanip>
#include "intc.h"
#include "instructions/InstructionR5900.hpp"

#ifdef _WIN32
#include <io.h>
#include <process.h>
#else
#include <unistd.h>
#endif

extern std::ofstream g_logFile;
uint32_t custom_syscall_addresses[256] = {0};
std::map<int, EventFlag> g_eventFlags;
int g_nextEventFlagId = 1; // Start from ID 1


const uint32_t KERNEL_SYSCALL_TABLE_BASE = 0x800002E0;

// A helper function to read a string from guest memory
std::string read_string_from_guest(uint32_t address) {
    return std::string(reinterpret_cast<char*>(memory::get_pointer(address)));
}

void NotImplemented_Syscall(const char* name, CpuContext& ctx) {
    g_logFile << "Syscall: " << name << " (Not Implemented)" << std::endl;
    exit(1);
}



void SetOsdConfigParam(CpuContext& ctx) {
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

void InitMainThread(CpuContext& ctx) {
    g_logFile << "Syscall: InitMainThread() called!" << std::endl;
        
    /*
    Initializes the current thread. Returns the stack pointer of the thread.
    If stack == -1, the stack pointer equals the end of RDRAM - stack_size. Else, it equals stack + stack_size.
    This function should only be called before the program's main function.
    */
    // input: uint32 gp, void* stack, int stack_size, char* args, int root
    uint32_t gp = ctx.cpuRegs.GPR.r[4].UL[0];
    uint32_t stack = (ctx.cpuRegs.GPR.r[5].UL[0]);
    uint32_t stack_size = (ctx.cpuRegs.GPR.r[6].SL[0]);
    uint32_t args = (ctx.cpuRegs.GPR.r[7].UL[0]);
    int32_t root = ctx.cpuRegs.GPR.r[8].SL[0];
    uint32_t sp = 0x0;
    if (stack == 0xFFFFFFFF){
        sp = 0x02000000 - stack_size;
    }else{
        sp = stack + stack_size;
    }
    sp &= ~0xF; 

    // Return the Guest Address to the guest register
    ctx.cpuRegs.GPR.r[2].UL[0] = sp;
    
}

void InitHeap(CpuContext& ctx) {
    g_logFile << "Syscall: InitHeap() called!" << std::endl;
    /*
    Initializes the current thread's heap. If heap == -1, the end of the heap resides at the thread's stack pointer. Else, the end of the heap is heap + heap_size.
    Returns the end of the thread's heap.
    */
    // input: void* heap, int heap_size
    uint32_t heap = (ctx.cpuRegs.GPR.r[4].UL[0]);
    uint32_t heap_size = (ctx.cpuRegs.GPR.r[5].SL[0]);

    if (heap == 0xFFFFFFFF){
        heap = ctx.cpuRegs.GPR.r[29].UL[0];
    }
    else{
        heap += heap_size;
    }
    ctx.cpuRegs.GPR.r[2].UL[0] = heap;
    
}

void CreateSema(CpuContext& ctx){
    g_logFile << "Syscall: CreateSema() called!" << std::endl;
    /*
        Creates a semaphore. Returns the semaphore's id if successful and -1 if not.
        Only s->init_count and s->max_count need to be specified. s->attr and s->option may also be specified.
    */
    uint32_t sema_ptr = ctx.cpuRegs.GPR.r[4].UL[0];

    // Translate the guest address to a host pointer
    ee_sema_t* guest_struct = reinterpret_cast<ee_sema_t*>(
        memory::translate_address(sema_ptr, sizeof(ee_sema_t))
    );

    if (!guest_struct) {
        ctx.cpuRegs.GPR.r[2].SL[0] = -1; // Return error
        return;
    }

    // Find an empty slot (skip index 0, as it's usually reserved)
    int id = -1;
    for (size_t i = 1; i < g_semaphores.size(); ++i) {
        if (!g_semaphores[i].active) {
            id = i;
            break;
        }
    }

    if (id != -1) {
        g_semaphores[id].active = true;
        g_semaphores[id].count = guest_struct->init_count;
        g_semaphores[id].max_count = guest_struct->max_count;
        
        // Return the semaphore ID in $v0
        ctx.cpuRegs.GPR.r[2].SL[0] = id;
    } else {
        ctx.cpuRegs.GPR.r[2].SL[0] = -1;
    }
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

void FlushCache(CpuContext& ctx) {

    /*

        input: int mode



            Modes of operation

            mode=0: Flush data cache (invalidate+writeback dirty contents to memory)

            mode=1: Invalidate data cache

            mode=2: Invalidate instruction cache

            All other modes invalidate both caches.

        

        Make sure to also do the action in the emulated memory as well



    */

    // Mode is passed in $a0 (register 4)
    uint32_t mode = ctx.cpuRegs.GPR.r[4].UL[0];

    // Log the event to your global log file
    g_logFile << "Syscall: FlushCache(mode: " << std::dec << mode << ") at PC: 0x" 
              << std::hex << ctx.cpuRegs.pc << std::endl;

    if (mode == 2 || mode > 2) {
        /* This log indicates the game is preparing to execute newly written 
           code in RAM, such as the 1.8 KB payload you identified at 0x80076000.
        */
        g_logFile << "  [!] Instruction Cache Invalidation: Possible Dynamic Code Execution." << std::endl;
    }

    // Standard PS2 return behavior: return 0 (Success) in $v0
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
            ctx.cop0.n.Status |= 0x1;  // Enable interrupts
            break;
            
        case RABBITIZER_INSTR_ID_r5900_di:
            ctx.cop0.n.Status &= ~0x1;  // Disable interrupts
            break;
            
        case RABBITIZER_INSTR_ID_cpu_nop:
            // No operation
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
    if (int_cause < 0 || int_cause > 15) {
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
        case 2: NotImplemented_Syscall("SetGsCrt", ctx); break;
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
        case 18: NotImplemented_Syscall("AddDmacHandler", ctx); break;
        case 19: NotImplemented_Syscall("RemoveDmacHandler", ctx); break;
        case 20: NotImplemented_Syscall("_EnableIntc", ctx); break;
        case 21: NotImplemented_Syscall("_DisableIntc", ctx); break;
        case 22: NotImplemented_Syscall("_EnableDmac", ctx); break;
        case 23: NotImplemented_Syscall("_DisableDmac", ctx); break;
        case 24: NotImplemented_Syscall("_SetAlarm", ctx); break;
        case 25: NotImplemented_Syscall("_ReleaseAlarm", ctx); break;
        case 26: NotImplemented_Syscall("_iEnableIntc", ctx); break;
        case 27: NotImplemented_Syscall("_iDisableIntc", ctx); break;
        case 28: NotImplemented_Syscall("_iEnableDmac", ctx); break;
        case 29: NotImplemented_Syscall("_iDisableDmac", ctx); break;
        case 30: NotImplemented_Syscall("_iSetAlarm", ctx); break;
        case 31: NotImplemented_Syscall("_iReleaseAlarm", ctx); break;
        case 32: NotImplemented_Syscall("CreateThread", ctx); break;
        case 33: NotImplemented_Syscall("DeleteThread", ctx); break;
        case 34: NotImplemented_Syscall("StartThread", ctx); break;
        case 35: NotImplemented_Syscall("ExitThread", ctx); break;
        case 36: NotImplemented_Syscall("ExitDeleteThread", ctx); break;
        case 37: NotImplemented_Syscall("TerminateThread", ctx); break;
        case 38: NotImplemented_Syscall("iTerminateThread", ctx); break;
        case 39: NotImplemented_Syscall("DisableDispatchThread", ctx); break;
        case 40: enableDispatchThread(ctx); break;
        case 41: NotImplemented_Syscall("ChangeThreadPriority", ctx); break;
        case 42: NotImplemented_Syscall("iChangeThreadPriority", ctx); break;
        case 43: NotImplemented_Syscall("RotateThreadReadyQueue", ctx); break;
        case 44: NotImplemented_Syscall("iRotateThreadReadyQueue", ctx); break;
        case 45: NotImplemented_Syscall("ReleaseWaitThread", ctx); break;
        case 46: NotImplemented_Syscall("iReleaseWaitThread", ctx); break;
        case 47: NotImplemented_Syscall("GetThreadId", ctx); break;
        case 48: NotImplemented_Syscall("ReferThreadStatus", ctx); break;
        case 49: NotImplemented_Syscall("iReferThreadStatus", ctx); break;
        case 50: NotImplemented_Syscall("SleepThread", ctx); break;
        case 51: NotImplemented_Syscall("WakeupThread", ctx); break;
        case 52: NotImplemented_Syscall("iWakeupThread", ctx); break;
        case 53: NotImplemented_Syscall("CancelWakeupThread", ctx); break;
        case 54: NotImplemented_Syscall("iCancelWakeupThread", ctx); break;
        case 55: NotImplemented_Syscall("SuspendThread", ctx); break;
        case 56: NotImplemented_Syscall("iSuspendThread", ctx); break;
        case 57: NotImplemented_Syscall("ResumeThread", ctx); break;
        case 58: NotImplemented_Syscall("iResumeThread", ctx); break;
        case 59: NotImplemented_Syscall("JoinThread", ctx); break;
        case 62: NotImplemented_Syscall("EndOfHeap", ctx); break;
        case 64: CreateSema(ctx); break;
        case 65: NotImplemented_Syscall("DeleteSema", ctx); break;
        case 66: NotImplemented_Syscall("SignalSema", ctx); break;
        case 67: NotImplemented_Syscall("iSignalSema", ctx); break;
        case 68: NotImplemented_Syscall("WaitSema", ctx); break;
        case 69: NotImplemented_Syscall("PollSema", ctx); break;
        case 70: NotImplemented_Syscall("iPollSema", ctx); break;
        case 71: NotImplemented_Syscall("ReferSemaStatus", ctx); break;
        case 72: NotImplemented_Syscall("iReferSemaStatus", ctx); break;
        case 74: SetOsdConfigParam(ctx); break;
        case 75: NotImplemented_Syscall("GetOsdConfigParam", ctx); break;
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
        case 107: NotImplemented_Syscall("sceSifStopDma", ctx); break;
        case 108: NotImplemented_Syscall("SetCPUTimerHandler", ctx); break;
        case 109: NotImplemented_Syscall("SetCPUTimer", ctx); break;
        case 110: NotImplemented_Syscall("ForceRead", ctx); break;
        case 111: NotImplemented_Syscall("ForceWrite", ctx); break;
        case 112: NotImplemented_Syscall("GsGetIMR", ctx); break;
        case 113: NotImplemented_Syscall("GsPutIMR", ctx); break;
        case 114: NotImplemented_Syscall("SetPgifHandler", ctx); break;
        case 115: NotImplemented_Syscall("SetVSyncFlag", ctx); break;
        case 116: SetSysCall(ctx); break;
        case 117: NotImplemented_Syscall("print", ctx); break;
        case 118: NotImplemented_Syscall("sceSifDmaStat", ctx); break;
        case 119: sceSifSetDma(ctx); break;
        case 120: NotImplemented_Syscall("sceSifSetDChain", ctx); break;
        case 121: NotImplemented_Syscall("sceSifSetReg", ctx); break;
        case 122: NotImplemented_Syscall("sceSifGetReg", ctx); break;
        case 123: NotImplemented_Syscall("ExecOSD", ctx); break;
        case 124: NotImplemented_Syscall("Deci2Call", ctx); break;
        case 125: NotImplemented_Syscall("PSMode", ctx); break;
        case 126: NotImplemented_Syscall("MachineType", ctx); break;
        case 127: NotImplemented_Syscall("GetMemorySize", ctx); break;
        
        // Your previously implemented syscalls
        case 60: InitMainThread(ctx); break;
        case 61: InitHeap(ctx); break;
        // The list has 0x74 as SetOsdConfigParam, but you implemented sceSifSetDma as 0x74
        // I will keep your implementation for now.
        // case 0x74: sceSifSetDma(ctx); break; 

        default:
            g_logFile << "Unhandled syscall: 0x" << std::hex << syscall_num << std::endl;
            exit(1);
    }
}