#include "gs.h"
#include "render.h"
#include <fstream>
extern std::ofstream g_logFile;
void GS_Reset() {
    // 1. Zero out all privileged registers
    std::memset(&g_gs_regs, 0, sizeof(GsRegs));

    // 2. Reset CSR to default state (Signal bit usually on, FIFO empty)
    // This value (0x1400) indicates FIFO Empty (bit 10) + Finish (bit 1)
    g_gs_regs.GS_CSR = 0x1402; 

    if (g_logFile.is_open()) {
        g_logFile << "[GS] System Reset Performed." << std::endl;
    }
}

void WritePrivilegedLower(uint32_t addr, uint32_t value) {
    // Only process the lower 32 bits. 
    // We treat this exactly like a 64-bit write where the upper 32 bits are 0.
    WritePrivilegedRegister(addr, static_cast<uint64_t>(value));
}

void WritePrivilegedUpper(uint32_t addr, uint32_t value) {
    // Shift the 32-bit value to the upper half.
    // The lower half is 0, so it won't trigger "Write-1-to-Clear" on bits 0-4.
    WritePrivilegedRegister(addr, static_cast<uint64_t>(value) << 32);
}

void WritePrivilegedRegister(uint32_t addr, uint64_t value) {
    // We only care about the offset from 0x1200xxxx
    uint32_t offset = addr & 0xFFFF;

    switch (offset) {
        // ====================================================================
        // CRT Control & Synchronization
        // ====================================================================
        case 0x0000: // PMODE (Page Mode)
            g_gs_regs.PMODE = value;
            if (g_logFile.is_open()) {
                g_logFile << "[GS] PMODE: " 
                          << " ReadCircuit1=" << ((value & 1) ? "ON" : "OFF")
                          << " ReadCircuit2=" << ((value & 2) ? "ON" : "OFF")
                          << " Alpha=" << ((value >> 5) & 0xFF) << std::endl;
            }
            break;

        case 0x0010: // SMODE1 (Setup Mode 1)
            g_gs_regs.SMODE1 = value;
            // Controls PLL settings, rarely needed for high-level emulation
            break;

        case 0x0020: // SMODE2 (Setup Mode 2)
            g_gs_regs.SMODE2 = value;
            g_video_state.interlaced = (value & 1);
            if (g_logFile.is_open()) {
                g_logFile << "[GS] SMODE2: " 
                          << (g_video_state.interlaced ? "Interlaced" : "Progressive") 
                          << " FFMD=" << ((value >> 1) & 1) // Field/Frame mode
                          << " DPMS=" << ((value >> 2) & 3) << std::endl;
            }
            break;

        case 0x0030: // SRFSH (Screen Refresh)
            g_gs_regs.SRFSH = value;
            break;

        case 0x0040: // SYNCH1 (HSync/VSync Timing)
            g_gs_regs.SYNCH1 = value;
            break;

        case 0x0050: // SYNCH2 (HSync/VSync Timing)
            g_gs_regs.SYNCH2 = value;
            break;

        case 0x0060: // SYNCV (Vertical Timing)
            g_gs_regs.SYNCV = value;
            break;

        case 0x0070: // DISPFB1
        {
            g_gs_regs.DISPFB1 = value;
            uint32_t fbp = value & 0x1FF;
            uint32_t fbw = (value >> 9) & 0x3F;
            uint32_t psm = (value >> 15) & 0x1F;

            // QUEUE THE CHANGE
            RenderJob job;
            job.type = RenderCommandType::SetFrontBuffer;
            job.args.arg1 = fbp * 8192; // Address in Bytes
            job.args.arg2 = fbw * 64;   // Width in Pixels
            job.args.arg3 = psm;        // Format
            g_renderQueue.Push(job);
            break;
        }

        case 0x0080: // DISPLAY1
        {
            // Skip if value unchanged
            if (g_gs_regs.DISPLAY1 == value) break;
            g_gs_regs.DISPLAY1 = value;
            
            // Parse DISPLAY register correctly
            uint32_t magh = (value >> 23) & 0xF;   // Horizontal magnification - 1
            uint32_t magv = (value >> 27) & 0x3;   // Vertical magnification - 1
            uint32_t dw   = (value >> 32) & 0xFFF; // Display width - 1 (in VCKs)
            uint32_t dh   = (value >> 44) & 0x7FF; // Display height - 1
            
            // Calculate actual pixel dimensions
            int width  = (dw + 1) / (magh + 1);
            int height = (dh + 1) / (magv + 1);
            
            // Only push if dimensions are valid and changed
            static int last_w1 = 0, last_h1 = 0;
            if (width > 0 && height > 0 && (width != last_w1 || height != last_h1)) {
                last_w1 = width;
                last_h1 = height;
                
                RenderJob job;
                job.type = RenderCommandType::SetWindow;
                job.args.arg1 = 1;
                job.args.arg2 = width;
                job.args.arg3 = height;
                g_renderQueue.Push(job);
                
                if (g_logFile.is_open()) {
                    g_logFile << "[GS] DISPLAY1: " << width << "x" << height 
                            << " (DW=" << dw << " DH=" << dh 
                            << " MAGH=" << magh << " MAGV=" << magv << ")" << std::endl;
                }
            }
            break;
        }

        // ====================================================================
        // Background Color
        // ====================================================================
        case 0x00E0: // BGCOLOR
        {
            g_gs_regs.BGCOLOR = value;
            uint8_t r = value & 0xFF;
            uint8_t g = (value >> 8) & 0xFF;
            uint8_t b = (value >> 16) & 0xFF;

            // QUEUE THE CHANGE
            RenderJob job;
            job.type = RenderCommandType::SetClearColor;
            job.args.arg1 = r;
            job.args.arg2 = g;
            job.args.arg3 = b;
            g_renderQueue.Push(job);
            break;
        }

        // ====================================================================
        // Context 2 Display Buffers
        // ====================================================================
        case 0x0090: // DISPFB2
        {
            g_gs_regs.DISPFB2 = value;
            uint32_t fbp = value & 0x1FF;
            uint32_t fbw = (value >> 9) & 0x3F;
            uint32_t psm = (value >> 15) & 0x1F;

            RenderJob job;
            job.type = RenderCommandType::SetFrontBuffer;
            job.args.arg1 = fbp * 8192;
            job.args.arg2 = fbw * 64;
            job.args.arg3 = psm;
            g_renderQueue.Push(job);
            break;
        }

        case 0x00A0: // DISPLAY2
        {
            if (g_gs_regs.DISPLAY2 == value) break;
            g_gs_regs.DISPLAY2 = value;
            
            uint32_t magh = (value >> 23) & 0xF;
            uint32_t magv = (value >> 27) & 0x3;
            uint32_t dw   = (value >> 32) & 0xFFF;
            uint32_t dh   = (value >> 44) & 0x7FF;
            
            int width  = (dw + 1) / (magh + 1);
            int height = (dh + 1) / (magv + 1);
            
            static int last_w2 = 0, last_h2 = 0;
            if (width > 0 && height > 0 && (width != last_w2 || height != last_h2)) {
                last_w2 = width;
                last_h2 = height;
                
                RenderJob job;
                job.type = RenderCommandType::SetWindow;
                job.args.arg1 = 2;
                job.args.arg2 = width;
                job.args.arg3 = height;
                g_renderQueue.Push(job);
                
                if (g_logFile.is_open()) {
                    g_logFile << "[GS] DISPLAY2: " << width << "x" << height << std::endl;
                }
            }
            break;
        }

        // ====================================================================
        // External & Background
        // ====================================================================
        case 0x00B0: // EXTBUF
            g_gs_regs.EXTBUF = value;
            break;

        case 0x00C0: // EXTDATA
            g_gs_regs.EXTDATA = value;
            break;

        case 0x00D0: // EXTWRITE
            g_gs_regs.EXTWRITE = value;
            break;

        // ====================================================================
        // System Control (Critical)
        // ====================================================================
        case 0x1000: // CSR (Privileged Status Register)
        {
            // CSR is special: Writing 1 to certain bits CLEARS them (Reset/Flush)
            // Bit 0: Signal Event
            // Bit 1: Finish Event
            // Bit 2: HSint
            // Bit 3: VSint
            // Bit 4: EDWint
            // Bit 8: Flush (Reset FIFO)
            // Bit 9: Reset (Reset entire GS)
            
            uint64_t current = g_gs_regs.GS_CSR;
            
            // Handle RESET (Bit 9)
            if (value & (1 << 9)) {
                if (g_logFile.is_open()) g_logFile << "[GS] CSR: GS RESET Triggered!" << std::endl;
                GS_Reset(); // Call your internal GS Reset function
                // Keep the write behavior specific to your needs (usually clears everything)
                g_gs_regs.GS_CSR = 0; 
                return;
            }
            
            // Handle FIFO FLUSH (Bit 8)
            if (value & (1 << 8)) {
                if (g_logFile.is_open()) g_logFile << "[GS] CSR: FIFO Flush Triggered!" << std::endl;
                // Clear your GIF/GS FIFO buffers here
            }

            // For the interrupt flags (Bits 0-4), writing 1 clears the interrupt
            // We use XOR logic or AND-NOT logic depending on implementation.
            // Standard behavior: Write 1 to Clear.
            uint64_t write_mask = value & 0x1F; 
            g_gs_regs.GS_CSR &= ~write_mask;
            
            // Bits 5-7, 10-12 are Read-Only or status, usually ignored on write
            break;
        }

        case 0x1010: // IMR (Interrupt Mask Register)
            // Bit 8 (MSK) - Base mask
            // Bits 0-4 correspond to CSR interrupt bits
            g_gs_regs.GS_IMR = value;
            if (g_logFile.is_open()) {
                g_logFile << "[GS] IMR Update: 0x" << std::hex << value << std::dec << std::endl;
            }
            break;

        case 0x1040: // BUSDIR (Bus Direction)
            g_gs_regs.BUSDIR = value;
            break;

        case 0x1080: // SIGLBLID (Signal Label ID)
            g_gs_regs.SIGLBLID = value;
            break;

        default:
            if (g_logFile.is_open()) {
                g_logFile << "[GS] Warning: Write to unknown privileged register 0x" 
                          << std::hex << addr << std::dec << " Value: 0x" << value << std::endl;
            }
            break;
    }
}


