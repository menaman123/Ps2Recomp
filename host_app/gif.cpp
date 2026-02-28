// gif.cpp
#include "gif.h"
#include "memory.h"
#include "render.h"
#include <iostream>
#include <fstream>
#include "gs_state.h"
#include <iomanip>

extern std::ofstream g_logFile;

GIF g_gif;

void GIF::Reset() {
    ctrl = 0;
    mode = 0;
    stat = 0;
    
    std::memset(fifo.data(), 0, fifo.size());
    fifo_count = 0;
    
    path3_masked = false;
    
    std::memset(&current_tag, 0, sizeof(current_tag));
    current_loop = 0;
    current_reg = 0;
    in_transfer = false;
    
    // Clear the packet queue
    std::lock_guard<std::mutex> lock(queue_mutex);
    while (!packet_queue.empty()) {
        packet_queue.pop();
    }
    
    g_logFile << "[GIF] Reset complete" << std::endl;
}

uint32_t GIF::Read(uint32_t addr) {
    switch (addr) {
        case 0x10003000:  // GIF_CTRL (write-only, but some games read)
            return ctrl;
            
        case 0x10003010:  // GIF_MODE
            return mode;
            
        case 0x10003020: { // GIF_STAT
            // Build status register dynamically
            uint32_t status = 0;
            
            // Bit 0: PATH3 masked by GIF_MODE
            if (mode & 1) status |= (1 << 0);
            
            // Bit 1: PATH3 masked by VIF1
            if (path3_masked) status |= (1 << 1);
            
            // Bit 2: IMT - Intermittent mode
            if (mode & 4) status |= (1 << 2);
            
            // Bit 3: PSE - Temporary stop
            if (ctrl & 8) status |= (1 << 3);
            
            // Bits 6-8: PATH queued flags
            // Bit 6: PATH3 queued
            // Bit 7: PATH2 queued  
            // Bit 8: PATH1 queued
            
            // Bit 9: OPH - Output path (transfer ongoing)
            if (in_transfer) status |= (1 << 9);
            
            // Bits 10-11: APTS - Active path
            // 0=Idle, 1=PATH1, 2=PATH2, 3=PATH3
            if (in_transfer) {
                status |= (3 << 10);  // Assume PATH3 for now
            }
            
            // Bits 24-28: FQC - FIFO quadword count
            status |= ((fifo_count / 16) & 0x1F) << 24;
            
            return status;
        }
        
        case 0x10003040:  // GIF_TAG0 (bits 0-31 of last tag)
            return (uint32_t)(current_tag.nloop | 
                             (current_tag.eop << 15));
            
        case 0x10003050:  // GIF_TAG1 (bits 32-63)
            return (uint32_t)((current_tag.pre << 14) |
                             (current_tag.prim << 15) |
                             ((uint32_t)current_tag.flg << 26) |
                             ((uint32_t)current_tag.nregs << 28));
            
        case 0x10003060:  // GIF_TAG2 (bits 64-95 - first 8 regs)
            return (current_tag.regs[0]) |
                   (current_tag.regs[1] << 4) |
                   (current_tag.regs[2] << 8) |
                   (current_tag.regs[3] << 12) |
                   (current_tag.regs[4] << 16) |
                   (current_tag.regs[5] << 20) |
                   (current_tag.regs[6] << 24) |
                   (current_tag.regs[7] << 28);
            
        case 0x10003070:  // GIF_TAG3 (bits 96-127 - last 8 regs)
            return (current_tag.regs[8]) |
                   (current_tag.regs[9] << 4) |
                   (current_tag.regs[10] << 8) |
                   (current_tag.regs[11] << 12) |
                   (current_tag.regs[12] << 16) |
                   (current_tag.regs[13] << 20) |
                   (current_tag.regs[14] << 24) |
                   (current_tag.regs[15] << 28);
            
        case 0x10003080:  // GIF_CNT
            return (current_tag.nloop - current_loop) |
                   (current_reg << 16);
            
        case 0x10003090:  // GIF_P3CNT
            return 0;  // PATH3 loop counter when interrupted
            
        case 0x100030A0:  // GIF_P3TAG
            return 0;  // PATH3 tag when interrupted
            
        default:
            g_logFile << "[GIF] Unknown read @ 0x" << std::hex << addr << std::endl;
            return 0;
    }
}

void GIF::Write(uint32_t addr, uint32_t value) {
    switch (addr) {
        case 0x10003000:  // GIF_CTRL
            if (value & 1) {
                // Reset GIF
                Reset();
                g_logFile << "[GIF] Software reset triggered" << std::endl;
            }
            if (value & 8) {
                // Temporary stop
                ctrl |= 8;
                g_logFile << "[GIF] Temporary stop" << std::endl;
            } else {
                ctrl &= ~8;
            }
            break;
            
        case 0x10003010:  // GIF_MODE
            mode = value & 0x7;  // Only bits 0-2 are used
            
            // Bit 0: PATH3 mask
            if (value & 1) {
                g_logFile << "[GIF] PATH3 masked by MODE" << std::endl;
            }
            
            // Bit 2: Intermittent mode
            if (value & 4) {
                g_logFile << "[GIF] Intermittent mode enabled" << std::endl;
            }
            break;
            
        default:
            g_logFile << "[GIF] Unknown write @ 0x" << std::hex << addr 
                      << " = 0x" << value << std::endl;
            break;
    }
}

