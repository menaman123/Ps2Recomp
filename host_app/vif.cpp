// vif.cpp - Fixed and comprehensive VIF implementation
#include "vif.h"
#include "gif.h"
#include "memory.h"
#include "vu.h"
#include <iostream>
#include <fstream>
#include <iomanip>
#include <cstring>

extern std::ofstream g_logFile;

VIF g_vif;

// ============================================================================
// Command Definitions
// ============================================================================
#define CMD_NOP        0x00
#define CMD_STCYCL     0x01
#define CMD_OFFSET     0x02
#define CMD_BASE       0x03
#define CMD_ITOP       0x04
#define CMD_STMOD      0x05
#define CMD_MSKPATH3   0x06
#define CMD_MARK       0x07
#define CMD_FLUSHE     0x10
#define CMD_FLUSH      0x11
#define CMD_FLUSHA     0x13
#define CMD_MSCAL      0x14
#define CMD_MSCALF     0x15
#define CMD_MSCNT      0x17
#define CMD_STMASK     0x20
#define CMD_STROW      0x30
#define CMD_STCOL      0x31
#define CMD_MPG        0x4A
#define CMD_DIRECT     0x50
#define CMD_DIRECTHL   0x51

// ============================================================================
// UNPACK Format Tables (per PS2 documentation)
// ============================================================================
// Number of elements per vector for each format
static const uint8_t UNPACK_ELEMS[16] = {
    1, 1, 1, 0,  // 0-3: S-32, S-16, S-8, reserved (S replicates to 4)
    2, 2, 2, 0,  // 4-7: V2-32, V2-16, V2-8, reserved
    3, 3, 3, 0,  // 8-B: V3-32, V3-16, V3-8, reserved
    4, 4, 4, 1   // C-F: V4-32, V4-16, V4-8, V4-5
};

// Bits per source element
static const uint8_t UNPACK_BITS[16] = {
    32, 16, 8, 0,
    32, 16, 8, 0,
    32, 16, 8, 0,
    32, 16, 8, 16  // V4-5 reads 16 bits total (5-5-5-1 packed)
};

// Number of output elements (always 4 for destination quadword, but S fills all 4)
static const uint8_t UNPACK_DEST_ELEMS[16] = {
    4, 4, 4, 0,  // S-types replicate to all 4
    2, 2, 2, 0,  // V2 writes 2 (x,y), z,w indeterminate
    3, 3, 3, 0,  // V3 writes 3 (x,y,z), w indeterminate
    4, 4, 4, 4   // V4 writes all 4
};

// ============================================================================
// Helper Functions
// ============================================================================
static bool IsUnpackCmd(uint32_t cmd) {
    return (cmd >= 0x60 && cmd <= 0x7F);
}

static uint32_t GetUnpackSize(uint32_t cmd, uint32_t num, uint32_t cl, uint32_t wl) {
    uint32_t format = cmd & 0x0F;
    uint32_t bits = UNPACK_BITS[format];
    
    if (bits == 0) return 0;
    
    uint32_t real_num = (num == 0) ? 256 : num;

    uint32_t source_vectors = real_num;
    if ( cl > wl && cl > 0 && wl > 0) {
        source_vectors = (real_num * wl + cl - 1) / cl;
    }
    
    // Calculate source elements per vector
    uint32_t elems_per_vector;
    if (format == 0x0F) {
        // V4-5: 1x 16-bit source per vector
        elems_per_vector = 1;
    } else if (format <= 0x03) {
        // S-type: 1 element replicated to 4
        elems_per_vector = 1;
    } else {
        elems_per_vector = UNPACK_ELEMS[format];
    }
    
    uint32_t total_bits = source_vectors * elems_per_vector * bits;
    uint32_t total_bytes = (total_bits + 7) / 8;
    
    // Align to 32-bit boundary
    return (total_bytes + 3) & ~3;
}

static uint32_t ReadUnpackValue(const uint8_t*& ptr, uint32_t bits, bool is_unsigned) {
    uint32_t val = 0;
    
    switch (bits) {
        case 8: {
            uint8_t v = *ptr++;
            val = is_unsigned ? v : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(v)));
            break;
        }
        case 16: {
            uint16_t v;
            std::memcpy(&v, ptr, 2);
            ptr += 2;
            val = is_unsigned ? v : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(v)));
            break;
        }
        case 32: {
            std::memcpy(&val, ptr, 4);
            ptr += 4;
            break;
        }
    }
    
    return val;
}

// ============================================================================
// VIF Constructor and Reset
// ============================================================================
VIF::VIF() {
    vu1_micro_mem.resize(16 * 1024);  // 16KB VU1 micro memory
    Reset();
}

