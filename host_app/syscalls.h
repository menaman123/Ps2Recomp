#pragma once

#include "cpu_state.h"

// Syscall handler functions
void sceOpen(CpuState& ctx);
// Add declarations for other syscall handlers here...

// The main dispatcher function
void runtime_syscall_dispatcher(uint32_t syscall_num, CpuState& ctx);