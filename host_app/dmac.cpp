#include "dmac.h"
#include "memory.h"
#include "intc.h"
#include "ps2_scheduler.h"
#include <iostream>
#include "recompiled.h"
#include <iomanip>
#include "sif.h"
#include "gif.h"
#include "vif.h"
#include <math.h>

extern std::ofstream g_logFile;

DMAC g_dmac;
// Definition of the global queue
std::map<int, std::vector<DmacHandler>> g_dmac_queues;

// Static counter for unique Handler IDs
int g_next_dmac_handler_id = 1;
void DMAC::Reset() {
for (int i = 0; i < DMA_COUNT; i++) {
 channels[i] = DMAChannelRegs{};
    }
    ctrl = 0;
    stat = 0;
    pcr = 0;
    sqwc = 0;
    rbsr = 0;
    rbor = 0;
    stadr = 0;
    enabler = 0x1201;  // Default enable state
    enablew = 0;
}

struct SifCmdHeader {
    uint32_t psize_dsize; // combined fields
    uint32_t dest;
    uint32_t cid;         // Command ID
    uint32_t opt;
};

void DMAC::ProcessSifDma(int ch) {
    // 1. Get the source address from the channel registers
    // Note: If using Chain Mode, you need to walk the tag. 
    // For simple SIF transfers, typically MADR points to the data.
    uint32_t addr = channels[ch].madr;
    
    // In HLE, we often need to peek into the memory to see what the game is sending.
    // The logs show addresses like 0x203cac80 (Uncached segment).
    
    // 2. Read the SIF Command Header
    // We read 4 uint32s to make up the 16-byte header
    uint32_t psize_dsize = memory::read<uint32_t>(addr);
    uint32_t dest_addr   = memory::read<uint32_t>(addr + 4);
    uint32_t command_id  = memory::read<uint32_t>(addr + 8);
    uint32_t opt         = memory::read<uint32_t>(addr + 12);
    
    uint32_t psize = psize_dsize & 0xFF; 
        uint32_t dsize = (psize_dsize >> 8); // Payload size in bytes

        g_logFile << "SIF1 HLE: Processing Packet at 0x" << std::hex << addr << std::dec << std::endl;
        g_logFile << "  Header: CID=0x" << std::hex << command_id 
                << " Opt=" << opt 
                << " PSize=" << psize 
                << " DSize=" << dsize << std::dec << std::endl;

        // Log the actual payload bytes (limit to ~64 bytes to avoid spam)
        g_logFile << "  Payload Raw: ";
        for (uint32_t i = 0; i < std::min(dsize, (uint32_t)64); i += 4) {
            uint32_t data = memory::read<uint32_t>(addr + 16 + i);
            g_logFile << std::hex << std::setw(8) << std::setfill('0') << data << " ";
        }
        g_logFile << std::dec << std::endl;
    // 3. Handle specific SIF Commands
    switch (command_id) {
        case 0x80000002: // SIFCMD_INIT
            g_logFile << "  SIFCMD_INIT detected." << std::endl;
            // The IOP acknowledges initialization by setting specific flags.
            // When opt=0, IOP sets SMFLG to 0x20000
            if (opt == 0) {
                 // Write to the SIF_SMFLAG register (IOP -> EE flag)
                 // Physical address 0x1000F230 (See memory map or dmac.cpp Write)
                 memory::write<uint32_t>(0x1000F230, 0x20000); 
                 g_logFile << "  SIF HLE: Set SMFLAG to 0x20000" << std::endl;
            }
            break;

        case 0x80000003: // SIFCMD_RESET (Reboot IOP)
            g_logFile << "  SIFCMD_RESET detected. Simulating IOP Reboot." << std::endl;
            
            // 1. Reset SIF flags to simulate IOP going down
            g_sif.smflag = 0; 
            g_sif.msflag = 0;
            
            // 2. Write the reset state to memory
            memory::write<uint32_t>(0x1000F230, 0); // SIF_SMFLAG
            memory::write<uint32_t>(0x1000F220, 0); // SIF_MSFLAG

            // 3. Set the "Boot End" flag (0x40000)
            // In real hardware, this takes time. in HLE, we can set it immediately
            // or set it on the next SIF GetReg call if the game checks for 0 first.
            g_sif.smflag = 0x40000; 
            memory::write<uint32_t>(0x1000F230, g_sif.smflag);
            
            g_logFile << "  SIF HLE: Set SMFLAG to 0x40000 (Boot End)" << std::endl;
            break;
             
        case 0x80000009: // SIF_BIND (Bind RPC)
        {
            g_logFile << "  SIF_BIND detected." << std::endl;
            
            // Read the client data address and server ID from the packet
            // Packet structure: [0x00] Header, [0x1C] client_data_pointer, [0x20] server_id_requested
            uint32_t client_data_addr = memory::read<uint32_t>(addr + 0x1C);
            uint32_t server_id = memory::read<uint32_t>(addr + 0x20);
            
            g_logFile << "    Server ID: 0x" << std::hex << server_id << std::dec << std::endl;
            g_logFile << "    Client Struct At: 0x" << std::hex << client_data_addr << std::dec << std::endl;
            
            // FIX THE INFINITE LOOP:
            // The game is waiting for 'client_data->server' to become non-zero.
            // Write a dummy handle (0x1) to tell the game "Connection Successful".
            if (client_data_addr != 0) {
                // Offset 0x24 in SifRpcClientData is the 'server' handle
                memory::write<uint32_t>(client_data_addr + 0x24, 0x1);
                g_logFile << "    [SIF Action] Wrote Success Handle (0x1) to client->server" << std::endl;
            }
            break;
        }

        default:
            g_logFile << "  Unknown SIF Command ID." << std::endl;
            break;
    }
}

