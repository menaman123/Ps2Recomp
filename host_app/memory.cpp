#include "memory.h"
#include "cpu_state.h"
#include "dmac.h"
#include <iostream>
#include "gif.h"
#include <vector>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <iomanip>
#include "intc.h"
#include "ps2_scheduler.h"
#include "sif.h"
#include "hle_heap.h"
#include "gs.h"
#include "vif.h"








// PS2 Memory Map
// Main RAM: 32MB at 0x00000000
// Scratchpad RAM: 16KB at 0x70000000
// IOP RAM: 2MB at 0x1C000000 (simplified)




// memory.h - Add these declarations
std::vector<uint8_t> main_memory(32 * 1024 * 1024);      // 32MB Main RAM
std::vector<uint8_t> scratchpad_memory(16 * 1024);       // 16KB Scratchpad
std::vector<uint8_t> iop_memory(2 * 1024 * 1024);        // 2MB IOP RAM
std::vector<uint8_t> bios_memory(4 * 1024 * 1024);       // 4MB BIOS
std::vector<uint8_t> vu0_code_memory(4 * 1024);          // 4KB VU0 code
std::vector<uint8_t> vu0_data_memory(4 * 1024);          // 4KB VU0 data
std::vector<uint8_t> vu1_code_memory(16 * 1024);         // 16KB VU1 code
std::vector<uint8_t> vu1_data_memory(16 * 1024);         // 16KB VU1 data
std::vector<uint8_t> io_memory(64 * 1024);               // EE I/O (10000000h)
std::vector<uint8_t> gs_priv_memory(8 * 1024);           // GS Privileged (12000000h)
std::vector<uint8_t> iop_io_memory(64 * 1024);           // IOP I/O (1F800000h)
std::vector<uint8_t> spu2_memory(2 * 1024);              // SPU2 Registers (1F900000h)
std::vector<uint8_t> sif_registers(512);                 // SIF Registers (1D000000h)
std::vector<uint8_t> spu2_regs_memory(2 * 1024);  // 2KB for SPU2 Registers at 1F900000h
std::vector<uint8_t> iop_sif_memory(512);         // IOP-side SIF registers at 1D000000h
std::vector<uint8_t> gs_vram(4 * 1024 * 1024);              // 4MB GS VRAM




extern std::ofstream g_logFile;


// [FUNC-TRACE] Watched function addresses for diagnostic tracing
std::unordered_set<uint32_t> g_watched_functions;


// [IO-TRACE] Lightweight PC tracking for I/O write attribution
uint32_t g_current_pc = 0;




