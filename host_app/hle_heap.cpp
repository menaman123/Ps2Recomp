#include "cpu_state.h"
#include "hle_heap.h"
#include "memory.h"
#include "gif.h"
#include <fstream>
#include <vector>
#include <map>
#include <string>
#include <cstring>
#include <iomanip>

HLEHeap g_heap;

extern std::ofstream g_logFile;
extern FILE* g_isoFile;


struct HleCdvdFile {
    uint32_t lba;
    uint32_t size;
    bool found;
    uint32_t offset; 
};
struct ArchiveEntry {
    uint32_t offset_in_bd;  // byte offset within CRASH.BD
    uint32_t size;
};

static std::map<std::string, ArchiveEntry> g_archive_entries;
static uint32_t g_crash_bd_lba = 0;
static bool g_archive_initialized = false;

static std::map<int, HleCdvdFile> g_hle_cdvd_files;
static int g_hle_cdvd_last_handle = -1;

static std::string NormalizePath(const std::string& path) {
    std::string result = path;
    for (char& c : result) {
        if (c == '/') c = '\\';
        if (c >= 'A' && c <= 'Z') c += 32;  // lowercase
    }
    return result;
}



// ============================================================
// ISO9660 Directory Search
// ============================================================
static HleCdvdFile SearchIso9660(const std::string& search_path) {
    HleCdvdFile result = {0, 0, false};
    if (!g_isoFile) {
        g_logFile << "HLE CDVD Search Failed: No ISO file loaded." << std::endl;
        return result;
    }
    g_logFile << "HLE CDVD Search: '" << search_path << "'" << std::endl;

    if (search_path == "StartUp\\Icons.psm"){
        g_logFile << "FOUND '" << search_path << "'" << std::endl;
    }


static bool s_dumped_root = false;
if (!s_dumped_root) {
    s_dumped_root = true;
    g_logFile << "[ISO9660] === ROOT DIRECTORY LISTING ===" << std::endl;
    
    // Read PVD
    uint8_t pvd[2048];
    fseek(g_isoFile, 16 * 2048, SEEK_SET);
    size_t pvd_read = fread(pvd, 1, 2048, g_isoFile);
    g_logFile << "[ISO9660] PVD read: " << pvd_read << " bytes" << std::endl;
    g_logFile << "[ISO9660] PVD sig: " << (int)pvd[0] << " '" 
              << pvd[1] << pvd[2] << pvd[3] << pvd[4] << pvd[5] << "'" << std::endl;
    
    if (pvd[0] == 1 && memcmp(pvd + 1, "CD001", 5) == 0) {
        uint32_t root_lba = *(uint32_t*)(pvd + 156 + 2);
        uint32_t root_size = *(uint32_t*)(pvd + 156 + 10);
        g_logFile << "[ISO9660] Root dir: LBA=" << root_lba 
                  << " Size=" << root_size << std::endl;
        
        std::vector<uint8_t> root_data(root_size);
        fseek(g_isoFile, (uint64_t)root_lba * 2048, SEEK_SET);
        fread(root_data.data(), 1, root_size, g_isoFile);
        
        uint32_t off = 0;
        int entry_count = 0;
        while (off < root_size && entry_count < 100) {
            uint8_t rec_len = root_data[off];
            if (rec_len == 0) {
                off = ((off / 2048) + 1) * 2048;
                if (off >= root_size) break;
                continue;
            }
            
            uint8_t name_len = root_data[off + 32];
            uint32_t e_lba = *(uint32_t*)&root_data[off + 2];
            uint32_t e_size = *(uint32_t*)&root_data[off + 10];
            uint8_t flags = root_data[off + 25];
            
            std::string name;
            if (name_len == 1 && root_data[off + 33] == 0) {
                name = ".";
            } else if (name_len == 1 && root_data[off + 33] == 1) {
                name = "..";
            } else {
                name = std::string((char*)&root_data[off + 33], name_len);
            }
            
            g_logFile << "[ISO9660]   " << (flags & 0x02 ? "DIR " : "FILE")
                      << " '" << name << "'"
                      << " LBA=" << e_lba
                      << " Size=" << e_size << std::endl;
            
            // If it's a directory, list its contents too
            if ((flags & 0x02) && name != "." && name != ".." && entry_count < 50) {
                std::vector<uint8_t> sub_data(e_size);
                fseek(g_isoFile, (uint64_t)e_lba * 2048, SEEK_SET);
                fread(sub_data.data(), 1, e_size, g_isoFile);
                
                uint32_t sub_off = 0;
                while (sub_off < e_size) {
                    uint8_t sub_len = sub_data[sub_off];
                    if (sub_len == 0) {
                        sub_off = ((sub_off / 2048) + 1) * 2048;
                        if (sub_off >= e_size) break;
                        continue;
                    }
                    uint8_t sub_name_len = sub_data[sub_off + 32];
                    uint32_t sub_lba = *(uint32_t*)&sub_data[sub_off + 2];
                    uint32_t sub_size = *(uint32_t*)&sub_data[sub_off + 10];
                    uint8_t sub_flags = sub_data[sub_off + 25];
                    
                    std::string sub_name;
                    if (sub_name_len == 1 && sub_data[sub_off + 33] == 0) {
                        sub_name = ".";
                    } else if (sub_name_len == 1 && sub_data[sub_off + 33] == 1) {
                        sub_name = "..";
                    } else {
                        sub_name = std::string((char*)&sub_data[sub_off + 33], sub_name_len);
                    }
                    
                    g_logFile << "[ISO9660]     " << (sub_flags & 0x02 ? "DIR " : "FILE")
                              << " '" << sub_name << "'"
                              << " LBA=" << sub_lba
                              << " Size=" << sub_size << std::endl;
                    
                    sub_off += sub_len;
                }
            }
            
            off += rec_len;
            entry_count++;
        }
        g_logFile << "[ISO9660] === END LISTING ===" << std::endl;
    } else {
        g_logFile << "[ISO9660] ERROR: Invalid PVD signature!" << std::endl;
    }
}


    
    // Read Primary Volume Descriptor at LBA 16
    uint8_t sector[2048];
    fseek(g_isoFile, 16 * 2048, SEEK_SET);
    if (fread(sector, 1, 2048, g_isoFile) != 2048) return result;
    
    // Verify PVD signature
    if (sector[0] != 1 || memcmp(sector + 1, "CD001", 5) != 0) return result;
    
    // Root directory record at PVD offset 156
    uint32_t dir_lba  = *(uint32_t*)(sector + 156 + 2);
    uint32_t dir_size = *(uint32_t*)(sector + 156 + 10);
    
    // Normalize search path
    std::string normalized = search_path;
    for (char& c : normalized) {
        if (c == '\\') c = '/';
        if (c >= 'a' && c <= 'z') c -= 32;
    }
    if (!normalized.empty() && normalized[0] == '/')
        normalized = normalized.substr(1);
    
    // Strip device prefix (cdrom0:/ etc)
    size_t colon = normalized.find(':');
    if (colon != std::string::npos) {
        normalized = normalized.substr(colon + 1);
        if (!normalized.empty() && normalized[0] == '/')
            normalized = normalized.substr(1);
    }
    
    // Split into path components
    std::vector<std::string> components;
    std::string current;
    for (char c : normalized) {
        if (c == '/') {
            if (!current.empty()) { components.push_back(current); current.clear(); }
        } else {
            current += c;
        }
    }
    if (!current.empty()) components.push_back(current);
    
    if (components.empty()) return result;
    
    // Walk directory tree

    g_logFile << "[ISO9660] Searching for components: ";
    for (size_t i = 0; i < components.size(); i++) {
        g_logFile << "[" << i << "]='" << components[i] << "' ";
    }
    g_logFile << std::endl;

    for (size_t comp_idx = 0; comp_idx < components.size(); comp_idx++) {
        bool is_last = (comp_idx == components.size() - 1);
        std::string& target = components[comp_idx];
        bool found_entry = false;
        
        // Read directory extent
        std::vector<uint8_t> dir_data(dir_size);
        fseek(g_isoFile, (uint64_t)dir_lba * 2048, SEEK_SET);
        fread(dir_data.data(), 1, dir_size, g_isoFile);
        
        uint32_t offset = 0;
        while (offset < dir_size) {
            uint8_t record_len = dir_data[offset];
            if (record_len == 0) {
                offset = ((offset / 2048) + 1) * 2048;
                if (offset >= dir_size) break;
                continue;
            }
            
            uint8_t name_len = dir_data[offset + 32];
            if (name_len > 0 && offset + 33 + name_len <= dir_size) {
                std::string entry_name((char*)&dir_data[offset + 33], name_len);
                
                // Strip version (;1)
                size_t semi = entry_name.find(';');
                if (semi != std::string::npos) entry_name = entry_name.substr(0, semi);
                // Strip trailing dot
                if (!entry_name.empty() && entry_name.back() == '.')
                    entry_name.pop_back();
                // Uppercase
                for (char& c : entry_name)
                    if (c >= 'a' && c <= 'z') c -= 32;
                
                if (entry_name == target) {
                    uint32_t entry_lba  = *(uint32_t*)&dir_data[offset + 2];
                    uint32_t entry_size = *(uint32_t*)&dir_data[offset + 10];
                    uint8_t  flags      = dir_data[offset + 25];
                    
                    if (is_last) {
                        result.lba = entry_lba;
                        result.size = entry_size;
                        result.found = true;
                        return result;
                    } else if (flags & 0x02) { // Directory
                        dir_lba = entry_lba;
                        dir_size = entry_size;
                        found_entry = true;
                        break;
                    }
                }
            }
            offset += record_len;
        }
        
        if (!found_entry) return result;
    }
    
    return result;
}