void GIF::ReceiveData(GIFPath path, const uint8_t* data, size_t size) {
    if (size == 0) return;

    g_logFile << "[GIF] ReceiveData: PATH" << (int)path 
              << " size=" << size << " bytes"
              << (in_transfer ? " (DATA)" : " (expecting TAG)") << std::endl;

    // Only interpret as tag if we're NOT mid-transfer
    if (!in_transfer && size >= 16) {
        uint64_t tag_lo, tag_hi;
        std::memcpy(&tag_lo, data, 8);
        std::memcpy(&tag_hi, data + 8, 8);
        
        uint16_t nloop = tag_lo & 0x7FFF;
        uint8_t flg = (tag_lo >> 58) & 0x3;
        const char* flg_names[] = {"PACKED", "REGLIST", "IMAGE", "DISABLE"};
        
        g_logFile << "[GIF]   Tag preview: FLG=" << flg_names[flg] 
                  << " NLOOP=" << nloop << std::endl;
    }

    size_t current_pos = dma_buffer.size();
    dma_buffer.resize(current_pos + size);
    std::memcpy(dma_buffer.data() + current_pos, data, size);

    ProcessBuffer();
}

/* void GIF::ProcessBuffer() {
    size_t processed_bytes = 0;
    
    // Safety check for empty buffer
    if (dma_buffer.empty()) return;

    // Use a persistent loop to process as much data as available
    while (processed_bytes < dma_buffer.size()) {
        
        // --- PHASE 1: Reading a New GIF Tag ---
        if (!in_transfer) {
            // A GIF Tag is always 128 bits (16 bytes)
            if (dma_buffer.size() - processed_bytes < 16) {
                g_logFile << "[GIF] Buffer partial: Need 16 bytes for Tag, have " 
                          << (dma_buffer.size() - processed_bytes) << ". Waiting." << std::endl;
                break; 
            }
            
            // Read 128-bit Tag
            uint64_t tag_lo, tag_hi;
            std::memcpy(&tag_lo, dma_buffer.data() + processed_bytes, 8);
            std::memcpy(&tag_hi, dma_buffer.data() + processed_bytes + 8, 8);
            
            current_tag.Parse(tag_lo, tag_hi);
            processed_bytes += 16;
            
            // Initialize Transfer State
            in_transfer = true;
            current_loop = 0;
            current_reg = 0;

            g_logFile << "[GIF] NEW TAG PARSED | NLOOP=" << current_tag.nloop 
                      << " EOP=" << current_tag.eop
                      << " PRE=" << current_tag.pre
                      << " PRIM=0x" << std::hex << current_tag.prim
                      << " FLG=" << (int)current_tag.flg
                      << " NREGS=" << (int)current_tag.nregs << std::dec << std::endl;

            // Handle PRE (Primitive Set) immediately if active
            if (current_tag.pre) {
                ProcessPacked(GS_PRIM, reinterpret_cast<const uint8_t*>(&current_tag.prim));
                g_logFile << "   -> PRE Active: Set PRIM to 0x" << std::hex << current_tag.prim << std::dec << std::endl;
            }

            // Handle NLOOP=0 (Tag only, no data payload)
            if (current_tag.nloop == 0) {
                g_logFile << "[GIF] NLOOP is 0. Packet finished immediately." << std::endl;
                if (current_tag.eop) {
                     g_logFile << "[GIF] EOP reached (Empty Payload)." << std::endl;
                     // Optional: Trigger end-of-packet logic here if needed
                }
                in_transfer = false;
                continue; // Loop back to see if there is another tag in the buffer
            }
        }
        
        // --- PHASE 2: Processing Data Payload ---
        
        // We are currently transferring data for the active tag.
        // We need to determine how many bytes the current operation requires.
        
        size_t bytes_needed = 0;
        GIFFormat fmt = current_tag.flg;

        if (fmt == GIFFormat::PACKED) {
            bytes_needed = 16; // PACKED is always 128-bit (16 bytes)
        } 
        else if (fmt == GIFFormat::REGLIST) {
            bytes_needed = 8;  // REGLIST is 64-bit (8 bytes) per register
        } 
        else if (fmt == GIFFormat::IMAGE) {
            bytes_needed = 16; // IMAGE is just raw 128-bit QWords to HWREG
        }
        else {
             // Disabled/Invalid
             g_logFile << "[GIF] CRITICAL ERROR: Unknown/Disabled GIF Format " << (int)fmt << std::endl;
             in_transfer = false;
             return; 
        }

        // Check if we have enough data in the buffer for ONE operation
        if (dma_buffer.size() - processed_bytes < bytes_needed) {
            g_logFile << "[GIF] Buffer partial: Need " << bytes_needed << " bytes for " 
                      << (fmt == GIFFormat::PACKED ? "PACKED" : "REGLIST/IMAGE") 
                      << " data, have " << (dma_buffer.size() - processed_bytes) 
                      << ". Stalling." << std::endl;
            break; // Stop processing and wait for next DMA chunk
        }

        // --- Execute the Operation ---
        const uint8_t* data_ptr = dma_buffer.data() + processed_bytes;

        if (fmt == GIFFormat::IMAGE) {
            // IMAGE mode ignores the register list and just pumps QWords to HWREG
            ProcessImage(data_ptr, 1); 
            
            // Image mode treats "NLOOP" as the total QWord count.
            // It does not loop over registers.
            current_loop++;
            if (current_loop >= current_tag.nloop) {
                in_transfer = false;
                g_logFile << "[GIF] IMAGE Transfer Complete." << std::endl;
            }
        } 
        else {
            // PACKED or REGLIST
            uint8_t reg = current_tag.regs[current_reg];

            if (fmt == GIFFormat::PACKED) {
                ProcessPacked(reg, data_ptr);
            } else {
                ProcessReglist(reg, data_ptr);
            }
            
            // Advance State Machine
            current_reg++;
            if (current_reg >= current_tag.nregs) {
                current_reg = 0;
                current_loop++;
                
                // Debug log for every loop completion (useful if it crashes mid-packet)
                // g_logFile << "[GIF] Finished Loop " << current_loop << "/" << current_tag.nloop << std::endl;
                
                if (current_loop >= current_tag.nloop) {
                    in_transfer = false;
                    g_logFile << "[GIF] Packet Data Complete (NLOOP reached)." << std::endl;
                }
            }
        }
        
        // Advance buffer pointer
        processed_bytes += bytes_needed;

        // Check EOP *after* the transfer finishes
        if (!in_transfer && current_tag.eop) {
            g_logFile << "[GIF] EOP Signal Processed. Dispatching Batch." << std::endl;
            
            if (!g_gs_state.draw_buffer.empty()) {
                RenderJob job;
                job.type = RenderCommandType::DrawBatch;
                job.batch.vertices = std::move(g_gs_state.draw_buffer);
                job.batch.prim_type = g_gs_state.prim_type;
                
                // Important: GS State needs to be copied or managed if it changes between batches
                // For now, we assume simple batching.
                
                g_renderQueue.Push(job);
                g_gs_state.draw_buffer.clear();
                
                g_logFile << "[GIF] Queued Batch to Graphics Thread (" 
                          << job.batch.vertices.size() << " verts)" << std::endl;
            }
        }
    }

    // 3. Remove processed data from buffer
    // Optimization: If we processed everything, just clear.
    if (processed_bytes > 0) {
        if (processed_bytes == dma_buffer.size()) {
            dma_buffer.clear();
            // g_logFile << "[GIF] Buffer fully drained." << std::endl;
        } else {
            dma_buffer.erase(dma_buffer.begin(), dma_buffer.begin() + processed_bytes);
            g_logFile << "[GIF] Buffer drained " << processed_bytes << " bytes. " 
                      << dma_buffer.size() << " bytes remaining." << std::endl;
        }
    }
}
 */