namespace memory {
  void initialize(){
      std::fill(main_memory.begin(), main_memory.end(), 0);
    
      std::string map_path = "C:/Users/Owner/Desktop/PS2_Recomp/ghidra_functions/ram_data_map_v50.txt";
      std::ifstream file(map_path);




      if (!file.is_open()) {
          std::cerr << "Fatal Error: Could not open " << map_path << std::endl;
          return;
      }




      std::string line;
      size_t entries_loaded = 0;




      while (std::getline(file, line)) {
          // Skip empty lines or purely comment lines
          if (line.empty() || line[0] == '#') continue;




          // Split line at the colon
          size_t colon_pos = line.find(':');
          if (colon_pos == std::string::npos) continue;




          // Extract address and the value string (before any potential comment)
          std::string addr_str = line.substr(0, colon_pos);
          std::string val_part = line.substr(colon_pos + 1);
        
          // Trim potential comments (starts with #)
          size_t hash_pos = val_part.find('#');
          std::string val_str = (hash_pos != std::string::npos) ? val_part.substr(0, hash_pos) : val_part;




          // Trim whitespace
          val_str.erase(val_str.find_last_not_of(" \n\r\t") + 1);




          try {
              // Convert hex address string to uint32
              uint32_t address = std::stoul(addr_str, nullptr, 16);
            
              // Remove "0x" from value string if present
              if (val_str.find("0x") == 0) val_str = val_str.substr(2);




              // Calculate number of bytes (2 hex chars = 1 byte)
              // Note: val_str is currently Big Endian representation of the LE bytes
              // Example: 0x002F3678 -> "002F3678"
              size_t num_bytes = val_str.length() / 2;




              for (size_t i = 0; i < num_bytes; ++i) {
                  // Extract byte-sized hex chunk (reading from the right for Little Endian)
                  // We extract 2 chars starting from the end of the string
                  std::string byte_hex = val_str.substr((num_bytes - 1 - i) * 2, 2);
                  uint8_t byte_val = static_cast<uint8_t>(std::stoul(byte_hex, nullptr, 16));




                  // Safely write to memory
                  if (address + i < main_memory.size()) {
                      main_memory[address + i] = byte_val;
                  }
              }
              entries_loaded++;
          } catch (const std::exception& e) {
              std::cerr << "Warning: Skipping malformed line: " << line << " (" << e.what() << ")" << std::endl;
          }
          uint32_t heap_start = 0x00480000;  // After game code/data
          uint32_t heap_size  = 0x00B7EC10;  // ~11.5 MB (same as game expects)




      }




      std::cout << "Memory initialized: Loaded " << entries_loaded << " entries from text map." << std::endl;
      file.close();
      write<uint32_t>(0x002e6f40, 0x10008000); // Index 0 -> VIF0 Base
      write<uint32_t>(0x002e6f44, 0x10009000); // Index 1 -> VIF1 Base
      write<uint32_t>(0x2E804C, 0x00000000); // Initialize this so it can move past in 2d18f8 which checks for
    
      std::cout << "Memory initialized: VIF Tables patched." << std::endl;




      // Sanity Check
      uint32_t check_addr = 0x002e6bc0; // Pointer address from your snippet
      uint32_t val = read<uint32_t>(check_addr);
      std::cout << "Sanity Check: Pointer at 0x" << std::hex << check_addr
                << " is 0x" << std::setw(8) << std::setfill('0') << val << std::dec << std::endl;
  }




  // Translate PS2 virtual address to host memory
// Translate PS2 virtual address to host memory
  uint8_t* translate_address(uint32_t address, size_t size) {
  // ----------------------------------------------------------------
  // 1. Handle Scratchpad (Internal EE Memory) - 0x70000000
  // ----------------------------------------------------------------
  // This is distinct from Main RAM and must be checked first.
  if ((address & 0x70000000) == 0x70000000) {
       // Mask off the top to get offset 0x0000 - 0x3FFF
      uint32_t offset = address & 0x00003FFF;
    
      if (offset + size > scratchpad_memory.size()) {
          std::cerr << "ERROR: Scratchpad access out of bounds at 0x" << std::hex << address << std::endl;
          return nullptr;
      }
      return &scratchpad_memory[offset];
  }




  // ----------------------------------------------------------------
  // 2. Handle Hardware Registers (0x10000000 - 0x1FFFFFFF)
  // ----------------------------------------------------------------
  // IO, VU, IOP, BIOS. These are not mirrors of Main RAM.
  if (address >= 0x10000000 && address < 0x20000000) {
      // VU0 Code
      if (address >= 0x11000000 && address < 0x11001000) {
          return &vu0_code_memory[address - 0x11000000];
      }
      // VU0 Data
      if (address >= 0x11004000 && address < 0x11005000) {
          return &vu0_data_memory[address - 0x11004000];
      }
      // VU1 Code
      if (address >= 0x11008000 && address < 0x1100C000) {
          return &vu1_code_memory[address - 0x11008000];
      }
      // VU1 Data
      if (address >= 0x1100C000 && address < 0x11010000) {
          return &vu1_data_memory[address - 0x1100C000];
      }
      // IOP RAM (0x1C000000)
      if (address >= 0x1C000000 && address < 0x1C200000) {
          return &iop_memory[address - 0x1C000000];
      }
    
      // I/O Registers are usually handled by read_io_register/write_io_register,
      // but if get_pointer needs them (e.g. for buffers), handle here:
      if (address >= 0x10000000 && address < 0x10010000) {
          return &io_memory[address - 0x10000000];
      }




      // BIOS (0x1FC00000)
      if (address >= 0x1FC00000 && address < 0x20000000) {
           return &bios_memory[address - 0x1FC00000];
      }




      // If it falls here, it's an unmapped IO region
      return nullptr;
  }
   // ----------------------------------------------------------------
  // 3. Handle Main RAM and Mirrors (KUSEG, KSEG0, KSEG1)
  // ----------------------------------------------------------------
  // KUSEG: 0x00000000 - 0x01FFFFFF (User)
  // KSEG0: 0x80000000 - 0x81FFFFFF (Kernel Cached)
  // KSEG1: 0xA0000000 - 0xA1FFFFFF (Kernel Uncached)
  //
  // All of these map to physical RAM at (address & 0x1FFFFFFF).
   // Mask off the top 3 bits (0xE0000000) to strip 0x80/0xA0 prefixes
  uint32_t physical_addr = address & 0x1FFFFFFF;




  // Check bounds against your 32MB Main RAM
  if (physical_addr < main_memory.size()) {
      if (physical_addr + size > main_memory.size()) {
          std::cerr << "ERROR: Main RAM access out of bounds at 0x" << std::hex << address << std::endl;
          return nullptr;
      }
      return &main_memory[physical_addr];
  }
   // ----------------------------------------------------------------
  // 4. Handle Extended BIOS Mirrors
  // ----------------------------------------------------------------
  // KSEG0/KSEG1 BIOS mirrors (0x9FC00000, 0xBFC00000)
  // These map to physical 0x1FC00000
  if ((address & 0x1FFFFFFF) >= 0x1FC00000 && (address & 0x1FFFFFFF) < 0x20000000) {
      uint32_t offset = (address & 0x1FFFFFFF) - 0x1FC00000;
      if (offset + size > bios_memory.size()) return nullptr;
      return &bios_memory[offset];
  }




  // Default failure
  // std::cerr << "WARNING: Unmapped memory access at 0x" << std::hex << address << std::endl;
  return nullptr;
}
 bool is_io_register(uint32_t address) {
      // EE I/O registers: 0x10000000 - 0x1000FFFF
      // GS privileged: 0x12000000 - 0x12001FFF
      return (address >= 0x10000000 && address <= 0x1000FFFF) ||
             (address >= 0x12000000 && address <= 0x12001FFF);
  }




