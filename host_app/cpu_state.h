#pragma once

#include "common/Pcsx2Types.h"
#include <array>
#include <cstring>

// --- Data Structures for CPU and Coprocessor Registers ---

// Represents a single 128-bit General Purpose Register (GPR).
union alignas(16) GPR_reg {
    u128 UQ;
    s128 SQ;
    u64 UD[2];
    s64 SD[2];
    u32 UL[4];
    s32 SL[4];
    u16 US[8];
    s16 SS[8];
    u8 UC[16];
    s8 SC[16];
};

union alignas(16) VR_reg {
    struct { float x, y, z, w; }; // Access as four floats
    float F[4];
    uint32_t I[4];
    s128 Q;

    // --- Add these operator overloads ---
    VR_reg operator+(const VR_reg& other) const {
        return {x + other.x, y + other.y, z + other.z, w + other.w};
    }

    VR_reg operator-(const VR_reg& other) const {
        return {x - other.x, y - other.y, z - other.z, w - other.w};
    }

    VR_reg operator*(const VR_reg& other) const {
        return {x * other.x, y * other.y, z * other.z, w * other.w};
    }

    VR_reg operator/(const VR_reg& other) const {
        return {x / other.x, y / other.y, z / other.z, w / other.w};
    }
};

// Union for all 32 GPRs.
union GPRregs {
    GPR_reg r[32];
};

// Coprocessor 0 (CP0) Registers, for system control.
union CP0regs {
    struct {
        u32 Index, Random, EntryLo0, EntryLo1, Context, PageMask, Wired, Reserved0,
            BadVAddr, Count, EntryHi, Compare, Status, Cause, EPC, PRid, Config,
            LLAddr, WatchLO, WatchHI, XContext, Reserved1, Reserved2, Debug, DEPC,
            PerfCnt, ErrCtl, CacheErr, TagLo, TagHi, ErrorEPC, DESAVE;
    } n;
    u32 r[32];
};

// Represents a single 32-bit Floating Point Register (FPR).
union FPRreg {
    float f;
    u32 UL;
    s32 SL;
};

// Floating Point Unit (FPU / COP1) Registers.
struct FPUregs {
    FPRreg fpr[32];
    u32 fprc[32]; // Control registers
    FPRreg ACC;
    u32 ACCflag;
};


struct VUregs {
    // =========================================================================
    // Vector Float Registers (VF0-VF31)
    // =========================================================================
    VR_reg VF[32];      // 32 Vector Float registers
                        // VF0 is hardwired: {0.0, 0.0, 0.0, 1.0}
    
    // =========================================================================
    // Integer Registers (VI0-VI15) - 16-bit each
    // =========================================================================
    u16 VI[16];         // VI0 always reads as 0
    
    // =========================================================================
    // Accumulator
    // =========================================================================
    VR_reg ACC;         // Accumulator for MADD/MSUB operations
    
    // =========================================================================
    // Special Float Registers
    // =========================================================================
    float Q;            // Q register - Division/sqrt result (reg 22)
    float P;            // P register - EFU result (VU1 only)
    
    // =========================================================================
    // Special Integer/Bit Registers (accessed as 32-bit via CFC2/CTC2)
    // =========================================================================
    u32 I;              // I register - Immediate value (reg 21)
    u32 R;              // R register - Random number (reg 20)
    
    // =========================================================================
    // Status and Flag Registers
    // =========================================================================
    u16 status_flag;    // Status flags (reg 16)
                        // Bits 0-5: Z,S,U,O flags
                        // Bits 6-11: ZS,SS,US,OS,IS,DS sticky flags
    
    u16 mac_flag;       // MAC flags (reg 17)
                        // 16 bits: 4 flags (Z,S,U,O) x 4 fields (x,y,z,w)
    
    u32 clip_flag;      // Clipping flags (reg 18)
                        // 24 bits: 6 flags x 4 CLIP instruction results
    