void GIF::FlushBatch() {
    if (g_gs_state.draw_buffer.empty()) return;
    
    // === DIAGNOSTIC OUTPUT ===
    g_gs_state.DumpState(g_logFile);
    g_gs_state.DiagnoseBatch(g_logFile);
    if (g_gs_state.texture) {
        int ctx = g_gs_state.context;
        g_gs_state.DumpVRAMRegion(
            g_gs_state.tex0[ctx].tbp,
            1 << g_gs_state.tex0[ctx].tw,
            1 << g_gs_state.tex0[ctx].th,
            g_gs_state.tex0[ctx].psm
        );
    }
    
    g_logFile << "[GIF] Flushing Batch: " 
              << g_gs_state.draw_buffer.size() << " verts, Type " 
              << (int)g_gs_state.prim_type << std::endl;

    RenderJob job;
    job.type = RenderCommandType::DrawBatch;
    job.batch.vertices = std::move(g_gs_state.draw_buffer);
    job.batch.prim_type = g_gs_state.prim_type;
    
    // Push to queue
    g_renderQueue.Push(job);
    
    // Clear local buffer
    g_gs_state.draw_buffer.clear();
}

/*
 void GIF::ProcessBuffer() {
    size_t processed_bytes = 0;
    
    if (dma_buffer.empty()) return;
    if (mode & 1) {
        return; 
    }
    while (processed_bytes < dma_buffer.size()) {
        
        // PHASE 1: Parse new GIFtag if not in transfer
        if (!in_transfer) {
            if (dma_buffer.size() - processed_bytes < 16) {
                break; // Need more data for tag
            }
            
            // Read 128-bit GIFtag
            uint64_t tag_lo, tag_hi;
            std::memcpy(&tag_lo, dma_buffer.data() + processed_bytes, 8);
            std::memcpy(&tag_hi, dma_buffer.data() + processed_bytes + 8, 8);
            
            g_logFile << "[GIF] RAW: lo=0x" << std::hex << std::setw(16) 
            << std::setfill('0') << tag_lo
            << " hi=0x" << std::setw(16) << tag_hi << std::dec << std::endl;

            // Parse tag fields
            current_tag.nloop = tag_lo & 0x7FFF;           // bits 0-14
            current_tag.eop   = (tag_lo >> 15) & 1;        // bit 15
            current_tag.pre   = (tag_lo >> 46) & 1;        // bit 46
            current_tag.prim  = (tag_lo >> 47) & 0x7FF;    // bits 47-57
            current_tag.flg   = static_cast<GIFFormat>((tag_lo >> 58) & 0x3); // bits 58-59
            current_tag.nregs = (tag_lo >> 60) & 0xF;      // bits 60-63
            if (current_tag.nregs == 0) current_tag.nregs = 16;
            
            // Extract register descriptors from hi word (each is 4 bits)
            for (int i = 0; i < 16; i++) {
                current_tag.regs[i] = (tag_hi >> (i * 4)) & 0xF;
            }
            
            processed_bytes += 16;
            
            // Validate tag - catch obviously corrupt data
            bool valid = true;
            
            // NLOOP sanity check based on remaining data
            size_t remaining_bytes = dma_buffer.size() - processed_bytes;
            size_t max_possible_loops = 0;
            
            if (current_tag.flg == GIFFormat::PACKED) {
                max_possible_loops = remaining_bytes / (16);
            } else if (current_tag.flg == GIFFormat::REGLIST) {
                max_possible_loops = remaining_bytes / (8 * current_tag.nregs);
            } else if (current_tag.flg == GIFFormat::IMAGE) {
                max_possible_loops = remaining_bytes / 16;
            }
            
            // Log the parsed tag
            g_logFile << "[GIF] TAG: NLOOP=" << current_tag.nloop 
                      << " EOP=" << (int)current_tag.eop
                      << " PRE=" << (int)current_tag.pre
                      << " PRIM=0x" << std::hex << current_tag.prim << std::dec
                      << " FLG=" << (int)current_tag.flg
                      << " NREGS=" << (int)current_tag.nregs;
            
            // Log register descriptors
            g_logFile << " REGS=[";
            for (int i = 0; i < current_tag.nregs; i++) {
                if (i > 0) g_logFile << ",";
                g_logFile << std::hex << (int)current_tag.regs[i];
            }
            g_logFile << "]" << std::dec << std::endl;
            
            // Warn if NLOOP seems too large
            if (current_tag.nloop > 0 && current_tag.nloop > max_possible_loops + 100) {
                g_logFile << "[GIF] WARNING: NLOOP=" << current_tag.nloop 
                          << " seems large for " << remaining_bytes << " remaining bytes" << std::endl;
            }
            
            in_transfer = true;
            current_loop = 0;
            current_reg = 0;
            
            // Apply PRIM if PRE is set
            if (current_tag.pre) {
                g_gs_state.SetPrim(current_tag.prim);
                g_logFile << "[GIF] PRE: Set PRIM to 0x" << std::hex << current_tag.prim 
                          << " (type=" << (current_tag.prim & 0x7) << ")" << std::dec << std::endl;
            }
            
            // Handle NLOOP=0 (tag only, no payload)
            if (current_tag.nloop == 0) {
                g_logFile << "[GIF] NLOOP=0, no payload" << std::endl;
                in_transfer = false;
                continue;
            }
        }
        
        // PHASE 2: Process data payload
        size_t bytes_needed = 0;
        GIFFormat fmt = current_tag.flg;
        
        if (fmt == GIFFormat::PACKED) {
            bytes_needed = 16; // 128 bits per register
        } else if (fmt == GIFFormat::REGLIST) {
            bytes_needed = 8;  // 64 bits per register
        } else if (fmt == GIFFormat::IMAGE) {
            bytes_needed = 16; // 128 bits per HWREG write
        } else {
            g_logFile << "[GIF] ERROR: Invalid format " << (int)fmt << std::endl;
            in_transfer = false;
            break;
        }
        
        if (dma_buffer.size() - processed_bytes < bytes_needed) {
            break; // Wait for more data
        }
        
        const uint8_t* data_ptr = dma_buffer.data() + processed_bytes;
        
        if (fmt == GIFFormat::IMAGE) {
            ProcessImage(data_ptr, 1);
            current_loop++; // Increment every quadword
        } 
        else if (fmt == GIFFormat::PACKED) {
            uint8_t reg = current_tag.regs[current_reg];
            ProcessPacked(reg, data_ptr);
            
            current_reg++;
            if (current_reg >= current_tag.nregs) current_reg = 0;
            
            current_loop++;
        } 
        else if (fmt == GIFFormat::REGLIST) {
            // REGLIST processes registers 64-bits at a time; 1 QW = 2 Registers
            // Your current ProcessReglist logic might need to handle the two halves of the QW
            uint8_t reg = current_tag.regs[current_reg];
            ProcessReglist(reg, data_ptr);
            
            current_reg++;
            if (current_reg >= current_tag.nregs) {
                current_reg = 0;
                current_loop++; // Increment only after the full register list is done
            }
        }
        
        processed_bytes += bytes_needed;
        
        // Check EOP after transfer finishes
        if (!in_transfer && current_tag.eop) {
            g_logFile << "[GIF] EOP - flushing " << g_gs_state.draw_buffer.size() << " vertices" << std::endl;
            
            if (!g_gs_state.draw_buffer.empty()) {
                RenderJob job;
                job.type = RenderCommandType::DrawBatch;
                job.batch.vertices = std::move(g_gs_state.draw_buffer);
                job.batch.prim_type = g_gs_state.prim_type;
                g_renderQueue.Push(job);
                g_gs_state.draw_buffer.clear();
            }
        }
    }
    
    // Remove processed data
    if (processed_bytes > 0) {
        if (processed_bytes == dma_buffer.size()) {
            dma_buffer.clear();
        } else {
            dma_buffer.erase(dma_buffer.begin(), dma_buffer.begin() + processed_bytes);
        }
    }
}
*/