  // Centralized I/O register read
  uint32_t read_io_register(uint32_t address) {
      static int debug_read_count = 0;
      debug_read_count++;




      // VIF0_FBRST
      if (address == 0x10003810){
          g_logFile << "VIF0_FBRST Read Address: " << std::hex << address << std::endl;
          int vif_id = 0;
          uint32_t value = g_vif.Read(vif_id, address);
          g_logFile << "VIF0_FBRST Read Value: " << std::hex << value << std::endl;
          return value;
      }




      // VIF0_ERR
      if (address == 0x10003820){
          g_logFile << "VIF0_ERR Read Address: " << std::hex << address << std::endl;
          int vif_id = 0;
          uint32_t value = g_vif.Read(vif_id, address);
          g_logFile << "VIF0_ERR Read Value: " << std::hex << value << std::endl;
          return value;
      }




      // VIF1_FBRST
      if (address == 0x10003c10){
          g_logFile << "VIF1_FBRST Read Address: " << std::hex << address << std::endl;
          int vif_id = 1;
          uint32_t value = g_vif.Read(vif_id, address); 
          g_logFile << "VIF1_FBRST Read Value: " << std::hex << value << std::endl;
          return value;
      }




      // VIF1_ERR
      if (address == 0x10003c20){
          g_logFile << "VIF1_ERR Read Address: " << std::hex << address << std::endl;
          int vif_id = 1;
          uint32_t value = g_vif.Read(vif_id, address);
          g_logFile << "VIF1_ERR Read Value: " << std::hex << value << std::endl;
          return value;
      }
      // INTC registers
      if (address == 0x10000000) {
          // Simple Hack: Use a static counter to simulate time passing.
          // Ideally, bind this to your emulator's cycle count: (cpu_cycles / 16) & 0xFFFF
          static uint32_t timer_fake_ticks = 0;
          timer_fake_ticks += 1000; // Advance time by 1000 ticks per read
          return timer_fake_ticks & 0xFFFF;
      }
      if (address >= 0x10003000 && address <= 0x100030AF) {
          return g_gif.Read(address);
      }
      if (address == 0x1000F000 || address == 0x1000F010) {
          g_scheduler.CheckAndFireVBlank();
          if (address == 0x1000F000 && (debug_read_count % 1000 == 0)) {




              g_logFile << "[STALL WATCH] CPU spamming read to INTC_STAT (Waiting for VSync?)" << std::endl;
          }
          return g_intc.Read(address);
      }
    
      // DMAC channel registers (0x10008000 - 0x1000D4FF)
      if (address >= 0x10008000 && address < 0x1000D800) {
          return g_dmac.Read(address);
      }
      // 2. DMAC Status (D_STAT)
      if (address == 0x1000E010) {
          if (debug_read_count % 1000 == 0) {
              g_logFile << "[STALL WATCH] CPU spamming read to D_STAT (Waiting for DMA?)" << std::endl;
          }
          return g_dmac.Read(address);
      }
      if (address >= 0x12000000 && address <= 0x12001FFF) {
          if (address == 0x12001000) { // GS_CSR
              if (debug_read_count % 1000 == 0) {
                  g_logFile << "[STALL WATCH] CPU spamming read to GS_CSR (Waiting for GPU?)" << std::endl;
              }
              // HLE Hack: Return 8 (Bit 3 set = VSync Occurred).
              // This tricks games waiting for VSync into continuing.
              // You might also need Bit 13 (Field) toggling in the future.
              return 0x8;
          }
          return 0; // Stub other GS registers
      }
      // DMAC control registers (0x1000E000 - 0x1000E060)
      if (address >= 0x1000E000 && address <= 0x1000E060) {
          return g_dmac.Read(address);
      }
      if (address == 0x10000000) {
          static uint32_t timer_fake_ticks = 0;
          timer_fake_ticks += 1000;
          return timer_fake_ticks & 0xFFFF;
      }
      // D_ENABLER (0x1000F520)
      if (address == 0x1000F520) {
          return g_dmac.Read(address);
      }
      if(address == 0x12001000){
          g_logFile << "Reading GS_CSR Register: " << std::hex << address << std::endl;
          return g_gs_regs.GS_CSR;
      }




      if (address >= 0x12000000 && address <= 0x12002000) {
      // 1. Read the full 64-bit register
      uint64_t full_val = ReadPrivilegedRegister(address & ~0x7); // Align to 8 bytes
    
      // 2. Check if we want the Lower or Upper half
      if (address & 0x4) {
          return (uint32_t)(full_val >> 32); // Upper 32 bits
      } else {
          return (uint32_t)(full_val & 0xFFFFFFFF); // Lower 32 bits
      }
      }








      if (address >= 0x1000F200 && address <= 0x1000F260) {
          switch (address) {
              case 0x1000F200: return g_sif.mscom;
              case 0x1000F210: return g_sif.smcom;
              case 0x1000F220: return g_sif.msflag;
              case 0x1000F230: return g_sif.smflag; // The game waits for this to become 0x20000
              case 0x1000F240: return g_sif.ctl_reg;
              default: return 0;
          }
      }
    
      // TODO: Add more I/O registers as needed (timers, GIF, VIF, etc.)
    
      // Unknown I/O register - log and return 0
      // printf("[MEM] Unknown I/O read @ 0x%08X\n", address);
      return 0;
  }




