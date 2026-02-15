#pragma once
#include <cstdint>

struct SifRpcContext {
    uint32_t server_id;
    uint32_t func_num;
    uint32_t payload_addr;
    uint32_t payload_size;
    uint32_t recv_buffer;
    uint32_t recv_size;
    uint32_t client_data_addr;
    uint32_t packet_addr;
    uint32_t sdt_addr;
};

void HandleFileIO(const SifRpcContext& rpc);
void HandleLoadFile(const SifRpcContext& rpc);
void HandlePadman(const SifRpcContext& rpc);
void HandleMcServ(const SifRpcContext& rpc);
void HandleCdvdSCmd(const SifRpcContext& rpc);
void HandleCdvdNCmd(const SifRpcContext& rpc);
void HandleCdvdSearchFile(const SifRpcContext& rpc);
void HandleCdvdDiskReady(const SifRpcContext& rpc);
void HandleLibSd(const SifRpcContext& rpc);
void HandleMtap(const SifRpcContext& rpc);
void HandleEyeToy(const SifRpcContext& rpc);
void HandleCustomLoader(const SifRpcContext& rpc);
void HandleUnknownServer(const SifRpcContext& rpc);
void DispatchSifRpcCall(const SifRpcContext& rpc);
void HandleIopHeap(const SifRpcContext& rpc);
void HandleCdvdInit(const SifRpcContext& rpc);
