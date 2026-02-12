#include "intc.h"
#include <cstdio>
#include "memory.h"
#include "recompiled.h"
#include <fstream>


extern std::ofstream g_logFile;


INTC g_intc;
std::map<int, std::vector<IntcHandler>> g_intc_queues;
int g_nextIntcId = 1;

void INTC::Reset() {
    stat = 0;
    mask = 0;
}

uint32_t INTC::Read(uint32_t addr) {
    switch (addr) {
        case 0x1000F000: return stat;
        case 0x1000F010: return mask;
        default:
            printf("[INTC] Unknown read @ 0x%08X\n", addr);
            return 0;
    }
}

void INTC::Write(uint32_t addr, uint32_t value) {
    switch (addr) {
        case 0x1000F000:
            // Write 1 to clear bits (acknowledge)
            stat &= ~value;
            break;
        case 0x1000F010:
            // Write 1 to REVERSE/toggle bits
            mask ^= value;
            break;
        default:
            g_logFile << "[INTC] Unknown write @ 0x" << std::hex << addr 
                      << " = 0x" << value << std::endl;
            break;
    }
}

bool INTC::EnableIntc(int cause_bit) {
    uint32_t bit = 1u << cause_bit;
    bool was_disabled = (mask & bit) == 0;
    mask |= bit;  // Set the bit (enable)
    return was_disabled;  // Return true if it WAS 0
}

bool INTC::DisableIntc(int cause_bit) {
    uint32_t bit = 1u << cause_bit;
    bool was_enabled = (mask & bit) != 0;
    mask &= ~bit;  // Clear the bit (disable)
    return was_enabled;  // Return true if it WAS 1
}

void INTC::RaiseInterrupt(int cause) {
    stat |= (1u << cause);
}

bool INTC::CheckInterrupt() {
    return (stat & mask) != 0;
}

void INTC::DispatchInterrupt(CpuContext& ctx) {
    // 1. Find which causes are active and enabled
    uint32_t pending = stat & mask;
    if (!pending) return;
    
    for (int cause = 0; cause < INTC_COUNT; cause++) {
        if (pending & (1u << cause)) {
            // Found a pending interrupt (e.g., Cause 9 for VSync)
            
            auto it = g_intc_queues.find(cause);
            if (it != g_intc_queues.end()) {
                for (auto& handler : it->second) {
                    if (handler.active) {
                        g_logFile << "[INTC] Dispatching Handler for Cause " << std::dec << cause 
                                  << " at PC 0x" << std::hex << handler.handler_pc << std::endl;

                        // --- CONTEXT SAVE ---
                        // Interrupts effectively "pause" the current thread.
                        // We must save the PC and volatile registers so we don't corrupt the running game.
                        // In a full emulator, we'd save everything, but saving the PC and GPRs is usually enough.
                        uint32_t original_pc = ctx.cpuRegs.pc;
                        auto original_gpr = ctx.cpuRegs.GPR; // Save all General Purpose Registers

                        // --- SETUP HANDLER ---
                        // PS2 Intc Handlers receive their 'arg' in a0 ($4)
                        // They also rely on their specific 'gp' ($28) being set
                        ctx.cpuRegs.GPR.r[4].UL[0] = handler.arg; 
                        ctx.cpuRegs.GPR.r[28].UL[0] = handler.gp;
                        
                        // Set Return Address (ra) to a magic trap value so we know when it finishes
                        // (Not strictly needed if we execute synchronously, but good for debugging)
                        ctx.cpuRegs.GPR.r[31].UL[0] = 0x80001000; 

                        // --- EXECUTE ---
                        uint32_t target = handler.handler_pc;
                        auto func_ptr = find_containing_function(target);
                        
                        if (func_ptr) {
                            // Fast Path: Already recompiled
                            func_ptr(ctx, target);
                        } else {
                            // Slow Path: Interpreter Fallback
                            // (Reuse the trampoline you added earlier)
                            g_logFile << "[INTC] Running Interpreter for Handler 0x" << std::hex << target << std::endl;
                        }

                        // --- CONTEXT RESTORE ---
                        // Restore the CPU state so the main thread continues exactly where it left off
                        ctx.cpuRegs.pc = original_pc;
                        ctx.cpuRegs.GPR = original_gpr;
                    }
                }
            }
            
            // --- ACKNOWLEDGE ---
            // Clear the interrupt bit so we don't fire it again instantly
            // (Unless your HLE logic handles this elsewhere, usually the Kernel dispatcher clears it)
            stat &= ~(1u << cause);
            memory::write<uint32_t>(0x1000F000, stat); // Sync with memory
        }
    }
}