void InitArchiveTable() {
    if (g_archive_initialized || !g_isoFile) return;
    g_archive_initialized = true;
    
    HleCdvdFile bh = SearchIso9660("CRASH6/CRASH.BH");
    HleCdvdFile bd = SearchIso9660("CRASH6/CRASH.BD");
    
    if (!bh.found || !bd.found) {
        g_logFile << "[ARCHIVE] ERROR: CRASH.BH or CRASH.BD not found!" << std::endl;
        return;
    }
    
    g_crash_bd_lba = bd.lba;
    g_logFile << "[ARCHIVE] CRASH.BD LBA=" << bd.lba << std::endl;
    
    // Read entire CRASH.BH
    std::vector<uint8_t> header(bh.size);
    fseek(g_isoFile, (uint64_t)bh.lba * 2048, SEEK_SET);
    fread(header.data(), 1, bh.size, g_isoFile);
    
    uint32_t num_entries = *(uint32_t*)&header[0];
    g_logFile << "[ARCHIVE] Parsing " << num_entries << " entries..." << std::endl;
    
    uint32_t pos = 4;  // skip num_entries
    for (uint32_t i = 0; i < num_entries && pos + 4 < bh.size; i++) {
        uint32_t str_len = *(uint32_t*)&header[pos];
        pos += 4;
        
        if (pos + str_len + 8 > bh.size) {
            g_logFile << "[ARCHIVE] WARNING: Truncated at entry " << i << std::endl;
            break;
        }
        
        std::string path((char*)&header[pos], str_len);
        pos += str_len;
        
        uint32_t offset = *(uint32_t*)&header[pos];
        pos += 4;
        uint32_t size = *(uint32_t*)&header[pos];
        pos += 4;
        
        std::string normalized = NormalizePath(path);
        g_archive_entries[normalized] = {offset, size};
        
        if (i < 10) {
            g_logFile << "[ARCHIVE]   [" << i << "] '" << path 
                      << "' offset=" << offset << " size=" << size << std::endl;
        }
    }
    
    g_logFile << "[ARCHIVE] Loaded " << g_archive_entries.size() << " entries" << std::endl;
}

