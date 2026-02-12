// vu.h - Fixed and comprehensive VU implementation
#pragma once
#include "cpu_state.h"
#include <cstdint>
#include <vector>

// ============================================================================
// VU Field Mask Bits (from instruction bits 21-24)
// ============================================================================
namespace VU_DEST {
    constexpr uint8_t X = 0x08;  // Bit 24
    constexpr uint8_t Y = 0x04;  // Bit 23
    constexpr uint8_t Z = 0x02;  // Bit 22
    constexpr uint8_t W = 0x01;  // Bit 21
    constexpr uint8_t XYZW = 0x0F;
}

// ============================================================================
// VU Broadcast Modes (bits 0-1 of upper opcode for broadcast variants)
// ============================================================================
enum VU_BC : uint8_t {
    BC_X = 0,
    BC_Y = 1,
    BC_Z = 2,
    BC_W = 3
};

// ============================================================================
// MAC Flag Bit Positions (4 flags x 4 fields = 16 bits)
// Format: [Ox Oy Oz Ow | Ux Uy Uz Uw | Sx Sy Sz Sw | Zx Zy Zz Zw]
// ============================================================================
namespace MAC_FLAG {
    // Zero flags (bits 0-3)
    constexpr uint16_t ZW = (1 << 0);
    constexpr uint16_t ZZ = (1 << 1);
    constexpr uint16_t ZY = (1 << 2);
    constexpr uint16_t ZX = (1 << 3);
    
    // Sign flags (bits 4-7)
    constexpr uint16_t SW = (1 << 4);
    constexpr uint16_t SZ = (1 << 5);
    constexpr uint16_t SY = (1 << 6);
    constexpr uint16_t SX = (1 << 7);
    
    // Underflow flags (bits 8-11)
    constexpr uint16_t UW = (1 << 8);
    constexpr uint16_t UZ = (1 << 9);
    constexpr uint16_t UY = (1 << 10);
    constexpr uint16_t UX = (1 << 11);
    
    // Overflow flags (bits 12-15)
    constexpr uint16_t OW = (1 << 12);
    constexpr uint16_t OZ = (1 << 13);
    constexpr uint16_t OY = (1 << 14);
    constexpr uint16_t OX = (1 << 15);
}

// ============================================================================
// Status Flag Bits
// ============================================================================
namespace STATUS_FLAG {
    constexpr uint16_t Z  = (1 << 0);   // Zero
    constexpr uint16_t S  = (1 << 1);   // Sign
    constexpr uint16_t U  = (1 << 2);   // Underflow
    constexpr uint16_t O  = (1 << 3);   // Overflow
    constexpr uint16_t I  = (1 << 4);   // Invalid (DIV 0/0)
    constexpr uint16_t D  = (1 << 5);   // Division by zero
    // Sticky flags (bits 6-11)
    constexpr uint16_t ZS = (1 << 6);
    constexpr uint16_t SS = (1 << 7);
    constexpr uint16_t US = (1 << 8);
    constexpr uint16_t OS = (1 << 9);
    constexpr uint16_t IS = (1 << 10);
    constexpr uint16_t DS = (1 << 11);
}

// ============================================================================
// VU Upper Instruction Opcodes (6-bit function field)
// ============================================================================
namespace VU_UPPER {
    // Standard operations (bits 0-5)
    constexpr uint8_t ADDx    = 0x00;
    constexpr uint8_t ADDy    = 0x01;
    constexpr uint8_t ADDz    = 0x02;
    constexpr uint8_t ADDw    = 0x03;
    constexpr uint8_t SUBx    = 0x04;
    constexpr uint8_t SUBy    = 0x05;
    constexpr uint8_t SUBz    = 0x06;
    constexpr uint8_t SUBw    = 0x07;
    constexpr uint8_t MADDx   = 0x08;
    constexpr uint8_t MADDy   = 0x09;
    constexpr uint8_t MADDz   = 0x0A;
    constexpr uint8_t MADDw   = 0x0B;
    constexpr uint8_t MSUBx   = 0x0C;
    constexpr uint8_t MSUBy   = 0x0D;
    constexpr uint8_t MSUBz   = 0x0E;
    constexpr uint8_t MSUBw   = 0x0F;
    constexpr uint8_t MAXx    = 0x10;
    constexpr uint8_t MAXy    = 0x11;
    constexpr uint8_t MAXz    = 0x12;
    constexpr uint8_t MAXw    = 0x13;
    constexpr uint8_t MINIx   = 0x14;
    constexpr uint8_t MINIy   = 0x15;
    constexpr uint8_t MINIz   = 0x16;
    constexpr uint8_t MINIw   = 0x17;
    constexpr uint8_t MULx    = 0x18;
    constexpr uint8_t MULy    = 0x19;
    constexpr uint8_t MULz    = 0x1A;
    constexpr uint8_t MULw    = 0x1B;
    constexpr uint8_t MULq    = 0x1C;
    constexpr uint8_t MAXi    = 0x1D;
    constexpr uint8_t MULi    = 0x1E;
    constexpr uint8_t MINIi   = 0x1F;
    constexpr uint8_t ADDq    = 0x20;
    constexpr uint8_t MADDq   = 0x21;
    constexpr uint8_t ADDi    = 0x22;
    constexpr uint8_t MADDi   = 0x23;
    constexpr uint8_t SUBq    = 0x24;
    constexpr uint8_t MSUBq   = 0x25;
    constexpr uint8_t SUBi    = 0x26;
    constexpr uint8_t MSUBi   = 0x27;
    constexpr uint8_t ADD     = 0x28;
    constexpr uint8_t MADD    = 0x29;
    constexpr uint8_t MUL     = 0x2A;
    constexpr uint8_t MAX     = 0x2B;
    constexpr uint8_t SUB     = 0x2C;
    constexpr uint8_t MSUB    = 0x2D;
    constexpr uint8_t OPMSUB  = 0x2E;
    constexpr uint8_t MINI    = 0x2F;
    