uint64_t ReadPrivilegedRegister(uint32_t address) {
    // Mask off the base 0x12000000 to get the offset
    // (Assuming this function is called for range 0x12000000 - 0x12001080)
    uint32_t offset = address & 0xFFFF;

    switch (offset) {
        // ====================================================================
        // CRT / Display Control
        // ====================================================================
        case 0x0000: // PMODE (Page Mode) - 64-bit
            return g_gs_regs.PMODE;

        case 0x0010: // SMODE1 (Setup Mode 1) - 64-bit
            return g_gs_regs.SMODE1;

        case 0x0020: // SMODE2 (Setup Mode 2: Interlace/PAL/NTSC) - 64-bit
            return g_gs_regs.SMODE2;

        case 0x0030: // SRFSH (Screen Refresh) - 64-bit
            return g_gs_regs.SRFSH;

        case 0x0040: // SYNCH1 (HSync/VSync timing) - 64-bit
            return g_gs_regs.SYNCH1;

        case 0x0050: // SYNCH2 (HSync/VSync timing) - 64-bit
            return g_gs_regs.SYNCH2;

        case 0x0060: // SYNCV (Vertical timing) - 64-bit
            return g_gs_regs.SYNCV;

        // ====================================================================
        // Buffer Offsets (Context 1)
        // ====================================================================
        case 0x0070: // DISPFB1 (Display Framebuffer 1 info) - 64-bit
            return g_gs_regs.DISPFB1;

        case 0x0080: // DISPLAY1 (Display 1 position/size) - 64-bit
            return g_gs_regs.DISPLAY1;

        // ====================================================================
        // Buffer Offsets (Context 2)
        // ====================================================================
        case 0x0090: // DISPFB2 (Display Framebuffer 2 info) - 64-bit
            return g_gs_regs.DISPFB2;

        case 0x00A0: // DISPLAY2 (Display 2 position/size) - 64-bit
            return g_gs_regs.DISPLAY2;

        // ====================================================================
        // External / Misc
        // ====================================================================
        case 0x00B0: // EXTBUF (External Buffer) - 64-bit
            return g_gs_regs.EXTBUF;

        case 0x00C0: // EXTDATA (External Data) - 64-bit
            return g_gs_regs.EXTDATA;

        case 0x00D0: // EXTWRITE (External Write) - 64-bit
            return g_gs_regs.EXTWRITE;

        case 0x00E0: // BGCOLOR (Background Color) - 64-bit
            return g_gs_regs.BGCOLOR;

        // ====================================================================
        // System Control (Critical)
        // ====================================================================
        case 0x1000: // CSR (Privileged Status Register) - 64-bit
            // Note: This register contains dynamic status flags!
            // You might need to OR in the current FIFO status or VSync status here.
            {
                uint64_t val = g_gs_regs.GS_CSR;
                // Example: Determine if FIFO is empty (Bit 10)
                // if (fifo.isEmpty()) val |= (1 << 10);
                return val;
            }

        case 0x1010: // IMR (Interrupt Mask Register) - 64-bit
            return g_gs_regs.GS_IMR;

        case 0x1040: // BUSDIR (Bus Direction) - 64-bit
            return g_gs_regs.BUSDIR;

        case 0x1080: // SIGLBLID (Signal Label ID) - 64-bit
            return g_gs_regs.SIGLBLID;

        default:
            if (g_logFile.is_open()) {
                g_logFile << "[GS] Warning: Read from unknown privileged register 0x" 
                          << std::hex << address << std::dec << std::endl;
            }
            return 0;
    }
}