// ============================================================
// HLE Replacement for FUN_002ad8f0
// ============================================================



void oldInitArchiveTable() {
    if (g_archive_initialized || !g_isoFile) return;
    g_archive_initialized = true;
    
    // First, find CRASH.BH and CRASH.BD on the ISO
    HleCdvdFile bh = SearchIso9660("CRASH6/CRASH.BH");
    HleCdvdFile bd = SearchIso9660("CRASH6/CRASH.BD");
    
    if (!bh.found || !bd.found) {
        g_logFile << "[ARCHIVE] ERROR: CRASH.BH or CRASH.BD not found!" << std::endl;
        return;
    }
    
    g_crash_bd_lba = bd.lba;
    g_logFile << "[ARCHIVE] CRASH.BH: LBA=" << bh.lba << " Size=" << bh.size << std::endl;
    g_logFile << "[ARCHIVE] CRASH.BD: LBA=" << bd.lba << " Size=" << bd.size << std::endl;
    
    // Read the entire CRASH.BH header
    std::vector<uint8_t> header(bh.size);
    fseek(g_isoFile, (uint64_t)bh.lba * 2048, SEEK_SET);
    fread(header.data(), 1, bh.size, g_isoFile);
    
    // Dump first 256 bytes to understand the format
    g_logFile << "[ARCHIVE] CRASH.BH header dump:" << std::endl;
    for (int i = 0; i < 256 && i < (int)bh.size; i += 16) {
        g_logFile << "  [" << std::hex << std::setw(4) << std::setfill('0') << i << "] ";
        for (int j = 0; j < 16 && (i + j) < (int)bh.size; j++) {
            g_logFile << std::hex << std::setw(2) << std::setfill('0') 
                      << (int)header[i + j] << " ";
        }
        g_logFile << " | ";
        for (int j = 0; j < 16 && (i + j) < (int)bh.size; j++) {
            char c = header[i + j];
            g_logFile << ((c >= 32 && c <= 126) ? c : '.');
        }
        g_logFile << std::endl;
    }
    
    // Also dump as 32-bit words
    g_logFile << "[ARCHIVE] CRASH.BH as uint32s:" << std::endl;
    for (int i = 0; i < 64 && i * 4 < (int)bh.size; i++) {
        uint32_t val = *(uint32_t*)&header[i * 4];
        g_logFile << "  [" << std::dec << i << "] = 0x" << std::hex << val 
                  << " (" << std::dec << val << ")" << std::endl;
    }
    
    // Scan for readable strings in the header
    g_logFile << "[ARCHIVE] Strings found in CRASH.BH:" << std::endl;
    std::string current;
    int string_start = -1;
    for (int i = 0; i < (int)bh.size; i++) {
        char c = header[i];
        if (c >= 32 && c <= 126) {
            if (current.empty()) string_start = i;
            current += c;
        } else {
            if (current.length() >= 4) {
                g_logFile << "  [0x" << std::hex << string_start << "] '" 
                          << current << "'" << std::endl;
            }
            current.clear();
        }
    }
}

