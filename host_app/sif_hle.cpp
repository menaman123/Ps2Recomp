#include "sif_hle.h"
#include "cpu_state.h"
#include "memory.h"
#include "ps2_scheduler.h"
#include <iostream>
#include <iomanip>
#include <fstream>
#include <cstring>
#include "sif.h"
#include "render.h"
#include "recompiled.h"
#include <SDL_timer.h>
#include <SDL.h>
#include <iostream>
// Undo Windows macro pollution that conflicts with rabbitizer
#undef CONST
#undef PURE
#undef TRUE
#undef FALSE
#include "recompiler_tool\recompiler.h"
#include "intc.h"






extern std::ofstream g_logFile;
extern std::map<uint32_t, std::function<void(CpuContext&, uint32_t)>> recompiled_functions;
extern std::function<void(CpuContext&, uint32_t)> find_containing_function(uint32_t pc);
extern std::atomic<bool> g_vsync_pending;
extern INTC g_intc;




void hle_sceSifInitRpc(CpuContext& ctx) {
  g_logFile << "HLE: sceSifInitRpc called at PC 0x" << std::hex << ctx.cpuRegs.pc << std::endl;




  // 1. Singleton Check: Has SIF already been initialized?
  // Address 0x002ea850 is the 'is_initialized' flag from your disassembly
  uint32_t init_flag_addr = 0x002ea850;
   if (memory::read<uint32_t>(init_flag_addr) != 0) {
      g_logFile << "  HLE: SIF already initialized. Returning success." << std::endl;
      // In the original assembly, it calls FUN_002d4e10 here.
      // We usually just return 1 (true/success) for HLE.
      ctx.cpuRegs.GPR.r[2].UD[0] = 1;
      return;
  }




  // 2. Mark as Initialized
  memory::write<uint32_t>(init_flag_addr, 1);




  // 3. Clear the SIF Client Data Queue (Critical Memory Side Effect)
  // The disassembly loop clears roughly 0x180 bytes starting at 0x003caa80
  // We use get_pointer to do a fast host-side memset.
  uint32_t queue_addr = 0x003caa80;
  uint32_t queue_size = 0x180; // (0x3cac00 - 0x3caa80)
   void* raw_mem = memory::get_pointer(queue_addr);
  if (raw_mem) {
      std::memset(raw_mem, 0, queue_size);
      g_logFile << "  HLE: Cleared SIF Client Queue at 0x" << std::hex << queue_addr << std::endl;
  }




  // 4. Initialize Global SIF Pointers
  // The game expects these specific global variables to point to valid structures.
  // Values taken directly from your disassembly:
  memory::write<uint32_t>(0x003caa58, 0x203ca980); // Uncached address?
  memory::write<uint32_t>(0x003caa74, 0x003cac00); // End of buffer?
  memory::write<uint32_t>(0x003caa5c, 0x203caa00);
  memory::write<uint32_t>(0x003caa68, 0x20);       // Packet size/step
  memory::write<uint32_t>(0x003caa60, 0);
  memory::write<uint32_t>(0x003caa64, 0x003caa80); // Head of queue
  memory::write<uint32_t>(0x003caa6c, 0);
  memory::write<uint32_t>(0x003caa70, 0);




  // 5. Set Function Pointers (Callbacks)
  // The game writes function pointers for the SIF handler to call later.
  // Ideally, these addresses (0x002d1a18, 0x002d19f8) should also be HLE'd or
  // point to valid recompiled code. We write them so the struct is complete.
  memory::write<uint32_t>(0x003caa80, 0x002d1a18); // Callback 1
  memory::write<uint32_t>(0x003caa8c, 0x002d19f8); // Callback 2
  memory::write<uint32_t>(0x003caa90, 0x003caa58); // Arg for Callback
  memory::write<uint32_t>(0x003caa84, 0x003caa58);




  // 6. Fake the IOP Handshake
  // The original code loops waiting for sceSifGetReg() & 0x20000.
  // We set the SIF flags directly to convince the game the IOP is ready.
  g_sif.smflag |= 0x20000; // SIF_STAT_CMDINIT
  g_sif.smflag |= 0x40000; // SIF_STAT_BOOTEND (Safe to add)
   // Write to the physical register so Read<T> picks it up if checked elsewhere
  memory::write<uint32_t>(0x1000F230, g_sif.smflag);




  g_logFile << "  HLE: Fake IOP Handshake complete (SMFLAG=0x60000)" << std::endl;




  // 7. Return Success
  // The function ends with a Bind RPC call. Since we HLE'd the init,
  // we assume the Bind succeeds.
  ctx.cpuRegs.GPR.r[2].UD[0] = 1;
}




