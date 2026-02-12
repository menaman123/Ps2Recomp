// vu.cpp - Fixed and comprehensive VU implementation
#include "vu.h"
#include "gif.h"
#include "memory.h"
#include <iostream>
#include <fstream>
#include <cstring>
#include <iomanip>
#include <cmath>
#include <algorithm>

extern std::ofstream g_logFile;

// ============================================================================
// Constructor and Reset
// ============================================================================
VU1::VU1(int id) : vu_id(id) {
    mem_mask = (id == 0) ? 0xFFF : 0x3FFF;
    Reset();
    vu1_data_memory.resize((id == 0) ? 4096 : 16384);
    vu1_code_memory.resize((id == 0) ? 4096 : 16384);
}

void VU1::Reset() {
    xgkick_active = false;
    xgkick_addr = 0;
}

// ============================================================================
// Memory Access
// ============================================================================
void VU1::WriteDataMem(uint32_t addr, const uint8_t* data, size_t size) {
    for (size_t i = 0; i < size; i++) {
        vu1_data_memory[(addr + i) & 0x3FFF] = data[i];
    }
}

void VU1::WriteMicroMem(uint32_t addr, const uint8_t* data, size_t size) {
    for (size_t i = 0; i < size; i++) {
        vu1_code_memory[(addr + i) & 0x3FFF] = data[i];
    }
}

void VU1::ReadDataMem(uint32_t addr, uint8_t* data, size_t size) const {
    for (size_t i = 0; i < size; i++) {
        data[i] = vu1_data_memory[(addr + i) & 0x3FFF];
    }
}

// ============================================================================
// Float Helpers
// ============================================================================
float VU1::ClampFloat(float val) const {
    // PS2 VU treats denormals as zero
    uint32_t bits;
    std::memcpy(&bits, &val, sizeof(bits));
    
    uint32_t exp = (bits >> 23) & 0xFF;
    if (exp == 0 && (bits & 0x7FFFFF) != 0) {
        // Denormal - flush to zero with sign preserved
        bits &= 0x80000000;
        std::memcpy(&val, &bits, sizeof(val));
    }
    
    return val;
}

float VU1::GetBroadcastValue(const VR_reg& src, VU_BC bc) const {
    switch (bc) {
        case BC_X: return src.x;
        case BC_Y: return src.y;
        case BC_Z: return src.z;
        case BC_W: return src.w;
        default: return src.x;
    }
}

VR_reg VU1::MakeBroadcast(float val) const {
    VR_reg result;
    result.x = result.y = result.z = result.w = val;
    return result;
}

// ============================================================================
// MAC Flag Computation
// ============================================================================
uint16_t VU1::ComputeMACFlags(float result, int field) const {
    uint16_t flags = 0;
    
    uint32_t bits;
    std::memcpy(&bits, &result, sizeof(bits));
    
    uint32_t exp = (bits >> 23) & 0xFF;
    uint32_t frac = bits & 0x7FFFFF;
    bool sign = (bits >> 31) != 0;
    
    // Zero flag: exponent and fraction both zero
    if (exp == 0 && frac == 0) {
        flags |= (1 << field);  // Z flag for this field
    }
    
    // Sign flag: bit 31 set and not zero
    if (sign && !(exp == 0 && frac == 0)) {
        flags |= (1 << (field + 4));  // S flag
    }
    
    // Underflow flag: denormal (exp=0, frac!=0) - PS2 flushes these
    if (exp == 0 && frac != 0) {
        flags |= (1 << (field + 8));  // U flag
    }
    
    // Overflow flag: exp=255 (infinity/NaN in IEEE, but PS2 treats differently)
    if (exp == 0xFF) {
        flags |= (1 << (field + 12));  // O flag
    }
    
    return flags;
}

void VU1::UpdateMACFlags(CpuContext& ctx, const VR_reg& result, uint8_t mask) {
    uint16_t new_mac = 0;
    
    if (mask & VU_DEST::X) new_mac |= ComputeMACFlags(result.x, 3);  // X is field 3 in MAC layout
    if (mask & VU_DEST::Y) new_mac |= ComputeMACFlags(result.y, 2);  // Y is field 2
    if (mask & VU_DEST::Z) new_mac |= ComputeMACFlags(result.z, 1);  // Z is field 1
    if (mask & VU_DEST::W) new_mac |= ComputeMACFlags(result.w, 0);  // W is field 0
    
    ctx.vuRegs.mac_flag = new_mac;
}

void VU1::UpdateStatusFlags(CpuContext& ctx) {
    uint16_t mac = ctx.vuRegs.mac_flag;
    uint16_t status = ctx.vuRegs.status_flag;
    
    // Clear non-sticky flags
    status &= 0xFC0;  // Keep sticky bits (6-11)
    
    // Set current flags from MAC
    if (mac & 0x000F) status |= STATUS_FLAG::Z;   // Any Z flag
    if (mac & 0x00F0) status |= STATUS_FLAG::S;   // Any S flag
    if (mac & 0x0F00) status |= STATUS_FLAG::U;   // Any U flag
    if (mac & 0xF000) status |= STATUS_FLAG::O;   // Any O flag
    
    // Update sticky flags
    if (status & STATUS_FLAG::Z) status |= STATUS_FLAG::ZS;
    if (status & STATUS_FLAG::S) status |= STATUS_FLAG::SS;
    if (status & STATUS_FLAG::U) status |= STATUS_FLAG::US;
    if (status & STATUS_FLAG::O) status |= STATUS_FLAG::OS;
    
    ctx.vuRegs.status_flag = status;
}

// ============================================================================
// Write with Destination Mask
// ============================================================================
void VU1::WriteWithMask(VR_reg& dest, const VR_reg& result, uint8_t mask, bool protect_vf0) {
    if (protect_vf0) return;  // VF0 is read-only
    
    if (mask & VU_DEST::X) dest.x = ClampFloat(result.x);
    if (mask & VU_DEST::Y) dest.y = ClampFloat(result.y);
    if (mask & VU_DEST::Z) dest.z = ClampFloat(result.z);
    if (mask & VU_DEST::W) dest.w = ClampFloat(result.w);
}

// ============================================================================
// Main Execution Loop (with delay slot support)
// ============================================================================
void VU1::Execute(CpuContext& ctx, uint32_t start_addr) {
    uint32_t pc = start_addr & 0x3FFF;
    int cycle_limit = 100000;
    
    if (g_logFile.is_open()) {
        g_logFile << "[VU1] Execute start at 0x" << std::hex << pc << std::dec << std::endl;
    }
    
    // Hardwire VF0
    ctx.vuRegs.VF[0].x = 0.0f;
    ctx.vuRegs.VF[0].y = 0.0f;
    ctx.vuRegs.VF[0].z = 0.0f;
    ctx.vuRegs.VF[0].w = 1.0f;
    
    bool in_delay_slot = false;
    bool branch_pending = false;
    uint32_t branch_target = 0;
    bool end_pending = false;
    
    while (cycle_limit-- > 0) {
        g_logFile << "[VU1] PC=0x" << std::hex << pc << std::dec << std::endl;

        if (pc + 8 > vu1_code_memory.size()) break;
        
        // Fetch instruction pair (64-bit)
        uint32_t lower_instr, upper_instr;
        std::memcpy(&lower_instr, &vu1_code_memory[pc], 4);
        std::memcpy(&upper_instr, &vu1_code_memory[pc + 4], 4);
        
        // Decode control bits from upper instruction
        bool i_bit = (upper_instr & (1u << 31)) != 0;  // I-bit: Load I register
        bool e_bit = (upper_instr & (1u << 30)) != 0;  // E-bit: End program
        bool m_bit = (upper_instr & (1u << 29)) != 0;  // M-bit: End interlock (VU0)
        bool d_bit = (upper_instr & (1u << 28)) != 0;  // D-bit: Debug break
        bool t_bit = (upper_instr & (1u << 27)) != 0;  // T-bit: Debug halt
        
        // Handle I-bit: Load lower instruction bits into I register
        if (i_bit) {
            ctx.vuRegs.I = lower_instr;
            if (g_logFile.is_open()) {
                g_logFile << "  [VU1] I-bit: I = 0x" << std::hex << lower_instr << std::dec << std::endl;
            }
        }
        
        // Track if this instruction has a branch
        bool this_branch_taken = false;
        uint32_t this_branch_target = 0;
        
        // Execute upper instruction (always runs)
        RunUpper(ctx, upper_instr);
        
        // Execute lower instruction (unless I-bit is set)
        if (!i_bit) {
            RunLower(ctx, lower_instr, pc, this_branch_taken, this_branch_target);
        }
        
        // Re-hardwire VF0 after any instruction (ensures writes are discarded)
        ctx.vuRegs.VF[0].x = 0.0f;
        ctx.vuRegs.VF[0].y = 0.0f;
        ctx.vuRegs.VF[0].z = 0.0f;
        ctx.vuRegs.VF[0].w = 1.0f;
        
        // VI0 is also always 0
        ctx.vuRegs.VI[0] = 0;
        
        // Handle delay slot execution
        if (in_delay_slot) {
            // We just executed the delay slot, now take the branch
            pc = branch_target & 0x3FFF;
            in_delay_slot = false;
            branch_pending = false;
            
            if (end_pending) {
                if (g_logFile.is_open()) {
                    g_logFile << "[VU1] E-bit: Program end after delay slot" << std::endl;
                }
                break;
            }
            continue;
        }
        
        // E-bit: End after delay slot
        if (e_bit) {
            end_pending = true;
            in_delay_slot = true;
            branch_target = pc + 8;  // Continue to next instruction (delay slot) then stop
            pc += 8;
            continue;
        }
        
        // Branch handling: if branch was taken, enter delay slot
        if (this_branch_taken) {
            in_delay_slot = true;
            branch_pending = true;
            branch_target = this_branch_target;
        }
        
        // Normal PC advancement
        pc = (pc + 8) & 0x3FFF;
        ctx.vuRegs.TPC = pc;
    }
    
    if (cycle_limit <= 0 && g_logFile.is_open()) {
        g_logFile << "[VU1] WARNING: Cycle limit reached!" << std::endl;
        g_logFile << "[VU1] Cycle limit: " << cycle_limit << std::endl;
    }
}