void HLE_FUN_002ad8f0(CpuContext& ctx, uint32_t /*addr*/) {
    InitArchiveTable();
    
    uint32_t path_ptr = ctx.cpuRegs.GPR.r[4].UL[0];
    std::string path;
    for (int i = 0; i < 255; i++) {
        char c = memory::read<uint8_t>(path_ptr + i);
        if (c == 0) break;
        path += c;
    }
    
    // Get and increment counter
    uint32_t gp = ctx.cpuRegs.GPR.r[28].UL[0];
    uint32_t counter_addr = static_cast<uint32_t>(gp + 0xFFFF8B28);
    int32_t handle = memory::read<int32_t>(counter_addr);
    memory::write<int32_t>(counter_addr, handle + 1);
    if (handle < 0) {
        handle = 0;
        memory::write<int32_t>(counter_addr, 1);
    }
    
    // Try ISO9660 first
    HleCdvdFile entry = SearchIso9660(path);
    
    // If not on ISO, search CRASH.BH archive
    if (!entry.found && g_crash_bd_lba != 0) {
        std::string normalized = NormalizePath(path);
        
        // 1. Exact match
        auto it = g_archive_entries.find(normalized);
        if (it != g_archive_entries.end()) {
            entry.found = true;
            entry.lba = g_crash_bd_lba;
            entry.size = it->second.size;
            entry.offset = it->second.offset_in_bd;
            
            g_logFile << "[HLE CDVD] ARCHIVE EXACT: '" << path
                      << "' bd_offset=" << std::hex << it->second.offset_in_bd
                      << " size=" << std::dec << it->second.size << std::endl;
        }
        
        // 2. Directory prefix match (path + backslash)
        if (!entry.found) {
            std::string dir_prefix = normalized;
            if (!dir_prefix.empty() && dir_prefix.back() != '\\') dir_prefix += '\\';
            
            for (auto& kv : g_archive_entries) {
                if (kv.first.substr(0, dir_prefix.size()) == dir_prefix) {
                    entry.found = true;
                    entry.lba = g_crash_bd_lba;
                    entry.size = kv.second.size;
                    entry.offset = kv.second.offset_in_bd;
                    
                    g_logFile << "[HLE CDVD] ARCHIVE DIR PREFIX: '" << path
                              << "' matched '" << kv.first
                              << "' bd_offset=" << std::hex << kv.second.offset_in_bd
                              << " size=" << std::dec << kv.second.size << std::endl;
                    break;
                }
            }
        }
        
        // 3. File prefix match (path without backslash — matches "hub01" → "hub01.psm")
        if (!entry.found) {
            for (auto& kv : g_archive_entries) {
                if (kv.first.size() > normalized.size() &&
                    kv.first.substr(0, normalized.size()) == normalized) {
                    // Make sure the next character after the prefix is a dot or backslash
                    // This prevents "hub01" matching "hub01x.psm"
                    char next_char = kv.first[normalized.size()];
                    if (next_char == '.' || next_char == '\\') {
                        entry.found = true;
                        entry.lba = g_crash_bd_lba;
                        entry.size = kv.second.size;
                        entry.offset = kv.second.offset_in_bd;
                        
                        g_logFile << "[HLE CDVD] ARCHIVE FILE PREFIX: '" << path
                                  << "' matched '" << kv.first
                                  << "' bd_offset=" << std::hex << kv.second.offset_in_bd
                                  << " size=" << std::dec << kv.second.size << std::endl;
                        break;
                    }
                }
            }
        }
    }
    
    if (entry.found) {
        g_hle_cdvd_files[handle] = entry;
        g_hle_cdvd_last_handle = handle;
        g_logFile << "[HLE CDVD] Opened '" << path << "' handle=" << handle
                  << " lba=" << std::hex << entry.lba
                  << " offset=" << entry.offset
                  << " size=" << std::dec << entry.size << std::endl;
    } else {
        g_hle_cdvd_files[handle] = {0, 0, false, 0};
        g_hle_cdvd_last_handle = handle;
        g_logFile << "[HLE CDVD] NOT FOUND: '" << path << "'" << std::endl;
    }
    
    ctx.cpuRegs.GPR.r[2].SL[0] = handle;
}