void hle_ContextRestore(CpuContext& ctx) {
  // 1. Base address of the saved context (from disassembly 002d68e8)
  uint32_t ctx_base = 0x003d17c0; //
   // 2. Restore General Purpose Registers (GPRs)
  // The disassembly 002d68ec-002d695c uses 'lq' (Load Quadword)
  // Structure offset 0x10 is $at, 0x20 is $v0, etc.
  for (int i = 1; i < 32; i++) {
      // Read 128-bit quadword into our GPR union
      memory::read_quad(ctx_base + (i * 0x10), (QuadWord&)ctx.cpuRegs.GPR.r[i]); //
  }




  // 3. Restore Special Registers (HI/LO/SA)
  // Offsets based on 002d68ac - 002d68dc
  uint32_t misc_base = 0x003d19c0;
   // Restoration using memory::read<uint64_t> as seen in disassembly 'ld'
  ctx.cpuRegs.HI.UD[0]  = memory::read<uint64_t>(misc_base + 0x00); //
  ctx.cpuRegs.HI1.UD[0] = memory::read<uint64_t>(misc_base + 0x08); //
  ctx.cpuRegs.LO.UD[0]  = memory::read<uint64_t>(misc_base + 0x10); //
  ctx.cpuRegs.LO1.UD[0] = memory::read<uint64_t>(misc_base + 0x18); //
   // Restore Shift Amount (SA) using mtsa at 002d68dc
  ctx.cpuRegs.sa = (u32)memory::read<uint64_t>(misc_base + 0x20); //




  // 4. Critical: Restore the Program Counter (EPC)
  // Load from 0x003d19e8 and move to CP0 EPC
  uint32_t target_pc = memory::read<uint32_t>(0x003d19e8); //
  ctx.cpuRegs.CP0.n.EPC = target_pc; //
   // 5. Force the Jump for the Recompiler
  ctx.cpuRegs.pc = target_pc; //
   // 6. Update CP0 Status
  // Masking and setting bits 0x13 as per 002d6880 and 002d6964
  uint32_t status = ctx.cpuRegs.CP0.n.Status; //
  status = (status & 0xffffffe4) | 0x13; //
  ctx.cpuRegs.CP0.n.Status = status; //




  // 7. Signal the recompiler to stop and dispatch to the new PC
  // throw Recompiler::BreakBlockException();
}
void hle_WaitForVblank(CpuContext& ctx) {
  g_logFile << "HLE: WaitForVblank (0x002cf770) called." << std::endl;




  // --- 1. Queue the Graphics Update ---
  // This part was correct; keep it to ensure the screen draws.
  RenderJob frameEnd;
  frameEnd.type = RenderCommandType::VSync;
  g_renderQueue.Push(frameEnd);




  // --- 2. Simulate Hardware State ---
  // Set VBLANK Start (Bit 2) and Timer 0 (Bit 9) in INTC_STAT
  // We update both your shadow copy and LLE memory.
  uint32_t stat_mask = (1 << 2) | (1 << 9);
   // Update internal struct (if you use one)
  // g_intc.stat |= stat_mask;




  // Update LLE Memory (0x1000F000)
  uint32_t current_stat = memory::read<uint32_t>(0x1000F000);
  memory::write<uint32_t>(0x1000F000, current_stat | stat_mask);




  // Toggle Field in GS_CSR (0x12001000)
  uint64_t csr = memory::read<uint64_t>(0x12001000);
  csr ^= (1ULL << 13);
  memory::write<uint64_t>(0x12001000, csr);




  // --- 3. The "Single-Threaded" Context Switch ---
  // Instead of returning to the game, we FORCE the CPU to jump to the Interrupt Handler.
   // A. Save the point where we want to return after the handler finishes.
  // Since this HLE function replaced a CALL, the return address is in RA ($31).
  // The handler will run, hit 'ERET', and jump back to this address.
  uint32_t return_addr = ctx.cpuRegs.GPR.r[31].UL[0];
  ctx.cpuRegs.CP0.n.EPC = return_addr;




  // B. Set Exception Level (EXL) bit in Status Register (Bit 1)
  // This tells the CPU we are in kernel/exception mode.
  ctx.cpuRegs.CP0.n.Status |= 2;




  // C. Set Cause Register
  // ExcCode = 0 (Int), IP2 = 1 (INTC Interrupt Pending)
  // IP2 is usually Bit 10.
  ctx.cpuRegs.CP0.n.Cause &= ~0x7C; // Clear ExcCode (0 = Int)
  ctx.cpuRegs.CP0.n.Cause |= (1 << 10); // Set IP2




  // D. Force PC to General Exception Vector
  // The main loop will pick this up next iteration and execute the handler MIPS code.
  ctx.cpuRegs.pc = 0x80000200;




  // E. Return Value (v0 = 1)
  // We set this now so it's ready when the handler returns to the game.
  ctx.cpuRegs.GPR.r[2].UL[0] = 1;




  // --- 4. Timing ---
  // Sleep to simulate frame time, keeping the emulator from running too fast.
  SDL_Delay(16);
   g_logFile << "HLE: Dispatching Interrupt to 0x80000200. Return Addr: 0x"
            << std::hex << return_addr << std::dec << std::endl;
}




