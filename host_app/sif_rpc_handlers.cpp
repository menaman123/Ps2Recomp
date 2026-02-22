
#include "sif_rpc_handlers.h"
#include "memory.h"
#include "sif_hle.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>
#include <string>
#include <fstream>
// ============================================================================
// External dependencies
// ============================================================================
extern std::ofstream g_logFile;
extern FILE* g_isoFile;
extern std::map<int, FILE*> g_file_io_handles;
// ============================================================================
// IOP Heap - simple bump allocator for fake IOP-side memory
// ============================================================================
static struct {
uint32_t base = 0x00100000; // Start of IOP heap (IOP address space)
uint32_t current = 0x00100000;
uint32_t end = 0x001F0000; // ~960KB usable
uint32_t Alloc(uint32_t size) {
    size = (size + 0xF) & ~0xF;  // 16-byte align
    if (current + size > end) return 0;
    uint32_t addr = current;
    current += size;
    return addr;
}

void Free(uint32_t /*addr*/) {
    // Bump allocator doesn't free. Good enough for HLE.
}
} s_iop_heap;
// ============================================================================
// File I/O state
// ============================================================================
static int s_next_fio_fd = 100;
static std::string s_active_mount_path;
// Helper: read null-terminated string from guest memory
static std::string ReadGuestString(uint32_t addr, int max_len = 256) {
    std::string result;
    result.reserve(max_len);
    for (int i = 0; i < max_len; i++) {
        char c = (char)memory::read<uint8_t>(addr + i);
        if (c == 0) break;
        result += c;
    }
    return result;
}
// Helper: strip PS2 device prefix from path (e.g. "cdrom0:\FOO" -> "FOO")
static std::string StripDevicePrefix(const std::string& path) {
    std::string result = path;
    size_t pos = result.find(':');
    if (pos != std::string::npos) result = result.substr(pos + 1);
    if (!result.empty() && (result[0] == '\\' || result[0] == '/'))
        result = result.substr(1);
    return result;
}
// Helper: write result to recv_buffer if it's non-zero
static void WriteResult32(uint32_t recv_buffer, int32_t value) {
    if (recv_buffer != 0) {
        memory::write<int32_t>(recv_buffer, value);
    }
}
// ============================================================================
// MASTER DISPATCHER
// ============================================================================
void DispatchSifRpcCall(const SifRpcContext& rpc) {
    switch (rpc.server_id) {
    // Basic I/O and System
        case 0x80000001: HandleFileIO(rpc); break;
        case 0x80000003: HandleIopHeap(rpc); break;
        case 0x80000006: HandleLoadFile(rpc); break;
        // Controllers
        case 0x80000100:
        case 0x80000101: HandlePadman(rpc);          break;
        
        // Memory Cards
        case 0x80000400: HandleMcServ(rpc);          break;
        
        // CDVD Drive
        case 0x80000592: HandleCdvdInit(rpc);        break;
        case 0x80000593: HandleCdvdSCmd(rpc);        break;
        case 0x80000595: HandleCdvdNCmd(rpc);        break;
        case 0x80000597: HandleCdvdSearchFile(rpc);  break;
        case 0x8000059A: HandleCdvdDiskReady(rpc);   break;
        
        // Sound
        case 0x80000701: HandleLibSd(rpc);           break;
        
        // Multitap
        case 0x80000901:
        case 0x80000902:
        case 0x80000903:
        case 0x80000904:
        case 0x80000905: HandleMtap(rpc);            break;
        
        // EyeToy
        case 0x80001400: HandleEyeToy(rpc);          break;
        
        // Game-specific
        case 0x00012345: HandleCustomLoader(rpc);    break;
        
        // Everything else
        default:         HandleUnknownServer(rpc);   break;
    }
}
// ============================================================================
// 0x80000001 - FILEIO
// ============================================================================
// Known functions:
// 0x00 = fioOpen 0x01 = fioClose 0x02 = fioRead
// 0x03 = fioWrite 0x04 = fioLseek 0x05 = fioIoctl
// 0x06 = fioRemove 0x07 = fioMkdir 0x08 = fioRmdir
// 0x09 = fioDopen 0x0A = fioDclose 0x0B = fioDread
// 0x0C = fioGetstat 0x0D = fioChstat 0x0E = fioFormat
// 0xFF = fioInit/Reset
//
// NOTE: The exact function numbering depends on the SDK version.
// Crash Twinsanity uses the "new" FILEIO numbering where:
// 0x04 = fioOpen, 0x05 = fioClose, 0x06 = fioRead, etc.
// We support both layouts by checking what the game actually sends.
// ============================================================================
void HandleFileIO(const SifRpcContext& rpc) {
g_logFile << " [HLE] FILEIO Call (Func: 0x" << std::hex << rpc.func_num
<< ")" << std::dec << std::endl;
int32_t result = 0;

if (rpc.payload_addr != 0) {
    g_logFile << "[FILEIO RAW] Payload at 0x" << std::hex << rpc.payload_addr 
                << " size=" << rpc.payload_size << std::endl;
    for (int i = 0; i < 64 && i < (int)rpc.payload_size + 32; i += 4) {
        uint32_t val = memory::read<uint32_t>(rpc.payload_addr + i);
        g_logFile << "  [+" << std::hex << i << "] = 0x" << val << std::dec << std::endl;
    }
    // Also try reading as string from various offsets
    for (int off = 0; off < 48; off += 4) {
        char buf[64] = {};
        for (int j = 0; j < 63; j++) {
            buf[j] = (char)memory::read<uint8_t>(rpc.payload_addr + off + j);
            if (buf[j] == 0) break;
            if (buf[j] < 0x20 || buf[j] > 0x7e) { buf[j] = '?'; break; }
        }
        if (buf[0] != 0 && buf[0] != '?') {
            g_logFile << "  [+" << std::hex << off << " str] '" << buf << "'" << std::endl;
        }
    }
}


switch (rpc.func_num) {
    // =============================================================
    // fioInit / Reset
    // =============================================================
    case 0xFF: {
        if (rpc.payload_addr != 0 && rpc.payload_size > 0) {
            std::string mount = ReadGuestString(rpc.payload_addr);
            if (!mount.empty()) {
                s_active_mount_path = mount;
                g_logFile << "    [FILEIO] Mount path: " << mount << std::endl;
            } else {
                g_logFile << "    [FILEIO] fioInit/Reset (no mount path)" << std::endl;
                s_active_mount_path.clear();
            }
        } else {
            g_logFile << "    [FILEIO] fioInit/Reset (empty payload)" << std::endl;
            s_active_mount_path.clear();
        }
        result = 0;
        break;
    }
    
    // =============================================================
    // fioOpen(filename, flags) -> fd
    // Payload: [char filename[...]]  or  [int32 flags, char filename[...]]
    // =============================================================
    case 0x00:
    case 0x04: {
        std::string filename = ReadGuestString(rpc.payload_addr);
        g_logFile << "    [FILEIO] fioOpen: \"" << filename << "\"" << std::endl;
        
        std::string host_path = StripDevicePrefix(filename);
        FILE* f = fopen(host_path.c_str(), "rb");
        if (f) {
            int fd = s_next_fio_fd++;
            g_file_io_handles[fd] = f;
            result = fd;
            g_logFile << "    [FILEIO] Opened as FD " << fd << std::endl;
        } else {
            g_logFile << "    [FILEIO] Failed to open: " << host_path << std::endl;
            result = -1;
        }
        break;
    }
    
    // =============================================================
    // fioClose(fd) -> 0 or -1
    // Payload: [int32 fd]
    // =============================================================
    case 0x01:
    case 0x05: {
        int fd = memory::read<int32_t>(rpc.payload_addr);
        g_logFile << "    [FILEIO] fioClose FD=" << fd << std::endl;
        
        auto it = g_file_io_handles.find(fd);
        if (it != g_file_io_handles.end()) {
            fclose(it->second);
            g_file_io_handles.erase(it);
            result = 0;
        } else {
            result = -1;
        }
        break;
    }
    
    // =============================================================
    // fioRead(fd, ptr, size) -> bytes_read or -1
    // Payload: [int32 fd, uint32 ptr, int32 size]
    // =============================================================
    case 0x02:
    case 0x06: {
        int fd       = memory::read<int32_t>(rpc.payload_addr + 0x00);
        uint32_t dst = memory::read<uint32_t>(rpc.payload_addr + 0x04);
        int size     = memory::read<int32_t>(rpc.payload_addr + 0x08);
        
        // Mask destination to 32MB physical RAM range
        // Game may pass uncached addresses like 0x30024000
        uint32_t phys_addr = dst & 0x01FFFFFF;
        
        g_logFile << "    [FILEIO] fioRead FD=" << fd << " -> 0x" << std::hex << phys_addr 
                  << " (" << std::dec << size << " bytes)" << std::endl;
        
        auto it = g_file_io_handles.find(fd);
        if (it != g_file_io_handles.end()) {
            FILE* f = it->second;
            std::vector<uint8_t> buf(size);
            size_t bytes = fread(buf.data(), 1, size, f);
            
            uint8_t* dest_ptr = reinterpret_cast<uint8_t*>(memory::get_pointer(phys_addr));
            if (dest_ptr) {
                std::memcpy(dest_ptr, buf.data(), bytes);
            } else {
                for (size_t k = 0; k < bytes; k++) {
                    memory::write<uint8_t>(phys_addr + k, buf[k]);
                }
            }
            
            result = (int32_t)bytes;
            g_logFile << "    [FILEIO] Read " << bytes << " bytes" << std::endl;
        } else {
            g_logFile << "    [FILEIO] ERROR: Invalid FD " << fd << std::endl;
            result = -1;
        }
        break;
    }
    
    // =============================================================
    // fioWrite(fd, ptr, size) -> bytes_written or -1
    // Payload: [int32 fd, uint32 ptr, int32 size]
    // =============================================================
    case 0x03:
    case 0x07: {
        int fd       = memory::read<int32_t>(rpc.payload_addr + 0x00);
        uint32_t src = memory::read<uint32_t>(rpc.payload_addr + 0x04);
        int size     = memory::read<int32_t>(rpc.payload_addr + 0x08);
        
        g_logFile << "    [FILEIO] fioWrite FD=" << fd << " src=0x" << std::hex << src
                  << " size=" << std::dec << size << " (stubbed)" << std::endl;
        
        // Stub: pretend we wrote everything
        result = size;
        break;
    }
    
    // =============================================================
    // fioLseek(fd, offset, whence) -> position or -1
    // Payload: [int32 fd, int32 offset, int32 whence]
    // =============================================================
    case 0x04 + 0x80: // Alternate numbering guard - see note below
    case 0x08: {
        int fd      = memory::read<int32_t>(rpc.payload_addr + 0x00);
        int offset  = memory::read<int32_t>(rpc.payload_addr + 0x04);
        int whence  = memory::read<int32_t>(rpc.payload_addr + 0x08);
        
        g_logFile << "    [FILEIO] fioLseek FD=" << fd << " offset=" << offset 
                  << " whence=" << whence << std::endl;
        
        auto it = g_file_io_handles.find(fd);
        if (it != g_file_io_handles.end()) {
            fseek(it->second, offset, whence);
            result = (int32_t)ftell(it->second);
        } else {
            result = -1;
        }
        break;
    }
    
    // =============================================================
    // fioRemove, fioMkdir, fioRmdir - stub success
    // =============================================================
    case 0x06 + 0x80:  // fioRemove alternate
    case 0x09:          // fioRemove
    case 0x07 + 0x80:  // fioMkdir alternate
    case 0x0A:          // fioMkdir
    case 0x0B: {        // fioRmdir
        g_logFile << "    [FILEIO] Filesystem operation 0x" << std::hex << rpc.func_num 
                  << " (stubbed success)" << std::dec << std::endl;
        result = 0;
        break;
    }
    
    // =============================================================
    // fioGetstat - return "file not found" by default
    // =============================================================
    case 0x0C:
    case 0x0E: {
        g_logFile << "    [FILEIO] fioGetstat (stubbed: not found)" << std::endl;
        result = -1;
        break;
    }
    
    // =============================================================
    // Default: stub with success
    // =============================================================
    default: {
        g_logFile << "    [FILEIO] Unhandled func 0x" << std::hex << rpc.func_num 
                  << " (stubbed 0)" << std::dec << std::endl;
        result = 0;
        break;
    }
}



WriteResult32(rpc.recv_buffer, result);
}
// ============================================================================
// 0x80000003 - IOP HEAP ALLOCATION
// ============================================================================
// Functions:
// 0x01 = SifAllocIopHeap(size) -> IOP address
// 0x02 = SifFreeIopHeap(addr) -> 0
// ============================================================================
void HandleIopHeap(const SifRpcContext& rpc) {
g_logFile << " [HLE] IOPHEAP Call (Func: 0x" << std::hex << rpc.func_num
<< ")" << std::dec << std::endl;
int32_t result = 0;

switch (rpc.func_num) {
    case 0x01: { // SifAllocIopHeap
        uint32_t size = memory::read<uint32_t>(rpc.payload_addr);
        uint32_t iop_addr = s_iop_heap.Alloc(size);
        
        g_logFile << "    [IOPHEAP] Alloc(" << size << ") = 0x" 
                  << std::hex << iop_addr << std::dec << std::endl;
        
        result = (int32_t)iop_addr;
        break;
    }
    
    case 0x02: { // SifFreeIopHeap
        uint32_t addr = memory::read<uint32_t>(rpc.payload_addr);
        s_iop_heap.Free(addr);
        
        g_logFile << "    [IOPHEAP] Free(0x" << std::hex << addr << ")" << std::dec << std::endl;
        result = 0;
        break;
    }
    
    default: {
        g_logFile << "    [IOPHEAP] Unhandled func 0x" << std::hex << rpc.func_num 
                  << std::dec << std::endl;
        result = 0;
        break;
    }
}

WriteResult32(rpc.recv_buffer, result);
}
// ============================================================================
// 0x80000006 - LOADFILE (Module/ELF Loader)
// ============================================================================
// Functions:
// 0x00 = LoadStartModule(path, args, arglen)
// 0x01 = LoadElf / LoadModuleBuffer
// 0x02 = LoadModuleBuffer
// 0x03 = StopModule
// 0x04 = UnloadModule
// 0x05 = SearchModuleByName
// 0x06 = SearchModuleByAddress
// 0xFF = Init
// ============================================================================
void HandleLoadFile(const SifRpcContext& rpc) {
g_logFile << " [HLE] LOADFILE Call (Func: 0x" << std::hex << rpc.func_num
<< ")" << std::dec << std::endl;
int32_t result = 0;

switch (rpc.func_num) {
    case 0xFF: { // Init
        g_logFile << "    [LOADFILE] Init (success)" << std::endl;
        result = 0;
        break;
    }
    
    case 0x00: { // LoadStartModule
        std::string module_name;
        if (rpc.payload_addr != 0 && rpc.payload_size > 0) {
            module_name = ReadGuestString(rpc.payload_addr);
        }
        
        g_logFile << "    [LOADFILE] LoadStartModule: \"" << module_name << "\"" << std::endl;
        g_logFile << "    [LOADFILE] Stubbed success (module ID = 0)" << std::endl;
        
        // Return module ID >= 0 for success
        result = 0;
        break;
    }
    
    case 0x01: { // LoadElf
        std::string elf_path;
        if (rpc.payload_addr != 0 && rpc.payload_size > 0) {
            elf_path = ReadGuestString(rpc.payload_addr);
        }
        
        g_logFile << "    [LOADFILE] LoadElf: \"" << elf_path << "\"" << std::endl;
        g_logFile << "    [LOADFILE] TODO: Implement ELF loading" << std::endl;
        
        // Return -1 (not loaded) for now
        // When implemented, load ELF from ISO into EE RAM and return 0
        result = -1;
        break;
    }
    
    case 0x02: { // LoadModuleBuffer
        g_logFile << "    [LOADFILE] LoadModuleBuffer (stubbed success)" << std::endl;
        result = 0;
        break;
    }
    
    case 0x03: { // StopModule
        g_logFile << "    [LOADFILE] StopModule (stubbed success)" << std::endl;
        result = 0;
        break;
    }
    
    case 0x04: { // UnloadModule
        g_logFile << "    [LOADFILE] UnloadModule (stubbed success)" << std::endl;
        result = 0;
        break;
    }
    
    case 0x05: { // SearchModuleByName
        std::string name;
        if (rpc.payload_addr != 0) {
            name = ReadGuestString(rpc.payload_addr);
        }
        g_logFile << "    [LOADFILE] SearchModuleByName: \"" << name 
                  << "\" (stubbed: not found)" << std::endl;
        result = -1; // Module not found
        break;
    }
    
    case 0x06: { // SearchModuleByAddress
        uint32_t addr = 0;
        if (rpc.payload_addr != 0) {
            addr = memory::read<uint32_t>(rpc.payload_addr);
        }
        g_logFile << "    [LOADFILE] SearchModuleByAddress: 0x" << std::hex << addr
                  << " (stubbed: not found)" << std::dec << std::endl;
        result = -1;
        break;
    }
    
    default: {
        g_logFile << "    [LOADFILE] Unhandled func 0x" << std::hex << rpc.func_num 
                  << " (stubbed 0)" << std::dec << std::endl;
        result = 0;
        break;
    }
}

WriteResult32(rpc.recv_buffer, result);
}
// ============================================================================
// 0x80000100 / 0x80000101 - PADMAN / PADMAN_EXT
// ============================================================================
// Functions:
// 0x01 = PadOpen(port, slot, ...) -> handle
// 0x02 = PadClose
// 0x03 = PadInfoMode
// 0x04 = PadInfoAct
// 0x05 = PadGetState -> 6 (DISCONN) or 2 (STABLE)
// 0x06 = PadRead -> pad data
// 0x07 = PadSetMainMode
// 0x08 = PadSetActDirect
// 0x09 = PadSetActAlign
// 0x0A = PadGetButtonMask
// 0x0B = PadSetButtonInfo
// 0xFF = PadInit
// ============================================================================
void HandlePadman(const SifRpcContext& rpc) {
g_logFile << " [HLE] PADMAN Call (Server: 0x" << std::hex << rpc.server_id
<< ", Func: 0x" << rpc.func_num << ")" << std::dec << std::endl;
int32_t result = 0;

switch (rpc.func_num) {
    case 0xFF: // PadInit
        g_logFile << "    [PADMAN] Init (success)" << std::endl;
        result = 1;
        break;
        
    case 0x01: // PadOpen
        g_logFile << "    [PADMAN] PadOpen (returning 1 = success)" << std::endl;
        result = 1;
        break;
        
    case 0x02: // PadClose
        g_logFile << "    [PADMAN] PadClose (success)" << std::endl;
        result = 1;
        break;
        
    case 0x03: // PadInfoMode
        // Return 0 = standard controller
        g_logFile << "    [PADMAN] PadInfoMode (returning 0 = standard)" << std::endl;
        result = 0;
        break;
        
    case 0x04: // PadInfoAct
        g_logFile << "    [PADMAN] PadInfoAct (returning 0)" << std::endl;
        result = 0;
        break;
        
    case 0x05: // PadGetState
        // 0x02 = PAD_STATE_STABLE (controller connected and ready)
        // 0x06 = PAD_STATE_DISCONN (not connected)
        g_logFile << "    [PADMAN] PadGetState (returning STABLE=2)" << std::endl;
        result = 2;
        break;
        
    case 0x06: { // PadRead
        // Write default pad data to recv_buffer
        // Button data format: 0xFFFF = no buttons pressed
        if (rpc.recv_buffer != 0) {
            // Standard 32-byte pad response
            // Offset 0: status byte (0x00 = OK)
            // Offset 1: half-words of button data  
            // Bytes 2-3: buttons (0xFF 0xFF = nothing pressed)
            // Bytes 4-5: right stick X,Y (0x80 = center)
            // Bytes 6-7: left stick X,Y (0x80 = center)
            memory::write<uint8_t>(rpc.recv_buffer + 0, 0x00); // Status OK
            memory::write<uint8_t>(rpc.recv_buffer + 1, 0x70); // Mode: standard digital
            memory::write<uint8_t>(rpc.recv_buffer + 2, 0xFF); // Buttons high (none pressed)
            memory::write<uint8_t>(rpc.recv_buffer + 3, 0xFF); // Buttons low (none pressed)
            memory::write<uint8_t>(rpc.recv_buffer + 4, 0x80); // Right stick X center
            memory::write<uint8_t>(rpc.recv_buffer + 5, 0x80); // Right stick Y center
            memory::write<uint8_t>(rpc.recv_buffer + 6, 0x80); // Left stick X center
            memory::write<uint8_t>(rpc.recv_buffer + 7, 0x80); // Left stick Y center
        }
        result = 1;
        break;
    }
        
    case 0x07: // PadSetMainMode
        g_logFile << "    [PADMAN] PadSetMainMode (success)" << std::endl;
        result = 1;
        break;
        
    case 0x08: // PadSetActDirect (rumble)
        g_logFile << "    [PADMAN] PadSetActDirect (stubbed)" << std::endl;
        result = 1;
        break;
        
    case 0x09: // PadSetActAlign
        g_logFile << "    [PADMAN] PadSetActAlign (stubbed)" << std::endl;
        result = 1;
        break;
        
    case 0x0A: // PadGetButtonMask
        g_logFile << "    [PADMAN] PadGetButtonMask (returning 0xFFFF)" << std::endl;
        result = 0xFFFF;
        break;
        
    case 0x0B: // PadSetButtonInfo
        g_logFile << "    [PADMAN] PadSetButtonInfo (stubbed)" << std::endl;
        result = 1;
        break;
        
    default:
        g_logFile << "    [PADMAN] Unhandled func 0x" << std::hex << rpc.func_num 
                  << " (returning 1)" << std::dec << std::endl;
        result = 1;
        break;
}

WriteResult32(rpc.recv_buffer, result);
}
// ============================================================================
// 0x80000400 - MCSERV (Memory Cards)
// ============================================================================
// Functions:
// 0xFE = McInit / VersionCheck
// 0x01 = McGetInfo
// 0x02 = McOpen
// 0x03 = McClose
// 0x04 = McSeek
// 0x05 = McRead / McDetectCard (context-dependent)
// 0x06 = McWrite
// 0x07 = McGetDir
// 0x08 = McSetFileInfo
// 0x09 = McDelete
// 0x0A = McFormat
// 0x0B = McUnformat
// 0x0C = McGetEntSpace
// 0x0D = McRename
// 0x0E = McChangeThreadPriority
// ============================================================================
void HandleMcServ(const SifRpcContext& rpc) {
g_logFile << " [HLE] MCSERV Call (Func: 0x" << std::hex << rpc.func_num
<< ")" << std::dec << std::endl;
int32_t result = 0;

switch (rpc.func_num) {
    case 0xFE: // McInit / VersionCheck
        g_logFile << "    [MCSERV] Init/VersionCheck (success)" << std::endl;
        result = 0;
        break;
        
    case 0x01: { // McGetInfo(port, slot, type, free, format)
        // Return: type=2 (PS2 MC), free=1000 clusters, formatted
        // Write to recv buffer: [type, free, format, result]
        if (rpc.recv_buffer) {
            memory::write<int32_t>(rpc.recv_buffer + 0x00, 2);     // Type: PS2 memory card
            memory::write<int32_t>(rpc.recv_buffer + 0x04, 1000);  // Free clusters
            memory::write<int32_t>(rpc.recv_buffer + 0x08, 1);     // Formatted = yes
        }
        g_logFile << "    [MCSERV] McGetInfo (PS2 MC, 1000 free, formatted)" << std::endl;
        result = 0;
        break;
    }
        
    case 0x02: // McOpen
        g_logFile << "    [MCSERV] McOpen (returning FD=1)" << std::endl;
        result = 1; // Fake file descriptor
        break;
        
    case 0x03: // McClose
        g_logFile << "    [MCSERV] McClose (success)" << std::endl;
        result = 0;
        break;
        
    case 0x04: // McSeek
        g_logFile << "    [MCSERV] McSeek (success)" << std::endl;
        result = 0;
        break;
        
    case 0x05: // McRead / McDetectCard
        g_logFile << "    [MCSERV] McDetectCard/McRead (success = card present)" << std::endl;
        result = 0;
        break;
        
    case 0x06: // McWrite
        g_logFile << "    [MCSERV] McWrite (stubbed success)" << std::endl;
        result = 0;
        break;
        
    case 0x07: { // McGetDir
        // Return 0 entries found (empty directory listing)
        g_logFile << "    [MCSERV] McGetDir (returning 0 entries)" << std::endl;
        result = 0;
        break;
    }
        
    case 0x08: // McSetFileInfo
        g_logFile << "    [MCSERV] McSetFileInfo (stubbed)" << std::endl;
        result = 0;
        break;
        
    case 0x09: // McDelete
        g_logFile << "    [MCSERV] McDelete (stubbed success)" << std::endl;
        result = 0;
        break;
        
    case 0x0A: // McFormat
        g_logFile << "    [MCSERV] McFormat (stubbed success)" << std::endl;
        result = 0;
        break;
        
    case 0x0B: // McUnformat
        g_logFile << "    [MCSERV] McUnformat (stubbed)" << std::endl;
        result = 0;
        break;
        
    case 0x0C: // McGetEntSpace
        g_logFile << "    [MCSERV] McGetEntSpace (returning 100)" << std::endl;
        result = 100; // 100 free directory entries
        break;
        
    case 0x0D: // McRename
        g_logFile << "    [MCSERV] McRename (stubbed success)" << std::endl;
        result = 0;
        break;
        
    case 0x0E: // McChangeThreadPriority
        g_logFile << "    [MCSERV] McChangeThreadPriority (stubbed)" << std::endl;
        result = 0;
        break;
        
    default:
        g_logFile << "    [MCSERV] Unhandled func 0x" << std::hex << rpc.func_num 
                  << " (stubbed 0)" << std::dec << std::endl;
        result = 0;
        break;
}

WriteResult32(rpc.recv_buffer, result);
}
// ============================================================================
// 0x80000592 - CDVD INIT
// ============================================================================
// Functions:
// 0x00 = sceCdInit(mode) -> 1 (ready)
// ============================================================================
void HandleCdvdInit(const SifRpcContext& rpc) {
g_logFile << " [HLE] CDVD Init (Func: 0x" << std::hex << rpc.func_num
<< ")" << std::dec << std::endl;
// Return 1 = drive ready for all functions
WriteResult32(rpc.recv_buffer, 1);
}
// ============================================================================
// 0x80000593 - CDVD S-COMMANDS (Synchronous)
// ============================================================================
// Functions:
// 0x01 = sceCdReadClock -> sceCdCLOCK struct (8 bytes)
// 0x02 = sceCdWriteClock
// 0x03 = sceCdGetDiskType -> disk type enum
// 0x04 = sceCdGetError -> error code
// 0x05 = sceCdTrayReq -> tray status
// 0x06 = sceCdApplySCmd -> raw S-command
// 0x09 = sceCdStatus -> drive status
// 0x0C = sceCdDiskType -> disk type
// 0x0F = sceCdOpenConfig
// 0x10 = sceCdCloseConfig
// 0x11 = sceCdReadConfig
// 0x12 = sceCdWriteConfig
// 0x13 = sceCdForbidDVDP
// 0x22 = sceCdReadModelID
// 0x24 = sceCdReadDvdDualInfo
// 0x27 = sceCdCancelPOffRdy
// ============================================================================
void HandleCdvdSCmd(const SifRpcContext& rpc) {
g_logFile << " [HLE] CDVD S-Command (Func: 0x" << std::hex << rpc.func_num
<< ")" << std::dec << std::endl;
switch (rpc.func_num) {
    case 0x01: { // sceCdReadClock
        if (rpc.recv_buffer) {
            // sceCdCLOCK struct: 8 bytes, BCD encoded
            // [stat, second, minute, hour, pad, day, month, year]
            memory::write<uint8_t>(rpc.recv_buffer + 0, 0x00); // stat = success
            memory::write<uint8_t>(rpc.recv_buffer + 1, 0x00); // second = 00
            memory::write<uint8_t>(rpc.recv_buffer + 2, 0x00); // minute = 00
            memory::write<uint8_t>(rpc.recv_buffer + 3, 0x12); // hour = 12 (BCD)
            memory::write<uint8_t>(rpc.recv_buffer + 4, 0x00); // pad
            memory::write<uint8_t>(rpc.recv_buffer + 5, 0x15); // day = 15 (BCD)
            memory::write<uint8_t>(rpc.recv_buffer + 6, 0x06); // month = 06 (BCD)
            memory::write<uint8_t>(rpc.recv_buffer + 7, 0x05); // year = 05 (BCD = 2005)
            g_logFile << "    [CDVD] ReadClock: 2005-06-15 12:00:00" << std::endl;
        }
        break;
    }
    
    case 0x02: // sceCdWriteClock
        g_logFile << "    [CDVD] WriteClock (stubbed)" << std::endl;
        WriteResult32(rpc.recv_buffer, 1);
        break;
    
    case 0x03: // sceCdGetDiskType
    case 0x0C: // sceCdDiskType
        // 0x14 = SCECdPS2DVD
        // 0x12 = SCECdPS2CD
        g_logFile << "    [CDVD] DiskType = PS2DVD (0x14)" << std::endl;
        WriteResult32(rpc.recv_buffer, 0x14);
        break;
        
    case 0x04: // sceCdGetError
        // 0x00 = no error
        g_logFile << "    [CDVD] GetError = 0 (no error)" << std::endl;
        WriteResult32(rpc.recv_buffer, 0x00);
        break;
        
    case 0x05: // sceCdTrayReq
        // Return 0 = success (tray closed)
        g_logFile << "    [CDVD] TrayReq (tray closed)" << std::endl;
        WriteResult32(rpc.recv_buffer, 0);
        break;
        
    case 0x06: // sceCdApplySCmd
        g_logFile << "    [CDVD] ApplySCmd (stubbed success)" << std::endl;
        WriteResult32(rpc.recv_buffer, 0);
        break;
        
    case 0x09: // sceCdStatus
        // 0x02 = SCECdStatPause (disc inserted, not spinning)
        // 0x0A = SCECdStatSpin (disc spinning)
        g_logFile << "    [CDVD] Status = Pause (0x02)" << std::endl;
        WriteResult32(rpc.recv_buffer, 0x02);
        break;
        
    case 0x0F: // sceCdOpenConfig
        g_logFile << "    [CDVD] OpenConfig (stubbed success)" << std::endl;
        WriteResult32(rpc.recv_buffer, 0);
        break;
        
    case 0x10: // sceCdCloseConfig
        g_logFile << "    [CDVD] CloseConfig (stubbed success)" << std::endl;
        WriteResult32(rpc.recv_buffer, 0);
        break;
        
    case 0x11: // sceCdReadConfig
        // Zero-fill the config buffer
        if (rpc.recv_buffer && rpc.recv_size > 0) {
            for (uint32_t i = 0; i < rpc.recv_size && i < 64; i++) {
                memory::write<uint8_t>(rpc.recv_buffer + i, 0);
            }
        }
        g_logFile << "    [CDVD] ReadConfig (zeroed)" << std::endl;
        break;
        
    case 0x12: // sceCdWriteConfig
        g_logFile << "    [CDVD] WriteConfig (stubbed success)" << std::endl;
        WriteResult32(rpc.recv_buffer, 0);
        break;
        
    case 0x13: // sceCdForbidDVDP
        g_logFile << "    [CDVD] ForbidDVDP (stubbed)" << std::endl;
        WriteResult32(rpc.recv_buffer, 0);
        break;
        
    case 0x22: // sceCdReadModelID
        // Write a fake model string
        if (rpc.recv_buffer) {
            memory::write<uint32_t>(rpc.recv_buffer, 0x00000000); // Empty model ID
        }
        g_logFile << "    [CDVD] ReadModelID (stubbed)" << std::endl;
        break;
        
    case 0x24: // sceCdReadDvdDualInfo
        // Return: not dual-layer
        if (rpc.recv_buffer) {
            memory::write<int32_t>(rpc.recv_buffer + 0, 0); // on_dual = false
            memory::write<int32_t>(rpc.recv_buffer + 4, 0); // layer1_start = 0
        }
        g_logFile << "    [CDVD] ReadDvdDualInfo (single layer)" << std::endl;
        break;
        
    case 0x27: // sceCdCancelPOffRdy
        g_logFile << "    [CDVD] CancelPOffRdy (stubbed)" << std::endl;
        WriteResult32(rpc.recv_buffer, 0);
        break;
        
    default:
        g_logFile << "    [CDVD S-CMD] Unhandled func 0x" << std::hex << rpc.func_num 
                  << " (stubbed 0)" << std::dec << std::endl;
        WriteResult32(rpc.recv_buffer, 0);
        break;
}
}
// ============================================================================
// 0x80000595 - CDVD N-COMMANDS (Asynchronous)
// ============================================================================
// Functions:
// 0x01 = sceCdRead(lba, sectors, buffer, mode)
// 0x02 = sceCdStandby
// 0x03 = sceCdStop
// 0x04 = sceCdSeek(lba)
// 0x05 = sceCdGetToc
// 0x06 = sceCdPause
// 0x07 = sceCdBreak
// 0x09 = sceCdReadIOPMem
// 0x0C = sceCdDiskReady (N-cmd variant)
// 0x0E = sceCdReadChain
// 0x12 = sceCdStream
// ============================================================================
void HandleCdvdNCmd(const SifRpcContext& rpc) {
g_logFile << " [HLE] CDVD N-Command (Func: 0x" << std::hex << rpc.func_num
<< ")" << std::dec << std::endl;
switch (rpc.func_num) {
    case 0x01: { // sceCdRead(lba, sectors, buffer, mode)
        uint32_t lba     = memory::read<uint32_t>(rpc.payload_addr + 0x00);
        uint32_t sectors = memory::read<uint32_t>(rpc.payload_addr + 0x04);
        uint32_t dest    = memory::read<uint32_t>(rpc.payload_addr + 0x08);
        
        // Mask to physical RAM
        uint32_t phys_addr = dest & 0x01FFFFFF;
        uint32_t size = sectors * 2048;
        
        g_logFile << "    [CDVD] Read: LBA=" << std::dec << lba 
                  << " Sectors=" << sectors 
                  << " -> 0x" << std::hex << phys_addr << std::dec << std::endl;
        
        if (g_isoFile) {
            uint64_t iso_offset = (uint64_t)lba * 2048;
            
            fseek(g_isoFile, 0, SEEK_END);
            uint64_t iso_size = ftell(g_isoFile);
            
            if (iso_offset + size <= iso_size) {
                fseek(g_isoFile, (long)iso_offset, SEEK_SET);
                
                // Try direct memory access first for speed
                uint8_t* dest_ptr = reinterpret_cast<uint8_t*>(memory::get_pointer(phys_addr));
                if (dest_ptr) {
                    size_t bytes_read = fread(dest_ptr, 1, size, g_isoFile);
                    g_logFile << "    [CDVD] Read " << bytes_read << " bytes (direct)" << std::endl;
                } else {
                    // Fallback: byte-by-byte
                    std::vector<uint8_t> buffer(size);
                    size_t bytes_read = fread(buffer.data(), 1, size, g_isoFile);
                    for (size_t k = 0; k < bytes_read; k++) {
                        memory::write<uint8_t>(phys_addr + k, buffer[k]);
                    }
                    g_logFile << "    [CDVD] Read " << bytes_read << " bytes (slow path)" << std::endl;
                }
            } else {
                g_logFile << "    [CDVD] ERROR: Read out of bounds!" << std::endl;
            }
        } else {
            g_logFile << "    [CDVD] ERROR: ISO file not open!" << std::endl;
        }
        
        WriteResult32(rpc.recv_buffer, 1); // Success
        break;
    }
    
    case 0x02: // sceCdStandby
        g_logFile << "    [CDVD] Standby (stubbed)" << std::endl;
        WriteResult32(rpc.recv_buffer, 1);
        break;
        
    case 0x03: // sceCdStop
        g_logFile << "    [CDVD] Stop (stubbed)" << std::endl;
        WriteResult32(rpc.recv_buffer, 1);
        break;
        
    case 0x04: // sceCdSeek
        g_logFile << "    [CDVD] Seek (stubbed success)" << std::endl;
        WriteResult32(rpc.recv_buffer, 1);
        break;
        
    case 0x05: // sceCdGetToc
        g_logFile << "    [CDVD] GetToc (stubbed: zero TOC)" << std::endl;
        if (rpc.recv_buffer) {
            // Zero-fill a minimal TOC (1024 bytes typical)
            for (int i = 0; i < 64; i++) {
                memory::write<uint32_t>(rpc.recv_buffer + i * 4, 0);
            }
        }
        WriteResult32(rpc.recv_buffer, 1);
        break;
        
    case 0x06: // sceCdPause
        g_logFile << "    [CDVD] Pause (stubbed)" << std::endl;
        WriteResult32(rpc.recv_buffer, 1);
        break;
        
    case 0x07: // sceCdBreak
        g_logFile << "    [CDVD] Break (stubbed)" << std::endl;
        WriteResult32(rpc.recv_buffer, 1);
        break;
        
    case 0x0C: // sceCdDiskReady (N-cmd variant)
        // Return 2 = SCECdComplete
        g_logFile << "    [CDVD] DiskReady = Complete (2)" << std::endl;
        WriteResult32(rpc.recv_buffer, 2);
        break;
        
    default:
        g_logFile << "    [CDVD N-CMD] Unhandled func 0x" << std::hex << rpc.func_num 
                  << " (stubbed 1)" << std::dec << std::endl;

           WriteResult32(rpc.recv_buffer, 1);
            break;
    }
}