void VIF::Reset() {
    for (int i = 0; i < 2; i++) {
        std::memset(&regs[i], 0, sizeof(VIFRegs));
        fifo[i].clear();
        state[i] = VIF_STATE_IDLE;
        pending_cmd[i] = 0;
        pending_bytes[i] = 0;
        pending_num[i] = 0;
        pending_imm[i] = 0;
        pending_irq[i] = false;
        pending_usn[i] = false;
        pending_use_tops[i] = false;
        pending_use_mask[i] = false;
        
        // Default cycle: CL=1, WL=1
        regs[i].cycle = 0x0101;
    }
    
    std::fill(vu1_micro_mem.begin(), vu1_micro_mem.end(), 0);
}

// ============================================================================
// Status Helpers
// ============================================================================
bool VIF::IsVUExecuting(int vif_num) const {
    // Check VPU_STAT or similar - for now return false
    // In real impl, check if VU is still running
    return false;
}

bool VIF::IsGIFBusy() const {
    // Check if GIF is busy with PATH3 IMAGE transfer
    // Would check GIF_STAT bits
    return false;
}

void VIF::UpdateStat(int vif_num, uint32_t vps) {
    regs[vif_num].stat = (regs[vif_num].stat & ~VIF_STAT::VPS_MASK) | (vps & VIF_STAT::VPS_MASK);
    
    // Update FQC (FIFO quadword count)
    uint32_t fifo_qwc = std::min((uint32_t)(fifo[vif_num].size() / 16), (uint32_t)VIF_STAT::FQC_MASK);
    regs[vif_num].stat = (regs[vif_num].stat & ~(VIF_STAT::FQC_MASK << VIF_STAT::FQC_SHIFT)) 
                       | (fifo_qwc << VIF_STAT::FQC_SHIFT);
}

// ============================================================================
// Mask Helper: Get action for a specific field at a specific write cycle
// VIF_MASK format: 32 bits = 4 rows x 4 fields x 2 bits
// Bits [1:0]   = Row 0, Field X (field 0)
// Bits [3:2]   = Row 0, Field Y (field 1)  
// Bits [5:4]   = Row 0, Field Z (field 2)
// Bits [7:6]   = Row 0, Field W (field 3)
// Bits [15:8]  = Row 1
// Bits [23:16] = Row 2
// Bits [31:24] = Row 3
// ============================================================================
MaskAction VIF::GetMaskAction(int vif_num, int field, int write_cycle) const {
    // field: 0=X, 1=Y, 2=Z, 3=W
    // write_cycle: 0-3 (row in mask)
    int row = write_cycle & 0x3;
    int bit_pos = (row * 8) + (field * 2);
    return static_cast<MaskAction>((regs[vif_num].mask >> bit_pos) & 0x3);
}

// ============================================================================
// Process incoming DMA data
// ============================================================================
void VIF::ProcessData(int vif_num, const uint8_t* data, size_t size) {
    if (g_logFile.is_open()) {
        g_logFile << "[VIF" << vif_num << "] ProcessData: " << size << " bytes" << std::endl;
    }
    
    if (!data || size == 0) return;

    // Process the incoming byte stream 4 bytes at a time (as 32-bit words)
    for (size_t i = 0; i < size; i += 4) {
        uint32_t word;
        // Ensure we don't read past the buffer if size isn't a multiple of 4
        std::memcpy(&word, &data[i], 4);
        
        // Use the latch system you've already built to assemble 128-bit packets
        // The address pattern 0, 4, 8, C tells WriteToLatch which slot to fill
        WriteToLatch2(vif_num, word);
    }
}

// ============================================================================
// Main FIFO Processing State Machine
// ============================================================================

void VIF::WriteToLatch2(int vif_num, uint32_t data) {

    if (g_logFile.is_open()) {
        g_logFile << "[VIF" << vif_num << "] WriteToLatch: Data=0x" 
                  << std::hex << std::setw(8) << std::setfill('0') << data << std::dec << std::endl;
    }

    // 1. Store data in the next available slot
    if (latch_fill_count[vif_num] < 4) {
        latch[vif_num][latch_fill_count[vif_num]] = data;
        latch_fill_count[vif_num]++;
    }

    // 2. Check if Latch is Full
    if (latch_fill_count[vif_num] == 4) {
        // Assemble the 128-bit packet
        std::array<uint32_t, 4> packet;
        for(int i=0; i<4; i++) packet[i] = latch[vif_num][i];

        // Push to FIFO
        fifo[vif_num].push_back(packet);
        
        // Reset Counter
        latch_fill_count[vif_num] = 0;
        
        // 3. Process Immediately
        // Crucial for TTE: If this packet contains a VIF command in the 
        // first half, the state machine must update NOW.
        ProcessFifo(vif_num);
    }
}