  // Centralized I/O register write
void write_io_register(uint32_t address, uint32_t value) {
  // [IO-TRACE] Log I/O writes to GS privileged and DMAC ranges with calling PC
  if ((address >= 0x12000000 && address <= 0x12002000) ||
      (address >= 0x10008000 && address < 0x1000E064)) {
      g_logFile << "[IO-TRACE] PC=0x" << std::hex << g_current_pc
                << " write addr=0x" << address
                << " value=0x" << value << std::dec << std::endl;
  }


  if (address >= 0x10000000 && address < 0x10002000) {
      // EE Timer registers - absorb silently
      // TODO: implement timers if needed for game timing
      g_logFile << "Timer Register Write (ignored): Address: " << std::hex << address
                << ", Value: " << value << std::endl;
      return;
  }
  if(address >= 0x10005000 && address <= 0x1000500C) {
      g_logFile << "VIF1 Write to FIFO/Latch: " << std::hex << value
                << " at address " << address << std::endl;
      g_vif.WriteToLatch(1, value, address);
      return;
  }
  // VIF0_FBRST
  if (address == 0x10003810){
      g_logFile << "VIF0_FBRST Write: " << std::hex << value << std::endl;
      int vif_id = 0;
      g_vif.Write(vif_id, address, value);
      return;
  }




  // VIF0_ERR
  if (address == 0x10003820){
      g_logFile << "VIF0_ERR Write: " << std::hex << value << std::endl;
      int vif_id = 0;
      g_vif.Write(vif_id, address, value);
      return;
  }




  // VIF1_FBRST
  if (address == 0x10003c10){
      g_logFile << "VIF1_FBRST Write: " << std::hex << value << std::endl;
      int vif_id = 1;
      g_vif.Write(vif_id, address, value);
      return;
  }




  // VIF1_ERR
  if (address == 0x10003c20){
      g_logFile << "VIF1_ERR Write: " << std::hex << value << std::endl;
      int vif_id = 1;
      g_vif.Write(vif_id, address, value);
      return;
  }
   // -------------------------------------------------
  // 1. GIF Registers (0x10003000 - 0x100030AF)
  // -------------------------------------------------
  if (address >= 0x10003000 && address <= 0x100030AF) {
      g_gif.Write(address, value);
      return;
  }




  // -------------------------------------------------
  // 2. DMAC Registers (Channels, Global, & Orphan)
  //    Ranges: 0x8000-0xE064 AND 0xF590
  // -------------------------------------------------
  if ((address >= 0x10008000 && address < 0x1000E064) || address == 0x1000F590) {
      // This function should handle the CHCR logic internally:
      // 1. Update the register state.
      // 2. If address == 0x1000A000 && (value & STR), trigger the Graphics Thread.
      g_logFile << "DMAC Write Address: " << std::hex << address
                << ", Value: " << value << std::endl;
      g_dmac.Write(address, value);
      return;
  }




  // -------------------------------------------------
  // 3. INTC Registers (Interrupt Controller)
  // -------------------------------------------------
  if (address == 0x1000F000 || address == 0x1000F010) {
      g_intc.Write(address, value);
      return;
  }




  // -------------------------------------------------
  // 4. SIF Registers (EE <-> IOP Communication)
  // -------------------------------------------------
  if (address >= 0x1000F200 && address <= 0x1000F260) {
      switch (address) {
          case 0x1000F200: g_sif.mscom = value;  break;
          case 0x1000F210: g_sif.smcom = value;  break;
          case 0x1000F220: g_sif.msflag = value; break;
          case 0x1000F230: g_sif.smflag = value; break;
          case 0x1000F240: g_sif.ctl_reg = value; break;
          default: break;
      }
      return;
  }




  if(address == 0x12001000){
      g_logFile << "Writing to GS_CSR Register: " << std::hex << value << std::endl;
      g_gs_regs.GS_CSR = value;
      return;
  }




  if(address >= 0x12000000 && address <= 0x12002000) {
      if (address & 0x4) {
          // Write specifically to the upper 32 bits
          WritePrivilegedUpper(address & ~0x7, value);
      } else {
          // Write specifically to the lower 32 bits
          WritePrivilegedLower(address & ~0x7, value);
      }
      return;
  }




  // Log unknown IO writes to help debug missed registers
  g_logFile << "[IO] Unhandled Write: " << std::hex << address << std::endl;
}
   template <typename T>
  T read(uint32_t address) {

        if (address == 0x437b60) {
            g_logFile << "[Memory Read] Address: " 
                << std::hex << address << std::endl;
        }
        if (address == 0x425280) {
            g_logFile << "[Memory Read] Address: " 
                << std::hex << address << std::endl;
        }

    
        if (address >= 0x3068e0 && address < 0x306938) {
            g_logFile << "[Memory Read] Address: " 
                << std::hex << address << std::endl;
        }


      if (address >= 0x002f67e0 && address <= 0x002f6808) {
          g_logFile << "[Memory Read] Address: " << std::hex << address << std::endl;
      }




      if (address >= 0x002f67e0 && address <= 0x002f6810) {
          g_logFile << "[VTable Read] Address: 0x" << std::hex << address << std::endl;
        
          // Optional: Since you know the exact address of your function pointer:
          if (address == 0x002f6804) {
              g_logFile << "!!! HIT: Reading pointer for FUN_001aa900 !!!" << std::endl;
          }
      }
    
      // Check I/O registers FIRST (before translate_address)
      if (is_io_register(address)) {
          // I/O registers are 32-bit aligned, handle smaller reads
          if constexpr (sizeof(T) <= 4) {
              uint32_t aligned_addr = address & ~0x3;
              uint32_t value = read_io_register(aligned_addr);
            
              // Handle sub-word reads if needed
              if constexpr (sizeof(T) == 4) {
                  return static_cast<T>(value);
              } else if constexpr (sizeof(T) == 2) {
                  int shift = (address & 0x2) * 8;
                  return static_cast<T>((value >> shift) & 0xFFFF);
              } else if constexpr (sizeof(T) == 1) {
                  int shift = (address & 0x3) * 8;
                  return static_cast<T>((value >> shift) & 0xFF);
              }
          } else {
              // 64-bit I/O read - read two 32-bit values
              uint64_t lo = read_io_register(address);
              uint64_t hi = read_io_register(address + 4);
              return static_cast<T>((hi << 32) | lo);
          }
      }




      uint8_t* ptr = translate_address(address, sizeof(T));
      if (!ptr) {
          return 0;
      }
    
      T value;
      std::memcpy(&value, ptr, sizeof(T));




      if (address == 0x4A16A0){
          g_logFile << "[FILENAME ADDR READ] Address: " << std::hex << address
                  << ", Value: " << value << std::endl;
      }




      //g_logFile << "[Read] Address: " << std::hex << address << ", Value: " << value << std::endl;
      return value;
  }