// ============================================================================
// Upper Instruction Execution
// ============================================================================
void VU1::RunUpper(CpuContext& ctx, uint32_t instr) {
    g_logFile << "[VU1] Upper Instr: 0x" << std::hex << instr << std::dec << std::endl;
    // Extract fields
    uint8_t func = instr & 0x3F;             // Bits 0-5: Function
    uint8_t fd   = (instr >> 6) & 0x1F;      // Bits 6-10: Destination VF
    uint8_t fs   = (instr >> 11) & 0x1F;     // Bits 11-15: Source 1 VF
    uint8_t ft   = (instr >> 16) & 0x1F;     // Bits 16-20: Source 2 VF
    uint8_t dest = (instr >> 21) & 0x0F;     // Bits 21-24: Destination mask (WZYX)
    
    // Get broadcast component (bits 0-1 for broadcast variants)
    VU_BC bc = static_cast<VU_BC>(func & 0x03);
    
    // Reference registers
    VR_reg& dest_reg = ctx.vuRegs.VF[fd];
    const VR_reg& src1 = ctx.vuRegs.VF[fs];
    const VR_reg& src2 = ctx.vuRegs.VF[ft];
    
    bool protect_fd = (fd == 0);
    VR_reg result = {};
    
    // Check for Special2 table (func >= 0x3C)
    if (func >= 0x3C) {
        RunSpecial2Upper(ctx, instr);
        return;
    }
    
    // Decode and execute based on function
    switch (func) {
        // ====================================================================
        // Broadcast ADD variants: ADDx, ADDy, ADDz, ADDw (0x00-0x03)
        // ====================================================================
        case VU_UPPER::ADDx:
        case VU_UPPER::ADDy:
        case VU_UPPER::ADDz:
        case VU_UPPER::ADDw:
        {
            float bc_val = GetBroadcastValue(src2, bc);
            result.x = src1.x + bc_val;
            result.y = src1.y + bc_val;
            result.z = src1.z + bc_val;
            result.w = src1.w + bc_val;
            WriteWithMask(dest_reg, result, dest, protect_fd);
            UpdateMACFlags(ctx, result, dest);
            break;
        }
        
        // ====================================================================
        // Broadcast SUB variants: SUBx, SUBy, SUBz, SUBw (0x04-0x07)
        // ====================================================================
        case VU_UPPER::SUBx:
        case VU_UPPER::SUBy:
        case VU_UPPER::SUBz:
        case VU_UPPER::SUBw:
        {
            float bc_val = GetBroadcastValue(src2, bc);
            result.x = src1.x - bc_val;
            result.y = src1.y - bc_val;
            result.z = src1.z - bc_val;
            result.w = src1.w - bc_val;
            WriteWithMask(dest_reg, result, dest, protect_fd);
            UpdateMACFlags(ctx, result, dest);
            break;
        }
        
        // ====================================================================
        // Broadcast MADD variants (0x08-0x0B)
        // ====================================================================
        case VU_UPPER::MADDx:
        case VU_UPPER::MADDy:
        case VU_UPPER::MADDz:
        case VU_UPPER::MADDw:
        {
            float bc_val = GetBroadcastValue(src2, bc);
            result.x = ctx.vuRegs.ACC.x + (src1.x * bc_val);
            result.y = ctx.vuRegs.ACC.y + (src1.y * bc_val);
            result.z = ctx.vuRegs.ACC.z + (src1.z * bc_val);
            result.w = ctx.vuRegs.ACC.w + (src1.w * bc_val);
            WriteWithMask(dest_reg, result, dest, protect_fd);
            UpdateMACFlags(ctx, result, dest);
            break;
        }
        
        // ====================================================================
        // Broadcast MSUB variants (0x0C-0x0F)
        // ====================================================================
        case VU_UPPER::MSUBx:
        case VU_UPPER::MSUBy:
        case VU_UPPER::MSUBz:
        case VU_UPPER::MSUBw:
        {
            float bc_val = GetBroadcastValue(src2, bc);
            result.x = ctx.vuRegs.ACC.x - (src1.x * bc_val);
            result.y = ctx.vuRegs.ACC.y - (src1.y * bc_val);
            result.z = ctx.vuRegs.ACC.z - (src1.z * bc_val);
            result.w = ctx.vuRegs.ACC.w - (src1.w * bc_val);
            WriteWithMask(dest_reg, result, dest, protect_fd);
            UpdateMACFlags(ctx, result, dest);
            break;
        }
        
        // ====================================================================
        // Broadcast MAX variants (0x10-0x13)
        // ====================================================================
        case VU_UPPER::MAXx:
        case VU_UPPER::MAXy:
        case VU_UPPER::MAXz:
        case VU_UPPER::MAXw:
        {
            float bc_val = GetBroadcastValue(src2, bc);
            result.x = std::max(src1.x, bc_val);
            result.y = std::max(src1.y, bc_val);
            result.z = std::max(src1.z, bc_val);
            result.w = std::max(src1.w, bc_val);
            WriteWithMask(dest_reg, result, dest, protect_fd);
            // MAX doesn't update MAC flags
            break;
        }
        
        // ====================================================================
        // Broadcast MINI variants (0x14-0x17)
        // ====================================================================
        case VU_UPPER::MINIx:
        case VU_UPPER::MINIy:
        case VU_UPPER::MINIz:
        case VU_UPPER::MINIw:
        {
            float bc_val = GetBroadcastValue(src2, bc);
            result.x = std::min(src1.x, bc_val);
            result.y = std::min(src1.y, bc_val);
            result.z = std::min(src1.z, bc_val);
            result.w = std::min(src1.w, bc_val);
            WriteWithMask(dest_reg, result, dest, protect_fd);
            break;
        }
        
        // ====================================================================
        // Broadcast MUL variants (0x18-0x1B)
        // ====================================================================
        case VU_UPPER::MULx:
        case VU_UPPER::MULy:
        case VU_UPPER::MULz:
        case VU_UPPER::MULw:
        {
            float bc_val = GetBroadcastValue(src2, bc);
            result.x = src1.x * bc_val;
            result.y = src1.y * bc_val;
            result.z = src1.z * bc_val;
            result.w = src1.w * bc_val;
            WriteWithMask(dest_reg, result, dest, protect_fd);
            UpdateMACFlags(ctx, result, dest);
            break;
        }
        
        // ====================================================================
        // MULq: Multiply by Q register (0x1C)
        // ====================================================================
        case VU_UPPER::MULq:
        {
            float q = ctx.vuRegs.Q;
            result.x = src1.x * q;
            result.y = src1.y * q;
            result.z = src1.z * q;
            result.w = src1.w * q;
            WriteWithMask(dest_reg, result, dest, protect_fd);
            UpdateMACFlags(ctx, result, dest);
            break;
        }
        
        // ====================================================================
        // MAXi: Max with I register (0x1D)
        // ====================================================================
        case VU_UPPER::MAXi:
        {
            float i_val;
            std::memcpy(&i_val, &ctx.vuRegs.I, sizeof(i_val));
            result.x = std::max(src1.x, i_val);
            result.y = std::max(src1.y, i_val);
            result.z = std::max(src1.z, i_val);
            result.w = std::max(src1.w, i_val);
            WriteWithMask(dest_reg, result, dest, protect_fd);
            break;
        }
        
        // ====================================================================
        // MULi: Multiply by I register (0x1E)
        // ====================================================================
        case VU_UPPER::MULi:
        {
            float i_val;
            std::memcpy(&i_val, &ctx.vuRegs.I, sizeof(i_val));
            result.x = src1.x * i_val;
            result.y = src1.y * i_val;
            result.z = src1.z * i_val;
            result.w = src1.w * i_val;
            WriteWithMask(dest_reg, result, dest, protect_fd);
            UpdateMACFlags(ctx, result, dest);
            break;
        }
        
        // ====================================================================
        // MINIi: Min with I register (0x1F)
        // ====================================================================
        case VU_UPPER::MINIi:
        {
            float i_val;
            std::memcpy(&i_val, &ctx.vuRegs.I, sizeof(i_val));
            result.x = std::min(src1.x, i_val);
            result.y = std::min(src1.y, i_val);
            result.z = std::min(src1.z, i_val);
            result.w = std::min(src1.w, i_val);
            WriteWithMask(dest_reg, result, dest, protect_fd);
            break;
        }
        
        // ====================================================================
        // ADDq: Add Q register (0x20)
        // ====================================================================
        case VU_UPPER::ADDq:
        {
            float q = ctx.vuRegs.Q;
            result.x = src1.x + q;
            result.y = src1.y + q;
            result.z = src1.z + q;
            result.w = src1.w + q;
            WriteWithMask(dest_reg, result, dest, protect_fd);
            UpdateMACFlags(ctx, result, dest);
            break;
        }
        
        // ====================================================================
        // MADDq: Multiply-add with Q (0x21)
        // ====================================================================
        case VU_UPPER::MADDq:
        {
            float q = ctx.vuRegs.Q;
            result.x = ctx.vuRegs.ACC.x + (src1.x * q);
            result.y = ctx.vuRegs.ACC.y + (src1.y * q);
            result.z = ctx.vuRegs.ACC.z + (src1.z * q);
            result.w = ctx.vuRegs.ACC.w + (src1.w * q);
            WriteWithMask(dest_reg, result, dest, protect_fd);
            UpdateMACFlags(ctx, result, dest);
            break;
        }
        
        // ====================================================================
        // ADDi: Add I register (0x22)
        // ====================================================================
        case VU_UPPER::ADDi:
        {
            float i_val;
            std::memcpy(&i_val, &ctx.vuRegs.I, sizeof(i_val));
            result.x = src1.x + i_val;
            result.y = src1.y + i_val;
            result.z = src1.z + i_val;
            result.w = src1.w + i_val;
            WriteWithMask(dest_reg, result, dest, protect_fd);
            UpdateMACFlags(ctx, result, dest);
            break;
        }
        
        // ====================================================================
        // MADDi: Multiply-add with I (0x23)
        // ====================================================================
        case VU_UPPER::MADDi:
        {
            float i_val;
            std::memcpy(&i_val, &ctx.vuRegs.I, sizeof(i_val));
            result.x = ctx.vuRegs.ACC.x + (src1.x * i_val);
            result.y = ctx.vuRegs.ACC.y + (src1.y * i_val);
            result.z = ctx.vuRegs.ACC.z + (src1.z * i_val);
            result.w = ctx.vuRegs.ACC.w + (src1.w * i_val);
            WriteWithMask(dest_reg, result, dest, protect_fd);
            UpdateMACFlags(ctx, result, dest);
            break;
        }
        
        // ====================================================================
        // SUBq: Subtract Q (0x24)
        // ====================================================================
        case VU_UPPER::SUBq:
        {
            float q = ctx.vuRegs.Q;
            result.x = src1.x - q;
            result.y = src1.y - q;
            result.z = src1.z - q;
            result.w = src1.w - q;
            WriteWithMask(dest_reg, result, dest, protect_fd);
            UpdateMACFlags(ctx, result, dest);
            break;
        }
        
        // ====================================================================
        // MSUBq: Multiply-subtract with Q (0x25)
        // ====================================================================
        case VU_UPPER::MSUBq:
        {
            float q = ctx.vuRegs.Q;
            result.x = ctx.vuRegs.ACC.x - (src1.x * q);
            result.y = ctx.vuRegs.ACC.y - (src1.y * q);
            result.z = ctx.vuRegs.ACC.z - (src1.z * q);
            result.w = ctx.vuRegs.ACC.w - (src1.w * q);
            WriteWithMask(dest_reg, result, dest, protect_fd);
            UpdateMACFlags(ctx, result, dest);
            break;
        }
        
        // ====================================================================
        // SUBi: Subtract I (0x26)
        // ====================================================================
        case VU_UPPER::SUBi:
        {
            float i_val;
            std::memcpy(&i_val, &ctx.vuRegs.I, sizeof(i_val));
            result.x = src1.x - i_val;
            result.y = src1.y - i_val;
            result.z = src1.z - i_val;
            result.w = src1.w - i_val;
            WriteWithMask(dest_reg, result, dest, protect_fd);
            UpdateMACFlags(ctx, result, dest);
            break;
        }
        
        // ====================================================================
        // MSUBi: Multiply-subtract with I (0x27)
        // ====================================================================
        case VU_UPPER::MSUBi:
        {
            float i_val;
            std::memcpy(&i_val, &ctx.vuRegs.I, sizeof(i_val));
            result.x = ctx.vuRegs.ACC.x - (src1.x * i_val);
            result.y = ctx.vuRegs.ACC.y - (src1.y * i_val);
            result.z = ctx.vuRegs.ACC.z - (src1.z * i_val);
            result.w = ctx.vuRegs.ACC.w - (src1.w * i_val);
            WriteWithMask(dest_reg, result, dest, protect_fd);
            UpdateMACFlags(ctx, result, dest);
            break;
        }
        
        // ====================================================================
        // ADD: Vector add (0x28)
        // ====================================================================
        case VU_UPPER::ADD:
        {
            result.x = src1.x + src2.x;
            result.y = src1.y + src2.y;
            result.z = src1.z + src2.z;
            result.w = src1.w + src2.w;
            WriteWithMask(dest_reg, result, dest, protect_fd);
            UpdateMACFlags(ctx, result, dest);
            break;
        }
        
        // ====================================================================
        // MADD: Multiply-add (0x29)
        // ====================================================================
        case VU_UPPER::MADD:
        {
            result.x = ctx.vuRegs.ACC.x + (src1.x * src2.x);
            result.y = ctx.vuRegs.ACC.y + (src1.y * src2.y);
            result.z = ctx.vuRegs.ACC.z + (src1.z * src2.z);
            result.w = ctx.vuRegs.ACC.w + (src1.w * src2.w);
            WriteWithMask(dest_reg, result, dest, protect_fd);
            UpdateMACFlags(ctx, result, dest);
            break;
        }
        
        // ====================================================================
        // MUL: Vector multiply (0x2A)
        // ====================================================================
        case VU_UPPER::MUL:
        {
            result.x = src1.x * src2.x;
            result.y = src1.y * src2.y;
            result.z = src1.z * src2.z;
            result.w = src1.w * src2.w;
            WriteWithMask(dest_reg, result, dest, protect_fd);
            UpdateMACFlags(ctx, result, dest);
            break;
        }
        
        // ====================================================================
        // MAX: Vector max (0x2B)
        // ====================================================================
        case VU_UPPER::MAX:
        {
            result.x = std::max(src1.x, src2.x);
            result.y = std::max(src1.y, src2.y);
            result.z = std::max(src1.z, src2.z);
            result.w = std::max(src1.w, src2.w);
            WriteWithMask(dest_reg, result, dest, protect_fd);
            break;
        }
        
        // ====================================================================
        // SUB: Vector subtract (0x2C)
        // ====================================================================
        case VU_UPPER::SUB:
        {
            result.x = src1.x - src2.x;
            result.y = src1.y - src2.y;
            result.z = src1.z - src2.z;
            result.w = src1.w - src2.w;
            WriteWithMask(dest_reg, result, dest, protect_fd);
            UpdateMACFlags(ctx, result, dest);
            break;
        }
        
        // ====================================================================
        // MSUB: Multiply-subtract (0x2D)
        // ====================================================================
        case VU_UPPER::MSUB:
        {
            result.x = ctx.vuRegs.ACC.x - (src1.x * src2.x);
            result.y = ctx.vuRegs.ACC.y - (src1.y * src2.y);
            result.z = ctx.vuRegs.ACC.z - (src1.z * src2.z);
            result.w = ctx.vuRegs.ACC.w - (src1.w * src2.w);
            WriteWithMask(dest_reg, result, dest, protect_fd);
            UpdateMACFlags(ctx, result, dest);
            break;
        }
        
        // ====================================================================
        // OPMSUB: Outer product multiply-subtract (0x2E)
        // Result = ACC - (fs x ft) where x is cross product
        // ====================================================================
        case VU_UPPER::OPMSUB:
        {
            result.x = ctx.vuRegs.ACC.x - (src1.y * src2.z);
            result.y = ctx.vuRegs.ACC.y - (src1.z * src2.x);
            result.z = ctx.vuRegs.ACC.z - (src1.x * src2.y);
            result.w = ctx.vuRegs.ACC.w;  // W unchanged
            WriteWithMask(dest_reg, result, dest, protect_fd);
            UpdateMACFlags(ctx, result, dest);
            break;
        }
        
        // ====================================================================
        // MINI: Vector min (0x2F)
        // ====================================================================
        case VU_UPPER::MINI:
        {
            result.x = std::min(src1.x, src2.x);
            result.y = std::min(src1.y, src2.y);
            result.z = std::min(src1.z, src2.z);
            result.w = std::min(src1.w, src2.w);
            WriteWithMask(dest_reg, result, dest, protect_fd);
            break;
        }
        
        default:
            // Unknown or NOP
            if (instr != 0 && g_logFile.is_open()) {
                g_logFile << "[VU1 Upper] Unknown func 0x" << std::hex << (int)func 
                          << " instr=0x" << instr << std::dec << std::endl;
            }
            break;
    }
    
    UpdateStatusFlags(ctx);
}