void VIF::WriteToLatch(int vif_num, uint32_t data, uint32_t address) {
    // 1. Calculate the slot index (0-3) based on the register address
    // 0x5000 -> 0, 0x5004 -> 1, 0x5008 -> 2, 0x500C -> 3
    int slot_index = (address & 0x0C) >> 2; 

    // 2. Update the specific slot
    latch[vif_num][slot_index] = data;

    // 3. TRIGGER: Only push to FIFO when the "Submit" address (slot 3) is hit
    if (slot_index == 3) {
        std::array<uint32_t, 4> packet;
        packet[0] = latch[vif_num][0];
        packet[1] = latch[vif_num][1];
        packet[2] = latch[vif_num][2];
        packet[3] = latch[vif_num][3];

        fifo[vif_num].push_back(packet);
        ProcessFifo(vif_num);
    }
}

void VIF::ProcessFifo(int v) {

    g_logFile << "[VIF" << v << "] ProcessFifo: State=" << state[v] << std::endl;
    // ========================================================================
    // PERSISTENT PROCESSING STATE (Per VIF Unit)
    // ========================================================================
    static uint32_t current_qw[2][4]; 
    static int word_index[2] = {4, 4}; // Start at 4 to force an initial fetch

    int loops = 0;
    const int MAX_LOOPS = 0x10000;

    while (loops++ < MAX_LOOPS) {

        // ====================================================================
        // STATE 1: DIRECT_STREAM 
        // ====================================================================
        if (state[v] == VIF_STATE_DIRECT_STREAM) {
            UpdateStat(v, VIF_STAT::VPS_TRANSFER);
            
            // Force alignment for DIRECT stream start
            if (word_index[v] < 4) word_index[v] = 4; 

            uint32_t available = static_cast<uint32_t>(fifo[v].size());
            uint32_t bytes_left = pending_bytes[v];
            uint32_t qwc_needed = (bytes_left + 15) / 16;

            
            uint32_t chunk_qwc = std::min(available, qwc_needed);



            if (chunk_qwc > 0) {
                for (uint32_t i = 0; i < chunk_qwc; i++) {
                    auto& packet = fifo[v].front();
                    g_gif.ReceiveData(GIFPath::PATH2, 
                                      reinterpret_cast<uint8_t*>(packet.data()), 16);
                    fifo[v].pop_front();
                }

                uint32_t bytes_transferred = chunk_qwc * 16;
                if (bytes_transferred > pending_bytes[v]) pending_bytes[v] = 0;
                else pending_bytes[v] -= bytes_transferred;
            }

            if (pending_bytes[v] == 0) {
                if (g_logFile.is_open()) g_logFile << "[VIF" << v << "] DIRECT complete" << std::endl;
                state[v] = VIF_STATE_IDLE;
                UpdateStat(v, VIF_STAT::VPS_IDLE);

                if (pending_irq[v] && !(regs[v].err & VIF_ERR::MII)) {
                    regs[v].stat |= VIF_STAT::INT;
                }
                pending_irq[v] = false;
            } else {
                return; // Wait for more data
            }
            continue;
        }

        // ====================================================================
        // STATE 2: WAIT_DATA
        // Transitions here from IDLE if payload is missing.
        // ====================================================================
        if (state[v] == VIF_STATE_WAIT_DATA) {
            
            uint32_t bytes_total = pending_bytes[v];
            uint32_t words_needed = (bytes_total + 3) / 4;

            // 1. Check Total Availability
            uint32_t words_in_curr = 4 - word_index[v];
            uint32_t fifo_words = (uint32_t)fifo[v].size() * 4;
            
            g_logFile << "[VIF" << v << "] BYTES_TOTAL: " << bytes_total << std::endl;
            g_logFile << "[VIF" << v << "] WORDS_NEEDED: " << words_needed << std::endl;
            g_logFile << "[VIF" << v << "] FIFO_WORDS: " << fifo_words << std::endl;
            g_logFile << "[VIF" << v << "] WAIT_DATA: Needs " << words_needed 
                      << " words, FIFO has " << fifo_words << " words" << std::endl;
            if (words_in_curr + fifo_words < words_needed) {
                return; // Not enough data yet
            }

            UpdateStat(v, VIF_STAT::VPS_TRANSFER);

            std::vector<uint8_t> payload;
            payload.reserve(words_needed * 4);

            // 2. UNIFIED DRAIN LOOP
            // This handles TTE leftovers, full FIFO packets, AND partial end packets safely.
            while (words_needed > 0) {
                
                // Refill 'current_qw' from FIFO if we exhausted the current packet
                if (word_index[v] >= 4) {
                    // We know FIFO is not empty because of the check in Step 1
                    auto& packet = fifo[v].front();
                    
                    // Fix: Use memcpy instead of direct assignment
                    std::memcpy(current_qw[v], packet.data(), 16);
                    
                    fifo[v].pop_front();
                    word_index[v] = 0;
                }

                // Consume one word from the cache
                uint32_t w = current_qw[v][word_index[v]++];
                
                payload.push_back(w & 0xFF);
                payload.push_back((w >> 8) & 0xFF);
                payload.push_back((w >> 16) & 0xFF);
                payload.push_back((w >> 24) & 0xFF);
                
                words_needed--;
            }

            // 3. Execute
            ExecuteCommand(v, pending_cmd[v], pending_imm[v], pending_num[v], 
                           payload, pending_irq[v], pending_use_mask[v]);

            // 4. Reset
            state[v] = VIF_STATE_IDLE;
            pending_bytes[v] = 0;
            UpdateStat(v, VIF_STAT::VPS_IDLE);
            
            // CRITICAL: Continue immediately! 
            // 'word_index' is now pointing exactly at the next command (e.g., index 2).
            // The IDLE state will pick it up in the very next iteration.
            continue; 
        }

        // ====================================================================
        // STATE 3: IDLE (Command Parsing)
        // ====================================================================
        if (state[v] == VIF_STATE_IDLE) {

            // 1. Fetch Logic
            if (word_index[v] >= 4) {
                if (fifo[v].empty()) return;
                auto& packet = fifo[v].front();
                std::memcpy(current_qw[v], packet.data(), 16);
                fifo[v].pop_front();
                word_index[v] = 0;
            }

            // 2. Decode Logic
            uint32_t vif_code = current_qw[v][word_index[v]++];
            
            bool irq_bit   = (vif_code >> 31) & 1;
            uint32_t cmd   = (vif_code >> 24) & 0x7F;
            uint32_t num   = (vif_code >> 16) & 0xFF;
            uint32_t imm   = vif_code & 0xFFFF;
            
            regs[v].code = vif_code;

            if (cmd != CMD_NOP && g_logFile.is_open()) {
                g_logFile << "[VIF" << v << "] Code=0x" << std::hex << std::setw(8) 
                          << std::setfill('0') << vif_code
                          << " CMD=0x" << (int)cmd << " IMM=0x" << imm << std::dec;
                if (irq_bit) g_logFile << " [IRQ]";
                g_logFile << std::endl;
            }

            // --- DIRECT Handling ---
            if (cmd == CMD_DIRECT || cmd == CMD_DIRECTHL) {
                g_logFile << "╔═══════════════════════════════════════════╗" << std::endl;
                g_logFile << "║ VIF" << v << " DIRECT: " << imm << " quadwords to GIF PATH2 ║" << std::endl;
                g_logFile << "╚═══════════════════════════════════════════╝" << std::endl;
                word_index[v] = 4; // Force alignment
                uint32_t qwc = (imm == 0) ? 0x10000 : imm;
                state[v] = VIF_STATE_DIRECT_STREAM;
                pending_bytes[v] = qwc * 16;
                pending_irq[v] = irq_bit;
                continue;
            }

            // --- Payload Size Calculation ---
            uint32_t bytes_needed = 0;
            bool use_mask = false;
            bool force_align = false; // UNPACK aligns to next QW, MPG does NOT necessarily

            if (IsUnpackCmd(cmd)) {
                uint32_t unpack_cl = regs[v].GetCL();
                uint32_t unpack_wl = regs[v].GetWL();
                bytes_needed = GetUnpackSize(cmd, num, unpack_cl, unpack_wl);
                use_mask = (cmd & 0x10);
                force_align = true; // UNPACK spec: "Data starts at next 128-bit boundary"
            } 
            else if (cmd == CMD_MPG) {
                bytes_needed = (num == 0 ? 256 : num) * 8;
                force_align = true;
            }
            else {
                switch (cmd) {
                    case CMD_STMASK:
                        bytes_needed = 4; 
                        force_align = false; // STMASK is usually inline, but technically could be separate
                        break;
                    case CMD_STROW:
                        bytes_needed = 16; 
                        force_align = false; // STROW/STCOL are usually inline
                        break;
                    case CMD_STCOL:
                        bytes_needed = 16; 
                        force_align = false; // STROW/STCOL are usually inline
                        break;
                    default: bytes_needed = 0; break;
                }
            }

            // --- Payload Extraction & Execution ---
            if (bytes_needed > 0) {
                
                // UNPACK discards the rest of the current QW
                if (force_align) {
                    word_index[v] = 4;
                }

                // Calculate total 32-bit words needed
                uint32_t words_needed = (bytes_needed + 3) / 4;
                uint32_t words_in_current = 4 - word_index[v];
                uint32_t fifo_words_avail = (uint32_t)fifo[v].size() * 4;

                // Check if we have enough data (Cache + FIFO)
                if (words_in_current + fifo_words_avail < words_needed) {
                    state[v] = VIF_STATE_WAIT_DATA;
                    pending_cmd[v] = cmd;
                    pending_num[v] = num;
                    pending_imm[v] = imm;
                    pending_bytes[v] = bytes_needed;
                    pending_irq[v] = irq_bit;
                    pending_use_mask[v] = use_mask;
                    
                    if (IsUnpackCmd(cmd)) {
                        pending_usn[v] = (imm & 0x4000) != 0;
                        pending_use_tops[v] = (imm & 0x8000) != 0;
                    }
                    UpdateStat(v, VIF_STAT::VPS_WAIT);
                    return; 
                }

                // Extract Payload using unified drain that preserves partial QWs
                std::vector<uint8_t> payload;
                payload.reserve(words_needed * 4);

                while (words_needed > 0) {
                    // Refill current_qw from FIFO if exhausted
                    if (word_index[v] >= 4) {
                        auto& packet = fifo[v].front();
                        std::memcpy(current_qw[v], packet.data(), 16);
                        fifo[v].pop_front();
                        word_index[v] = 0;
                    }
                    
                    // Consume one word
                    uint32_t w = current_qw[v][word_index[v]++];
                    payload.push_back(w & 0xFF);
                    payload.push_back((w >> 8) & 0xFF);
                    payload.push_back((w >> 16) & 0xFF);
                    payload.push_back((w >> 24) & 0xFF);
                    words_needed--;
                }
                // word_index[v] now correctly points to the next unconsumed word
                // Remaining words in current_qw will be picked up by the next iteration

                ExecuteCommand(v, cmd, imm, num, payload, irq_bit, use_mask);

            } else {
                // Command with no payload
                std::vector<uint8_t> empty;
                ExecuteCommand(v, cmd, imm, num, empty, irq_bit, use_mask);
            }

        }


    }
}