  template <typename T>
  void write(uint32_t address, T value) {




      /*
      if (address == 0x31b008 && value != 0) {
          g_logFile << "[POOL INIT] base=0x" << std::hex << value << std::endl;
      }
      // Also watch the pool size field
      if (address == 0x31b00c) {
          g_logFile << "[POOL INIT] total_size=0x" << std::hex << value << std::endl;
      }
      if (address == 0x31b010) {
          g_logFile << "[POOL INIT] elem_size=0x" << std::hex << value << std::endl;
      }








      if (address == 0xb8cde0) {  // slot 42 of the array
          g_logFile << "[WATCHPOINT SLOT42] write value=0x" << std::hex << value << std::endl;
      }
      if (address == 0xb8cef0) {
          g_logFile << "[WATCHPOINT OBJ_VTABLE] write value=0x" << std::hex << value << std::endl;
      }




      if (address == 0xb8cb30 && value != 0) {
          g_logFile << "[MANAGER INIT] write value=0x" << std::hex << value << std::endl;
      }








      if (address == 0x70003ABC) { // 0x70003AB0 + 0xC
          g_logFile << "[WATCHPOINT] write to stream read_ptr: value=0x"
                  << std::hex << value << std::endl;
      }
      if (address == 0x70003AB4) { // 0x70003AB0 + 0xC
          g_logFile << "[WATCHPOINT 70003AB4] write to stream read_ptr: value=0x"
                  << std::hex << value << std::endl;
      }
      if (address == 0x31B190){
          g_logFile << "[WRITING TO VIRTUAL ADDRESS IN MAIN RAM] Address: " << std::hex << address
                  << ", Value: " << value <<std::endl;
      }
      if (address >= 0x11000000 && address < 0x11001000){
      g_logFile << "[VU0 Write called] Write called for address: " << std::hex << address << ""
                  << " with value: " << value <<std::endl;
      }
      if (address == 0x309780){
          g_logFile << "[MEMORY WATCH GIF TAG] Write called for address: " << std::hex << address << ""
                  << " with value: " << value <<std::endl;




      }
      if (address == 0x306b50){
          g_logFile << "[MEMORY WATCH] Potential .BIN/ .DAT write called for address: " << std::hex << address
                  << " with value: " << value <<std::endl;




      }
      if (address >= 0x12000000 && address < 0x12000100) {
          g_logFile << "[GS PRIVILEGED REGISTER WRITE] Address: " << std::hex << address
                  << ", Value: " << value << std::endl;
      }
      if (address == 0x3d4400){
          g_logFile << "[MEMORY WATCH GIF TAG] Write called for address: " << std::hex << address
                  << " with value: " << value <<std::endl;
      }




      if (address == 0x4A16A0){
          g_logFile << "[FILENAME ADDR WRITE] Address: " << std::hex << address
                  << ", Value: " << value << std::endl;
      }




      if (address == 0x4A18D0){
          g_logFile << "[SUSPECTED TEXTURE BUFFER ADDRESS] Address: " << std::hex << address
                  << ", Value: " << value << std::endl;




      }
    
      */



      if(address >= 0x4202e0 && address < 0x4204e8){
          g_logFile << "[STATE-WATCH] Write<" << sizeof(T) << "> called for address: " << std::hex << address
                  << " with value: " << (uint64_t)value << std::dec
                  << " PC: 0x" << std::hex << g_current_pc << std::dec << std::endl;

      }

      if(address == (0x3C73D0 + 0x20)){ // This is the address of the pointer used in the function that creates the main menu background. It gets set to a non-zero value when the game writes the background texture address into it.
          g_logFile << "[CLIENT-ADDR 0x3C7390] Write<" << sizeof(T) << "> called for address: " << std::hex << address
                  << " with value: " << (uint64_t)value << std::dec;

      }
     

      if(address >= 0x4202e0 && address < 0x4204e8 && value ==  0x60000000010000){
          g_logFile << "[STATE-WATCH] Write<" << sizeof(T) << "> called for address: " << std::hex << address
                  << " with value: " << (uint64_t)value << std::dec
                  << " PC: 0x" << std::hex << g_current_pc << std::dec << std::endl;

      }
      if (address >= 0x450170 && address < 0x450198) {
              g_logFile << "[MEMORY WATCH] Write<" << sizeof(T) << "> called for address: " << std::hex << address
                      << " with value: " << (uint64_t)value << std::dec << std::endl;
      }

      if (address == 0x3d4400) {
              g_logFile << "[MEMORY WATCH] Write<" << sizeof(T) << "> called for address: " << std::hex << address
                      << " with value: " << (uint64_t)value << std::dec << std::endl;
      }

      if (address == 0x1000A000) {
              g_logFile << "[MEMORY WATCH] Write<" << sizeof(T) << "> called for address: " << std::hex << address
                      << " with value: " << (uint64_t)value << std::dec << std::endl;
      }

      if (address == 0xb9f5a8) {
              g_logFile << "[MEMORY WATCH] Write<" << sizeof(T) << "> called for address: " << std::hex << address
                      << " with value: " << (uint64_t)value << std::dec << std::endl;
      }

      if (address == 0x323300 && value == 0x50) {
              g_logFile << "[MEMORY WATCH] Write<" << sizeof(T) << "> called for address: " << std::hex << address
                      << " with value: " << (uint64_t)value << std::dec << std::endl;
      }








      // Check I/O registers FIRST
      if (is_io_register(address)) {
          if constexpr (sizeof(T) <= 4) {
              // Most I/O writes are 32-bit
              g_logFile << "[IO WRITE] Address: " << std::hex << address
                        << ", Value: " << static_cast<uint32_t>(value) << std::endl;
              write_io_register(address & ~0x3, static_cast<uint32_t>(value));
          } else {
              // 64-bit I/O write
              write_io_register(address, static_cast<uint32_t>(value));
              write_io_register(address + 4, static_cast<uint32_t>(value >> 32));
          }
          return;
      }




      uint8_t* ptr = translate_address(address, sizeof(T));
      if (!ptr) {
          return;
      }
      std::memcpy(ptr, &value, sizeof(T));




      //g_logFile << "[Write] Address: " << std::hex << address << ", Value: " << value << std::endl;
      if (address == 0x30971C && value == 0x7B2748){
          //g_logFile << "[Write] Address: " << std::hex << address << ", Value: " << value << std::endl;
          //g_logFile << "FUNCTION WROTE HERE!!!!!" << std::endl;
      }
      if (address == 0x1000A000) { // D2_CHCR
          if (value & (1 << 8)) {  // STR bit set?
              g_logFile << "DEBUG: GIF DMA Channel 2 Started! Waking Graphics Thread.\n" << std::endl;
              // ACTION: Signal your Graphics Thread Condition Variable here
              g_logFile << "Check to see if sceSifSetDma woke up the graphics thread.\n" << std::endl;




          }
          else{
              g_logFile << "Writing to this adddress but with a different bit set\n" << std::endl;
              g_logFile << "Address: " << std::hex << address << ", Value: " << value << std::endl;




          }
      }
      if (address == 0x1000F000){
          g_logFile << "Writing to INTC_STAT\n" << std::endl;
          g_logFile << "Address: " << std::hex << address << ", Value: " << value << std::endl;




      }
      if (address == 0x1000F010){
          g_logFile << "Writing to INTC_MASK\n" << std::endl;
          g_logFile << "Address: " << std::hex << address << ", Value: " << value << std::endl;




      }
      if (address == 0x10009000){
          g_logFile << "DEBUG: VIF1 DMA Channel 1 Written to!\n" << std::endl;
          g_logFile << "Address: " << std::hex << address << ", Value: " << value << std::endl;




      }




      if (address == 0x700039b0 && value == 0x30024000){
          //g_logFile << "[Write] Address: " << std::hex << address << ", Value: " << value << std::endl;
          //g_logFile << "FUNCTION WROTE HERE!!!!!" << std::endl;
      }




      if (address >= 0x2ec190 && address < 0x2ec200) {
              printf("[Overlay Trap] Write to %x detected! Value: %x\n", address, value);
              // Print the current PC to see which function triggered the write
              // If PC is 0 (or inside kernel), it's likely a DMA transfer.
          }




      //g_logFile << "[Write] Address: " << std::hex << address << ", Value: " << value << std::endl;
      if (address >= 0x6280 && address <= 0x6290) {
          g_logFile << "[Write] Address: " << std::hex << address << ", Value: " << value << std::endl;
      }
    
  }
   // Implementation for 128-bit read
  void read_quad(uint32_t address, QuadWord& value) {
      // I/O registers don't typically support 128-bit access,
      // but handle it just in case
      if (is_io_register(address)) {
          value.u32[0] = read_io_register(address);
          value.u32[1] = read_io_register(address + 4);
          value.u32[2] = read_io_register(address + 8);
          value.u32[3] = read_io_register(address + 12);
          return;
      }




      uint8_t* ptr = translate_address(address, sizeof(QuadWord));
      if (!ptr) {
          std::memset(&value, 0, sizeof(QuadWord));
          return;
      }
      std::memcpy(&value, ptr, sizeof(QuadWord));
  }