// ============================================================
// HLE Replacement for FUN_002adb18
// ============================================================
void HLE_FUN_002adb18(CpuContext& ctx, uint32_t /*addr*/) {
    // Return file size for the last opened handle
    uint32_t size = 0;
    
    if (g_hle_cdvd_last_handle >= 0) {
        auto it = g_hle_cdvd_files.find(g_hle_cdvd_last_handle);
        if (it != g_hle_cdvd_files.end() && it->second.found) {
            size = it->second.size;
        }
    }
    
    g_logFile << "[HLE CDVD] GetFileSize → " << size 
              << " (handle=" << g_hle_cdvd_last_handle << ")" << std::endl;
    
    ctx.cpuRegs.GPR.r[2].UL[0] = size;
}

void HLE_FUN_002adb40(CpuContext& ctx, uint32_t /*addr*/) {
    int32_t handle = ctx.cpuRegs.GPR.r[4].SL[0]; // a0: handle to close
    
    g_logFile << "[HLE CDVD] CloseHandle(" << std::dec << handle << ")" << std::endl;
    
    // Remove from our tracking map if it exists
    auto it = g_hle_cdvd_files.find(handle);
    if (it != g_hle_cdvd_files.end()) {
        g_hle_cdvd_files.erase(it);
    }
    
    // Return 0 (success)
    ctx.cpuRegs.GPR.r[2].UL[0] = 0;
}


void HLE_001815c0(CpuContext& ctx) {
    uint32_t size = ctx.cpuRegs.GPR.r[4].UL[0]; // a0 is the ONLY parameter

    uint32_t ptr = 0;

    if (size < 0x10000) {
        ptr = g_heap.alloc_high(size);
    } else {
        ptr = g_heap.alloc(size);
    }

    if (ptr == 0) {
        printf("Malloc Failed! Size: %u\n", size);
        g_logFile << "Malloc Failed! Size: " << size << std::endl;
    }

    ctx.cpuRegs.GPR.r[2].UL[0] = ptr;
}