// ============================================================================
// Special2 Upper Instructions (FDIV unit, conversions, etc.)
// ============================================================================
void VU1::RunSpecial2Upper(CpuContext& ctx, uint32_t instr) {
    // Special2 encoding: bits 0-1 combined with bits 6-10 give the operation
    uint8_t func_lo = instr & 0x03;
    uint8_t func_hi = (instr >> 6) & 0x1F;
    uint8_t special2_op = (func_hi << 2) | func_lo;
    
    uint8_t fd   = (instr >> 6) & 0x1F;
    uint8_t fs   = (instr >> 11) & 0x1F;
    uint8_t ft   = (instr >> 16) & 0x1F;
    uint8_t dest = (instr >> 21) & 0x0F;
    uint8_t fsf  = (instr >> 21) & 0x03;  // Source field selector for fs
    uint8_t ftf  = (instr >> 23) & 0x03;  // Source field selector for ft
    
    VR_reg& dest_reg = ctx.vuRegs.VF[fd];
    const VR_reg& src1 = ctx.vuRegs.VF[fs];
    const VR_reg& src2 = ctx.vuRegs.VF[ft];
    
    bool protect_fd = (fd == 0);
    VR_reg result = {};
    
    // Get field value based on selector
    auto GetField = [](const VR_reg& v, int sel) -> float {
        switch (sel) {
            case 0: return v.x;
            case 1: return v.y;
            case 2: return v.z;
            case 3: return v.w;
            default: return v.x;
        }
    };
    
    // Determine operation based on func pattern
    uint8_t base_func = (instr & 0x3F);
    
    switch (base_func) {
        // ====================================================================
        // ADDAx/y/z/w: ACC = fs + ft.bc (0x30-0x33)
        // ====================================================================
        case 0x30: case 0x31: case 0x32: case 0x33:
        {
            VU_BC bc = static_cast<VU_BC>(base_func & 0x03);
            float bc_val = GetBroadcastValue(src2, bc);
            ctx.vuRegs.ACC.x = src1.x + bc_val;
            ctx.vuRegs.ACC.y = src1.y + bc_val;
            ctx.vuRegs.ACC.z = src1.z + bc_val;
            ctx.vuRegs.ACC.w = src1.w + bc_val;
            UpdateMACFlags(ctx, ctx.vuRegs.ACC, dest);
            break;
        }
        
        // ====================================================================
        // SUBAx/y/z/w: ACC = fs - ft.bc (0x34-0x37)
        // ====================================================================
        case 0x34: case 0x35: case 0x36: case 0x37:
        {
            VU_BC bc = static_cast<VU_BC>(base_func & 0x03);
            float bc_val = GetBroadcastValue(src2, bc);
            ctx.vuRegs.ACC.x = src1.x - bc_val;
            ctx.vuRegs.ACC.y = src1.y - bc_val;
            ctx.vuRegs.ACC.z = src1.z - bc_val;
            ctx.vuRegs.ACC.w = src1.w - bc_val;
            UpdateMACFlags(ctx, ctx.vuRegs.ACC, dest);
            break;
        }
        
        // ====================================================================
        // MADDAx/y/z/w: ACC = ACC + fs * ft.bc (0x38-0x3B)
        // ====================================================================
        case 0x38: case 0x39: case 0x3A: case 0x3B:
        {
            VU_BC bc = static_cast<VU_BC>(base_func & 0x03);
            float bc_val = GetBroadcastValue(src2, bc);
            ctx.vuRegs.ACC.x = ctx.vuRegs.ACC.x + (src1.x * bc_val);
            ctx.vuRegs.ACC.y = ctx.vuRegs.ACC.y + (src1.y * bc_val);
            ctx.vuRegs.ACC.z = ctx.vuRegs.ACC.z + (src1.z * bc_val);
            ctx.vuRegs.ACC.w = ctx.vuRegs.ACC.w + (src1.w * bc_val);
            UpdateMACFlags(ctx, ctx.vuRegs.ACC, dest);
            break;
        }
        
        // ====================================================================
        // MULAx/y/z/w: ACC = fs * ft.bc
        // ====================================================================
        case 0x3C: case 0x3D: case 0x3E: case 0x3F:
        {
            // But 0x3C-0x3F also used for other ops, check special2_op
            break;
        }
        
        default:
            break;
    }
    
    // Handle the inner Special2 table operations
    // These use a different decoding based on bits 2-5 when bits 0-1 = 11
    if ((base_func & 0x3C) == 0x3C) {
        uint8_t inner_op = ((instr >> 6) & 0x1F) | ((instr & 0x03) << 5);
        
        // Decode inner special2 operations
        uint8_t op_code = (instr >> 6) & 0x1F;
        uint8_t bc_or_flag = instr & 0x03;
        
        switch (op_code) {
            // ITOF0-ITOF15, FTOI0-FTOI15
            case 0x04: // ITOF0
            {
                if (!protect_fd) {
                    if (dest & VU_DEST::X) dest_reg.x = static_cast<float>(static_cast<int32_t>(src2.I[0]));
                    if (dest & VU_DEST::Y) dest_reg.y = static_cast<float>(static_cast<int32_t>(src2.I[1]));
                    if (dest & VU_DEST::Z) dest_reg.z = static_cast<float>(static_cast<int32_t>(src2.I[2]));
                    if (dest & VU_DEST::W) dest_reg.w = static_cast<float>(static_cast<int32_t>(src2.I[3]));
                }
                break;
            }
            
            case 0x05: // ITOF4 (divide by 16)
            {
                if (!protect_fd) {
                    if (dest & VU_DEST::X) dest_reg.x = static_cast<float>(static_cast<int32_t>(src2.I[0])) / 16.0f;
                    if (dest & VU_DEST::Y) dest_reg.y = static_cast<float>(static_cast<int32_t>(src2.I[1])) / 16.0f;
                    if (dest & VU_DEST::Z) dest_reg.z = static_cast<float>(static_cast<int32_t>(src2.I[2])) / 16.0f;
                    if (dest & VU_DEST::W) dest_reg.w = static_cast<float>(static_cast<int32_t>(src2.I[3])) / 16.0f;
                }
                break;
            }
            
            case 0x06: // ITOF12 (divide by 4096)
            {
                if (!protect_fd) {
                    if (dest & VU_DEST::X) dest_reg.x = static_cast<float>(static_cast<int32_t>(src2.I[0])) / 4096.0f;
                    if (dest & VU_DEST::Y) dest_reg.y = static_cast<float>(static_cast<int32_t>(src2.I[1])) / 4096.0f;
                    if (dest & VU_DEST::Z) dest_reg.z = static_cast<float>(static_cast<int32_t>(src2.I[2])) / 4096.0f;
                    if (dest & VU_DEST::W) dest_reg.w = static_cast<float>(static_cast<int32_t>(src2.I[3])) / 4096.0f;
                }
                break;
            }
            
            case 0x07: // ITOF15 (divide by 32768)
            {
                if (!protect_fd) {
                    if (dest & VU_DEST::X) dest_reg.x = static_cast<float>(static_cast<int32_t>(src2.I[0])) / 32768.0f;
                    if (dest & VU_DEST::Y) dest_reg.y = static_cast<float>(static_cast<int32_t>(src2.I[1])) / 32768.0f;
                    if (dest & VU_DEST::Z) dest_reg.z = static_cast<float>(static_cast<int32_t>(src2.I[2])) / 32768.0f;
                    if (dest & VU_DEST::W) dest_reg.w = static_cast<float>(static_cast<int32_t>(src2.I[3])) / 32768.0f;
                }
                break;
            }
            
            case 0x08: // FTOI0
            {
                if (!protect_fd) {
                    if (dest & VU_DEST::X) dest_reg.I[0] = static_cast<int32_t>(src2.x);
                    if (dest & VU_DEST::Y) dest_reg.I[1] = static_cast<int32_t>(src2.y);
                    if (dest & VU_DEST::Z) dest_reg.I[2] = static_cast<int32_t>(src2.z);
                    if (dest & VU_DEST::W) dest_reg.I[3] = static_cast<int32_t>(src2.w);
                }
                break;
            }
            
            case 0x09: // FTOI4 (multiply by 16)
            {
                if (!protect_fd) {
                    if (dest & VU_DEST::X) dest_reg.I[0] = static_cast<int32_t>(src2.x * 16.0f);
                    if (dest & VU_DEST::Y) dest_reg.I[1] = static_cast<int32_t>(src2.y * 16.0f);
                    if (dest & VU_DEST::Z) dest_reg.I[2] = static_cast<int32_t>(src2.z * 16.0f);
                    if (dest & VU_DEST::W) dest_reg.I[3] = static_cast<int32_t>(src2.w * 16.0f);
                }
                break;
            }
            
            case 0x0A: // FTOI12
            {
                if (!protect_fd) {
                    if (dest & VU_DEST::X) dest_reg.I[0] = static_cast<int32_t>(src2.x * 4096.0f);
                    if (dest & VU_DEST::Y) dest_reg.I[1] = static_cast<int32_t>(src2.y * 4096.0f);
                    if (dest & VU_DEST::Z) dest_reg.I[2] = static_cast<int32_t>(src2.z * 4096.0f);
                    if (dest & VU_DEST::W) dest_reg.I[3] = static_cast<int32_t>(src2.w * 4096.0f);
                }
                break;
            }
            
            case 0x0B: // FTOI15
            {
                if (!protect_fd) {
                    if (dest & VU_DEST::X) dest_reg.I[0] = static_cast<int32_t>(src2.x * 32768.0f);
                    if (dest & VU_DEST::Y) dest_reg.I[1] = static_cast<int32_t>(src2.y * 32768.0f);
                    if (dest & VU_DEST::Z) dest_reg.I[2] = static_cast<int32_t>(src2.z * 32768.0f);
                    if (dest & VU_DEST::W) dest_reg.I[3] = static_cast<int32_t>(src2.w * 32768.0f);
                }
                break;
            }
            
            case 0x0C: // MULAx/y/z/w
            {
                VU_BC bc = static_cast<VU_BC>(bc_or_flag);
                float bc_val = GetBroadcastValue(src2, bc);
                ctx.vuRegs.ACC.x = src1.x * bc_val;
                ctx.vuRegs.ACC.y = src1.y * bc_val;
                ctx.vuRegs.ACC.z = src1.z * bc_val;
                ctx.vuRegs.ACC.w = src1.w * bc_val;
                UpdateMACFlags(ctx, ctx.vuRegs.ACC, dest);
                break;
            }
            
            case 0x0D: // MULAq
            {
                ctx.vuRegs.ACC.x = src1.x * ctx.vuRegs.Q;
                ctx.vuRegs.ACC.y = src1.y * ctx.vuRegs.Q;
                ctx.vuRegs.ACC.z = src1.z * ctx.vuRegs.Q;
                ctx.vuRegs.ACC.w = src1.w * ctx.vuRegs.Q;
                break;
            }
            
            case 0x0E: // ABS
            {
                if (!protect_fd) {
                    if (dest & VU_DEST::X) dest_reg.x = std::abs(src2.x);
                    if (dest & VU_DEST::Y) dest_reg.y = std::abs(src2.y);
                    if (dest & VU_DEST::Z) dest_reg.z = std::abs(src2.z);
                    if (dest & VU_DEST::W) dest_reg.w = std::abs(src2.w);
                }
                break;
            }
            
            case 0x0F: // CLIP
            {
                // CLIP: Compare xyz of fs against +/- w of ft
                // Shift old flags left by 6, add new 6 flags
                float w = std::abs(src2.w);
                uint32_t new_flags = 0;
                
                if (src1.x >  w) new_flags |= 0x01;  // +X
                if (src1.x < -w) new_flags |= 0x02;  // -X
                if (src1.y >  w) new_flags |= 0x04;  // +Y
                if (src1.y < -w) new_flags |= 0x08;  // -Y
                if (src1.z >  w) new_flags |= 0x10;  // +Z
                if (src1.z < -w) new_flags |= 0x20;  // -Z
                
                // Shift and combine (keep 24 bits)
                ctx.vuRegs.clip_flag = ((ctx.vuRegs.clip_flag << 6) | new_flags) & 0xFFFFFF;
                break;
            }
            
            // ADDA, SUBA, MULA, OPMULA
            case 0x10: // ADDA
            {
                ctx.vuRegs.ACC.x = src1.x + src2.x;
                ctx.vuRegs.ACC.y = src1.y + src2.y;
                ctx.vuRegs.ACC.z = src1.z + src2.z;
                ctx.vuRegs.ACC.w = src1.w + src2.w;
                UpdateMACFlags(ctx, ctx.vuRegs.ACC, dest);
                break;
            }
            
            case 0x11: // MADDA
            {
                ctx.vuRegs.ACC.x += src1.x * src2.x;
                ctx.vuRegs.ACC.y += src1.y * src2.y;
                ctx.vuRegs.ACC.z += src1.z * src2.z;
                ctx.vuRegs.ACC.w += src1.w * src2.w;
                UpdateMACFlags(ctx, ctx.vuRegs.ACC, dest);
                break;
            }
            
            case 0x12: // MULA
            {
                ctx.vuRegs.ACC.x = src1.x * src2.x;
                ctx.vuRegs.ACC.y = src1.y * src2.y;
                ctx.vuRegs.ACC.z = src1.z * src2.z;
                ctx.vuRegs.ACC.w = src1.w * src2.w;
                UpdateMACFlags(ctx, ctx.vuRegs.ACC, dest);
                break;
            }
            
            case 0x14: // SUBA
            {
                ctx.vuRegs.ACC.x = src1.x - src2.x;
                ctx.vuRegs.ACC.y = src1.y - src2.y;
                ctx.vuRegs.ACC.z = src1.z - src2.z;
                ctx.vuRegs.ACC.w = src1.w - src2.w;
                UpdateMACFlags(ctx, ctx.vuRegs.ACC, dest);
                break;
            }
            
            case 0x15: // MSUBA
            {
                ctx.vuRegs.ACC.x -= src1.x * src2.x;
                ctx.vuRegs.ACC.y -= src1.y * src2.y;
                ctx.vuRegs.ACC.z -= src1.z * src2.z;
                ctx.vuRegs.ACC.w -= src1.w * src2.w;
                UpdateMACFlags(ctx, ctx.vuRegs.ACC, dest);
                break;
            }
            
            case 0x16: // OPMULA: ACC = fs x ft (cross product to ACC)
            {
                ctx.vuRegs.ACC.x = src1.y * src2.z;
                ctx.vuRegs.ACC.y = src1.z * src2.x;
                ctx.vuRegs.ACC.z = src1.x * src2.y;
                UpdateMACFlags(ctx, ctx.vuRegs.ACC, dest);
                break;
            }
            
            case 0x17: // NOP
                break;
            
            case 0x18: // MOVE
            {
                if (!protect_fd) {
                    if (dest & VU_DEST::X) dest_reg.x = src2.x;
                    if (dest & VU_DEST::Y) dest_reg.y = src2.y;
                    if (dest & VU_DEST::Z) dest_reg.z = src2.z;
                    if (dest & VU_DEST::W) dest_reg.w = src2.w;
                }
                break;
            }
            
            case 0x19: // MR32: Rotate right by 32 bits (shift components)
            {
                if (!protect_fd) {
                    VR_reg temp = src2;
                    if (dest & VU_DEST::X) dest_reg.x = temp.y;
                    if (dest & VU_DEST::Y) dest_reg.y = temp.z;
                    if (dest & VU_DEST::Z) dest_reg.z = temp.w;
                    if (dest & VU_DEST::W) dest_reg.w = temp.x;
                }
                break;
            }
            
            case 0x1C: // DIV: Q = fs.fsf / ft.ftf
            {
                float num = GetField(src1, fsf);
                float den = GetField(src2, ftf);
                
                if (den == 0.0f) {
                    if (num == 0.0f) {
                        // 0/0 = invalid
                        ctx.vuRegs.status_flag |= STATUS_FLAG::I | STATUS_FLAG::IS;
                        ctx.vuRegs.Q = 0.0f;
                    } else {
                        // x/0 = infinity (set D flag)
                        ctx.vuRegs.status_flag |= STATUS_FLAG::D | STATUS_FLAG::DS;
                        // Return max float with appropriate sign
                        uint32_t sign = (std::signbit(num) != std::signbit(den)) ? 0x80000000 : 0;
                        uint32_t max_val = sign | 0x7F7FFFFF;
                        std::memcpy(&ctx.vuRegs.Q, &max_val, sizeof(float));
                    }
                } else {
                    ctx.vuRegs.Q = num / den;
                }
                break;
            }
            
            case 0x1D: // SQRT: Q = sqrt(ft.ftf)
            {
                float val = GetField(src2, ftf);
                if (val < 0.0f) {
                    ctx.vuRegs.status_flag |= STATUS_FLAG::I | STATUS_FLAG::IS;
                    ctx.vuRegs.Q = std::sqrt(std::abs(val));
                } else {
                    ctx.vuRegs.Q = std::sqrt(val);
                }
                break;
            }
            
            case 0x1E: // RSQRT: Q = fs.fsf / sqrt(ft.ftf)
            {
                float num = GetField(src1, fsf);
                float den_sq = GetField(src2, ftf);
                
                if (den_sq <= 0.0f) {
                    if (den_sq < 0.0f) {
                        ctx.vuRegs.status_flag |= STATUS_FLAG::I | STATUS_FLAG::IS;
                    }
                    if (den_sq == 0.0f) {
                        ctx.vuRegs.status_flag |= STATUS_FLAG::D | STATUS_FLAG::DS;
                    }
                    ctx.vuRegs.Q = 0.0f;
                } else {
                    ctx.vuRegs.Q = num / std::sqrt(den_sq);
                }
                break;
            }
            
            case 0x1F: // WAITQ - wait for Q register (NOP in interpreter)
                break;
            
            default:
                if (g_logFile.is_open()) {
                    g_logFile << "[VU1] Unknown Special2 op_code=0x" << std::hex << (int)op_code << std::dec << std::endl;
                }
                break;
        }
    }
}

