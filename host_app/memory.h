#pragma once


#include "cpu_state.h"
#include <vector>
#include <iostream>


// Declare a global variable for the PS2's main memory.
extern std::vector<uint8_t> main_memory;
extern std::vector<uint8_t> vu1_code_memory;
extern std::vector<uint8_t> vu1_data_memory;
extern std::vector<uint8_t> gs_vram;


// Define a 128-bit data type for quadword operations.
struct alignas(16) QuadWord {
   union {
       uint8_t  u8[16];
       uint16_t u16[8];
       uint32_t u32[4];
       uint64_t u64[2];
       int8_t   s8[16];
       int16_t  s16[8];
       int32_t  s32[4];
       int64_t  s64[2];
       float    f32[4];
       uint8_t  m2[16];  // Keep for backward compatibility
   };
};


namespace memory {
   uint8_t* translate_address(uint32_t address, size_t size);
   void initialize();
   // Templated read and write functions
   template <typename T>
   T read(uint32_t address);


   template <typename T>
   void write(uint32_t address, T value);


   // Functions for reading and writing 128-bit quadwords
   void read_quad(uint32_t address, QuadWord& value);
   void write_quad(uint32_t address, const QuadWord& value);


   // Function to get a direct pointer to memory
   void* get_pointer(uint32_t address);
}


// [FUNC-TRACE] Watched function addresses for diagnostic tracing
#include <unordered_set>
extern std::unordered_set<uint32_t> g_watched_functions;


// [IO-TRACE] Lightweight PC tracking for I/O write attribution
extern uint32_t g_current_pc;