void DMAC::ProcessGifDmaChain() {
    auto& ch = channels[DMA_GIF];
    
    if (!(ch.chcr & CHCR_STR)) return;
    
    int mode = (ch.chcr >> 2) & 0x3;
    bool tte = (ch.chcr >> 6) & 1;  // Transfer tag enable
    
    g_logFile << "[DMAC] GIF DMA started (Chain Mode)" << std::endl;
    g_logFile << "  Mode: " << mode 
              << " MADR: 0x" << std::hex << ch.madr
              << " TADR: 0x" << ch.tadr
              << " QWC: " << std::dec << ch.qwc 
              << " CHCR: 0x" << std::hex << ch.chcr << std::dec << std::endl;
    
    if (mode != 1) {  // We only handle chain mode here
        g_logFile << "[DMAC] ERROR: ProcessGifDmaChain called with non-chain mode " << mode << std::endl;
        ch.chcr &= ~CHCR_STR;
        CompleteChannel(DMA_GIF);
        return;
    }

    // We'll collect ALL payload data from the chain into one buffer
    std::vector<uint8_t> full_payload;
    uint32_t current_tadr = ch.tadr & 0x0FFFFFFF;  // Physical address only
    bool tag_end = false;
    int tag_count = 0;
    const int MAX_TAGS = 10000;  // Safety

    g_logFile << "[DMAC] GIF Processing Source Chain (TADR start = 0x" << std::hex << current_tadr << ")" << std::dec << std::endl;

    while (!tag_end && tag_count < MAX_TAGS) {
        // Read 128-bit DMA tag
        uint64_t tag_lo = memory::read<uint64_t>(current_tadr);
        uint64_t tag_hi = memory::read<uint64_t>(current_tadr + 8);

        uint16_t qwc  = tag_lo & 0xFFFF;
        uint8_t  id   = (tag_lo >> 28) & 0x7;
        bool     irq  = (tag_lo >> 31) & 1;
        uint32_t addr = (tag_lo >> 32) & 0x7FFFFFF0;
        addr &= ~0xF;
        bool     spr  = (tag_lo >> 63) & 1;

        // Log tag
        g_logFile << "  [Tag " << tag_count << "] @ 0x" << std::hex << current_tadr 
                  << " ID=" << (int)id 
                  << " (" << (id==0?"REFE":id==1?"CNT":id==2?"NEXT":id==3?"REF":id==4?"REFS":id==5?"CALL":id==6?"RET":"END") << ")"
                  << " QWC=" << std::dec << qwc 
                  << " ADDR=0x" << std::hex << addr 
                  << " IRQ=" << irq 
                  << " SPR=" << spr << std::dec << std::endl;

        // Update CHCR.TAG field (bits 16-31) with bits 16-31 of tag_lo
        ch.chcr = (ch.chcr & 0x0000FFFF) | (tag_lo & 0xFFFF0000);

        // Determine source address and advance TADR
        uint32_t data_addr = 0;
        uint32_t data_bytes = qwc * 16;
        
        switch (id) {
            case 0: // REFE - Reference + End
                data_addr = addr;
                current_tadr += 16;
                tag_end = true;
                break;

            case 1: // CNT - Continue (data follows tag)
                data_addr = current_tadr + 16;
                current_tadr += 16 + data_bytes;
                break;

            case 2: // NEXT - Jump to ADDR after data
                data_addr = current_tadr + 16;
                current_tadr = addr;
                break;

            case 3: // REF - Reference, continue
                data_addr = addr;
                current_tadr += 16;
                break;

            case 4: // REFS - Reference + Stall control, continue
                data_addr = addr;
                current_tadr += 16;
                break;

            case 5: // CALL - Push return, jump to ADDR
            {
                data_addr = current_tadr + 16;
                int asp = (ch.chcr >> 4) & 3;
                
                if (asp == 0) {
                    ch.asr0 = current_tadr + 16 + data_bytes;
                    ch.chcr = (ch.chcr & ~(3 << 4)) | (1 << 4);  // ASP = 1
                } else if (asp == 1) {
                    ch.asr1 = current_tadr + 16 + data_bytes;
                    ch.chcr = (ch.chcr & ~(3 << 4)) | (2 << 4);  // ASP = 2
                } else {
                    // Stack overflow (ASP == 2) - undefined behavior on real HW
                    g_logFile << "[DMAC] WARNING: CALL with ASP=2 (stack overflow)" << std::endl;
                    // Continue anyway, real HW behavior unclear
                }
                current_tadr = addr;
                break;
            }

            case 6: // RET - Pop return address
            {
                data_addr = current_tadr + 16;
                int asp = (ch.chcr >> 4) & 3;
                if (asp == 2) {
                    current_tadr = ch.asr1;
                    ch.chcr = (ch.chcr & ~(3 << 4)) | (1 << 4);  // ASP = 1
                } else if (asp == 1) {
                    current_tadr = ch.asr0;
                    ch.chcr = (ch.chcr & ~(3 << 4));  // ASP = 0
                } else {
                    tag_end = true;  // Stack empty → end
                }
                break;
            }

            case 7: // END - End after data
            {
                data_addr = current_tadr + 16;
                current_tadr += 16 + data_bytes;
                tag_end = true;
                break;
            }

            default:
                g_logFile << "[DMAC] Unknown GIF tag ID " << (int)id << " - forcing end" << std::endl;
                tag_end = true;
                break;
        }

        // ===== UPDATE REGISTERS FOR GAMES THAT POLL =====
        // Update MADR to point to current data source
        ch.madr = data_addr;
        // Update TADR to next tag location
        ch.tadr = current_tadr;
        // Update QWC to current transfer size
        ch.qwc = qwc;

        // ===== TRANSFER PAYLOAD DATA =====
        if (data_bytes > 0) { 
            // Handle Scratchpad
            uint8_t* src;
            bool use_scratchpad = false;

            // For REFE, REF, REFS, CALL, NEXT - the SPR bit applies to the ADDR field
            if (id == 0 || id == 3) {
                use_scratchpad = spr;
            }
            // For CNT(1), NEXT(2), REFS(4), CALL(5), RET(6), END(7)
            // Data follows the tag or uses stack. Logic is derived from current location.
            else {
                uint32_t tag_addr = (id == 1) ? (data_addr - 16) : (data_addr - 16);
                use_scratchpad = ((tag_addr & 0x70000000) == 0x70000000);
            }
            // For CNT, RET, END - data follows tag, check if tag is in scratchpad
            // Note: We check against the ORIGINAL tag address, not current_tadr which may have moved

            if (use_scratchpad) {
                src = memory::translate_address(0x70000000 | (data_addr & 0x3FFF), data_bytes);
                g_logFile << "    Reading from Scratchpad @ 0x" << std::hex 
                          << (0x70000000 | (data_addr & 0x3FFF)) << std::dec << std::endl;
            } else {
                src = memory::translate_address(data_addr & 0x0FFFFFFF, data_bytes);
            }

            if (src) {
                full_payload.insert(full_payload.end(), src, src + data_bytes);
                g_logFile << "    Transferred " << data_bytes << " bytes from 0x" 
                          << std::hex << data_addr << std::dec << " to GIF payload buffer." << std::endl;
                
                // Debug: dump first 64 bytes
                g_logFile << "     Payload: " << std::hex;
                for (size_t i = 0; i < std::min((size_t)64, (size_t)data_bytes); i++) {
                    g_logFile << std::setw(2) << std::setfill('0') << (int)src[i];
                    if ((i + 1) % 16 == 0) g_logFile << std::endl << "              ";
                    else g_logFile << " ";
                }
                g_logFile << std::dec << std::endl;

                // Simulate QWC decrementing (for games that poll mid-transfer)
                ch.qwc = 0;  // Transfer complete for this tag
                ch.madr += data_bytes;  // MADR advances
            }
            else {
                g_logFile << "[DMAC] ERROR: Invalid data address 0x" << std::hex << data_addr 
                          << " for " << std::dec << data_bytes << " bytes" << std::endl;
            }
        }

        // Handle tag IRQ (for games that use TIE)
        if (irq && (ch.chcr & CHCR_TIE)) {
            g_logFile << "    Tag IRQ bit set (TIE enabled) - ending chain" << std::endl;
            tag_end = true;
            // Note: In full emulation you'd raise INT1 here
        }

        tag_count++;
    }

    if (tag_count >= MAX_TAGS) {
        g_logFile << "[DMAC] ERROR: Chain exceeded " << MAX_TAGS << " tags - possible infinite loop" << std::endl;
    }

    // Send the entire collected payload to GIF
    if (!full_payload.empty()) {
        g_logFile << "[DMAC] Sending full chain payload to GIF: " 
                  << (full_payload.size() / 16) << " quadwords" << std::endl;
        g_gif.ReceiveData(GIFPath::PATH3, full_payload.data(), full_payload.size());
    } else {
        g_logFile << "[DMAC] Chain had no payload data" << std::endl;
    }

    // Tell GIF the DMA chain is fully done
    g_gif.FinishDMA();

    // Chain complete - clear STR and finalize
    ch.chcr &= ~CHCR_STR;
    ch.qwc = 0;  // Final state
    CompleteChannel(DMA_GIF);
    g_logFile << "[DMAC] GIF chain transfer complete" << std::endl;
}

