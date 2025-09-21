#pragma once

#include "cpu_state.h"
#include <iostream>
#include "syscalls.h"

void runtime_syscall_dispatcher(uint32_t syscall_num, CpuContext& ctx);