  // Implementation for 128-bit write
  void write_quad(uint32_t address, const QuadWord& value) {
          if (address >= 0x628980 && address < 0x6289B0) {
      g_logFile << "[SQ] addr=0x" << std::hex << address
                << " value=0x" << value.u32[0] << value.u32[1] << value.u32[2] << value.u32[3] << std::endl;
  }
      // Catch quad writes that overlap the DMA tag at 0x450180
      if (address >= 0x450170 && address < 0x450198) {
          g_logFile << "[MEMORY WATCH QUAD] Write_quad at address: 0x" << std::hex << address
                    << " values: " << value.u32[0] << " " << value.u32[1]
                    << " " << value.u32[2] << " " << value.u32[3] << std::dec << std::endl;
      }
      if (is_io_register(address)) {
          write_io_register(address, value.u32[0]);
          write_io_register(address + 4, value.u32[1]);
          write_io_register(address + 8, value.u32[2]);
          write_io_register(address + 12, value.u32[3]);
          return;
      }




      uint8_t* ptr = translate_address(address, sizeof(QuadWord));
      if (!ptr) {
          return;
      }
      std::memcpy(ptr, &value, sizeof(QuadWord));
  }




  void* get_pointer(uint32_t address) {
      // Don't return pointers to I/O registers!
      if (is_io_register(address)) {
          return nullptr;
      }
      return translate_address(address, 1);
  }
  // Explicit template instantiations
  template uint8_t read<uint8_t>(uint32_t address);
  template uint16_t read<uint16_t>(uint32_t address);
  template uint32_t read<uint32_t>(uint32_t address);
  template uint64_t read<uint64_t>(uint32_t address);
  template int32_t  memory::read<int32_t>(uint32_t address);
  template void write<uint8_t>(uint32_t address, uint8_t value);
  template void write<uint16_t>(uint32_t address, uint16_t value);
  template void write<uint32_t>(uint32_t address, uint32_t value);
  template void write<int32_t>(uint32_t address, int32_t value);
  template void write<uint64_t>(uint32_t address, uint64_t value);
 }





