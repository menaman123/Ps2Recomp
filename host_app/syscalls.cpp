#include "syscalls.h"
#include "memory.h" // For memory access functions
#include <iostream>
#include <unistd.h> // For write syscall

// A helper function to read a string from guest memory
// You'll need to add the declaration to memory.h and the definition to memory.cpp
std::string read_string_from_guest(uint32_t address) {
    return std::string(reinterpret_cast<char*>(memory::get_pointer(address)));
}

// Exit syscall
void sceExit(CpuState& ctx) {
    int exit_code = static_cast<int>(ctx.gpr[4]); // $a0
    std::cout << "Syscall: exit(" << exit_code << ")" << std::endl;
    exit(exit_code);
}

// Write syscall
void sceWrite(CpuState& ctx) {
    int fd = static_cast<int>(ctx.gpr[4]); // $a0
    uint32_t ptr = ctx.gpr[5]; // $a1
    int len = static_cast<int>(ctx.gpr[6]); // $a2

    std::string str = read_string_from_guest(ptr);
    std::cout << "Syscall: write(" << fd << ", \"" << str << "\", " << len << ")" << std::endl;

    if (fd == 1 || fd == 2) { // stdout or stderr
        write(fd, str.c_str(), len);
    }
    
    // Return the number of bytes written
    ctx.gpr[2] = len; // $v0
}


// Placeholder for open syscall
void sceOpen(CpuState& ctx) {
    // Arguments: const char* filename, int flags
    uint32_t filename_ptr = ctx.gpr[4]; // $a0
    int flags = static_cast<int>(ctx.gpr[5]); // $a1
    
    std::string filename = read_string_from_guest(filename_ptr);
    std::cout << "System Call: sceOpen(filename: \"" << filename << "\", flags: " << flags << ") called!" << std::endl;
    
    // The return value (file descriptor) is placed in $v0
    ctx.gpr[2] = 0; // Return a dummy file descriptor for now
}

void runtime_syscall_dispatcher(uint32_t syscall_num, CpuState& ctx) {
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