    // =========================================================================
    // Program Control Registers
    // =========================================================================
    u32 TPC;            // VU micro program counter (reg 26)
    u32 CMSAR0;         // VU0 micro subroutine call return address (reg 27)
    u32 FBRST;          // Force break/reset control (reg 28)
    u32 VPU_STAT;       // VU0/VU1 execution status (reg 29, read-only)
    u32 CMSAR1;         // VU1 micro subroutine call return address (reg 31)
    
    // =========================================================================
    // Helper to read control register by number (for CFC2)
    // =========================================================================
    int32_t ReadControlReg(int reg) const {
        switch (reg) {
            case 0:  return 0;  // VI0 always 0
            case 1: case 2: case 3: case 4: case 5: case 6: case 7:
            case 8: case 9: case 10: case 11: case 12: case 13: case 14: case 15:
                // Sign-extend 16-bit VI to 32-bit
                return static_cast<int32_t>(static_cast<int16_t>(VI[reg]));
            case 16: return static_cast<int32_t>(status_flag);
            case 17: return static_cast<int32_t>(mac_flag);
            case 18: return static_cast<int32_t>(clip_flag);
            case 20: return static_cast<int32_t>(R);
            case 21: return static_cast<int32_t>(I);
            case 22: {
                // Return Q as raw bits
                u32 bits;
                std::memcpy(&bits, &Q, sizeof(bits));
                return static_cast<int32_t>(bits);
            }
            case 26: return static_cast<int32_t>(TPC);
            case 27: return static_cast<int32_t>(CMSAR0);
            case 28: return static_cast<int32_t>(FBRST);
            case 29: return static_cast<int32_t>(VPU_STAT);
            case 31: return static_cast<int32_t>(CMSAR1);
            default: return 0;
        }
    }
    
    // =========================================================================
    // Helper to write control register by number (for CTC2)
    // =========================================================================
    void WriteControlReg(int reg, u32 value) {
        switch (reg) {
            case 0:  break;  // VI0 is read-only
            case 1: case 2: case 3: case 4: case 5: case 6: case 7:
            case 8: case 9: case 10: case 11: case 12: case 13: case 14: case 15:
                VI[reg] = static_cast<u16>(value);
                break;
            case 16: status_flag = static_cast<u16>(value & 0xFFF); break;
            case 17: mac_flag = static_cast<u16>(value); break;
            case 18: clip_flag = value & 0xFFFFFF; break;
            case 20: R = value; break;
            case 21: I = value; break;
            case 22: std::memcpy(&Q, &value, sizeof(Q)); break;
            case 26: TPC = value; break;
            case 27: CMSAR0 = value; break;
            case 28: FBRST = value; break;
            // case 29: VPU_STAT is read-only
            case 31: CMSAR1 = value; break;
            default: break;
        }
    }
    
    // =========================================================================
    // Initialize VU registers to power-on state
    // =========================================================================
    void Reset() {
        std::memset(this, 0, sizeof(VUregs));
        
        // VF0 is hardwired to {0, 0, 0, 1}
        VF[0].x = 0.0f;
        VF[0].y = 0.0f;
        VF[0].z = 0.0f;
        VF[0].w = 1.0f;
        
        // Q is typically initialized to 1.0
        Q = 1.0f;
    }
};


// Core CPU Registers
struct CPURegs {
    GPRregs GPR;
    GPR_reg HI;
    GPR_reg LO;
    GPR_reg HI1;  // R5900 Pipeline 1 HI register
    GPR_reg LO1;  // R5900 Pipeline 1 LO register
    CP0regs CP0;
    u32 sa;
    u32 pc;
};

// Represents the full state of the Emotion Engine.
struct EmotionEngineState {
    CPURegs cpuRegs;
    FPUregs fpuRegs;
    VUregs vuRegs;
};

typedef EmotionEngineState CpuContext;
extern CpuContext* g_cpuContext;