// ============================================================================
// 0x80000597 - CDVD SEARCHFILE
// ============================================================================
// Functions:
//   0x01 = sceCdSearchFile(file_struct_ptr, filename)
//
// The game passes a filename and expects back a CdlFILE struct:
//   struct sceCdlFILE {
//       uint32_t lsn;       // Logical Sector Number (LBA)
//       uint32_t size;      // File size in bytes
//       char     name[16];  // Filename (null-terminated)
//       uint8_t  date[8];   // Date (year, month, day, hour, minute, second)
//   };
//
// For HLE: we don't have an ISO9660 parser yet, so we stub this.
// When a real ISO filesystem is implemented, this should look up the file
// in the ISO directory and return the actual LBA and size.
// ============================================================================
void HandleCdvdSearchFile(const SifRpcContext& rpc) {
    g_logFile << "    [HLE] CDVD SearchFile (Func: 0x" << std::hex << rpc.func_num 
              << ")" << std::dec << std::endl;
    
    switch (rpc.func_num) {
        case 0x01: { // sceCdSearchFile
            std::string filename;
            if (rpc.payload_addr != 0 && rpc.payload_size > 0) {
                // The filename is typically at an offset within the payload struct.
                // Common layout: [sceCdlFILE output_struct (32 bytes)] [char path[...]]
                // The path often starts at offset 0x20 (after the output struct)
                // but some SDK versions put it at offset 0x00.
                // Try reading from offset 0x00 first; if it looks like a path, use it.
                filename = ReadGuestString(rpc.payload_addr);
                
                // If the first read looks like garbage (no '/' or '\'), try offset 0x20
                if (filename.find('/') == std::string::npos && 
                    filename.find('\\') == std::string::npos &&
                    filename.find('.') == std::string::npos) {
                    filename = ReadGuestString(rpc.payload_addr + 0x20);
                }
            }
            
            g_logFile << "    [CDVD] SearchFile: \"" << filename << "\"" << std::endl;
            
            // TODO: Implement ISO9660 directory lookup
            // For now, return "file not found" (0)
            // When implemented:
            //   1. Parse ISO9660 PVD at LBA 16
            //   2. Walk directory tree to find the file
            //   3. Write sceCdlFILE struct to recv_buffer with LBA and size
            //   4. Return 1 (found)
            
            g_logFile << "    [CDVD] SearchFile: NOT FOUND (stub)" << std::endl;
            WriteResult32(rpc.recv_buffer, 0); // 0 = not found, 1 = found
            break;
        }
        
        default:
            g_logFile << "    [CDVD SearchFile] Unhandled func 0x" << std::hex << rpc.func_num 
                      << " (stubbed 0)" << std::dec << std::endl;
            WriteResult32(rpc.recv_buffer, 0);
            break;
    }
}