void hle_DoGlobalConstructors(CpuContext& ctx) {
  g_logFile << "HLE: Executing Global Constructors (Address 0x002d69a0)" << std::endl;




  // 1. Get the Start Address and Count
  // Based on Disassembly: lw a0, -0x40fc(v0) where v0=0x2f0000 -> 0x002ebf04
  uint32_t table_info_addr = 0x002ebf04;
   // The table of pointers usually starts 4 bytes after the count variable
  uint32_t table_start_addr = table_info_addr + 4;




  int32_t count = memory::read<int32_t>(table_info_addr);




  // 2. Handle Dynamic Counting (Sentinel -1)
  // The disassembly checks: if (DAT_002ebf04 == -1) ...
  if (count == -1) {
      g_logFile << "  HLE: Count is -1, scanning for NULL terminator..." << std::endl;
      count = 0;
      uint32_t scan_ptr = table_start_addr;
    
      while (memory::read<uint32_t>(scan_ptr) != 0) {
          count++;
          scan_ptr += 4;
      }
  }




  g_logFile << "  HLE: Found " << count << " constructors." << std::endl;




  // 3. Iterate Backwards and Execute
  // The assembly loop iterates backwards: while (count > 0)
  if (count > 0) {
      // Points to the *end* of the array
      uint32_t current_entry_ptr = table_start_addr + (count * 4);




      while (count > 0) {
          // Decrement pointer first (pre-decrement logic in some MIPS loops,
          // but here we just grab the last valid entry)
          current_entry_ptr -= 4;
        
          // Read the Function Pointer (MIPS Address)
          uint32_t constructor_addr = memory::read<uint32_t>(current_entry_ptr);




          if (constructor_addr != 0) {
              g_logFile << "  HLE: Calling Constructor at 0x" << std::hex << constructor_addr << std::dec << std::endl;




              // --- CRITICAL: EXECUTE THE TARGET FUNCTION ---
              // We use your existing infrastructure to find and run the recompiled block.
            
              // Option A: Direct lookup if we know it's a block start
              if (recompiled_functions.count(constructor_addr)) {
                  recompiled_functions[constructor_addr](ctx, constructor_addr);
              }
              // Option B: Smart lookup (handles cases where it jumps into the middle of a block)
              else {
                  auto func_ptr = find_containing_function(constructor_addr);
                  if (func_ptr) {
                      func_ptr(ctx, constructor_addr);
                  } else {
                      g_logFile << "  [ERROR] Constructor at 0x" << std::hex << constructor_addr
                                << " has not been recompiled!" << std::dec << std::endl;
                      // Optional: Fallback to interpreter here if you have one
                  }
              }
          }




          count--;
      }
  }




  g_logFile << "HLE: Global Constructors Finished. Returning." << std::endl;




  // 4. Return (Simulate 'jr ra')
  ctx.cpuRegs.pc = ctx.cpuRegs.GPR.r[31].UL[0];
}