void DMAC::ProcessGifDmaNormal() {
    auto& ch = channels[DMA_GIF];
    
    if (ch.qwc == 0) return;
    
    g_logFile << "[DMAC] GIF Normal DMA Start" << std::endl;
    g_logFile << "  MADR=0x" << std::hex << ch.madr 
              << " QWC=" << std::dec << ch.qwc 
              << " Bytes=" << (ch.qwc * 16) << std::endl;
    
    TransferToGIF(ch.madr, ch.qwc);

    g_gif.FinishDMA();
    
    ch.madr += ch.qwc * 16;
    ch.qwc = 0;
}

void DMAC::TransferToGIF(uint32_t addr, uint32_t qwc) {
    if (qwc == 0) return;
    
    size_t size = qwc * 16;
    uint8_t* src = memory::translate_address(addr, size);
    
    if (src) {
        // DIAGNOSTIC: Hex dump first 64 bytes (4 quadwords = 1 GIFtag + 3 data)
        g_logFile << "[DMAC->GIF] Raw data from 0x" << std::hex << addr << ":" << std::endl;
        g_logFile << "  ";
        for (size_t i = 0; i < std::min(size, (size_t)64); i++) {
            g_logFile << std::hex << std::setw(2) << std::setfill('0') << (int)src[i];
            if ((i + 1) % 16 == 0) {
                g_logFile << std::endl;
                if (i + 1 < 64) g_logFile << "  ";
            } else {
                g_logFile << " ";
            }
        }
        g_logFile << std::dec << std::endl;
        
        g_gif.ReceiveData(GIFPath::PATH3, src, size);
    } else {
        g_logFile << "[DMAC] ERROR: Invalid address 0x" << std::hex << addr << std::dec << std::endl;
    }
}

uint32_t DMAC::Read(uint32_t addr) {
    uint32_t reg = addr & 0xFFFF;
    
    // Channel registers: 0x8000-0xD4FF
    if (reg >= 0x8000 && reg < 0xE000) {
        int ch = -1;
        uint32_t offset = 0;
        
        // Decode channel from address
        if (reg >= 0x8000 && reg < 0x9000) { ch = 0; offset = reg - 0x8000; }
        else if (reg >= 0x9000 && reg < 0xA000) { ch = 1; offset = reg - 0x9000; }
        else if (reg >= 0xA000 && reg < 0xB000) { ch = 2; offset = reg - 0xA000; }
        else if (reg >= 0xB000 && reg < 0xB400) { ch = 3; offset = reg - 0xB000; }
        else if (reg >= 0xB400 && reg < 0xC000) { ch = 4; offset = reg - 0xB400; }
        else if (reg >= 0xC000 && reg < 0xC400) { ch = 5; offset = reg - 0xC000; }
        else if (reg >= 0xC400 && reg < 0xC800) { ch = 6; offset = reg - 0xC400; }
        else if (reg >= 0xC800 && reg < 0xD000) { ch = 7; offset = reg - 0xC800; }
        else if (reg >= 0xD000 && reg < 0xD400) { ch = 8; offset = reg - 0xD000; }
        else if (reg >= 0xD400 && reg < 0xE000) { ch = 9; offset = reg - 0xD400; }
        
        if (ch >= 0) {
            switch (offset) {
                case 0x00: return channels[ch].chcr;
                case 0x10: return channels[ch].madr;
                case 0x20: return channels[ch].qwc;
                case 0x30: return channels[ch].tadr;
                case 0x40: return channels[ch].asr0;
                case 0x50: return channels[ch].asr1;
                case 0x80: return channels[ch].sadr;
            }
        }
    }
    
    // Global DMAC registers: 0xE000-0xEFFF
    switch (reg) {
        case 0xE000: return ctrl;
        case 0xE010: return stat;
        case 0xE020: return pcr;
        case 0xE030: return sqwc;
        case 0xE040: return rbsr;
        case 0xE050: return rbor;
        case 0xE060: return stadr;
    }
    
    // Enable registers: 0xF520, 0xF590
    if (reg == 0xF520) return enabler;
    if (reg == 0xF590) return 0;
    
    return 0;
}

int GetDmaChannelID(uint32_t address) {
    // Isolate the offset from 0x10000000
    uint32_t offset = address & 0xFFFF;
    
    switch (offset & 0xFF00) { // Check the "page"
        case 0x8000: return 0; // VIF0
        case 0x9000: return 1; // VIF1
        case 0xA000: return 2; // GIF
        case 0xB000: 
            return (offset & 0x400) ? 4 : 3; // 0xB400=Ch4, 0xB000=Ch3
        case 0xC000: 
            if (offset & 0x800) return 7;    // 0xC800=Ch7
            return (offset & 0x400) ? 6 : 5; // 0xC400=Ch6, 0xC000=Ch5
        case 0xD000: 
            return (offset & 0x400) ? 9 : 8; // 0xD400=Ch9, 0xD000=Ch8
        default: return -1;
    }
}