void GIF::ProcessBuffer() {
    size_t processed_bytes = 0;
    
    if (dma_buffer.empty()) return;

    // --- NEW FIX: PATH 3 MASKING ---
    // If the GIF is masked by GIF_MODE (bit 0), we must not process PATH3 data.
    // Documentation: "Data sent by PATH3 will reside in the FIFO until the mask is lifted."
    if (mode & 1) {
        return; 
    }

    while (processed_bytes < dma_buffer.size()) {
        
        // PHASE 1: Parse new GIFtag if not currently in a transfer
        if (!in_transfer) {
            if (dma_buffer.size() - processed_bytes < 16) {
                break; // Wait for the full 16-byte tag
            }
            
            uint64_t tag_lo, tag_hi;
            std::memcpy(&tag_lo, dma_buffer.data() + processed_bytes, 8);
            std::memcpy(&tag_hi, dma_buffer.data() + processed_bytes + 8, 8);
            
            current_tag.Parse(tag_lo, tag_hi);
            processed_bytes += 16;
            
            // --- NEW FIX: Q-REGISTER INITIALIZATION ---
            // Documentation: "The GS Q register is initialized to 1.0f when reading a GIFtag."
            g_gs_state.q = 1.0f;

            in_transfer = true;
            current_loop = 0;
            current_reg = 0;
            
            // Apply PRIM immediately if PRE is set
            if (current_tag.pre) {
                g_gs_state.SetPrim(current_tag.prim);
            }
            
            // Handle NLOOP=0 (Documentation: "All fields ignored except EOP")
            if (current_tag.nloop == 0) {
                
                if (current_tag.eop) {
                    FlushBatch(); // Finalize if EOP set
                    in_transfer = false;
                }
                continue;
            }
        }
        
        // PHASE 2: Process data payload
        // All GIF data units are physically 128-bit (16 bytes) in the DMA stream
        size_t bytes_needed = 16; 
        GIFFormat fmt = current_tag.flg;
        
        if (dma_buffer.size() - processed_bytes < bytes_needed) {
            break; // Stall and wait for more DMA data
        }
        
        const uint8_t* data_ptr = dma_buffer.data() + processed_bytes;
        
        if (fmt == GIFFormat::IMAGE) {
            // IMAGE mode: NLOOP is the total number of quadwords
            g_logFile << "[GIF] IMAGE Mode: Processing QW " << current_loop + 1 
                      << " of " << current_tag.nloop << std::endl;
            ProcessImage(data_ptr, 1);
            current_loop++;
        } 
        else if (fmt == GIFFormat::PACKED) {
            // PACKED mode: 1 Quadword = 1 Register update
            uint8_t reg = current_tag.regs[current_reg];
            ProcessPacked(reg, data_ptr);
            
            current_reg++;
            
            // FIX: Only increment loop count when we have finished the full register list
            if (current_reg >= current_tag.nregs) {
                current_reg = 0;
                current_loop++; 
            }
        }
        else if (fmt == GIFFormat::REGLIST) {
            // REGLIST mode: 1 Quadword = 2 Register updates (64-bits each)
            // NLOOP is the number of times the ENTIRE register list is processed
            
            // First Register (bits 0-63)
            ProcessReglist(current_tag.regs[current_reg], data_ptr);
            current_reg++;
            if (current_reg >= current_tag.nregs) {
                current_reg = 0;
                current_loop++;
            }

            // Second Register (bits 64-127) - Only if NLOOP not already reached
            if (current_loop < current_tag.nloop) {
                ProcessReglist(current_tag.regs[current_reg], data_ptr + 8);
                current_reg++;
                if (current_reg >= current_tag.nregs) {
                    current_reg = 0;
                    current_loop++;
                }
            }
        }

        processed_bytes += bytes_needed;
        
        // Finalize transfer if target reached
        if (current_loop >= current_tag.nloop) {
            in_transfer = false;
            
            // Check EOP (End of Packet)
            if (current_tag.eop) {
                FlushBatch();
            }
        }
    }
    
    // Remove processed data from the DMA buffer
    if (processed_bytes > 0) {
        if (processed_bytes == dma_buffer.size()) {
            dma_buffer.clear();
        } else {
            dma_buffer.erase(dma_buffer.begin(), dma_buffer.begin() + processed_bytes);
        }
    }
}