void hle_InitTLB(CpuContext& ctx) {
  g_logFile << "HLE: InitTLB called. (Skipping hardware TLB setup)" << std::endl;
  // In a pure HLE, we don't strictly need to write to the hardware TLB registers
  // unless the game reads them back (which is rare).
   // However, we SHOULD update the Wired register in our CPU state
  // because some games check it to see how many slots are free.
  // Based on the disassembly, typical Wired value after init is around 13-18.
   // A safe guess or reading the value from the game's data segment:
  ctx.cpuRegs.CP0.n.Wired = 18; // Example value
}








namespace sif_bind_rpc {




BoundClient g_bound_clients[MAX_BOUND_CLIENTS];




void InitBindingTracker() {
  std::memset(g_bound_clients, 0, sizeof(g_bound_clients));
}




int RegisterBinding(uint32_t client_addr, uint32_t server_id) {
  // Check if already registered
  for (int i = 0; i < MAX_BOUND_CLIENTS; i++) {
      if (g_bound_clients[i].is_bound &&
          g_bound_clients[i].client_addr == client_addr) {
          // Update existing binding
          g_bound_clients[i].server_id = server_id;
          return i;
      }
  }
   // Find empty slot
  for (int i = 0; i < MAX_BOUND_CLIENTS; i++) {
      if (!g_bound_clients[i].is_bound) {
          g_bound_clients[i].client_addr = client_addr;
          g_bound_clients[i].server_id = server_id;
          g_bound_clients[i].is_bound = true;
          return i;
      }
  }
   return -1; // No slots available
}




BoundClient* FindBindingByClient(uint32_t client_addr) {
  for (int i = 0; i < MAX_BOUND_CLIENTS; i++) {
      if (g_bound_clients[i].is_bound &&
          g_bound_clients[i].client_addr == client_addr) {
          return &g_bound_clients[i];
      }
  }
  return nullptr;
}




BoundClient* FindBindingByServer(uint32_t server_id) {
  for (int i = 0; i < MAX_BOUND_CLIENTS; i++) {
      if (g_bound_clients[i].is_bound &&
          g_bound_clients[i].server_id == server_id) {
          return &g_bound_clients[i];
      }
  }
  return nullptr;
}




const char* GetServerName(uint32_t server_id) {
  switch (server_id) {
      case 0x80000100: return "PADMAN";
      case 0x80000101: return "PADMAN_EXT";
      case 0x80000001: return "FILEIO";
      case 0x80000003: return "IOPHEAP";
      case 0x80000006: return "LOADFILE";
      case 0x80000400: return "MCSERV";
      case 0x80000592: return "CDVD_INIT";
      case 0x80000593: return "CDVD_SCMD";
      case 0x80000595: return "CDVD_NCMD";
      case 0x80000597: return "CDVD_SEARCHFILE";
      case 0x8000059A: return "CDVD_DISKREADY";
      case 0x80000701: return "LIBSD Remote";
      default:         return "UNKNOWN";
  }
}




} // namespace sif