// ============================================================================
// Execute a VIF Command
// ============================================================================
void VIF::ExecuteCommand(int v, uint32_t cmd, uint32_t immediate, uint32_t num,
                         const std::vector<uint8_t>& payload, bool irq, bool use_mask) {
    g_logFile << "[VIF" << v << "] ExecuteCommand: CMD=0x" << std::hex << cmd 
          << " IMM=0x" << immediate << " NUM=" << std::dec << (int)num
          << " Payload=" << payload.size() << " bytes" << std::endl;
    // Handle IRQ bit (stall on NEXT command if not masked)
    if (irq && !(regs[v].err & VIF_ERR::MII)) {
        regs[v].stat |= VIF_STAT::INT;
        // Note: Real hardware stalls on the NEXT command, not this one
    }
    
    // ========================================================================
    // UNPACK Commands (0x60 - 0x7F)
    // ========================================================================
    if (IsUnpackCmd(cmd)) {
        ExecuteUnpack(v, cmd, immediate, num, payload, use_mask);
        return;
    }
    
    // ========================================================================
    // Standard Commands
    // ========================================================================
    switch (cmd) {
        case CMD_NOP:
            // Do nothing
            break;
            
        case CMD_STCYCL:
            // Set CYCLE register: CL (bits 0-7), WL (bits 8-15)
            regs[v].cycle = immediate;
            if (g_logFile.is_open()) {
                g_logFile << "[VIF" << v << "] STCYCL: CL=" << (immediate & 0xFF)
                          << " WL=" << ((immediate >> 8) & 0xFF) << std::endl;
            }
            break;
            
        case CMD_OFFSET:
            // VIF1 only: Set OFST, clear DBF, set BASE=TOPS
            if (v == 1) {
                regs[v].ofst = immediate & 0x3FF;
                regs[v].stat &= ~VIF_STAT::DBF;  // Clear double buffer flag
                regs[v].base = regs[v].tops;
                if (g_logFile.is_open()) {
                    g_logFile << "[VIF1] OFFSET: " << regs[v].ofst 
                              << " BASE=" << regs[v].base << std::endl;
                }
            }
            break;
            
        case CMD_BASE:
            // VIF1 only: Set BASE
            if (v == 1) {
                regs[v].base = immediate & 0x3FF;
            }
            break;
            
        case CMD_ITOP:
            // Set ITOP (readable by VU via XITOP)
            regs[v].itop = immediate & 0x3FF;
            break;
            
        case CMD_STMOD:
            // Set addition MODE (0-3)
            regs[v].mode = immediate & 0x3;
            if (g_logFile.is_open()) {
                g_logFile << "[VIF" << v << "] STMOD: " << regs[v].mode << std::endl;
            }
            break;
            
        case CMD_MSKPATH3:
            // VIF1 only: Mask PATH3
            if (v == 1) {
                bool mask = (immediate & 0x8000) != 0;
                g_gif.path3_masked = mask;
                if (g_logFile.is_open()) {
                    g_logFile << "[VIF1] MSKPATH3: " << (mask ? "MASKED" : "UNMASKED") << std::endl;
                }
            }
            break;
            
        case CMD_MARK:
            // Set MARK register and MRK flag
            regs[v].mark = immediate;
            regs[v].stat |= VIF_STAT::MRK;
            break;
            
        case CMD_FLUSHE:
            // Stall until VU finishes
            // In real impl: check VU status and stall if running
            if (IsVUExecuting(v)) {
                regs[v].stat |= VIF_STAT::VEW;
                // Would stall here
            }
            break;
            
        case CMD_FLUSH:
            // VIF1 only: Stall until VU finishes AND PATH1/PATH2 inactive
            if (v == 1) {
                if (IsVUExecuting(v) || IsGIFBusy()) {
                    regs[v].stat |= VIF_STAT::VEW | VIF_STAT::VGW;
                    // Would stall here
                }
            }
            break;
            
        case CMD_FLUSHA:
            // VIF1 only: Like FLUSH but also waits for PATH3
            if (v == 1) {
                // Full sync with GIF
            }
            break;
            
        case CMD_MSCALF:
            // VIF1 only: Like MSCAL but waits for PATH1/PATH2 first
            if (v == 1 && g_cpuContext) {
                // Would wait for GIF paths here
                uint32_t addr = immediate * 8;
                g_vu1.Execute(*g_cpuContext, addr);
            }
            break;
            
        case CMD_MSCNT:
            // Continue VU from current TPC
            if (v == 1 && g_cpuContext) {
                if (g_logFile.is_open()) {
                    g_logFile << "[VIF1] MSCNT: Continue VU1 at TPC=0x" 
                              << std::hex << g_cpuContext->vuRegs.TPC << std::dec << std::endl;
                }

                if(g_cpuContext->vuRegs.TPC == 0 && g_logFile.is_open()) {
                    g_logFile << "[VIF1] WARNING: MSCNT called but VU1 TPC is 0. VU may not start correctly." << std::endl;

                }
                g_vu1.Execute(*g_cpuContext, g_cpuContext->vuRegs.TPC);
            }
            break;
            
        case CMD_STMASK:
            // Set MASK register (32-bit)
            if (payload.size() >= 4) {
                std::memcpy(&regs[v].mask, payload.data(), 4);
                if (g_logFile.is_open()) {
                    g_logFile << "[VIF" << v << "] STMASK: 0x" << std::hex 
                              << regs[v].mask << std::dec << std::endl;
                }
            }
            break;
            
        case CMD_STROW:
            // Set ROW registers (4x 32-bit)
            if (payload.size() >= 16) {
                std::memcpy(regs[v].row, payload.data(), 16);
                if (g_logFile.is_open()) {
                    g_logFile << "[VIF" << v << "] STROW: " 
                              << regs[v].row[0] << ", " << regs[v].row[1] << ", "
                              << regs[v].row[2] << ", " << regs[v].row[3] << std::endl;
                }
            }
            break;
            
        case CMD_STCOL:
            // Set COL registers (4x 32-bit)
            if (payload.size() >= 16) {
                std::memcpy(regs[v].col, payload.data(), 16);
            }
            break;
            
        case CMD_MPG:
            {            
            g_logFile << "[VIF" << v << "] MPG: Writing " << payload.size() 
                      << " bytes to VU" << v << " micro memory at 0x" 
                      << std::hex << (immediate * 8) << std::dec << std::endl;
            uint32_t addr = immediate * 8;
            if (v == 1) {
                g_vu1.WriteMicroMem(addr, payload.data(), payload.size());
            } else {
                g_vu0.WriteMicroMem(addr, payload.data(), payload.size());
            }
            break;
            }

        case CMD_MSCAL:
            g_logFile << "[VIF" << v << "] MSCAL: Starting VU" << v 
                      << " at address 0x" << std::hex << (immediate * 8) << std::dec << std::endl;
{            if (g_cpuContext) {
                uint32_t addr = immediate * 8;
                if (v == 1) g_vu1.Execute(*g_cpuContext, addr);
                else        g_vu0.Execute(*g_cpuContext, addr);
            }
            break;}
        default:
            if (g_logFile.is_open()) {
                g_logFile << "[VIF" << v << "] Unhandled command: 0x" 
                          << std::hex << cmd << std::dec << std::endl;
            }
            break;
    }
}