void HLE_001815f0(CpuContext& ctx) { // free
    uint32_t ptr = ctx.cpuRegs.GPR.r[4].UL[0];
    
    // Log the free
    printf("Free: freeing ptr 0x%X\n", ptr);
    g_logFile << "Free: freeing ptr 0x" << std::hex << ptr << std::endl;
    
    g_heap.free(ptr);
}


void HLE_FUN_002aac80(CpuContext& ctx, uint32_t /*addr*/) {
    g_logFile << "!!! HLE_FUN_002aac80 CALLED !!!" << std::endl;
    
    uint32_t fs_obj = ctx.cpuRegs.GPR.r[4].UL[0];
    int32_t  offset = ctx.cpuRegs.GPR.r[5].SL[0];
    uint32_t size   = ctx.cpuRegs.GPR.r[6].UL[0];
    uint32_t dest   = ctx.cpuRegs.GPR.r[7].UL[0];
    uint32_t sync   = ctx.cpuRegs.GPR.r[8].UL[0];
    uint32_t status = ctx.cpuRegs.GPR.r[9].UL[0];
    
    uint32_t file_size = memory::read<uint32_t>(fs_obj + 0x30);
    uint32_t handle    = memory::read<uint32_t>(fs_obj + 0x28);
    
    // Get LBA and bd_offset from OUR handle table, not from game memory
    uint32_t start_lba = 0;
    uint32_t bd_byte_offset = 0;
    
    auto it = g_hle_cdvd_files.find(handle);
    if (it != g_hle_cdvd_files.end() && it->second.found) {
        start_lba = it->second.lba;
        bd_byte_offset = it->second.offset;
        g_logFile << "[HLE READ] Found handle " << std::dec << handle
                  << " lba=0x" << std::hex << start_lba
                  << " bd_offset=0x" << bd_byte_offset
                  << " size=" << std::dec << it->second.size << std::endl;
    } else {
        // Fallback to game memory if handle not in our table
        start_lba = memory::read<uint32_t>(fs_obj + 0x2C);
        g_logFile << "[HLE READ] WARNING: Handle " << std::dec << handle
                  << " not found in table, using game lba=0x"
                  << std::hex << start_lba << std::endl;
    }
    
    g_logFile << "[HLE READ] fs=0x" << std::hex << fs_obj
              << " handle=" << std::dec << handle
              << " offset=" << offset
              << " size=" << size
              << " dest=0x" << std::hex << dest
              << " file_size=" << std::dec << file_size << std::endl;
    
    // Write requested size to status output
    if (status != 0) {
        memory::write<uint32_t>(status, size);
    }
    
    if (size == 0 || !g_isoFile) {
        g_logFile << "[HLE READ] Nothing to read" << std::endl;
        ctx.cpuRegs.GPR.r[2].UL[0] = 0;
        return;
    }
    
    // Clamp
    if (file_size > 0 && (uint32_t)offset + size > file_size) {
        size = file_size - offset;
    }
    
    // Calculate ISO position: base sector + archive offset + read offset
    uint64_t iso_offset = (uint64_t)start_lba * 2048 + bd_byte_offset + offset;
    
    g_logFile << "[HLE READ] ISO calculation: lba=0x" << std::hex << start_lba
              << " * 2048 + bd_off=0x" << bd_byte_offset
              << " + read_off=" << std::dec << offset
              << " = 0x" << std::hex << iso_offset << std::endl;
    g_logFile << "[HLE READ] Reading " << std::dec << size << " bytes → 0x"
              << std::hex << dest << std::endl;
    
    fseek(g_isoFile, (long)iso_offset, SEEK_SET);
    
    uint8_t* dest_ptr = reinterpret_cast<uint8_t*>(memory::get_pointer(dest));
    if (dest_ptr) {
        size_t bytes = fread(dest_ptr, 1, size, g_isoFile);
        g_logFile << "[HLE READ] Read " << std::dec << bytes << " bytes (direct)" << std::endl;
        
        g_logFile << "[HLE READ] First 16 bytes: ";
        for (int i = 0; i < 16 && i < (int)bytes; i++) {
            g_logFile << std::hex << std::setw(2) << std::setfill('0')
                      << (int)dest_ptr[i] << " ";
        }
        g_logFile << std::endl;
    } else {
        std::vector<uint8_t> buf(size);
        size_t bytes = fread(buf.data(), 1, size, g_isoFile);
        for (size_t i = 0; i < bytes; i++) {
            memory::write<uint8_t>(dest + i, buf[i]);
        }
        g_logFile << "[HLE READ] Read " << std::dec << bytes << " bytes (slow)" << std::endl;
    }
    
    ctx.cpuRegs.GPR.r[2].UL[0] = (sync != 0) ? 1 : 0;
}