// =============================================================================
// HLE: sceSifBindRpc
// =============================================================================
// Original function: FUN_002d27c8
//
// Signature: int sceSifBindRpc(SifRpcClientData* client, uint32_t server_id, uint32_t mode)
//
// SifRpcClientData structure layout:
//   0x00: void* packet         - Internal packet buffer
//   0x04: void* next           - Linked list pointer
//   0x08: int   sema_id        - Semaphore for blocking calls
//   0x0C: void* unused
//   0x10: uint  command        - Current command (cleared on bind)
//   0x14: void* buff           - Send buffer
//   0x18: void* cbuff          - Callback buffer
//   0x1C: void* end_function   - Completion callback
//   0x20: void* end_param      - Callback parameter
//   0x24: void* server         - IOP server pointer (non-zero = bound successfully)
//
// Returns:
//   0  = Success
//  -1  = Packet allocation failed
//  -2  = Send failed
//  -3  = Semaphore creation failed
// =============================================================================
void hle_sceSifBindRpc(CpuContext& ctx) {
  // 1. Read arguments from registers
  uint32_t client_addr = ctx.cpuRegs.GPR.r[4].UL[0];  // a0: SifRpcClientData*
  uint32_t server_id   = ctx.cpuRegs.GPR.r[5].UL[0];  // a1: Server ID
  uint32_t mode        = ctx.cpuRegs.GPR.r[6].UL[0];  // a2: Mode (bit 0 = non-blocking)
  uint32_t return_addr = ctx.cpuRegs.GPR.r[31].UL[0]; // ra: who called us
   const char* server_name = sif_bind_rpc::GetServerName(server_id);
   g_logFile << "[SIF-BIND-HLE] ========== BIND VIA HLE ==========" << std::endl;
  g_logFile << "[SIF-BIND-HLE] caller_ra=0x" << std::hex << return_addr << std::endl;
  g_logFile << "[SIF-BIND-HLE] client_addr=0x" << client_addr << std::endl;
  g_logFile << "[SIF-BIND-HLE] server_id=0x" << server_id << " (" << server_name << ")" << std::endl;
  g_logFile << "[SIF-BIND-HLE] mode=0x" << mode << (mode & 1 ? " (non-blocking)" : " (blocking)") << std::dec << std::endl;
  g_logFile << "[SIF-BIND-HLE] =================================" << std::endl;




  // 2. Validate client address
  if (client_addr == 0 || client_addr >= 0x02000000) {
      g_logFile << "     ERROR: Invalid client address!" << std::endl;
      ctx.cpuRegs.GPR.r[2].UD[0] = 0xFFFFFFFF; // Return -1
      ctx.cpuRegs.pc = ctx.cpuRegs.GPR.r[31].UL[0]; // jr ra
      return;
  }




  // 3. Clear the command field (offset 0x10) - matches original behavior
  memory::write<uint32_t>(client_addr + 0x10, 0);
   // 4. Set the server pointer (offset 0x24) to a fake non-zero value
  //    The game only checks if this is non-zero to confirm bind success.
  //    We encode the server_id into it for debugging purposes.
  uint32_t fake_server_ptr = 0xDEAD0000 | (server_id & 0xFFFF);
  memory::write<uint32_t>(client_addr + 0x24, fake_server_ptr);
   // 5. Initialize other client fields to sensible defaults
  memory::write<uint32_t>(client_addr + 0x00, 0x00100000);  // Fake packet pointer
  memory::write<uint32_t>(client_addr + 0x04, 0);           // next = NULL
  memory::write<uint32_t>(client_addr + 0x08, -1);          // sema_id = -1 (no semaphore needed in HLE)
  memory::write<uint32_t>(client_addr + 0x0C, 0);           // unused
  memory::write<uint32_t>(client_addr + 0x14, 0);           // buff
  memory::write<uint32_t>(client_addr + 0x18, 0);           // cbuff
  memory::write<uint32_t>(client_addr + 0x1C, 0);           // end_function
  memory::write<uint32_t>(client_addr + 0x20, 0);           // end_param
   // 6. Register this binding in our tracker (for future RPC call routing)
  int slot = sif_bind_rpc::RegisterBinding(client_addr, server_id);
  if (slot >= 0) {
      g_logFile << "     Registered binding in slot " << slot << std::endl;
  } else {
      g_logFile << "     WARNING: Could not register binding (tracker full)" << std::endl;
  }
   // 7. Return success
  ctx.cpuRegs.GPR.r[2].UD[0] = 0; // v0 = 0 (success)
   g_logFile << "     SUCCESS: Bind complete, server_ptr = 0x" << std::hex << fake_server_ptr << std::dec << std::endl;
   // 8. Return to caller (simulate jr ra)
  ctx.cpuRegs.pc = ctx.cpuRegs.GPR.r[31].UL[0];
}




void FUN_00181490(CpuContext& ctx) {
  // HLE Bypass: Assume the check always passes
  // Write 0 to the "Piracy Check Failed" flag
  memory::write<uint32_t>(0x0030a458, 0);
}