void DMAC::Write(uint32_t address, uint32_t value) {
    int channel_id = GetDmaChannelID(address);
    if (channel_id == -1) return; // Should catch this earlier, but safety first

    // Register Offset: The lower 8 bits usually indicate the register type
    // Note: Use 0xFF to catch SADR at 0x80
    int reg_offset = address & 0xFF; 

    // --- CHCR (Control) ---
    if (reg_offset == 0x00) {
        // 1. Extract Payload
        bool dir = (value >> 0) & 0x1; // determines read or write direction 0=read and 1=write
        bool str = (value >> 8) & 0x1;

        g_logFile << "DMAC DIRECTION: " << (dir) << std::endl;
        
        // 2. Update Internal State
        // Important: If we are about to run synchronously, we might
        // want to toggle STR off immediately after StartChannel.
        channels[channel_id].chcr = value; 

        // 3. Trigger
        if (str) {
            StartChannel(channel_id);
            
            // 4. Synchronous Cleanup (If StartChannel finishes instantly)
            // Clear the STR bit to indicate "Done"
            channels[channel_id].chcr &= ~(1 << 8); 
        } else {
            //PauseTransfer(channel_id);
            g_logFile << "DMAC: Channel " << channel_id << " STR cleared (paused/stopped)" << std::endl;
        }
    }
    // --- Other Registers ---
    else if (reg_offset == 0x10) { channels[channel_id].madr = value; }
    else if (reg_offset == 0x20) { channels[channel_id].qwc  = value; }
    else if (reg_offset == 0x30) { channels[channel_id].tadr = value; }
    else if (reg_offset == 0x40) { channels[channel_id].asr0 = value; } // Call Stack 0
    else if (reg_offset == 0x50) { channels[channel_id].asr1 = value; } // Call Stack 1
    else if (reg_offset == 0x80) { channels[channel_id].sadr = value; } // Scratchpad (Ch 8/9 only)
}

void DMAC::StartChannel(int ch) {
    g_logFile << "DMAC: Starting channel " << ch << std::endl;
    g_logFile << "  CHCR: 0x" << std::hex << channels[ch].chcr << std::dec << std::endl;
    g_logFile << "  MADR: 0x" << std::hex << channels[ch].madr << std::dec << std::endl;
    g_logFile << "  QWC:  " << channels[ch].qwc << std::endl;
    g_logFile << "  TADR: 0x" << std::hex << channels[ch].tadr << std::dec << std::endl;
    
    uint32_t chcr = channels[ch].chcr;
    int mode = (chcr >> 2) & 0x3;  // Transfer mode
    
    switch (ch) {
        case DMA_VIF0:
        case DMA_VIF1:
            {
                g_logFile << "DMAC: VIF" << ch << " DMA started (Mode: " << mode << ")" << std::endl;
                if (mode == 1) {
                    ProcessVifDmaChain(ch);
                    // REMOVE THIS LINE - ProcessVifDmaChain already calls CompleteChannel
                    // CompleteChannel(ch);  
                } else if (mode == 0) {
                    ProcessVifDmaNormal(ch);
                    CompleteChannel(ch);  // Keep this one since Normal doesn't call it
                }
                else {
                    g_logFile << "[DMAC] ERROR: Unsupported VIF DMA Mode " << mode << std::endl;
                    exit(1); // Complete anyway to avoid hang
                }
                
                break;
            }

            
        case DMA_GIF:
            {
                // ============================================================
                // >>> GRAPHICS THREAD WAKE-UP POINT <
                // ============================================================
                g_logFile << "========================================" << std::endl;
                g_logFile << ">>> GRAPHICS THREAD WAKE-UP <<<" << std::endl;
                g_logFile << "  Channel: GIF (Path 3)" << std::endl;
                g_logFile << "  Mode: " << (mode == 0 ? "Normal" : (mode == 1 ? "Chain" : "Interleave")) << std::endl;
                g_logFile << "========================================" << std::endl;
                
                if (mode == 1) {
                    // CHAIN MODE (The one 99% of games use)
                    // This function (defined lower in your file) walks the tags correctly.
                    ProcessGifDmaChain(); 
                } 
                else if (mode == 0) {
                    // NORMAL MODE (Rare, but your file has it)
                    ProcessGifDmaNormal();
                }
                else {
                    g_logFile << "[DMAC] ERROR: Unsupported GIF DMA Mode " << mode << std::endl;
                    CompleteChannel(ch); // Complete anyway to avoid hang
                }
                break;
            }

            
        case DMA_IPU0:  // Channel 3
        case DMA_IPU1:    // Channel 4
            g_logFile << "DMAC: IPU DMA (stub)" << std::endl;
            CompleteChannel(ch);
            break;
            
        case DMA_SIF0:  // Channel 5 - IOP → EE
        case DMA_SIF1:  // Channel 6 - EE → IOP  
        case DMA_SIF2:  // Channel 7
            if (ch == DMA_SIF1) {
                ProcessSifDma(ch);
            }
            CompleteChannel(ch);
            break;
            
        case DMA_SPR0:  // Channel 8
        case DMA_SPR1:    // Channel 9
            g_logFile << "DMAC: Scratchpad DMA (stub)" << std::endl;
            //ProcessSprDma(ch);
            CompleteChannel(ch);
            break;
            
        default:
            g_logFile << "DMAC: Unknown channel " << ch << std::endl;
            CompleteChannel(ch);  // Still clear STR to prevent hang!
            break;
    }
}
void DMAC::ProcessVifDmaNormal(int ch) {
    auto& channel = channels[ch];
    
    g_logFile << "========================================" << std::endl;
    g_logFile << "[VIF" << ch << " DMA] Normal Mode Transfer" << std::endl;
    g_logFile << "========================================" << std::endl;
    g_logFile << "  MADR: 0x" << std::hex << channel.madr << std::dec << std::endl;
    g_logFile << "  QWC:  " << channel.qwc << std::endl;
    g_logFile << "  CHCR: 0x" << std::hex << channel.chcr << std::dec << std::endl;
    
    if (channel.qwc == 0) {
        g_logFile << "  QWC is 0, nothing to transfer" << std::endl;
        return;
    }
    
    uint32_t data_bytes = channel.qwc * 16;
    uint32_t addr = channel.madr & 0x01FFFFFF;
    
    uint8_t* data_ptr = memory::translate_address(addr, data_bytes);
    
    if (data_ptr) {
        // Hex dump first 64 bytes
        size_t dump_size = std::min((size_t)data_bytes, (size_t)64);
        g_logFile << "  First " << dump_size << " bytes:" << std::endl;
        g_logFile << "    ";
        for (size_t i = 0; i < dump_size; i++) {
            g_logFile << std::hex << std::setw(2) << std::setfill('0') << (int)data_ptr[i] << " ";
            if ((i + 1) % 16 == 0 && (i + 1) < dump_size) {
                g_logFile << std::endl << "    ";
            }
        }
        g_logFile << std::dec << std::endl;
        
        // Send to VIF
        int vif_idx = (ch == DMA_VIF1) ? 1 : 0;
        g_logFile << "  Sending " << data_bytes << " bytes to VIF" << vif_idx << std::endl;
        g_vif.ProcessData(vif_idx, data_ptr, data_bytes);
        
        // Update registers
        channel.madr += data_bytes;
        channel.qwc = 0;
        
        g_logFile << "  Transfer complete" << std::endl;
    } else {
        g_logFile << "  ERROR: Invalid address 0x" << std::hex << channel.madr << std::dec << std::endl;
    }
    
    g_logFile << "========================================" << std::endl << std::endl;
}