void HLE_002cf930(CpuContext& ctx) {
    uint32_t caller = ctx.cpuRegs.GPR.r[31].UL[0];  // return address
    
  
    uint32_t size_needed = ctx.cpuRegs.GPR.r[4].UL[0];
    printf("SysAlloc: size=%u (0x%X) caller=0x%X\n", size_needed, size_needed, caller);
    g_logFile << "SysAlloc: size=" << size_needed << " (0x" << std::hex << size_needed << ") caller=0x" << caller << std::endl;

    
    // Use the actual heap manager instead of a static counter
    // This allows freed blocks to be reused.
    uint32_t ptr = g_heap.alloc(size_needed);

    static int call_count = 0;
    call_count++;

    if (ptr == 0) { // Assuming g_heap.alloc returns 0 on failure
        printf("SysAlloc #%d: Out of Memory! Requested %d bytes\n", call_count, size_needed);
        g_logFile << "SysAlloc #" << call_count << ": Out of Memory! Requested " << size_needed << " bytes" << std::endl;
        ctx.cpuRegs.GPR.r[2].UL[0] = 0xFFFFFFFF; // Return -1 (Failure)
    } else {
        printf("SysAlloc #%d: Allocated %d bytes at 0x%X\n", call_count, size_needed, ptr);
        g_logFile << "SysAlloc #" << call_count << ": Allocated " << size_needed << " bytes at 0x" << std::hex << ptr << std::endl;
        ctx.cpuRegs.GPR.r[2].UL[0] = ptr;
    }
}


void HLE_002c6ce0(CpuContext& ctx) {
    uint32_t dest = ctx.cpuRegs.GPR.r[4].UL[0];
    uint32_t src  = ctx.cpuRegs.GPR.r[5].UL[0];
    uint32_t size = ctx.cpuRegs.GPR.r[6].UL[0];

    // Guard: block copies into IO register space
    if (dest >= 0x10000000 && dest < 0x10010000) {
        g_logFile << "[memcpy] BLOCKED IO dest=0x" << std::hex << dest
                  << " src=0x" << src << " size=0x" << size
                  << " ra=0x" << ctx.cpuRegs.GPR.r[31].UL[0] << std::endl;
        // Return dest (standard memcpy return)
        ctx.cpuRegs.GPR.r[2].UD[0] = static_cast<uint64_t>(dest);
        return;
    }

    if (size > 0) {
        uint8_t* dest_ptr = memory::translate_address(dest, size);
        uint8_t* src_ptr  = memory::translate_address(src, size);

        if (dest_ptr && src_ptr) {
            // Fast path: both pointers resolved, use native memcpy
            std::memmove(dest_ptr, src_ptr, size);  // memmove handles overlap safely
        } else {
            // Fallback: byte-by-byte through memory system
            g_logFile << "[memcpy] Slow path: dest=0x" << std::hex << dest
                      << " src=0x" << src << " size=0x" << size 
                      << " ra=0x" << ctx.cpuRegs.GPR.r[31].UL[0] << std::endl;
            for (uint32_t i = 0; i < size; i++) {
                uint8_t byte = memory::read<uint8_t>(src + i);
                memory::write<uint8_t>(dest + i, byte);
            }
        }
    }

    // Return dest pointer (standard memcpy behavior)
    ctx.cpuRegs.GPR.r[2].UD[0] = static_cast<uint64_t>(dest);
}