// ============================================================================
// UNPACK Implementation (with full STCYCL, MODE, and MASK support)
// ============================================================================
void VIF::ExecuteUnpack(int v, uint32_t cmd, uint32_t immediate, uint32_t num,
                        const std::vector<uint8_t>& payload, bool use_mask) {
    g_logFile << "[VIF" << v << "] UNPACK: CMD=0x" 
              << std::hex << cmd << " IMM=0x" << immediate 
              << " NUM=" << std::dec << num 
              << " PAYLOAD_SIZE=" << payload.size() << " bytes"
              << (use_mask ? " [MASKED]" : "") << std::endl;
    
    // Decode immediate field
    bool use_tops    = (immediate & 0x8000) != 0;  // Bit 15: Add TOPS to address
    bool is_unsigned = (immediate & 0x4000) != 0;  // Bit 14: Zero-extend (1) or Sign-extend (0)
    uint32_t addr_qw = immediate & 0x3FF;          // Bits 0-9: Destination address in QW
    
    // Apply TOPS if requested (VIF1 only)
    if (use_tops && v == 1) {
        addr_qw = (addr_qw + regs[v].tops) & 0x3FF;
    }
    
    // Decode format
    uint32_t format = cmd & 0x0F;
    uint32_t bits = UNPACK_BITS[format];
    uint32_t src_elems = UNPACK_ELEMS[format];
    
    if (bits == 0) return;  // Invalid format
    
    bool is_s_type = (format <= 0x03);
    bool is_v4_5   = (format == 0x0F);
    
    // Number of vectors to unpack
    uint32_t vectors_to_unpack = (num == 0) ? 256 : num;
    
    // STCYCL values
    uint32_t cl = regs[v].GetCL();
    uint32_t wl = regs[v].GetWL();
    if (cl == 0) cl = 1;
    if (wl == 0) wl = 1;
    
    // Mode for addition
    uint32_t mode = regs[v].mode;
    
    const uint8_t* src_ptr = payload.data();
    const uint8_t* src_end = payload.data() + payload.size();
    
    // Track write cycle for STCYCL and masking
    uint32_t write_cycle = 0;  // Cycles through 0 to WL-1 for mask rows
    
    for (uint32_t i = 0; i < vectors_to_unpack && src_ptr < src_end; i++) {
        // Calculate cycle position for STCYCL
        uint32_t cycle_pos = i % cl;
        
        // Check if this is a write cycle or skip cycle
        bool do_write = (cycle_pos < wl);

        if (!do_write) {
            // Skip cycle: Still need to read source data to advance pointer, but won't write
            addr_qw = (addr_qw + 1) & 0x3FF; // Address increments even on skip
            g_logFile << "[VIF" << v << "] UNPACK: Cycle " << i 
                      << " is a SKIP cycle (CL=" << cl << ", WL=" << wl << ")" << std::endl;
            continue;

        }
        
        // Read source data into temporary vector
        uint32_t vec[4] = {0, 0, 0, 0};
        
        if (is_v4_5) {
            // V4-5: 16-bit packed (5-5-5-1 format)
            if (src_ptr + 2 <= src_end) {
                uint16_t raw;
                std::memcpy(&raw, src_ptr, 2);
                src_ptr += 2;
                
                // Extract: R(0-4), G(5-9), B(10-14), A(15)
                // Scale 5-bit to 8-bit range (multiply by 8, or shift left 3)
                vec[0] = ((raw >>  0) & 0x1F) << 3;  // R (X)
                vec[1] = ((raw >>  5) & 0x1F) << 3;  // G (Y)
                vec[2] = ((raw >> 10) & 0x1F) << 3;  // B (Z)
                vec[3] = ((raw >> 15) & 0x01) * 0xFF; // A (W) - 0 or 255
            }
        }
        else if (is_s_type) {
            // S-Type: Read 1 value, replicate to all 4 components
            uint32_t val = ReadUnpackValue(src_ptr, bits, is_unsigned);
            vec[0] = vec[1] = vec[2] = vec[3] = val;
        }
        else {
            // V-Type: Read src_elems values
            for (uint32_t e = 0; e < src_elems && src_ptr < src_end; e++) {
                vec[e] = ReadUnpackValue(src_ptr, bits, is_unsigned);
            }
        }
        
        // Apply MODE (addition decompression)
        if (mode != 0) {
            uint32_t col_idx = write_cycle & 0x3;  // Column index for COL addition
            
            for (int f = 0; f < 4; f++) {
                uint32_t add_val = 0;
                
                if (mode == 1 || mode == 3) {
                    // Add ROW
                    add_val += regs[v].row[f];
                }
                if (mode == 2 || mode == 3) {
                    // Add COL (indexed by write cycle)
                    add_val += regs[v].col[col_idx];
                }
                
                vec[f] += add_val;
            }
        }
        
        // Write to VU memory (if this is a write cycle)
        if (do_write) {
            uint32_t dest_addr = (addr_qw * 16) & 0x3FFF;  // 16KB wrap for VU1
            
            // Apply masking if enabled
            if (use_mask) {
                // Read existing data
                uint32_t existing[4];
                std::memcpy(existing, &vu1_data_memory[dest_addr], 16);
                
                // Apply mask per field
                uint32_t final_vec[4];
                uint32_t mask_row = write_cycle & 0x3;
                
                for (int f = 0; f < 4; f++) {
                    MaskAction action = GetMaskAction(v, f, mask_row);
                    
                    switch (action) {
                        case MASK_WRITE:
                            final_vec[f] = vec[f];
                            break;
                        case MASK_NOWRITE:
                            final_vec[f] = existing[f];
                            break;
                        case MASK_ROW:
                            final_vec[f] = regs[v].row[f];
                            break;
                        case MASK_COL:
                            final_vec[f] = regs[v].col[mask_row];
                            break;
                    }
                }
                
                g_vu1.WriteDataMem(dest_addr, reinterpret_cast<uint8_t*>(final_vec), 16);
            }
            else {
                // No masking - write full vector
                g_vu1.WriteDataMem(dest_addr, reinterpret_cast<uint8_t*>(vec), 16);
            }
            
            // Increment write cycle counter
            write_cycle++;
            if (write_cycle >= wl) write_cycle = 0;
        }
        
        // Address always increments (even on skip cycles)
        addr_qw = (addr_qw + 1) & 0x3FF;
    }
    
    if (g_logFile.is_open()) {
        g_logFile << "[VIF" << v << "] UNPACK complete: " << vectors_to_unpack << " vectors" << std::endl;
    }
}