void GIF::ProcessPacket(GIFPath path, const uint8_t* data, size_t size) {
    // This function processes a complete packet at once
    // Used when we know we have the full packet (e.g., from XGKICK)
    g_logFile << "[GIF] ProcessPacket PATH" << (int)path << " size=" << size << std::endl;
    ReceiveData(path, data, size);
}

void GIF::FinishDMA() {
    // If we're mid-transfer, force-finish it
    if (in_transfer) {

        if(current_tag.flg == GIFFormat::IMAGE && current_loop < current_tag.nloop) {
            g_logFile << "[GIF] FinishDMA: Mid-IMAGE transfer (loop=" 
                      << current_loop << "/" << current_tag.nloop << ") - preserving state for next DMA." << std::endl;

            return;

        }

        g_logFile << "[GIF] WARNING: DMA finished but GIF packet incomplete (loop=" 
                  << current_loop << "/" << current_tag.nloop 
                  << ", reg=" << current_reg << ")" << std::endl;
        in_transfer = false;
    }
    

    if(!in_transfer && !g_gs_state.draw_buffer.empty()) {
        g_gs_state.Flush();
        FlushBatch();

    }
    
    // Clear any leftover partial data
    dma_buffer.clear();
}


void GIF::ProcessPacked(uint8_t reg, const uint8_t* data) {
    uint64_t lo, hi;
    std::memcpy(&lo, data, 8);
    std::memcpy(&hi, data + 8, 8);
    
    switch (reg) {
        case GS_PRIM: {
            FlushBatch();
            uint32_t prim_val = lo & 0x7FF;
            g_gs_state.SetPrim(prim_val);
            g_logFile << "[GIF] PACKED PRIM: type=" << (prim_val & 0x7) 
                      << " IIP=" << ((prim_val >> 3) & 1)
                      << " TME=" << ((prim_val >> 4) & 1)
                      << " FGE=" << ((prim_val >> 5) & 1)
                      << " ABE=" << ((prim_val >> 6) & 1)
                      << " AA1=" << ((prim_val >> 7) & 1)
                      << " FST=" << ((prim_val >> 8) & 1)
                      << " CTXT=" << ((prim_val >> 9) & 1)
                      << " FIX=" << ((prim_val >> 10) & 1) << std::endl;
            break;
        }
        
        case GS_RGBAQ: {
            uint8_t r = lo & 0xFF;
            uint8_t g = (lo >> 32) & 0xFF;
            uint8_t b = hi & 0xFF;
            uint8_t a = (hi >> 32) & 0xFF;
            g_gs_state.r = r;
            g_gs_state.g = g;
            g_gs_state.b = b;
            g_gs_state.a = a;
            g_logFile << "[GIF] PACKED RGBAQ: R=" << (int)r << " G=" << (int)g 
                      << " B=" << (int)b << " A=" << (int)a << std::endl;
            break;
        }
        
        case GS_ST: {
            float s, t;
            std::memcpy(&s, &lo, 4);
            std::memcpy(&t, ((uint8_t*)&lo) + 4, 4);
            float q;
            std::memcpy(&q, &hi, 4);
            g_gs_state.s = s;
            g_gs_state.t = t;
            if (q != 0.0f) g_gs_state.q = q;
            g_logFile << "[GIF] PACKED ST: S=" << s << " T=" << t 
                      << " Q=" << g_gs_state.q << std::endl;
            break;
        }
        
        case GS_UV: {
            uint16_t u = lo & 0x3FFF;
            uint16_t v = (lo >> 16) & 0x3FFF;
            g_gs_state.u = u;
            g_gs_state.v = v;
            g_logFile << "[GIF] PACKED UV: U=" << u << " V=" << v << std::endl;
            break;
        }
        
        case GS_XYZF2:
        case GS_XYZF3: {
            uint16_t x_raw = lo & 0xFFFF;
            uint16_t y_raw = (lo >> 32) & 0xFFFF;
            uint32_t z_raw = (hi >> 4) & 0xFFFFFF;
            uint8_t f = (hi >> 36) & 0xFF;
            bool adc = (hi >> 47) & 1;
            float x_final = (float)((int16_t)x_raw) / 16.0f;
            float y_final = (float)((int16_t)y_raw) / 16.0f;
            float z = (float)z_raw;
            bool draw = (reg == GS_XYZF2) && !adc;
            g_gs_state.KickVertex(x_final, y_final, z, f, draw);
            g_logFile << "[GIF] PACKED " << (reg == GS_XYZF2 ? "XYZF2" : "XYZF3")
                      << ": X=" << x_final << " Y=" << y_final << " Z=" << z 
                      << " F=" << (int)f << " ADC=" << adc 
                      << " draw=" << draw << std::endl;
            break;
        }
        
        case GS_XYZ2:
        case GS_XYZ3: {
            // PACKED XYZ: X[0:15], Y[32:47], Z[64:95], ADC[111]
            uint16_t x_raw = lo & 0xFFFF;
            uint16_t y_raw = (lo >> 32) & 0xFFFF;
            uint32_t z_raw = hi & 0xFFFFFFFF;
            bool adc = (hi >> 47) & 1;
            float x_final = (float)((int16_t)x_raw) / 16.0f;
            float y_final = (float)((int16_t)y_raw) / 16.0f;
            bool draw = (reg == GS_XYZ2) && !adc;
            g_gs_state.KickVertex(x_final, y_final, (float)z_raw, 0, draw);
            g_logFile << "[GIF] PACKED " << (reg == GS_XYZ2 ? "XYZ2" : "XYZ3")
                      << ": X=" << x_final << " Y=" << y_final << " Z=" << z_raw
                      << " ADC=" << adc << " draw=" << draw << std::endl;
            break;
        }
        
        case GS_FOG: {
            uint8_t f = (hi >> 36) & 0xFF;
            g_gs_state.fog = f;
            g_logFile << "[GIF] PACKED FOG: F=" << (int)f << std::endl;
            break;
        }
        
        case GS_AD: {
            uint64_t data_val = lo;
            uint8_t addr = hi & 0xFF;
            g_logFile << "[GIF] PACKED A+D: Addr=0x" << std::hex << (int)addr 
                      << " Data=0x" << data_val << std::dec << std::endl;
            ProcessReglist(addr, (const uint8_t*)&data_val);
            break;
        }
        
        case GS_NOP:
            break;
            
        default:
            // For all other registers in PACKED mode, the low 64 bits
            // are written directly as the register value (same as REGLIST)
            g_logFile << "[GIF] PACKED reg 0x" << std::hex << (int)reg 
                      << " → routing to REGLIST (lo=0x" << lo << ")" 
                      << std::dec << std::endl;
            ProcessReglist(reg, (const uint8_t*)&lo);
            break;
    }
}

