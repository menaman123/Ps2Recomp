#include "syscalls.h"
#include "memory.h" // For memory access functions
#include <iostream>
#include <fstream>

#ifdef _WIN32
#include <io.h>
#include <process.h>
#define write _write
#else
#include <unistd.h> // For write syscall
#endif

extern std::ofstream g_logFile;

// A helper function to read a string from guest memory
std::string read_string_from_guest(uint32_t address) {
    return std::string(reinterpret_cast<char*>(memory::get_pointer(address)));
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

void runtime_syscall_dispatcher(uint32_t syscall_num, CpuContext& ctx) {
    switch (syscall_num) {
        case 1: // Exit
            sceExit(ctx);
            break;
        case 4: // Write
            sceWrite(ctx);
            break;
        case 5: // Open
            sceOpen(ctx);
            break;
        case 0x3c: // sifRpcBind
            sifRpcBind(ctx);
            break;
        case 0x3d: // sifRpcCall
            sifRpcCall(ctx);
            break;
        case 0x40:
            sifSetRpcQueue(ctx);
            break;
        // Add cases for other syscalls here
        default:
            std::cerr << "Unhandled syscall: 0x" << std::hex << syscall_num << std::endl;
            std::cout << "Unhandled syscall: 0x" << std::hex << syscall_num << std::endl;
            g_logFile << "Unhandled syscall: 0x" << std::hex << syscall_num << std::endl;
            // It's a good idea to have a default case to catch unimplemented syscalls.
            // You can choose to either terminate emulation or simply log the unhandled syscall and continue.
            // For now, we will just log it.
            exit(1); // Terminate emulation
            break;
    }
}