// ============================================================================
// 0x8000059A - CDVD DISK READY
// ============================================================================
// Functions:
//   0x01 = sceCdDiskReady(mode)
//
// Returns:
//   0x02 = SCECdComplete  (drive ready)
//   0x06 = SCECdNotReady  (drive busy)
//
// mode parameter:
//   0 = blocking (wait until ready)
//   1 = non-blocking (return immediately)
//
// For HLE we always return "ready" since there's no real drive.
// ============================================================================
void HandleCdvdDiskReady(const SifRpcContext& rpc) {
    g_logFile << "    [HLE] CDVD DiskReady (Func: 0x" << std::hex << rpc.func_num 
              << ")" << std::dec << std::endl;
    
    // SCECdComplete = 0x02 for all functions
    // This tells the game the disc is inserted and the drive is idle.
    WriteResult32(rpc.recv_buffer, 0x02);
    
    g_logFile << "    [CDVD] DiskReady = Complete (0x02)" << std::endl;
}

// ============================================================================
// 0x80000701 - LIBSD REMOTE (SDRDRV - Sound Driver)
// ============================================================================
// This is the IOP-side sound driver used by games via libsd.
// Functions:
//   0x00 = SdInit(flag)           -> 0 (success)
//   0x01 = SdSetParam             -> 0
//   0x02 = SdGetParam             -> 0
//   0x03 = SdSetSwitch            -> 0
//   0x04 = SdGetSwitch            -> 0
//   0x05 = SdSetAddr              -> 0
//   0x06 = SdGetAddr              -> 0
//   0x07 = SdSetCoreAttr          -> 0
//   0x08 = SdGetCoreAttr          -> 0
//   0x09 = SdNote2Pitch           -> pitch value
//   0x0A = SdPitch2Note           -> note value
//   0x0B = SdProcBatch            -> 0
//   0x0C = SdProcBatchEx          -> 0
//   0x0D = SdVoiceTrans           -> 0 (transfer started)
//   0x0E = SdBlockTrans           -> 0
//   0x0F = SdVoiceTransStatus     -> 0 (transfer complete)
//   0x10 = SdBlockTransStatus     -> 0 (transfer complete)
//   0x11 = SdSetEffectAttr        -> 0
//   0x12 = SdGetEffectAttr        -> 0
//   0x13 = SdSetTransCallback     -> 0
//   0x14 = SdSetIRQCallback       -> 0
//   0x19 = SdSetEffectMode        -> 0
//   0x1A = SdClearEffectWorkArea  -> 0
//
// For HLE: we stub everything to success/silence. No actual audio output.
// ============================================================================
void HandleLibSd(const SifRpcContext& rpc) {
    g_logFile << "    [HLE] LIBSD Call (Func: 0x" << std::hex << rpc.func_num 
              << ")" << std::dec << std::endl;
    
    int32_t result = 0;
    
    switch (rpc.func_num) {
        case 0x00: // SdInit
            g_logFile << "    [LIBSD] SdInit (success, audio muted)" << std::endl;
            result = 0;
            break;
            
        case 0x01: // SdSetParam
        case 0x03: // SdSetSwitch
        case 0x05: // SdSetAddr
        case 0x07: // SdSetCoreAttr
            g_logFile << "    [LIBSD] Set operation 0x" << std::hex << rpc.func_num 
                      << " (stubbed)" << std::dec << std::endl;
            result = 0;
            break;
            
        case 0x02: // SdGetParam
        case 0x04: // SdGetSwitch
        case 0x06: // SdGetAddr
        case 0x08: // SdGetCoreAttr
            g_logFile << "    [LIBSD] Get operation 0x" << std::hex << rpc.func_num 
                      << " (returning 0)" << std::dec << std::endl;
            result = 0;
            break;
            
        case 0x09: // SdNote2Pitch
            // Returns a pitch value. 0x1000 = base pitch (44100 Hz)
            g_logFile << "    [LIBSD] Note2Pitch (returning 0x1000)" << std::endl;
            result = 0x1000;
            break;
            
        case 0x0A: // SdPitch2Note
            // Returns a note value. 60 = middle C
            g_logFile << "    [LIBSD] Pitch2Note (returning 60)" << std::endl;
            result = 60;
            break;
            
        case 0x0B: // SdProcBatch
        case 0x0C: // SdProcBatchEx
            g_logFile << "    [LIBSD] ProcBatch (stubbed)" << std::endl;
            result = 0;
            break;
            
        case 0x0D: // SdVoiceTrans
        case 0x0E: // SdBlockTrans
            g_logFile << "    [LIBSD] Transfer started (stubbed)" << std::endl;
            result = 0; // Transfer "started"
            break;
            
        case 0x0F: // SdVoiceTransStatus
        case 0x10: // SdBlockTransStatus
            // 0 = transfer complete, 1 = in progress
            g_logFile << "    [LIBSD] TransferStatus = complete (0)" << std::endl;
            result = 0;
            break;
            
        case 0x11: // SdSetEffectAttr
        case 0x19: // SdSetEffectMode
        case 0x1A: // SdClearEffectWorkArea
            g_logFile << "    [LIBSD] Effect operation 0x" << std::hex << rpc.func_num 
                      << " (stubbed)" << std::dec << std::endl;
            result = 0;
            break;
            
        case 0x12: // SdGetEffectAttr
            g_logFile << "    [LIBSD] GetEffectAttr (returning 0)" << std::endl;
            result = 0;
            break;
            
        case 0x13: // SdSetTransCallback
        case 0x14: // SdSetIRQCallback
            g_logFile << "    [LIBSD] SetCallback 0x" << std::hex << rpc.func_num 
                      << " (stubbed, no callbacks)" << std::dec << std::endl;
            result = 0; // Return old callback (NULL)
            break;
            
        default:
            g_logFile << "    [LIBSD] Unhandled func 0x" << std::hex << rpc.func_num 
                      << " (stubbed 0)" << std::dec << std::endl;
            result = 0;
            break;
    }
    
    WriteResult32(rpc.recv_buffer, result);
}

