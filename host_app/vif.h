// vif.h - Fixed and comprehensive VIF implementation
#pragma once
#include <cstdint>
#include <cstddef>
#include <deque>
#include <vector>
#include <array>

// ============================================================================
// VIF State Machine States
// ============================================================================
enum VIFState {
    VIF_STATE_IDLE,          // Waiting for a 4-byte VIF Code
    VIF_STATE_WAIT_DATA,     // Waiting for N bytes of payload
    VIF_STATE_DIRECT_STREAM, // Streaming data to GIF PATH2
    VIF_STATE_STALL_VU,      // Stalled waiting for VU to finish
    VIF_STATE_STALL_GIF      // Stalled waiting for GIF
};

// ============================================================================
// VIF Commands (from PS2 documentation)
// ============================================================================
enum VIFCommand : uint8_t {
    VIF_NOP        = 0x00,
    VIF_STCYCL     = 0x01,
    VIF_OFFSET     = 0x02,  // VIF1 only
    VIF_BASE       = 0x03,  // VIF1 only
    VIF_ITOP       = 0x04,
    VIF_STMOD      = 0x05,
    VIF_MSKPATH3   = 0x06,  // VIF1 only
    VIF_MARK       = 0x07,
    VIF_FLUSHE     = 0x10,
    VIF_FLUSH      = 0x11,  // VIF1 only
    VIF_FLUSHA     = 0x13,  // VIF1 only
    VIF_MSCAL      = 0x14,
    VIF_MSCALF     = 0x15,  // VIF1 only
    VIF_MSCNT      = 0x17,
    VIF_STMASK     = 0x20,
    VIF_STROW      = 0x30,
    VIF_STCOL      = 0x31,
    VIF_MPG        = 0x4A,
    VIF_DIRECT     = 0x50,  // VIF1 only
    VIF_DIRECTHL   = 0x51,  // VIF1 only
    // UNPACK commands: 0x60-0x7F
};

// ============================================================================
// VIF_STAT Register Bits (per PS2 documentation)
// ============================================================================
namespace VIF_STAT {
    constexpr uint32_t VPS_MASK     = 0x03;       // Bits 0-1: VIF command status
    constexpr uint32_t VPS_IDLE     = 0x00;       // Idle
    constexpr uint32_t VPS_WAIT     = 0x01;       // Waiting for data
    constexpr uint32_t VPS_DECODE   = 0x02;       // Decoding command
    constexpr uint32_t VPS_TRANSFER = 0x03;       // Decompressing/transferring
    
    constexpr uint32_t VEW          = (1 << 2);   // VU executing microprogram
    constexpr uint32_t VGW          = (1 << 3);   // Stalled waiting for GIF (VIF1)
    constexpr uint32_t MRK          = (1 << 6);   // MARK detected
    constexpr uint32_t DBF          = (1 << 7);   // Double buffer flag (VIF1)
    constexpr uint32_t VSS          = (1 << 8);   // Stalled after STOP
    constexpr uint32_t VFS          = (1 << 9);   // Stalled after force break
    constexpr uint32_t VIS          = (1 << 10);  // Stalled on interrupt bit
    constexpr uint32_t INT          = (1 << 11);  // Interrupt bit detected
    constexpr uint32_t ER0          = (1 << 12);  // DMAtag mismatch error
    constexpr uint32_t ER1          = (1 << 13);  // Invalid VIF command
    constexpr uint32_t FDR          = (1 << 23);  // FIFO direction (VIF1)
    constexpr uint32_t FQC_SHIFT    = 24;         // Bits 24-28: FIFO quadword count
    constexpr uint32_t FQC_MASK     = 0x1F;
}

// ============================================================================
// VIF_ERR Register Bits
// ============================================================================
namespace VIF_ERR {
    constexpr uint32_t MII = (1 << 0);  // Disable interrupt bit stalls
    constexpr uint32_t ME0 = (1 << 1);  // Disable DMAtag mismatch error
    constexpr uint32_t ME1 = (1 << 2);  // Disable invalid command error
}

namespace VIF_FBRST{
    constexpr uint32_t RST = (1 << 0);  // Reset VIF
    constexpr uint32_t FBK = (1 << 1);  // Force break
    constexpr uint32_t STP = (1 << 2);  // Stop
    constexpr uint32_t STC = (1 << 3);  // Stall cancel
}