// ============================================================================
// Lower Instruction Execution
// ============================================================================
void VU1::RunLower(CpuContext& ctx, uint32_t instr, uint32_t& pc, bool& branch_taken, uint32_t& branch_target) {
    g_logFile << "[VU1 Lower] Executing instr=0x" << std::hex << instr << std::dec << std::endl;
    uint8_t opcode = (instr >> 25) & 0x7F;
    
    // Extract common fields
    uint8_t ft = (instr >> 16) & 0x1F;
    uint8_t fs = (instr >> 11) & 0x1F;
    uint8_t fd = (instr >> 6) & 0x1F;
    uint8_t dest = (instr >> 21) & 0x0F;
    int16_t imm11 = instr & 0x7FF;
    int16_t imm15 = instr & 0x7FFF;
    
    // Sign-extend 11-bit immediate
    if (imm11 & 0x400) imm11 |= 0xF800;
    
    // Sign-extend 15-bit immediate  
    if (imm15 & 0x4000) imm15 |= 0x8000;
    
    branch_taken = false;
    
    // Check for XGKICK first (special opcode)
    if (opcode == 0x36 && ((instr >> 6) & 0x1F) == 0x1A) {
        if (vu_id == 1) { // Only valid on VU1
            g_logFile << "[VU1] XGKICK invoked with addr=0x" << std::hex << (ctx.vuRegs.VI[fs & 0xF]) << std::dec << std::endl;
            uint16_t vi_addr = ctx.vuRegs.VI[fs & 0xF];
            Op_XGKICK(ctx, vi_addr);
        } else {
            if (g_logFile.is_open()) {
                g_logFile << "[VU0] Warning: Attempted XGKICK (Invalid)" << std::endl;
            }
        }
        return;
    }
    
    switch (opcode) {
        // ====================================================================
        // LQ: Load Quadword (0x00)
        // fd = Mem[VI[fs] + imm11]
        // ====================================================================
        case 0x00:
        {
            int16_t offset = imm11;
            uint32_t addr = (ctx.vuRegs.VI[fs & 0xF] + offset) * 16;
            addr &= 0x3FFF;
            
            if (ft != 0) {
                std::memcpy(&ctx.vuRegs.VF[ft], &vu1_data_memory[addr], 16);
            }
            break;
        }
        
        // ====================================================================
        // SQ: Store Quadword (0x01)
        // Mem[VI[ft] + imm11] = fs
        // ====================================================================
        case 0x01:
        {
            int16_t offset = imm11;
            uint32_t addr = (ctx.vuRegs.VI[ft & 0xF] + offset) * 16;
            addr &= 0x3FFF;
            
            std::memcpy(&vu1_data_memory[addr], &ctx.vuRegs.VF[fs], 16);
            break;
        }
        
        // ====================================================================
        // ILW: Integer Load Word (0x04)
        // ====================================================================
        case 0x04:
        {
            int16_t offset = imm11;
            uint32_t addr = (ctx.vuRegs.VI[fs & 0xF] + offset) * 16;
            addr &= 0x3FFF;
            
            // Load based on dest field (which component)
            uint32_t word;
            if (dest & VU_DEST::X) {
                std::memcpy(&word, &vu1_data_memory[addr], 4);
            } else if (dest & VU_DEST::Y) {
                std::memcpy(&word, &vu1_data_memory[addr + 4], 4);
            } else if (dest & VU_DEST::Z) {
                std::memcpy(&word, &vu1_data_memory[addr + 8], 4);
            } else {
                std::memcpy(&word, &vu1_data_memory[addr + 12], 4);
            }
            
            if (ft != 0) {
                ctx.vuRegs.VI[ft & 0xF] = static_cast<uint16_t>(word);
            }
            break;
        }
        
        // ====================================================================
        // ISW: Integer Store Word (0x05)
        // ====================================================================
        case 0x05:
        {
            int16_t offset = imm11;
            uint32_t addr = (ctx.vuRegs.VI[fs & 0xF] + offset) * 16;
            addr &= 0x3FFF;
            
            uint32_t word = ctx.vuRegs.VI[ft & 0xF];
            
            // Store to all selected components
            if (dest & VU_DEST::X) std::memcpy(&vu1_data_memory[addr], &word, 4);
            if (dest & VU_DEST::Y) std::memcpy(&vu1_data_memory[addr + 4], &word, 4);
            if (dest & VU_DEST::Z) std::memcpy(&vu1_data_memory[addr + 8], &word, 4);
            if (dest & VU_DEST::W) std::memcpy(&vu1_data_memory[addr + 12], &word, 4);
            break;
        }
        
        // ====================================================================
        // IADDIU: Integer Add Immediate Unsigned (0x08)
        // ====================================================================
        case 0x08:
        {
            uint16_t imm = imm15 & 0x7FFF;
            if (ft != 0) {
                ctx.vuRegs.VI[ft & 0xF] = ctx.vuRegs.VI[fs & 0xF] + imm;
            }
            break;
        }
        
        // ====================================================================
        // ISUBIU: Integer Sub Immediate Unsigned (0x09)
        // ====================================================================
        case 0x09:
        {
            uint16_t imm = imm15 & 0x7FFF;
            if (ft != 0) {
                ctx.vuRegs.VI[ft & 0xF] = ctx.vuRegs.VI[fs & 0xF] - imm;
            }
            break;
        }
        
        // ====================================================================
        // FCEQ: If clip_flag == imm24, VI01 = 1 (0x10)
        // ====================================================================
        case 0x10:
        {
            uint32_t imm24 = instr & 0xFFFFFF;
            ctx.vuRegs.VI[1] = (ctx.vuRegs.clip_flag == imm24) ? 1 : 0;
            break;
        }
        
        // ====================================================================
        // FCSET: Set clip flags (0x11)
        // ====================================================================
        case 0x11:
        {
            ctx.vuRegs.clip_flag = instr & 0xFFFFFF;
            break;
        }
        
        // ====================================================================
        // FCAND: VI01 = (clip_flag & imm24) != 0 (0x12)
        // ====================================================================
        case 0x12:
        {
            uint32_t imm24 = instr & 0xFFFFFF;
            ctx.vuRegs.VI[1] = ((ctx.vuRegs.clip_flag & imm24) != 0) ? 1 : 0;
            break;
        }
        
        // ====================================================================
        // FCOR: VI01 = (clip_flag | imm24) == 0xFFFFFF (0x13)
        // ====================================================================
        case 0x13:
        {
            uint32_t imm24 = instr & 0xFFFFFF;
            ctx.vuRegs.VI[1] = ((ctx.vuRegs.clip_flag | imm24) == 0xFFFFFF) ? 1 : 0;
            break;
        }
        
        // ====================================================================
        // FSEQ: If status_flag == imm12, VI01 = 1 (0x14)
        // ====================================================================
        case 0x14:
        {
            uint16_t imm12 = instr & 0xFFF;
            ctx.vuRegs.VI[1] = (ctx.vuRegs.status_flag == imm12) ? 1 : 0;
            break;
        }
        
        // ====================================================================
        // FSSET: Set status flags (sticky only) (0x15)
        // ====================================================================
        case 0x15:
        {
            uint16_t imm12 = instr & 0xFFF;
            // Only sticky flags (bits 6-11) can be set
            ctx.vuRegs.status_flag = (ctx.vuRegs.status_flag & 0x3F) | (imm12 & 0xFC0);
            break;
        }
        
        // ====================================================================
        // FSAND: VI01 = status_flag & imm12 (0x16)
        // ====================================================================
        case 0x16:
        {
            uint16_t imm12 = instr & 0xFFF;
            ctx.vuRegs.VI[1] = ctx.vuRegs.status_flag & imm12;
            break;
        }
        
        // ====================================================================
        // FSOR: VI01 = status_flag | imm12 (0x17)
        // ====================================================================
        case 0x17:
        {
            uint16_t imm12 = instr & 0xFFF;
            ctx.vuRegs.VI[1] = ctx.vuRegs.status_flag | imm12;
            break;
        }
        
        // ====================================================================
        // FMEQ: If mac_flag == VI[fs], VI01 = 1 (0x18)
        // ====================================================================
        case 0x18:
        {
            ctx.vuRegs.VI[1] = (ctx.vuRegs.mac_flag == ctx.vuRegs.VI[fs & 0xF]) ? 1 : 0;
            break;
        }
        
        // ====================================================================
        // FMAND: VI01 = mac_flag & VI[fs] (0x1A)
        // ====================================================================
        case 0x1A:
        {
            ctx.vuRegs.VI[1] = ctx.vuRegs.mac_flag & ctx.vuRegs.VI[fs & 0xF];
            break;
        }
        
        // ====================================================================
        // FMOR: VI01 = mac_flag | VI[fs] (0x1B)
        // ====================================================================
        case 0x1B:
        {
            ctx.vuRegs.VI[1] = ctx.vuRegs.mac_flag | ctx.vuRegs.VI[fs & 0xF];
            break;
        }
        
        // ====================================================================
        // FCGET: VI[ft] = clip_flag & 0xFFF (0x1C)
        // ====================================================================
        case 0x1C:
        {
            if (ft != 0) {
                ctx.vuRegs.VI[ft & 0xF] = ctx.vuRegs.clip_flag & 0xFFF;
            }
            break;
        }
        
        // ====================================================================
        // Integer arithmetic (0x20-0x22, 0x28-0x29)
        // ====================================================================
        case 0x20: // IADD
        {
            if (fd != 0) {
                ctx.vuRegs.VI[fd & 0xF] = ctx.vuRegs.VI[fs & 0xF] + ctx.vuRegs.VI[ft & 0xF];
            }
            break;
        }
        
        case 0x21: // ISUB
        {
            if (fd != 0) {
                ctx.vuRegs.VI[fd & 0xF] = ctx.vuRegs.VI[fs & 0xF] - ctx.vuRegs.VI[ft & 0xF];
            }
            break;
        }
        
        case 0x22: // IADDI
        {
            int8_t imm5 = (instr >> 6) & 0x1F;
            if (imm5 & 0x10) imm5 |= 0xE0;  // Sign extend
            
            if (ft != 0) {
                ctx.vuRegs.VI[ft & 0xF] = ctx.vuRegs.VI[fs & 0xF] + imm5;
            }
            break;
        }
        
        case 0x28: // IAND
        {
            if (fd != 0) {
                ctx.vuRegs.VI[fd & 0xF] = ctx.vuRegs.VI[fs & 0xF] & ctx.vuRegs.VI[ft & 0xF];
            }
            break;
        }
        
        case 0x29: // IOR
        {
            if (fd != 0) {
                ctx.vuRegs.VI[fd & 0xF] = ctx.vuRegs.VI[fs & 0xF] | ctx.vuRegs.VI[ft & 0xF];
            }
            break;
        }
        
        // ====================================================================
        // Branches (0x24-0x2F)
        // ====================================================================
        case 0x24: // JR: Jump Register
        {
            branch_taken = true;
            branch_target = (ctx.vuRegs.VI[fs & 0xF] * 8) & 0x3FFF;
            break;
        }
        
        case 0x25: // JALR: Jump And Link Register
        {
            if (ft != 0) {
                ctx.vuRegs.VI[ft & 0xF] = ((pc + 16) / 8) & 0x3FF;  // Return address in words
            }
            branch_taken = true;
            branch_target = (ctx.vuRegs.VI[fs & 0xF] * 8) & 0x3FFF;
            break;
        }
        
        case 0x30: // IBEQ: Branch if equal
        {
            if (ctx.vuRegs.VI[fs & 0xF] == ctx.vuRegs.VI[ft & 0xF]) {
                branch_taken = true;
                branch_target = (pc + 8 + imm11 * 8) & 0x3FFF;
            }
            break;
        }
        
        case 0x31: // IBNE: Branch if not equal
        {
            if (ctx.vuRegs.VI[fs & 0xF] != ctx.vuRegs.VI[ft & 0xF]) {
                branch_taken = true;
                branch_target = (pc + 8 + imm11 * 8) & 0x3FFF;
            }
            break;
        }
        
        case 0x34: // IBLTZ: Branch if less than zero
        {
            if (static_cast<int16_t>(ctx.vuRegs.VI[fs & 0xF]) < 0) {
                branch_taken = true;
                branch_target = (pc + 8 + imm11 * 8) & 0x3FFF;
            }
            break;
        }
        
        case 0x35: // IBGTZ: Branch if greater than zero
        {
            if (static_cast<int16_t>(ctx.vuRegs.VI[fs & 0xF]) > 0) {
                branch_taken = true;
                branch_target = (pc + 8 + imm11 * 8) & 0x3FFF;
            }
            break;
        }
        
        case 0x36: // IBLEZ: Branch if <= zero
        {
            if (static_cast<int16_t>(ctx.vuRegs.VI[fs & 0xF]) <= 0) {
                branch_taken = true;
                branch_target = (pc + 8 + imm11 * 8) & 0x3FFF;
            }
            break;
        }
        
        case 0x37: // IBGEZ: Branch if >= zero
        {
            if (static_cast<int16_t>(ctx.vuRegs.VI[fs & 0xF]) >= 0) {
                branch_taken = true;
                branch_target = (pc + 8 + imm11 * 8) & 0x3FFF;
            }
            break;
        }
        
        // B: Unconditional branch (special encoding)
        case 0x40: // B (also LQI sometimes)
        {
            // Check if this is LQI or B based on other bits
            if ((instr & 0x1FC0000) == 0) {
                // B: Unconditional branch
                branch_taken = true;
                branch_target = (pc + 8 + imm11 * 8) & 0x3FFF;
            } else {
                // LQI: Load Quadword Increment
                uint32_t addr = ctx.vuRegs.VI[fs & 0xF] * 16;
                addr &= 0x3FFF;
                
                if (ft != 0) {
                    std::memcpy(&ctx.vuRegs.VF[ft], &vu1_data_memory[addr], 16);
                }
                if (fs != 0) {
                    ctx.vuRegs.VI[fs & 0xF]++;
                }
            }
            break;
        }
        
        case 0x41: // BAL or SQI
        {
            if ((instr & 0x1FC0000) == 0) {
                // BAL: Branch And Link
                if (ft != 0) {
                    ctx.vuRegs.VI[ft & 0xF] = ((pc + 16) / 8) & 0x3FF;
                }
                branch_taken = true;
                branch_target = (pc + 8 + imm11 * 8) & 0x3FFF;
            } else {
                // SQI: Store Quadword Increment
                uint32_t addr = ctx.vuRegs.VI[ft & 0xF] * 16;
                addr &= 0x3FFF;
                
                std::memcpy(&vu1_data_memory[addr], &ctx.vuRegs.VF[fs], 16);
                if (ft != 0) {
                    ctx.vuRegs.VI[ft & 0xF]++;
                }
            }
            break;
        }
        
        case 0x42: // LQD: Load Quadword Decrement
        {
            if (fs != 0) {
                ctx.vuRegs.VI[fs & 0xF]--;
            }
            uint32_t addr = ctx.vuRegs.VI[fs & 0xF] * 16;
            addr &= 0x3FFF;
            
            if (ft != 0) {
                std::memcpy(&ctx.vuRegs.VF[ft], &vu1_data_memory[addr], 16);
            }
            break;
        }
        
        case 0x43: // SQD: Store Quadword Decrement
        {
            if (ft != 0) {
                ctx.vuRegs.VI[ft & 0xF]--;
            }
            uint32_t addr = ctx.vuRegs.VI[ft & 0xF] * 16;
            addr &= 0x3FFF;
            
            std::memcpy(&vu1_data_memory[addr], &ctx.vuRegs.VF[fs], 16);
            break;
        }
        
        // ====================================================================
        // Move instructions
        // ====================================================================
        case 0x44: // MTIR: Move To Integer Register (VI[ft] = VF[fs].fsf)
        {
            uint8_t fsf = (dest >> 2) & 0x03;  // Field selector from dest bits
            
            float field_val;
            switch (fsf) {
                case 0: field_val = ctx.vuRegs.VF[fs].x; break;
                case 1: field_val = ctx.vuRegs.VF[fs].y; break;
                case 2: field_val = ctx.vuRegs.VF[fs].z; break;
                default: field_val = ctx.vuRegs.VF[fs].w; break;
            }
            
            // Extract lower 16 bits of the float's bit pattern
            uint32_t bits;
            std::memcpy(&bits, &field_val, sizeof(bits));
            
            if (ft != 0) {
                ctx.vuRegs.VI[ft & 0xF] = static_cast<uint16_t>(bits);
            }
            break;
        }
        
        case 0x45: // MFIR: Move From Integer Register (VF[ft].fsf = VI[fs])
        {
            uint8_t fsf = (dest >> 2) & 0x03;
            
            // Sign-extend 16-bit to float
            int16_t int_val = static_cast<int16_t>(ctx.vuRegs.VI[fs & 0xF]);
            float float_val = static_cast<float>(int_val);
            
            if (ft != 0) {
                switch (fsf) {
                    case 0: ctx.vuRegs.VF[ft].x = float_val; break;
                    case 1: ctx.vuRegs.VF[ft].y = float_val; break;
                    case 2: ctx.vuRegs.VF[ft].z = float_val; break;
                    default: ctx.vuRegs.VF[ft].w = float_val; break;
                }
            }
            break;
        }
        
        case 0x46: // ILWR: Integer Load Word from VU memory (single component)
        {
            uint32_t addr = ctx.vuRegs.VI[fs & 0xF] * 16;
            addr &= 0x3FFF;
            
            uint32_t word;
            // Select component based on dest
            if (dest & VU_DEST::X) std::memcpy(&word, &vu1_data_memory[addr], 4);
            else if (dest & VU_DEST::Y) std::memcpy(&word, &vu1_data_memory[addr + 4], 4);
            else if (dest & VU_DEST::Z) std::memcpy(&word, &vu1_data_memory[addr + 8], 4);
            else std::memcpy(&word, &vu1_data_memory[addr + 12], 4);
            
            if (ft != 0) {
                ctx.vuRegs.VI[ft & 0xF] = static_cast<uint16_t>(word);
            }
            break;
        }
        
        case 0x47: // ISWR: Integer Store Word to VU memory
        {
            uint32_t addr = ctx.vuRegs.VI[ft & 0xF] * 16;
            addr &= 0x3FFF;
            
            uint32_t word = ctx.vuRegs.VI[fs & 0xF];
            
            if (dest & VU_DEST::X) std::memcpy(&vu1_data_memory[addr], &word, 4);
            if (dest & VU_DEST::Y) std::memcpy(&vu1_data_memory[addr + 4], &word, 4);
            if (dest & VU_DEST::Z) std::memcpy(&vu1_data_memory[addr + 8], &word, 4);
            if (dest & VU_DEST::W) std::memcpy(&vu1_data_memory[addr + 12], &word, 4);
            break;
        }
        
        // ====================================================================
        // Special instructions
        // ====================================================================
        case 0x50: // RINIT: Initialize R register
        {
            uint32_t val;
            std::memcpy(&val, &ctx.vuRegs.VF[fs], sizeof(val));
            ctx.vuRegs.R = (val & 0x7FFFFF) | 0x3F800000;  // mantissa | 1.0
            break;
        }
        
        case 0x51: // RGET: Get R register
        {
            if (ft != 0) {
                float r_val;
                std::memcpy(&r_val, &ctx.vuRegs.R, sizeof(r_val));
                if (dest & VU_DEST::X) ctx.vuRegs.VF[ft].x = r_val;
                if (dest & VU_DEST::Y) ctx.vuRegs.VF[ft].y = r_val;
                if (dest & VU_DEST::Z) ctx.vuRegs.VF[ft].z = r_val;
                if (dest & VU_DEST::W) ctx.vuRegs.VF[ft].w = r_val;
            }
            break;
        }
        
        case 0x52: // RNEXT: Advance R and get
        {
            // Simple LFSR-like update
            ctx.vuRegs.R = ((ctx.vuRegs.R >> 4) | (ctx.vuRegs.R << 19)) & 0x7FFFFF | 0x3F800000;
            
            if (ft != 0) {
                float r_val;
                std::memcpy(&r_val, &ctx.vuRegs.R, sizeof(r_val));
                if (dest & VU_DEST::X) ctx.vuRegs.VF[ft].x = r_val;
                if (dest & VU_DEST::Y) ctx.vuRegs.VF[ft].y = r_val;
                if (dest & VU_DEST::Z) ctx.vuRegs.VF[ft].z = r_val;
                if (dest & VU_DEST::W) ctx.vuRegs.VF[ft].w = r_val;
            }
            break;
        }
        
        case 0x53: // RXOR: XOR with R
        {
            uint32_t fs_val;
            std::memcpy(&fs_val, &ctx.vuRegs.VF[fs], sizeof(fs_val));
            ctx.vuRegs.R ^= (fs_val & 0x7FFFFF);
            ctx.vuRegs.R = (ctx.vuRegs.R & 0x7FFFFF) | 0x3F800000;
            break;
        }
        
        case 0x7E: // WAITQ - NOP in interpreter
        case 0x7F: // WAITP - NOP in interpreter (VU1 only)
            break;
        
        default:
            if (g_logFile.is_open()) {
                g_logFile << "[VU1 Lower] Unknown opcode 0x" << std::hex << (int)opcode 
                          << " instr=0x" << instr << std::dec << std::endl;
            }
            break;
    }
}