// ============================================================================
// 0x80000901 - 0x80000905 - MTAPMAN (Multitap)
// ============================================================================
// Server IDs:
//   0x80000901 = MTAP Port Open
//   0x80000902 = MTAP Port Close
//   0x80000903 = MTAP Get Connection
//   0x80000904 = MTAP Unknown 4
//   0x80000905 = MTAP Unknown 5
//
// Functions within each server:
//   Typically 0x01 = the main operation
//
// For HLE: We report "no multitap connected" for all operations.
// This causes games to fall back to single-controller mode.
// ============================================================================
void HandleMtap(const SifRpcContext& rpc) {
    g_logFile << "    [HLE] MTAPMAN Call (Server: 0x" << std::hex << rpc.server_id
              << ", Func: 0x" << rpc.func_num << ")" << std::dec << std::endl;
    
    int32_t result = 0;
    
    switch (rpc.server_id) {
        case 0x80000901: { // MTAP Port Open
            // Open a multitap port. Return 1 = success (port opened, but no tap connected).
            // Some games require this to succeed even without a multitap.
            g_logFile << "    [MTAP] PortOpen (success, no multitap)" << std::endl;
            result = 1;
            break;
        }
        
        case 0x80000902: { // MTAP Port Close
            g_logFile << "    [MTAP] PortClose (success)" << std::endl;
            result = 1;
            break;
        }
        
        case 0x80000903: { // MTAP Get Connection
            // Returns number of controllers connected to the multitap on this port.
            // 0 = no multitap / 1 controller (direct connection)
            // 1-4 = multitap with N controllers
            // Returning 1 means "1 controller, no multitap" which is standard.
            g_logFile << "    [MTAP] GetConnection = 1 (single controller)" << std::endl;
            result = 1;
            break;
        }
        
        case 0x80000904: { // Unknown 4
            g_logFile << "    [MTAP] Unknown4 (stubbed 0)" << std::endl;
            result = 0;
            break;
        }
        
        case 0x80000905: { // Unknown 5
            g_logFile << "    [MTAP] Unknown5 (stubbed 0)" << std::endl;
            result = 0;
            break;
        }
        
        default:
            g_logFile << "    [MTAP] Unknown server 0x" << std::hex << rpc.server_id 
                      << " (stubbed 0)" << std::dec << std::endl;
            result = 0;
            break;
    }
    
    WriteResult32(rpc.recv_buffer, result);
}