// ============================================================================
// Register Read/Write
// ============================================================================
uint32_t VIF::Read(int vif_num, uint32_t addr) {
    uint32_t offset = addr & 0xFF;
    
    switch (offset) {
        case 0x00: return regs[vif_num].stat;
        case 0x10: return regs[vif_num].fbrst;
        case 0x20: return regs[vif_num].err;
        case 0x30: return regs[vif_num].mark;
        case 0x40: return regs[vif_num].cycle;
        case 0x50: return regs[vif_num].mode;
        case 0x60: return regs[vif_num].num;
        case 0x70: return regs[vif_num].mask;
        case 0x80: return regs[vif_num].code;
        case 0x90: return regs[vif_num].itops;
        case 0xA0: return (vif_num == 1) ? regs[vif_num].base : 0;
        case 0xB0: return (vif_num == 1) ? regs[vif_num].ofst : 0;
        case 0xC0: return (vif_num == 1) ? regs[vif_num].tops : 0;
        case 0xD0: return regs[vif_num].itop;
        case 0xE0: return (vif_num == 1) ? regs[vif_num].top : 0;
        default:
            // ROW/COL registers
            if (offset >= 0x100 && offset < 0x110) {
                return regs[vif_num].row[(offset - 0x100) / 4];
            }
            if (offset >= 0x140 && offset < 0x150) {
                return regs[vif_num].col[(offset - 0x140) / 4];
            }
            return 0;
    }
}

