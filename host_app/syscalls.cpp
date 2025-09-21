#include "syscalls.h"
#include "memory.h" // For memory access functions
#include <iostream>

#ifdef _WIN32
#include <io.h>
#include <process.h>
#define write _write
#else
#include <unistd.h> // For write syscall
#endif

// A helper function to read a string from guest memory
std::string read_string_from_guest(uint32_t address) {
    return std::string(reinterpret_cast<char*>(memory::get_pointer(address)));
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
        // Add cases for other syscalls here
        default:
            std::cerr << "Unhandled syscall: 0x" << std::hex << syscall_num << std::endl;
            // It's a good idea to have a default case to catch unimplemented syscalls.
            // You can choose to either terminate emulation or simply log the unhandled syscall and continue.
            // For now, we will just log it.
            break;
    }
}