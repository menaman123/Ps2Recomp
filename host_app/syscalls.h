#pragma once

#include "cpu_state.h"

// Syscall handler functions
void sceOpen(CpuContext& ctx);
void sifRpcBind(CpuContext& ctx);
// Add declarations for other syscall handlers here...
void _Exit(CpuContext& ctx);

// The main dispatcher function
void runtime_syscall_dispatcher(uint32_t syscall_num, CpuContext& ctx);