// ============================================================================
// XGKICK: DMA Kick to GIF
// ============================================================================
void VU1::Op_XGKICK(CpuContext& ctx, uint32_t addr) {
    g_logFile << "[VU1] XGKICK invoked with addr=0x" << std::hex << addr << std::dec << std::endl;
    uint32_t mem_offset = (addr * 16) & 0x3FFF;
    
    if (mem_offset + 16 > vu1_data_memory.size()) {
        if (g_logFile.is_open()) {
            g_logFile << "[VU1] XGKICK error: OOB address 0x" << std::hex << mem_offset << std::dec << std::endl;
        }
        return;
    }
    
    // Check for active transfer (stall if second XGKICK during first)
    if (xgkick_active) {
        if (g_logFile.is_open()) {
            g_logFile << "[VU1] XGKICK stall: Previous transfer active" << std::endl;
        }
        // In real hardware this would stall, for interpreter we just continue
    }
    
    // Read GIF tag
    uint64_t tag_lo;
    std::memcpy(&tag_lo, &vu1_data_memory[mem_offset], 8);
    
    uint16_t nloop = tag_lo & 0x7FFF;
    bool eop = (tag_lo & (1ULL << 15)) != 0;
    uint8_t flg = (tag_lo >> 58) & 0x03;
    uint8_t nregs = (tag_lo >> 60) & 0x0F;
    if (nregs == 0) nregs = 16;
    
    // Calculate packet size based on format
    size_t packet_size;
    switch (flg) {
        case 0:  // PACKED
            packet_size = 16 + (nloop * nregs * 16);
            break;
        case 1:  // REGLIST
            packet_size = 16 + ((nloop * nregs + 1) / 2) * 16;
            break;
        case 2:  // IMAGE
        case 3:
            packet_size = 16 + (nloop * 16);
            break;
        default:
            packet_size = 16 + (nloop * 16);
            break;
    }
    
    if (g_logFile.is_open()) {
        g_logFile << "[VU1] XGKICK @0x" << std::hex << mem_offset 
                  << " NLOOP=" << std::dec << nloop
                  << " NREGS=" << (int)nregs
                  << " FLG=" << (int)flg
                  << " EOP=" << eop
                  << " Size=" << packet_size << std::endl;
    }
    
    // Safety check
    if (packet_size > 65536) {
        if (g_logFile.is_open()) {
            g_logFile << "[VU1] XGKICK aborted: Size too large" << std::endl;
        }
        return;
    }
    
    // Copy data (handling wrap-around)
    std::vector<uint8_t> buffer(packet_size);
    for (size_t i = 0; i < packet_size; i++) {
        buffer[i] = vu1_data_memory[(mem_offset + i) & 0x3FFF];
    }
    
    // Mark transfer active
    xgkick_active = true;
    xgkick_addr = addr;
    
    // Send to GIF PATH1
    g_gif.ReceiveData(GIFPath::PATH1, buffer.data(), packet_size);
    
    // For interpreter, immediately mark complete
    xgkick_active = false;
}

VU1 g_vu0(0);
VU1 g_vu1(1);