// ============================================================================
// UNPACK Mask Actions (2 bits per field in VIF_MASK)
// ============================================================================
enum MaskAction : uint8_t {
    MASK_WRITE   = 0,  // Write data from source
    MASK_NOWRITE = 1,  // Don't write (keep existing)
    MASK_ROW     = 2,  // Write from ROW register
    MASK_COL     = 3   // Write from COL register
};

// ============================================================================
// VIF Register Structure (per PS2 documentation)
// ============================================================================
struct VIFRegs {
    uint32_t stat;    // 0x00: Status
    uint32_t fbrst;   // 0x10: Force break/reset
    uint32_t err;     // 0x20: Error mask
    uint32_t mark;    // 0x30: Mark value
    uint32_t cycle;   // 0x40: CL (bits 0-7) / WL (bits 8-15)
    uint32_t mode;    // 0x50: Addition mode (0-1)
    uint32_t num;     // 0x60: Data remaining
    uint32_t mask;    // 0x70: Write mask (32 bits: 2 bits per field x 4 fields x 4 rows)
    uint32_t code;    // 0x80: Current VIF code
    uint32_t itops;   // 0x90: ITOPS value
    uint32_t base;    // 0xA0: BASE (VIF1 only)
    uint32_t ofst;    // 0xB0: OFST (VIF1 only)
    uint32_t tops;    // 0xC0: TOPS (VIF1 only)
    uint32_t itop;    // 0xD0: ITOP value
    uint32_t top;     // 0xE0: TOP (VIF1 only)
    uint32_t row[4];  // R0-R3 for row fill/addition
    uint32_t col[4];  // C0-C3 for column fill
    
    // Helper to get CL value
    uint32_t GetCL() const { return cycle & 0xFF; }
    // Helper to get WL value  
    uint32_t GetWL() const { return (cycle >> 8) & 0xFF; }
};

// ============================================================================
// VIF Class
// ============================================================================
class VIF {
public:
    VIFRegs regs[2];  // VIF0 and VIF1
    
    // Internal FIFO state
    std::deque<std::array<uint32_t, 4>> fifo[2];
    uint32_t latch[2][4];
    uint32_t latch_fill_count[2] = {0, 0};
    
    VIFState state[2] = {VIF_STATE_IDLE, VIF_STATE_IDLE};
    uint32_t pending_cmd[2] = {0};
    uint32_t pending_bytes[2] = {0};
    uint32_t pending_num[2] = {0};
    uint32_t pending_imm[2] = {0};
    bool pending_irq[2] = {false, false};  // IRQ bit from VIF code
    bool pending_usn[2] = {false, false};  // Unsigned flag for UNPACK
    bool pending_use_tops[2] = {false, false};
    bool pending_use_mask[2] = {false, false};  // CMD bit 4 for UNPACK masking
    
    // VU memory target
    std::vector<uint8_t> vu1_micro_mem;

    VIF();
    void Reset();
    
    // DMA feeds data here
    void ProcessData(int vif_num, const uint8_t* data, size_t size);
    
    // Register access
    uint32_t Read(int vif_num, uint32_t addr);
    void Write(int vif_num, uint32_t addr, uint32_t value);
    
    // Status helpers for external checking
    bool IsVUExecuting(int vif_num) const;
    bool IsGIFBusy() const;

    void WriteToLatch(int vif_num, uint32_t data, uint32_t address);
    void WriteToLatch2(int vif_num, uint32_t data);
    
private:
    void ProcessFifo(int vif_num);
    void ExecuteCommand(int vif_num, uint32_t cmd, uint32_t immediate, uint32_t num, 
                        const std::vector<uint8_t>& payload, bool irq, bool use_mask);
    
    // UNPACK implementation
    void ExecuteUnpack(int vif_num, uint32_t cmd, uint32_t immediate, uint32_t num,
                       const std::vector<uint8_t>& payload, bool use_mask);
    
    // Mask helper: Get action for a specific field (0-3) at a specific write cycle (0-3)
    MaskAction GetMaskAction(int vif_num, int field, int write_cycle) const;
    
    // Update VIF_STAT register
    void UpdateStat(int vif_num, uint32_t vps);
};

extern VIF g_vif;
