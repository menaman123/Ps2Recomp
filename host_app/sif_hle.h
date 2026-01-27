#pragma once 
#include "cpu_state.h"
#include <cstdint>
void hle_sceSifInitRpc(CpuContext& ctx);
void hle_WaitForVblank(CpuContext& ctx);
void hle_InitTLB(CpuContext& ctx);
void hle_ContextRestore(CpuContext& ctx);
void hle_DoGlobalConstructors(CpuContext& ctx);
void hle_sceSifBindRpc(CpuContext& ctx);
void FUN_00181490(CpuContext& ctx);


namespace sif_bind_rpc {

// Known PS2 RPC Server IDs
enum class RpcServerId : uint32_t {
    PADMAN          = 0x80000100,
    PADMAN_EXT      = 0x80000101,
    FILEIO          = 0x80000001,
    IOPHEAP         = 0x80000003,
    LOADFILE        = 0x80000006,
    MCSERV          = 0x80000400,
    CDVD_INIT       = 0x80000592,
    CDVD_SCMD       = 0x80000593,
    CDVD_NCMD       = 0x80000595,
    CDVD_SEARCHFILE = 0x80000597,
    CDVD_DISKREADY  = 0x8000059A,
};

// Structure to track a bound RPC client
struct BoundClient {
    uint32_t client_addr;    // Address of SifRpcClientData in PS2 memory
    uint32_t server_id;      // Which IOP service this is bound to
    bool     is_bound;       // Whether binding succeeded
};

// Maximum number of simultaneous bindings to track
constexpr int MAX_BOUND_CLIENTS = 32;

// Global tracking array
extern BoundClient g_bound_clients[MAX_BOUND_CLIENTS];

// Helper functions
void InitBindingTracker();
int  RegisterBinding(uint32_t client_addr, uint32_t server_id);
BoundClient* FindBindingByClient(uint32_t client_addr);
BoundClient* FindBindingByServer(uint32_t server_id);
const char* GetServerName(uint32_t server_id);

} 