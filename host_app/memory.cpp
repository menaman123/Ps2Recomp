#include "memory.h"
#include "cpu_state.h"
#include <iostream>

// Defines and allocates the main memory for the emulated PS2.
// 32MB = 32 * 1024 * 1024 bytes.
std::vector<uint8_t> main_memory(32 * 1024 * 1024);

u32 ReadMemory32(u32 address) {
    // TODO: Implement the logic to read a 32-bit value.
    // 1. Check if the address is within the bounds of main_memory.
    //    (e.g., if (address > main_memory.size() - 4) { /* handle error */ }) 
    // 2. The PS2 is little-endian. You need to read 4 bytes and combine them.
    //    u32 value = 0;
    //    value |= (u32)main_memory[address];
    //    value |= (u32)main_memory[address + 1] << 8;
    //    value |= (u32)main_memory[address + 2] << 16;
    //    value |= (u32)main_memory[address + 3] << 24;
    //    return value;


    /*
    So this u32 address will be somewhere within this 32MB block size

    We need to check if this is even a valid address and exists within this address

    32 bits = 4 bytes = 1 word

    So at the minimum the address is 4 bytes long so if the address begins 3 bytes before the end then it will exceed the current memory capacity
    */

    if (address > (main_memory.size() - 4) ){
        // log the error
        std::cerr << "FATAL_ERROR: Out-of-bounds memory read." << std::endl;
        std::cerr << "Attempted to read 4 bytes at address: 0x" << std::hex << address << std::endl;
        std::cerr << "Valid memory range is 0x0 to 0x" << std::hex << (main_memory.size() - 1) << std::endl;
        exit(1);
    }

    u32 val = 0;
    val |= (u32)main_memory[address]; // Grabs the first 8 bits or 1 byte from the address and stores it into value
    val |= (u32)main_memory[address + 1] << 8; // Going up one in address is grabbing the next 8 bits and storing it into value. In order to store it into value and not overwrite the previous 8 bits, we need to shift this to the left by 8.
    val |= (u32)main_memory[address + 2] << 16;
    val |= (u32)main_memory[address + 3] << 24;


    return val;
}

u16 ReadMemory16(u32 address) {
    // TODO: Implement the logic to read a 32-bit value.
    // 1. Check if the address is within the bounds of main_memory.
    //    (e.g., if (address > main_memory.size() - 4) { /* handle error */ }) 
    // 2. The PS2 is little-endian. You need to read 4 bytes and combine them.
    //    u32 value = 0;
    //    value |= (u32)main_memory[address];
    //    value |= (u32)main_memory[address + 1] << 8;
    //    value |= (u32)main_memory[address + 2] << 16;
    //    value |= (u32)main_memory[address + 3] << 24;
    //    return value;


    /*
    So this u32 address will be somewhere within this 32MB block size

    We need to check if this is even a valid address and exists within this address

    32 bits = 4 bytes = 1 word

    So at the minimum the address is 4 bytes long so if the address begins 3 bytes before the end then it will exceed the current memory capacity
    */

    if (address > (main_memory.size() - 4) ){
        // log the error
        std::cerr << "FATAL_ERROR: Out-of-bounds memory read." << std::endl;
        std::cerr << "Attempted to read 2 bytes at address: 0x" << std::hex << address << std::endl;
        std::cerr << "Valid memory range is 0x0 to 0x" << std::hex << (main_memory.size() - 1) << std::endl;
        exit(1);
    }

    u16 val = 0;
    val |= (u16)main_memory[address]; // Grabs the first 8 bits or 1 byte from the address and stores it into value
    val |= (u16)main_memory[address + 1] << 8; // Going up one in address is grabbing the next 8 bits and storing it into value. In order to store it into value and not overwrite the previous 8 bits, we need to shift this to the left by 8.

    return val;
}

u8 ReadMemory8(u32 address) {
    // TODO: Implement the logic to read a 32-bit value.
    // 1. Check if the address is within the bounds of main_memory.
    //    (e.g., if (address > main_memory.size() - 4) { /* handle error */ }) 
    // 2. The PS2 is little-endian. You need to read 4 bytes and combine them.
    //    u32 value = 0;
    //    value |= (u32)main_memory[address];
    //    value |= (u32)main_memory[address + 1] << 8;
    //    value |= (u32)main_memory[address + 2] << 16;
    //    value |= (u32)main_memory[address + 3] << 24;
    //    return value;


    /*
    So this u32 address will be somewhere within this 32MB block size

    We need to check if this is even a valid address and exists within this address

    32 bits = 4 bytes = 1 word

    So at the minimum the address is 4 bytes long so if the address begins 3 bytes before the end then it will exceed the current memory capacity
    */

    if (address > (main_memory.size() - 4) ){
        // log the error
        std::cerr << "FATAL_ERROR: Out-of-bounds memory read." << std::endl;
        std::cerr << "Attempted to read 1 bytes at address: 0x" << std::hex << address << std::endl;
        std::cerr << "Valid memory range is 0x0 to 0x" << std::hex << (main_memory.size() - 1) << std::endl;
        exit(1);
    }

    u8 val = 0;
    val |= (u8)main_memory[address]; // Grabs the first 8 bits or 1 byte from the address and stores it into value


    return val;
}