void VIF::Write(int vif_num, uint32_t addr, uint32_t value) {
    uint32_t offset = addr & 0xFF;
    
    switch (offset) {
        case 0x00:
            // STAT - some bits are read-only, some are write-to-clear
            // Writing 1 to bits 6,8,9,10,11,12,13 clears them
            regs[vif_num].stat &= ~(value & 0x3F40);
            break;
            
        case 0x10:
            // FBRST - Force break/reset
            regs[vif_num].fbrst = value;
            if (value & 1) {
                // RST - Reset VIF
                fifo[vif_num].clear();
                state[vif_num] = VIF_STATE_IDLE;
                pending_bytes[vif_num] = 0;
            }
            if (value & 2) {
                // FBK - Force break
                regs[vif_num].stat |= VIF_STAT::VFS;
            }
            if (value & 4) {
                // STP - Stop
                regs[vif_num].stat |= VIF_STAT::VSS;
            }
            if (value & 8) {
                // STC - Stall cancel (clear VSS, VFS, VIS, INT, ER0, ER1)
                regs[vif_num].stat &= ~(VIF_STAT::VSS | VIF_STAT::VFS | VIF_STAT::VIS | 
                                        VIF_STAT::INT | VIF_STAT::ER0 | VIF_STAT::ER1);
            }
            break;
            
        case 0x20:
            regs[vif_num].err = value & 0x7;
            break;
            
        case 0x30:
            regs[vif_num].mark = value;
            regs[vif_num].stat &= ~VIF_STAT::MRK;  // Clear MRK flag on write
            break;
            
        case 0x40:
            regs[vif_num].cycle = value;
            break;
            
        case 0x50:
            regs[vif_num].mode = value & 0x3;
            break;
            
        case 0x70:
            regs[vif_num].mask = value;
            break;
            
        default:
            // ROW registers
            if (offset >= 0x100 && offset < 0x110) {
                regs[vif_num].row[(offset - 0x100) / 4] = value;
            }
            // COL registers
            else if (offset >= 0x140 && offset < 0x150) {
                regs[vif_num].col[(offset - 0x140) / 4] = value;
            }
            break;
    }
}