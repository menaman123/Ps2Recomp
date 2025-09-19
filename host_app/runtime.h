#pragma once

#include "cpu_state.h"
#include <iostream>

void runtime_syscall_dispatcher(uint32_t syscall_num, CpuContext& ctx);