void WriteMemory32(u32 address, u32 value) {
    // TODO: Implement the logic to write a 32-bit value.
    // 1. Check if the address is within the bounds of main_memory.
    // 2. The PS2 is little-endian. You need to break the 32-bit value into 4 bytes.
    //    main_memory[address]     = (value & 0x000000FF);
    //    main_memory[address + 1] = (value & 0x0000FF00) >> 8;
    //    main_memory[address + 2] = (value & 0x00FF0000) >> 16;
    //    main_memory[address + 3] = (value & 0xFF000000) >> 24;

    if (address > (main_memory.size() - 4) ){
        // log the error
        std::cerr << "FATAL_ERROR: Out-of-bounds memory write." << std::endl;
        std::cerr << "Attempted to write 4 bytes at address: 0x" << std::hex << address << std::endl;
        std::cerr << "Valid memory range is 0x0 to 0x" << std::hex << (main_memory.size() - 1) << std::endl;
        exit(1);
    }

    // Putting this value into address
    // We start with the address and go up byte by byte till we hit 4 bytes
    // The value is already 4 bytes tho, so how do we store each byte at each index?
    // zero all the bytes that we do not need at that index


    main_memory[address] = (value & (0x000000FF)); // First byte in address contains the first byte in Value
    main_memory[address + 1] = (value & (0x0000FF00)) >> 8; // Since each index in mainmemory is 1 byte we need to shift the current value byte to the right most part
    main_memory[address + 2] = (value & (0x00FF0000)) >> 16;
    main_memory[address + 3] = (value & (0xFF000000)) >> 24; // Last byte contains the last byte in value

}

void WriteMemory16(u32 address, u16 value) {
    // TODO: Implement the logic to write a 32-bit value.
    // 1. Check if the address is within the bounds of main_memory.
    // 2. The PS2 is little-endian. You need to break the 32-bit value into 4 bytes.
    //    main_memory[address]     = (value & 0x000000FF);
    //    main_memory[address + 1] = (value & 0x0000FF00) >> 8;
    //    main_memory[address + 2] = (value & 0x00FF0000) >> 16;
    //    main_memory[address + 3] = (value & 0xFF000000) >> 24;

    if (address > (main_memory.size() - 2) ){
        // log the error
        std::cerr << "FATAL_ERROR: Out-of-bounds memory write." << std::endl;
        std::cerr << "Attempted to write 4 bytes at address: 0x" << std::hex << address << std::endl;
        std::cerr << "Valid memory range is 0x0 to 0x" << std::hex << (main_memory.size() - 1) << std::endl;
        exit(1);
    }

    // Putting this value into address
    // We start with the address and go up byte by byte till we hit 4 bytes
    // The value is already 4 bytes tho, so how do we store each byte at each index?
    // zero all the bytes that we do not need at that index


    main_memory[address] = (value & (0x000000FF)); // First byte in address contains the first byte in Value
    main_memory[address + 1] = (value & (0x0000FF00)) >> 8; // Since each index in mainmemory is 1 byte we need to shift the current value byte to the right most part

}

void WriteMemory8(u32 address, u8 value) {
    // TODO: Implement the logic to write a 32-bit value.
    // 1. Check if the address is within the bounds of main_memory.
    // 2. The PS2 is little-endian. You need to break the 32-bit value into 4 bytes.
    //    main_memory[address]     = (value & 0x000000FF);
    //    main_memory[address + 1] = (value & 0x0000FF00) >> 8;
    //    main_memory[address + 2] = (value & 0x00FF0000) >> 16;
    //    main_memory[address + 3] = (value & 0xFF000000) >> 24;

    if (address > (main_memory.size() - 1) ){
        // log the error
        std::cerr << "FATAL_ERROR: Out-of-bounds memory write." << std::endl;
        std::cerr << "Attempted to write 4 bytes at address: 0x" << std::hex << address << std::endl;
        std::cerr << "Valid memory range is 0x0 to 0x" << std::hex << (main_memory.size() - 1) << std::endl;
        exit(1);
    }

    // Putting this value into address
    // We start with the address and go up byte by byte till we hit 4 bytes
    // The value is already 4 bytes tho, so how do we store each byte at each index?
    // zero all the bytes that we do not need at that index


    main_memory[address] = (value & (0x000000FF)); // First byte in address contains the first byte in Value

}