void DMAC::ProcessVifDmaChain(int ch) {
    auto& channel = channels[ch];

    int vif_idx = (ch == DMA_VIF1) ? 1 : 0;
    
    g_vif.latch_fill_count[vif_idx] = 0;
    for (int i = 0; i < 4; i++) {
        g_vif.latch[vif_idx][i] = 0;
    }
    // Check VIF Direction: 0 = Write (DMA to VIF), 1 = Read (VIF to DMA)
    if (g_vif.regs[vif_idx].stat & VIF_STAT::FDR) {
        g_logFile << "[VIF" << ch << " DMA] ERROR: Chain called while VIF is in READ mode (FDR=1)" << std::endl;
        channel.chcr &= ~CHCR_STR;
        return;
    }


    
    // ===== INITIAL STATE LOGGING =====
    g_logFile << "========================================" << std::endl;
    g_logFile << "[VIF" << ch << " DMA] Chain Processing Started" << std::endl;
    g_logFile << "========================================" << std::endl;
    g_logFile << "  Initial Registers:" << std::endl;
    g_logFile << "    CHCR: 0x" << std::hex << channel.chcr << std::dec << std::endl;
    g_logFile << "    MADR: 0x" << std::hex << channel.madr << std::dec << std::endl;
    g_logFile << "    TADR: 0x" << std::hex << channel.tadr << std::dec << std::endl;
    g_logFile << "    QWC:  " << channel.qwc << std::endl;
    g_logFile << "    ASR0: 0x" << std::hex << channel.asr0 << std::dec << std::endl;
    g_logFile << "    ASR1: 0x" << std::hex << channel.asr1 << std::dec << std::endl;
    
    bool is_vif0 = (ch == DMA_VIF0);

    // ===== EXTRACT CHCR FLAGS =====
    bool tte = (channel.chcr & CHCR_TTE) != 0;
    bool tie = (channel.chcr & CHCR_TIE) != 0;
    int mode = (channel.chcr >> 2) & 0x3;
    int asp = (channel.chcr >> 4) & 0x3;
    
    g_logFile << "  CHCR Flags:" << std::endl;
    g_logFile << "    Mode: " << mode << " (should be 1 for chain)" << std::endl;
    g_logFile << "    TTE:  " << (tte ? "YES - Tag data transferred to VIF" : "NO") << std::endl;
    g_logFile << "    TIE:  " << (tie ? "YES - IRQ on tag" : "NO") << std::endl;
    g_logFile << "    ASP:  " << asp << " (call stack depth)" << std::endl;
    
    if (mode != 1) {
        g_logFile << "[VIF" << ch << " DMA] ERROR: ProcessVifDmaChain called with non-chain mode " << mode << std::endl;
        channel.chcr &= ~CHCR_STR;
        return;
    }
    
    // ===== INITIALIZE CHAIN WALK =====
    // FIX: Correct scratchpad detection (0x70000000, not 0x80000000)
    uint32_t tadr = channel.tadr & 0x0FFFFFFF;  // Physical address
    bool fromSpr = ((channel.tadr & 0x70000000) == 0x70000000);
    
    g_logFile << "  Chain Start:" << std::endl;
    g_logFile << "    TADR (physical): 0x" << std::hex << tadr << std::dec << std::endl;
    g_logFile << "    Starting from:   " << (fromSpr ? "SCRATCHPAD" : "MAIN RAM") << std::endl;
    
    // Safety check
    if (ch == DMA_VIF1 && tadr == 0x00000000) {
        g_logFile << "[VIF" << ch << " DMA] FATAL: TADR is NULL! Aborting." << std::endl;
        channel.chcr &= ~CHCR_STR;
        return;
    }
    
    const int MAX_TAGS = 10000;
    int tag_count = 0;
    bool tag_end = false;



    g_logFile << "[DEBUG] Pre-scan of DMA chain at 0x" << std::hex << tadr << ":" << std::endl;
    uint32_t scan_addr = tadr;
    for (int i = 0; i < 10; i++) {
        g_logFile << "  Reading lo_tag at 0x" << std::hex << scan_addr<< std::endl;
        uint64_t scan_lo = memory::read<uint64_t>(scan_addr);
        uint64_t scan_mid = memory::read<uint64_t>(scan_addr + 4);
        g_logFile << "  Value at lo: 0x" << std::hex << (scan_lo) << std::endl;
        g_logFile << "  Value at mid: 0x" << std::hex << (scan_mid) << std::endl;
        uint32_t scan_hi = memory::read<uint32_t>(scan_addr + 8);
        g_logFile << "  Value at hi_tag: 0x" << std::hex << scan_hi << std::endl;
        uint8_t scan_id = (scan_lo >> 28) & 0x7;
        uint32_t scan_qwc = scan_lo & 0xFFFF;
        uint32_t scan_addr_field = ((scan_lo >> 32) & 0x7FFFFFFF) & ~0xF;
        
        g_logFile << "  [" << i << "] @ 0x" << std::hex << scan_addr 
                  << ": ID=" << (int)scan_id << " QWC=" << std::dec << scan_qwc
                  << " ADDR=0x" << std::hex << scan_addr_field 
                  << " LO: 0x" << std::hex << scan_lo << " HI: 0x" << std::hex << scan_hi << std::endl;
        
        if (scan_id == 0 || scan_id == 7) break; // REFE or END
        scan_addr += 16; // Next tag (simplified, doesn't follow NEXT/CALL)
    }
    g_logFile << std::dec << std::endl;


    g_logFile << "Raw bytes @ 0x628990: ";
    for (int i = 0; i < 8; i++) {
        g_logFile << std::hex << std::setw(2) << std::setfill('0') 
                << (int)memory::read<uint8_t>(0x628990 + i) << " ";
    }
    g_logFile << std::endl;
    
    // ===== MAIN CHAIN PROCESSING LOOP =====
    while (!tag_end && tag_count < MAX_TAGS) {
        g_logFile << std::endl;
        g_logFile << "  ┌─────────────────────────────────────────────────" << std::endl;
        g_logFile << "  │ TAG #" << tag_count << std::endl;
        g_logFile << "  └─────────────────────────────────────────────────" << std::endl;
        
        // ===== READ 128-BIT DMA TAG =====
        uint32_t tag_read_addr;
        if (fromSpr) {
            tag_read_addr = 0x70000000 | (tadr & 0x3FFF);
            g_logFile << "    Reading tag from SCRATCHPAD @ 0x" << std::hex << tag_read_addr << std::dec << std::endl;
        } else {
            tag_read_addr = tadr & 0x01FFFFFF;  // Mask to RAM range
            g_logFile << "    Reading tag from RAM @ 0x" << std::hex << tag_read_addr << std::dec << std::endl;
        }
        
        uint64_t tag_lo = memory::read<uint64_t>(tag_read_addr);
        uint64_t tag_hi = memory::read<uint64_t>(tag_read_addr + 8);
        
        g_logFile << "    Raw Tag Data:" << std::endl;
        g_logFile << "      tag_lo: 0x" << std::hex << std::setw(16) << std::setfill('0') << tag_lo << std::dec << std::endl;
        g_logFile << "      tag_hi: 0x" << std::hex << std::setw(16) << std::setfill('0') << tag_hi << std::dec << std::endl;
        
        // ===== PARSE TAG FIELDS =====
        uint16_t qwc  = tag_lo & 0xFFFF;
        uint8_t  pce  = (tag_lo >> 26) & 0x3;   // Priority control
        uint8_t  id   = (tag_lo >> 28) & 0x7;
        bool     irq  = (tag_lo >> 31) & 1;
        uint32_t addr = (tag_lo >> 32) & 0x7FFFFFFF;
        addr &= ~0xF;  // Align to 16 bytes
        bool     spr  = (tag_lo >> 63) & 1;
        
        // Tag ID names for logging
        const char* tag_names[] = {"REFE", "CNT", "NEXT", "REF", "REFS", "CALL", "RET", "END"};
        const char* tag_name = (id < 8) ? tag_names[id] : "UNKNOWN";
        
        g_logFile << "    Parsed Tag Fields:" << std::endl;
        g_logFile << "      ID:   " << (int)id << " (" << tag_name << ")" << std::endl;
        g_logFile << "      QWC:  " << qwc << " quadwords (" << (qwc * 16) << " bytes)" << std::endl;
        g_logFile << "      ADDR: 0x" << std::hex << addr << std::dec << std::endl;
        g_logFile << "      IRQ:  " << (irq ? "YES" : "NO") << std::endl;
        g_logFile << "      SPR:  " << (spr ? "YES (scratchpad)" : "NO (RAM)") << std::endl;
        g_logFile << "      PCE:  " << (int)pce << std::endl;
        
        // ===== UPDATE CHCR.TAG (bits 16-31) =====
        // FIX: Use bits 16-31 of tag_lo directly (not shifted)
        uint32_t old_chcr = channel.chcr;
        channel.chcr = (channel.chcr & 0x0000FFFF) | (tag_lo & 0xFFFF0000);
        g_logFile << "    CHCR.TAG updated: 0x" << std::hex << old_chcr << " -> 0x" << channel.chcr << std::dec << std::endl;
        
        // ===== LOG TTE DATA IF ENABLED =====
        if (tte) {
            uint32_t vif_cmd0 = (uint32_t)(tag_hi & 0xFFFFFFFF);
            uint32_t vif_cmd1 = (uint32_t)(tag_hi >> 32);
            uint8_t cmd0_op = (vif_cmd0 >> 24) & 0x7F;
            uint8_t cmd1_op = (vif_cmd1 >> 24) & 0x7F;
            
            g_logFile << "    TTE Data (VIF commands in tag_hi):" << std::endl;
            g_logFile << "      Word 0: 0x" << std::hex << std::setw(8) << std::setfill('0') << vif_cmd0 
                      << " (CMD=0x" << std::setw(2) << (int)cmd0_op << ")" << std::dec << std::endl;
            g_logFile << "      Word 1: 0x" << std::hex << std::setw(8) << std::setfill('0') << vif_cmd1 
                      << " (CMD=0x" << std::setw(2) << (int)cmd1_op << ")" << std::dec << std::endl;
        }
        
        // ===== DETERMINE SOURCE ADDRESS AND NEXT TADR =====
        uint32_t src_addr = 0;
        bool src_from_spr = false;
        bool do_transfer = false;
        uint32_t next_tadr = tadr;
        bool next_from_spr = fromSpr;
        
        g_logFile << "    Processing Tag ID " << (int)id << " (" << tag_name << "):" << std::endl;
        
        switch (id) {
            case 0: // REFE - Reference + End
            {
                g_logFile << "      REFE: Transfer from ADDR, then END" << std::endl;
                src_addr = addr;
                src_from_spr = spr;
                next_tadr = tadr + 16;  // Doesn't matter, chain ends
                tag_end = true;         // ← Only REFE ends!
                do_transfer = true;
                
                g_logFile << "        Data source: 0x" << std::hex << src_addr 
                        << (src_from_spr ? " (SPR)" : " (RAM)") << std::dec << std::endl;
                g_logFile << "        Chain ends after this transfer" << std::endl;
                break;
            }

            case 3: // REF - Reference (data at ADDR, next tag follows THIS tag)
            case 4: // REFS - Reference + Stall control (same behavior)
            {
                g_logFile << "      REF/REFS: Transfer from ADDR, continue to next tag" << std::endl;
                src_addr = addr;
                src_from_spr = spr;
                next_tadr = tadr + 16;  // Next tag is immediately after THIS tag
                next_from_spr = fromSpr; // Stay in same memory region for tag reading
                // tag_end = false;      // ← DO NOT END! (default is already false)
                do_transfer = true;
                
                g_logFile << "        Data source: 0x" << std::hex << src_addr 
                        << (src_from_spr ? " (SPR)" : " (RAM)") << std::dec << std::endl;
                g_logFile << "        Next tag at: 0x" << std::hex << next_tadr << std::dec << std::endl;
                break;
            }
            case 1: // CNT - Continue (data follows tag)
            {
                src_addr = tadr + 16;  // Data immediately after tag
                src_from_spr = fromSpr;
                next_tadr = tadr + 16 + (qwc * 16);  // Next tag after data
                // next_from_spr unchanged
                do_transfer = true;
                
                g_logFile << "      CNT: Transfer data after tag, continue" << std::endl;
                g_logFile << "        Data source: 0x" << std::hex << src_addr << (src_from_spr ? " (SPR)" : " (RAM)") << std::dec << std::endl;
                g_logFile << "        Next TADR:   0x" << std::hex << next_tadr << std::dec << std::endl;
                break;
            }
            
            case 2: // NEXT - Jump to ADDR after data
            {
                src_addr = tadr + 16;  // Data immediately after tag
                src_from_spr = fromSpr;
                next_tadr = addr;       // Jump to ADDR for next tag
                next_from_spr = spr;    // SPR bit applies to jump destination
                do_transfer = true;
                
                g_logFile << "      NEXT: Transfer data after tag, jump to ADDR" << std::endl;
                g_logFile << "        Data source: 0x" << std::hex << src_addr << (src_from_spr ? " (SPR)" : " (RAM)") << std::dec << std::endl;
                g_logFile << "        Next TADR:   0x" << std::hex << next_tadr << (next_from_spr ? " (SPR)" : " (RAM)") << std::dec << std::endl;
                break;
            }
            case 5: // CALL - Push return address, jump to ADDR
            {
                src_addr = tadr + 16;
                src_from_spr = fromSpr;
                
                // FIX: Proper ASP (Address Stack Pointer) management
                int current_asp = (channel.chcr >> 4) & 3;
                g_logFile << "      CALL: Push return, jump to ADDR" << std::endl;
                g_logFile << "        Current ASP: " << current_asp << std::endl;
                
                if (current_asp == 0) {
                    channel.asr0 = tadr + 16 + (qwc * 16);  // Return address (after data)
                    channel.chcr = (channel.chcr & ~(3 << 4)) | (1 << 4);  // ASP = 1
                    g_logFile << "        Pushed ASR0: 0x" << std::hex << channel.asr0 << std::dec << std::endl;
                    g_logFile << "        New ASP: 1" << std::endl;
                } else if (current_asp == 1) {
                    channel.asr1 = tadr + 16 + (qwc * 16);
                    channel.chcr = (channel.chcr & ~(3 << 4)) | (2 << 4);  // ASP = 2
                    g_logFile << "        Pushed ASR1: 0x" << std::hex << channel.asr1 << std::dec << std::endl;
                    g_logFile << "        New ASP: 2" << std::endl;
                } else {
                    g_logFile << "        WARNING: CALL with ASP=2 (stack overflow!)" << std::endl;
                    g_logFile << "        Real hardware behavior undefined - continuing anyway" << std::endl;
                }
                
                next_tadr = addr;
                next_from_spr = spr;
                do_transfer = true;
                
                g_logFile << "        Data source: 0x" << std::hex << src_addr << (src_from_spr ? " (SPR)" : " (RAM)") << std::dec << std::endl;
                g_logFile << "        Jump to:     0x" << std::hex << next_tadr << (next_from_spr ? " (SPR)" : " (RAM)") << std::dec << std::endl;
                break;
            }
            
            case 6: // RET - Pop return address
            {
                src_addr = tadr + 16;
                src_from_spr = fromSpr;
                
                // FIX: Proper ASP management for RET
                int current_asp = (channel.chcr >> 4) & 3;
                g_logFile << "      RET: Pop return address" << std::endl;
                g_logFile << "        Current ASP: " << current_asp << std::endl;
                
                if (current_asp == 2) {
                    next_tadr = channel.asr1;
                    channel.chcr = (channel.chcr & ~(3 << 4)) | (1 << 4);  // ASP = 1
                    g_logFile << "        Popped ASR1: 0x" << std::hex << next_tadr << std::dec << std::endl;
                    g_logFile << "        New ASP: 1" << std::endl;
                    // TODO: Restore fromSpr state if you saved it during CALL
                } else if (current_asp == 1) {
                    next_tadr = channel.asr0;
                    channel.chcr = (channel.chcr & ~(3 << 4));  // ASP = 0
                    g_logFile << "        Popped ASR0: 0x" << std::hex << next_tadr << std::dec << std::endl;
                    g_logFile << "        New ASP: 0" << std::endl;
                } else {
                    g_logFile << "        Stack empty (ASP=0) - ending chain" << std::endl;
                    tag_end = true;
                }
                
                do_transfer = true;
                g_logFile << "        Data source: 0x" << std::hex << src_addr << (src_from_spr ? " (SPR)" : " (RAM)") << std::dec << std::endl;
                break;
            }
            
            case 7: // END - End after data
            {
                src_addr = tadr + 16;
                src_from_spr = fromSpr;
                next_tadr = tadr + 16 + (qwc * 16);
                tag_end = true;
                do_transfer = true;
                
                g_logFile << "      END: Transfer data after tag, then terminate" << std::endl;
                g_logFile << "        Data source: 0x" << std::hex << src_addr << (src_from_spr ? " (SPR)" : " (RAM)") << std::dec << std::endl;
                g_logFile << "        Chain ends after this transfer" << std::endl;
                break;
            }
            
            default:
                g_logFile << "      UNKNOWN TAG ID " << (int)id << " - forcing chain end!" << std::endl;
                tag_end = true;
                break;
        }
        
        // ===== UPDATE REGISTERS (for games that poll) =====
        channel.madr = src_addr;
        channel.tadr = next_tadr;
        channel.qwc = qwc;
        
        g_logFile << "    Register Updates:" << std::endl;
        g_logFile << "      MADR -> 0x" << std::hex << channel.madr << std::dec << std::endl;
        g_logFile << "      TADR -> 0x" << std::hex << channel.tadr << std::dec << std::endl;
        g_logFile << "      QWC  -> " << channel.qwc << std::endl;
        
        // ===== HANDLE TTE: Send tag_hi to VIF BEFORE payload =====
        if (tte) {
            g_logFile << "    TTE Transfer:" << std::endl;
            g_logFile << "      Sending tag_hi (8 bytes) to VIF" << ch << " before payload" << std::endl;
            
            uint8_t tte_data[8];
            std::memcpy(tte_data, &tag_hi, 8);
            
            // Log TTE bytes
            g_logFile << "      TTE bytes: ";
            for (int i = 0; i < 8; i++) {
                g_logFile << std::hex << std::setw(2) << std::setfill('0') << (int)tte_data[i] << " ";
            }
            g_logFile << std::dec << std::endl;
            
            int vif_idx = (ch == DMA_VIF1) ? 1 : 0;
            g_vif.ProcessData(vif_idx, tte_data, 8);
        }
        
        // ===== TRANSFER PAYLOAD DATA =====
        if (do_transfer && qwc > 0) {
            uint32_t data_bytes = qwc * 16;
            
            // Translate address
            uint8_t* data_ptr;
            uint32_t translated_addr;
            
            if (src_from_spr) {
                translated_addr = 0x70000000 | (src_addr & 0x3FFF);
                data_ptr = memory::translate_address(translated_addr, data_bytes);
                g_logFile << "    Data Transfer (SCRATCHPAD):" << std::endl;
            } else {
                translated_addr = src_addr & 0x01FFFFFF;
                data_ptr = memory::translate_address(translated_addr, data_bytes);
                g_logFile << "    Data Transfer (RAM):" << std::endl;
            }
            
            g_logFile << "      Source:     0x" << std::hex << src_addr << std::dec << std::endl;
            g_logFile << "      Translated: 0x" << std::hex << translated_addr << std::dec << std::endl;
            g_logFile << "      Size:       " << data_bytes << " bytes (" << qwc << " QW)" << std::endl;
            
            if (data_ptr) {
                // Hex dump first 64 bytes (or less)
                size_t dump_size = std::min((size_t)data_bytes, (size_t)64);
                g_logFile << "      First " << dump_size << " bytes:" << std::endl;
                g_logFile << "        ";
                for (size_t i = 0; i < dump_size; i++) {
                    g_logFile << std::hex << std::setw(2) << std::setfill('0') << (int)data_ptr[i] << " ";
                    if ((i + 1) % 16 == 0 && (i + 1) < dump_size) {
                        g_logFile << std::endl << "        ";
                    }
                }
                g_logFile << std::dec << std::endl;
                
                // Send to VIF
                int vif_idx = (ch == DMA_VIF1) ? 1 : 0;
                g_logFile << "      Sending to VIF" << vif_idx << "..." << std::endl;
                g_vif.ProcessData(vif_idx, data_ptr, data_bytes);
                g_logFile << "      Transfer complete." << std::endl;
                
                // Update MADR and clear QWC (simulates transfer completion)
                channel.madr += data_bytes;
                channel.qwc = 0;
            } else {
                g_logFile << "      ERROR: Failed to translate address 0x" << std::hex << src_addr << std::dec << std::endl;
                g_logFile << "      Skipping transfer!" << std::endl;
            }
        } else if (qwc == 0) {
            g_logFile << "    No data to transfer (QWC=0)" << std::endl;
        }
        
        // ===== HANDLE TAG IRQ =====
        if (irq) {
            g_logFile << "    IRQ bit set in tag" << std::endl;
            if (tie) {
                g_logFile << "      TIE enabled - ending chain and raising interrupt" << std::endl;
                tag_end = true;
                // In full emulation, raise INT1 here via D_STAT
            } else {
                g_logFile << "      TIE disabled - IRQ ignored" << std::endl;
            }
        }
        
        // ===== ADVANCE TO NEXT TAG =====
        tadr = next_tadr;
        fromSpr = next_from_spr;
        
        g_logFile << "    Next iteration: TADR=0x" << std::hex << tadr 
                  << " fromSpr=" << (fromSpr ? "YES" : "NO") << std::dec << std::endl;
        
        tag_count++;
    }
    
    // ===== CHAIN COMPLETE =====
    g_logFile << std::endl;
    g_logFile << "========================================" << std::endl;
    g_logFile << "[VIF" << ch << " DMA] Chain Processing Complete" << std::endl;
    g_logFile << "========================================" << std::endl;
    g_logFile << "  Tags processed: " << tag_count << std::endl;
    g_logFile << "  End reason:     " << (tag_count >= MAX_TAGS ? "MAX_TAGS EXCEEDED (possible infinite loop!)" : "Normal termination") << std::endl;
    g_logFile << "  Final Registers:" << std::endl;
    g_logFile << "    CHCR: 0x" << std::hex << channel.chcr << std::dec << std::endl;
    g_logFile << "    MADR: 0x" << std::hex << channel.madr << std::dec << std::endl;
    g_logFile << "    TADR: 0x" << std::hex << channel.tadr << std::dec << std::endl;
    g_logFile << "    QWC:  " << channel.qwc << std::endl;
    
    if (tag_count >= MAX_TAGS) {
        g_logFile << "[VIF" << ch << " DMA] ERROR: Chain exceeded " << MAX_TAGS << " tags!" << std::endl;
    }
    
    // Clear STR bit to indicate completion
    channel.chcr &= ~CHCR_STR;
    channel.qwc = 0;
    
    // NOTE: Don't call CompleteChannel here if StartChannel already calls it!
    // Check your StartChannel function - if it calls CompleteChannel after 
    // ProcessVifDmaChain, remove this line:
    CompleteChannel(ch);
    
    g_logFile << "[VIF" << ch << " DMA] STR cleared, channel complete" << std::endl;
    g_logFile << "========================================" << std::endl << std::endl;
}

