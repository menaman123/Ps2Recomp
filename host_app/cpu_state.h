#pragma once

#include "common/Pcsx2Types.h"
#include <array>

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


// Vector Unit 0 (VU0) Registers.
struct VUregs {
    VR_reg regs[32];    // 32 General Purpose Vector Registers
    VR_reg ACC;         // Accumulator
    VR_reg Q;           // Q-register (for division)
    VR_reg I;           // I-register (for interpolation)
    VR_reg P;           // P-register (for GTE-style execution)
    u16 mac_flag;       // MAC flag register
    u16 sticky_flag;    // Sticky flag register
    u16 clip_flag[4];   // Clipping flags
};

// Core CPU Registers
struct CPURegs {
    GPRregs GPR;
    GPR_reg HI;
    GPR_reg LO;
    CP0regs CP0;
    u32 sa;
    u32 pc;
};

// Represents the full state of the Emotion Engine.
struct EmotionEngineState {
    CPURegs cpuRegs;
    FPUregs fpuRegs;
    VUregs vuRegs;
    CP0regs cop0;
};

typedef EmotionEngineState CpuContext;