void GIF::ProcessReglist(uint8_t reg, const uint8_t* data) {
    uint64_t value;
    std::memcpy(&value, data, 8);
    
    switch (reg) {
        case GS_PRIM:

            FlushBatch();
            g_gs_state.SetPrim(value & 0x7FF);
            g_logFile << "[GIF] REGLIST PRIM = 0x" << std::hex << (value & 0x7FF) << std::dec << std::endl;
            break;
            
        case GS_RGBAQ: {
            g_gs_state.r = value & 0xFF;
            g_gs_state.g = (value >> 8) & 0xFF;
            g_gs_state.b = (value >> 16) & 0xFF;
            g_gs_state.a = (value >> 24) & 0xFF;
            // Q is in bits 32-63
            float q;
            uint32_t q_bits = (value >> 32) & 0xFFFFFFFF;
            std::memcpy(&q, &q_bits, 4);
            if (q != 0.0f) g_gs_state.q = q;
            g_logFile << "[GIF] REGLIST RGBAQ: R=" << (int)g_gs_state.r 
                      << " G=" << (int)g_gs_state.g 
                      << " B=" << (int)g_gs_state.b 
                      << " A=" << (int)g_gs_state.a 
                      << " Q=" << g_gs_state.q << std::endl;
            break;
        }
        
        case GS_ST: {
            float s, t;
            std::memcpy(&s, data, 4);
            std::memcpy(&t, data + 4, 4);
            g_gs_state.s = s;
            g_gs_state.t = t;
            g_logFile << "[GIF] REGLIST ST: S=" << s << " T=" << t << std::endl;
            break;
        }
        
        case GS_UV: {
            g_gs_state.u = value & 0x3FFF;
            g_gs_state.v = (value >> 16) & 0x3FFF;
            g_logFile << "[GIF] REGLIST UV: U=" << g_gs_state.u << " V=" << g_gs_state.v << std::endl;
            break;
        }
        
        case GS_XYZF2:
        case GS_XYZF3: {
            // REGLIST XYZF: X[0:15], Y[16:31], Z[32:55], F[56:63]
            uint16_t x_raw = value & 0xFFFF;
            uint16_t y_raw = (value >> 16) & 0xFFFF;
            uint32_t z_raw = (value >> 32) & 0xFFFFFF;
            uint8_t f = (value >> 56) & 0xFF;
            
            float x = x_raw / 16.0f;
            float y = y_raw / 16.0f;
            
            bool draw = (reg == GS_XYZF2);
            g_gs_state.KickVertex(x, y, (float)z_raw, f, draw);
            g_logFile << "[GIF] REGLIST " << (reg == GS_XYZF2 ? "XYZF2" : "XYZF3")
                      << ": X=" << x << " Y=" << y << " Z=" << z_raw << " F=" << (int)f << std::endl;
            break;
        }
        
        case GS_XYZ2:
        case GS_XYZ3: {
            // REGLIST XYZ (no fog): X[0:15], Y[16:31], Z[32:63]
            uint16_t x_raw = value & 0xFFFF;
            uint16_t y_raw = (value >> 16) & 0xFFFF;
            uint32_t z_raw = (value >> 32) & 0xFFFFFFFF;
            
            float x = x_raw / 16.0f;
            float y = y_raw / 16.0f;
            
            bool draw = (reg == GS_XYZ2);
            g_gs_state.KickVertex(x, y, (float)z_raw, 0, draw);
            g_logFile << "[GIF] REGLIST " << (reg == GS_XYZ2 ? "XYZ2" : "XYZ3")
                      << ": X=" << x << " Y=" << y << " Z=" << z_raw << std::endl;
            break;
        }
        
        // =====================================================================
        // FRAME / ZBUF - Drawing target configuration
        // =====================================================================
        case GS_FRAME_1: 
            g_logFile << "[GIF] REGLIST FRAME 1 = 0x" << std::hex << value << std::dec << std::endl;
            g_gs_state.SetFrame(0, value); 
            break;
        case GS_FRAME_2: 
            g_logFile << "[GIF] REGLIST FRAME 2 = 0x" << std::hex << value << std::dec << std::endl;
            g_gs_state.SetFrame(1, value); 
            break;
            
        case GS_ZBUF_1:  
            g_logFile << "[GIF] REGLIST ZBUF 1 = 0x" << std::hex << value << std::dec << std::endl;
            g_gs_state.SetZBuf(0, value); 
            break;
        case GS_ZBUF_2:  
            g_logFile << "[GIF] REGLIST ZBUF 2 = 0x" << std::hex << value << std::dec << std::endl;
            g_gs_state.SetZBuf(1, value); 
            break;
        
        // =====================================================================
        // TEX0 / TEX1 - Texture configuration
        // =====================================================================
        case GS_TEX0_1:  
            g_logFile << "[GIF] REGLIST TEX0_1 = 0x" << std::hex << value << std::dec << std::endl;
            g_gs_state.SetTex0(0, value); 
            break;
        case GS_TEX0_2:  
            g_logFile << "[GIF] REGLIST TEX0_2 = 0x" << std::hex << value << std::dec << std::endl;
            g_gs_state.SetTex0(1, value); 
            break;
            
        case GS_TEX1_1:
            g_logFile << "[GIF] REGLIST TEX1_1 = 0x" << std::hex << value << std::dec << std::endl; 
            g_gs_state.SetTex1(0, value);
            break;
        case GS_TEX1_2:
            g_logFile << "[GIF] REGLIST TEX1_2 = 0x" << std::hex << value << std::dec << std::endl; 
            g_gs_state.SetTex1(1, value);
            break;
        
        // =====================================================================
        // SCISSOR / XYOFFSET - Clipping and coordinate offset
        // =====================================================================
        case GS_SCISSOR_1: 
            g_logFile << "[GIF] REGLIST SCISSOR 1 = 0x" << std::hex << value << std::dec << std::endl;
            g_gs_state.SetScissor(0, value); 
            break;
        case GS_SCISSOR_2: 
            g_logFile << "[GIF] REGLIST SCISSOR 2 = 0x" << std::hex << value << std::dec << std::endl;
            g_gs_state.SetScissor(1, value); 
            break;

        case GS_XYOFFSET_1: 
            g_logFile << "[GIF] REGLIST XYOFFSET 1 = 0x" << std::hex << value << std::dec << std::endl;
            g_gs_state.SetXYOffset(0, value); 
            break;
        case GS_XYOFFSET_2: 
            g_logFile << "[GIF] REGLIST XYOFFSET 2 = 0x" << std::hex << value << std::dec << std::endl;
            g_gs_state.SetXYOffset(1, value); 
            break;
        
        // =====================================================================
        // TEST / ALPHA - Pixel tests and blending
        // =====================================================================
        case GS_TEST_1:  
            g_logFile << "[GIF] REGLIST TEST 1 = 0x" << std::hex << value << std::dec << std::endl;
            g_gs_state.SetTest(0, value); 
            break;
        case GS_TEST_2:  
            g_logFile << "[GIF] REGLIST TEST 2 = 0x" << std::hex << value << std::dec << std::endl;
            g_gs_state.SetTest(1, value); 
            break;
            
        case GS_ALPHA_1: 
            g_logFile << "[GIF] REGLIST ALPHA 1 = 0x" << std::hex << value << std::dec << std::endl;
            g_gs_state.SetAlpha(0, value); 
            break;
        case GS_ALPHA_2: 
            g_logFile << "[GIF] REGLIST ALPHA 2 = 0x" << std::hex << value << std::dec << std::endl;
            g_gs_state.SetAlpha(1, value); 
            break;
        
        // =====================================================================
        // CLAMP - Texture clamping/wrapping
        // =====================================================================
        case GS_CLAMP_1:
            g_logFile << "[GIF] REGLIST CLAMP 1 = 0x" << std::hex << value << std::dec << std::endl;
            g_gs_state.SetClamp(0, value);
            break;
        case GS_CLAMP_2:
            g_logFile << "[GIF] REGLIST CLAMP 2 = 0x" << std::hex << value << std::dec << std::endl;
            g_gs_state.SetClamp(1, value);
            break;
            
        // =====================================================================
        // Texture transfer registers (BITBLT)
        // =====================================================================
        case GS_BITBLTBUF:
            g_logFile << "[GIF] REGLIST BITBLTBUF = 0x" << std::hex << value << std::dec << std::endl;
            g_gs_state.SetBitBltBuf(value);
            break;
        case GS_TRXPOS:
            g_logFile << "[GIF] REGLIST TRXPOS = 0x" << std::hex << value << std::dec << std::endl;
            g_gs_state.SetTrxPos(value);
            break;
        case GS_TRXREG:
            g_logFile << "[GIF] REGLIST TRXREG = 0x" << std::hex << value << std::dec << std::endl;
            g_gs_state.SetTrxReg(value);
            break;
        case GS_TRXDIR:
            g_logFile << "[GIF] REGLIST TRXDIR = 0x" << std::hex << value << std::dec << std::endl;
            g_gs_state.SetTrxDir(value);
            break;
        case GS_HWREG:
            g_logFile << "[GIF] REGLIST HWREG = 0x" << std::hex << value << std::dec << std::endl;  
            g_gs_state.WriteHWReg(value);
            break;
            
        // =====================================================================
        // Interrupt / sync registers
        // =====================================================================
        case GS_SIGNAL:
            g_logFile << "[GIF] REGLIST SIGNAL = 0x" << std::hex << value << std::dec << std::endl;
            // TODO: Raise SIGNAL interrupt
            break;
        case GS_FINISH:
            g_logFile << "[GIF] REGLIST FINISH = 0x" << std::hex << value << std::dec << std::endl;
            // TODO: Raise FINISH interrupt
            break;
        case GS_LABEL:
            g_logFile << "[GIF] REGLIST LABEL = 0x" << std::hex << value << std::dec << std::endl;
            break;
            
        // =====================================================================
        // Misc registers
        // =====================================================================
        case GS_TEXFLUSH: // 0x3F
            g_logFile << "[GIF] REGLIST TEXFLUSH" << std::endl;
            // Texture cache invalidation - important for texture uploads
            break;
            
        case GS_PRMODECONT: // 0x1A
            g_logFile << "[GIF] REGLIST PRMODECONT = " << (value & 1) << std::endl;
            // 0 = Use PRMODE, 1 = Use PRIM
            break;
            
        case GS_PRMODE: // 0x1B
            g_logFile << "[GIF] REGLIST PRMODE = 0x" << std::hex << value << std::dec << std::endl;
            // Alternative to PRIM bits 3-10
            break;
            
        case GS_TEXA: // 0x3B
            g_logFile << "[GIF] REGLIST TEXA = 0x" << std::hex << value << std::dec << std::endl;
            // Texture alpha settings
            break;
            
        case GS_FOGCOL: // 0x3D
            g_logFile << "[GIF] REGLIST FOGCOL = 0x" << std::hex << value << std::dec << std::endl;
            // Fog color
            break;
            
        case GS_COLCLAMP: // 0x46
            g_logFile << "[GIF] REGLIST COLCLAMP = " << (value & 1) << std::endl;
            // 0 = Mask, 1 = Clamp
            break;
            
        case GS_DTHE: // 0x45
            g_logFile << "[GIF] REGLIST DTHE = " << (value & 1) << std::endl;
            // Dithering enable
            break;
            
        case GS_DIMX: // 0x44
            g_logFile << "[GIF] REGLIST DIMX = 0x" << std::hex << value << std::dec << std::endl;
            // Dither matrix
            break;
            
        case GS_PABE: // 0x49
            g_logFile << "[GIF] REGLIST PABE = " << (value & 1) << std::endl;
            // Per-pixel alpha blending
            break;
            
        case GS_FBA_1: // 0x4A
        case GS_FBA_2: // 0x4B
            g_logFile << "[GIF] REGLIST FBA_" << ((reg == GS_FBA_2) ? 2 : 1) 
                      << " = " << (value & 1) << std::endl;
            // Frame buffer alpha fix
            break;
            
        case GS_MIPTBP1_1: // 0x34
        case GS_MIPTBP1_2: // 0x35
        case GS_MIPTBP2_1: // 0x36
        case GS_MIPTBP2_2: // 0x37
            g_logFile << "[GIF] REGLIST MIPTBP = 0x" << std::hex << value << std::dec << std::endl;
            // Mipmap texture base pointers
            break;
            
        case GS_TEX2_1: // 0x16
        case GS_TEX2_2: // 0x17
            g_logFile << "[GIF] REGLIST TEX2 = 0x" << std::hex << value << std::dec << std::endl;
            // Additional texture params (PSM, CBP, etc)
            break;
            
        case GS_SCANMSK: // 0x22
            g_logFile << "[GIF] REGLIST SCANMSK = " << (value & 3) << std::endl;
            // Scanline masking for interlacing
            break;
            
        case GS_FOG: // 0x0A
            g_gs_state.fog_coef = (value >> 56) & 0xFF;
            g_logFile << "[GIF] REGLIST FOG = " << (int)g_gs_state.fog_coef << std::endl;
            break;
            
        case GS_NOP:
            break;
            
        default:
            g_logFile << "[GIF] REGLIST unhandled reg 0x" << std::hex << (int)reg 
                      << " = 0x" << value << std::dec << std::endl;
            break;
    }
}


void GIF::ProcessImage(const uint8_t* data, size_t qwords) {
    g_logFile << "[GIF] IMAGE: " << qwords << " QWs (" << (qwords * 16) << " bytes)" << std::endl;
    
    if (!g_gs_state.transfer.active) {
        g_logFile << "[GIF] WARNING: IMAGE data but no transfer active!" << std::endl;
        return;
    }
    
    for (size_t i = 0; i < qwords; i++) {
        uint64_t data_lo, data_hi;
        std::memcpy(&data_lo, data + (i * 16), 8);
        std::memcpy(&data_hi, data + (i * 16) + 8, 8);
        
        g_gs_state.WriteHWReg(data_lo);
        g_gs_state.WriteHWReg(data_hi);
    }
}