void DMAC::CompleteChannel(int ch) {
    // Clear STR bit
    channels[ch].chcr &= ~CHCR_STR;
    
    // Set channel interrupt status (CISx)
    stat |= (1 << ch);
    
    // Clear QWC
    channels[ch].qwc = 0;
    
    g_logFile << "DMAC: Channel " << ch << " completed" << std::endl;
}

bool DMAC::CheckInterrupt() {
    // Interrupt fires if any enabled channel has its status bit set
    uint16_t status = stat & 0xFFFF;
    uint16_t mask = (stat >> 16) & 0xFFFF;
    return (status & mask) != 0;
}

void DMAC::DispatchInterrupt(CpuContext& ctx) {
    // Find which channels have pending interrupts
    uint16_t status = stat & 0xFFFF;
    uint16_t mask = (stat >> 16) & 0xFFFF;
    uint16_t pending = status & mask;
    
    for (int ch = 0; ch < DMA_COUNT; ch++) {
        if (pending & (1 << ch)) {
            g_logFile << "DMAC: Dispatching interrupt for channel " << ch << std::endl;
            
            // Look up registered handler
            auto it = g_dmac_queues.find(ch);
            if (it != g_dmac_queues.end() && !it->second.empty()) {
                for (auto& handler : it->second) {
                    if (handler.active) {
                        g_logFile << "  Calling handler at 0x" << std::hex 
                                  << handler.handler_pc << std::dec << std::endl;
                        
                        // Save current context
                        uint32_t saved_pc = ctx.cpuRegs.pc;
                        uint32_t saved_ra = ctx.cpuRegs.GPR.r[31].UL[0];
                        
                        // Set up call to handler
                        // Handler signature: int handler(int channel)
                        ctx.cpuRegs.GPR.r[4].UL[0] = ch;  // a0 = channel
                        ctx.cpuRegs.GPR.r[28].UL[0] = handler.gp;  // gp
                        
                        // Call the handler (you need to implement this based on your execution model)
                        // This depends on how your recompiled code works
                        uint32_t target_addr = handler.handler_pc;
                        auto it = recompiled_functions.find(target_addr);
                        if (it != recompiled_functions.end()) {
                            g_logFile << "Dynamic interpreter: Jumping to recompiled function at 0x" 
                                    << std::hex << target_addr << std::endl;
                            it->second(ctx, target_addr);
                        }
                        else{
                            g_logFile << "DMAC: Handler at 0x" << std::hex << target_addr << " not compiled. Interpreting..." << std::endl;
                        }
                        
                        // Handler returns 0 to continue chain, -1 to stop
                        int result = (int32_t)ctx.cpuRegs.GPR.r[2].UL[0];
                        
                        // Restore context
                        ctx.cpuRegs.pc = saved_pc;
                        ctx.cpuRegs.GPR.r[31].UL[0] = saved_ra;
                        
                        if (result == -1) break;  // Handler says stop
                    }
                }
            }
            
            // Clear the interrupt bit
            stat &= ~(1 << ch);
        }
    }
}

// Syscall 0x13: RemoveDmacHandler
int RemoveDmacHandler(int channel, int handler_id) {
    if (channel < 0 || channel >= DMA_COUNT) {
        return -1;
    }

    auto& queue = g_dmac_queues[channel];
    
    // Find and remove the handler with the matching ID
    auto original_size = queue.size();
    
    queue.erase(
        std::remove_if(queue.begin(), queue.end(), 
            [handler_id](const DmacHandler& h) { return h.id == handler_id; }),
        queue.end()
    );

    if (queue.size() == original_size) {
        // Handler not found
        return -1;
    }

    extern std::ofstream g_logFile;
    g_logFile << "DMAC: Removed handler ID " << handler_id 
              << " from channel " << channel << std::endl;

    // Return the number of remaining handlers (standard PS2 behavior)
    return (int)queue.size();
}