    // Special2 table (when bits 0-1 = 11xx)
    constexpr uint8_t SPECIAL2_BASE = 0x3C;
}

// ============================================================================
// VU Lower Instruction Opcodes (7-bit opcode field, bits 25-31)
// ============================================================================
namespace VU_LOWER {
    constexpr uint8_t LQ      = 0x00;
    constexpr uint8_t SQ      = 0x01;
    constexpr uint8_t ILW     = 0x04;
    constexpr uint8_t ISW     = 0x05;
    constexpr uint8_t IADDIU  = 0x08;
    constexpr uint8_t ISUBIU  = 0x09;
    constexpr uint8_t FCEQ    = 0x10;
    constexpr uint8_t FCSET   = 0x11;
    constexpr uint8_t FCAND   = 0x12;
    constexpr uint8_t FCOR    = 0x13;
    constexpr uint8_t FSEQ    = 0x14;
    constexpr uint8_t FSSET   = 0x15;
    constexpr uint8_t FSAND   = 0x16;
    constexpr uint8_t FSOR    = 0x17;
    constexpr uint8_t FMEQ    = 0x18;
    constexpr uint8_t FMAND   = 0x1A;
    constexpr uint8_t FMOR    = 0x1B;
    constexpr uint8_t FCGET   = 0x1C;
    constexpr uint8_t IADD    = 0x20;
    constexpr uint8_t ISUB    = 0x21;
    constexpr uint8_t IADDI   = 0x22;
    constexpr uint8_t IAND    = 0x28;
    constexpr uint8_t IOR     = 0x29;
    
    // Branch/Jump
    constexpr uint8_t B       = 0x20;  // Unconditional (special encoding)
    constexpr uint8_t BAL     = 0x21;
    constexpr uint8_t JR      = 0x24;
    constexpr uint8_t JALR    = 0x25;
    constexpr uint8_t IBEQ    = 0x28;
    constexpr uint8_t IBNE    = 0x29;
    constexpr uint8_t IBLTZ   = 0x2C;
    constexpr uint8_t IBGTZ   = 0x2D;
    constexpr uint8_t IBLEZ   = 0x2E;
    constexpr uint8_t IBGEZ   = 0x2F;
    
    // Load/Store with increment/decrement
    constexpr uint8_t LQI     = 0x40;
    constexpr uint8_t SQI     = 0x41;
    constexpr uint8_t LQD     = 0x42;
    constexpr uint8_t SQD     = 0x43;
    
    // Special
    constexpr uint8_t XGKICK  = 0x6C;  // VU1 only - DMA kick
}

// ============================================================================
// VU1 Class
// ============================================================================
class VU1 {
public:
    VU1(int id);
    void Reset();
    
    // Memory access
    void WriteDataMem(uint32_t addr, const uint8_t* data, size_t size);
    void WriteMicroMem(uint32_t addr, const uint8_t* data, size_t size);
    void ReadDataMem(uint32_t addr, uint8_t* data, size_t size) const;
    
    // Execution
    void Execute(CpuContext& ctx, uint32_t start_addr);
    
    // XGKICK state for stall detection
    bool xgkick_active = false;
    uint32_t xgkick_addr = 0;

private:
    // ========================================================================
    // Instruction Dispatchers
    // ========================================================================
    void RunUpper(CpuContext& ctx, uint32_t instr);
    void RunLower(CpuContext& ctx, uint32_t instr, uint32_t& pc, bool& branch_taken, uint32_t& branch_target);
    int vu_id;  // 0 = VU0, 1 = VU1
    uint32_t mem_mask;
    
    // ========================================================================
    // Upper Pipeline Helpers
    // ========================================================================
    
    // Get broadcast value from source register based on BC field
    float GetBroadcastValue(const VR_reg& src, VU_BC bc) const;
    
    // Create broadcast vector (all 4 fields = same value)
    VR_reg MakeBroadcast(float val) const;
    
    // Apply destination mask and write result
    void WriteWithMask(VR_reg& dest, const VR_reg& result, uint8_t mask, bool protect_vf0);
    
    // Update MAC flags based on result for a single field
    uint16_t ComputeMACFlags(float result, int field) const;
    
    // Full MAC flag update for a vector result with mask
    void UpdateMACFlags(CpuContext& ctx, const VR_reg& result, uint8_t mask);
    
    // Update status flags from MAC flags
    void UpdateStatusFlags(CpuContext& ctx);
    
    // Clamp PS2 float (handle denormals, etc.)
    float ClampFloat(float val) const;
    
    // ========================================================================
    // Lower Pipeline Operations
    // ========================================================================
    void Op_XGKICK(CpuContext& ctx, uint32_t addr);
    
    // ========================================================================
    // Special2 Upper Operations (FDIV, etc.)
    // ========================================================================
    void RunSpecial2Upper(CpuContext& ctx, uint32_t instr);
};
extern VU1 g_vu0;
extern VU1 g_vu1;