// ============================================================================
// 0x80001400 - EYETOY
// ============================================================================
// Functions:
//   0x00 = EyeToyInit       -> 0 (no camera)
//   0x01 = EyeToyGetFrame   -> 0
//   0x02 = EyeToySetParam   -> 0
//   0x03 = EyeToyGetStatus  -> 0 (not connected)
//
// For HLE: Report "no camera connected" for everything.
// ============================================================================
void HandleEyeToy(const SifRpcContext& rpc) {
    g_logFile << "    [HLE] EyeToy Call (Func: 0x" << std::hex << rpc.func_num 
              << ")" << std::dec << std::endl;
    
    int32_t result = 0;
    
    switch (rpc.func_num) {
        case 0x00: // EyeToyInit
            g_logFile << "    [EYETOY] Init (no camera)" << std::endl;
            result = 0;
            break;
            
        case 0x01: // EyeToyGetFrame
            g_logFile << "    [EYETOY] GetFrame (no data)" << std::endl;
            result = 0;
            break;
            
        case 0x02: // EyeToySetParam
            g_logFile << "    [EYETOY] SetParam (stubbed)" << std::endl;
            result = 0;
            break;
            
        case 0x03: // EyeToyGetStatus
            // 0 = not connected
            g_logFile << "    [EYETOY] GetStatus = not connected (0)" << std::endl;
            result = 0;
            break;
            
        default:
            g_logFile << "    [EYETOY] Unhandled func 0x" << std::hex << rpc.func_num 
                      << " (stubbed 0)" << std::dec << std::endl;
            result = 0;
            break;
    }
    
    WriteResult32(rpc.recv_buffer, result);
}

// ============================================================================
// 0x12345 - CUSTOM ISO LOADER (Game-specific)
// ============================================================================
// This is a non-system server registered by Crash Twinsanity's overlay loader.
// It reads raw sectors from the ISO and loads them into EE RAM.
//
// Function 0x00:
//   Payload struct (at Transfer 0 src):
//     +0x00: uint32_t lba        - Starting logical block address
//     +0x04: uint32_t size       - Number of bytes to read
//     +0x1C: uint32_t dest_addr  - EE RAM destination (may have flags in low bits)
// ============================================================================
void HandleCustomLoader(const SifRpcContext& rpc) {
    g_logFile << "    [HLE] Custom Server 0x12345 Call (Func: 0x" << std::hex 
              << rpc.func_num << ")" << std::dec << std::endl;
    
    if (rpc.func_num == 0x00) {
        // Read the command struct from the payload
        // For this game-specific server, the payload IS the command struct
        uint32_t cmd_ptr = rpc.payload_addr;
        
        if (cmd_ptr == 0) {
            g_logFile << "    [CustomLoader] ERROR: No payload!" << std::endl;
            WriteResult32(rpc.recv_buffer, 0);
            return;
        }
        
        uint32_t lba  = memory::read<uint32_t>(cmd_ptr + 0x00);
        uint32_t size = memory::read<uint32_t>(cmd_ptr + 0x04);
        
        // Destination address - mask off alignment/flag bits
        uint32_t raw_dest  = memory::read<uint32_t>(cmd_ptr + 0x1C);
        uint32_t dest_addr = raw_dest & 0xFFFFFFFC; // Align to 4 bytes
        
        g_logFile << "    [CustomLoader] LBA=" << std::dec << lba 
                  << " Size=" << size 
                  << " -> RAM 0x" << std::hex << dest_addr << std::dec << std::endl;
        
        if (g_isoFile) {
            uint64_t iso_offset = (uint64_t)lba * 2048;
            
            fseek(g_isoFile, 0, SEEK_END);
            uint64_t iso_size = ftell(g_isoFile);
            
            if (iso_offset + size <= iso_size) {
                fseek(g_isoFile, (long)iso_offset, SEEK_SET);
                
                // Try direct memory access for speed
                uint8_t* dest_ptr = reinterpret_cast<uint8_t*>(memory::get_pointer(dest_addr));
                if (dest_ptr) {
                    size_t bytes_read = fread(dest_ptr, 1, size, g_isoFile);
                    g_logFile << "    [CustomLoader] Loaded " << bytes_read 
                              << " bytes (direct)" << std::endl;
                } else {
                    // Fallback: chunked write through memory system
                    std::vector<uint8_t> buffer(size);
                    size_t bytes_read = fread(buffer.data(), 1, size, g_isoFile);
                    for (size_t k = 0; k < bytes_read; k++) {
                        memory::write<uint8_t>(dest_addr + k, buffer[k]);
                    }
                    g_logFile << "    [CustomLoader] Loaded " << bytes_read 
                              << " bytes (slow path)" << std::endl;
                }
            } else {
                g_logFile << "    [CustomLoader] ERROR: Read out of bounds! "
                          << "offset=0x" << std::hex << iso_offset 
                          << " size=" << std::dec << size 
                          << " iso_size=" << iso_size << std::endl;
            }
        } else {
            g_logFile << "    [CustomLoader] ERROR: ISO file not open!" << std::endl;
        }
        
        WriteResult32(rpc.recv_buffer, 1); // Success
    } else {
        g_logFile << "    [CustomLoader] Unknown func 0x" << std::hex << rpc.func_num 
                  << " (stubbed 0)" << std::dec << std::endl;
        WriteResult32(rpc.recv_buffer, 0);
    }
}

// ============================================================================
// UNKNOWN / UNDOCUMENTED SERVERS
// ============================================================================
// Handles any server ID not in the known list.
// Known undocumented servers seen in logs:
//   0x8000059C - Possibly CDVD streaming extension (func 0x0 = init)
// ============================================================================
void HandleUnknownServer(const SifRpcContext& rpc) {
    g_logFile << "    [SIF] Unhandled RPC Call (Server: 0x" << std::hex << rpc.server_id 
              << ", Func: 0x" << rpc.func_num << ")" << std::dec << std::endl;
    
    // Best-effort response: write 0 to recv_buffer
    // This allows the game to continue for init/version-check calls
    // that only check for non-negative return values.
    WriteResult32(rpc.recv_buffer, 0);
}

