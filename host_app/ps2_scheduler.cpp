#include "ps2_scheduler.h"
#include <cstring>
#include <iostream>
#include "memory.h"
#include "hle_heap.h"
#include "recompiled.h"
#include <fstream>

// ============================================================================
// GLOBAL INSTANCE
// ============================================================================
PS2Scheduler g_scheduler;

// ============================================================================
// CONSTRUCTOR / RESET
// ============================================================================
PS2Scheduler::PS2Scheduler() {
    Reset();
}
extern std::ofstream g_logFile;
extern HLEHeap g_heap;
void PS2Scheduler::Reset() {
    for (int i = 0; i < MAX_THREADS; i++) {
        threads_[i] = PS2Thread{};
        threads_[i].prev_id = -1;
        threads_[i].next_id = -1;
    }
    
    for (int i = 0; i < MAX_PRIORITIES; i++) {
        ready_queues_[i].head = -1;
        ready_queues_[i].tail = -1;
    }
    
    for (int i = 0; i < MAX_SEMAPHORES; i++) {
        semaphores_[i] = PS2Semaphore{};
        semaphores_[i].wait_head = -1;
        semaphores_[i].wait_tail = -1;
    }
    
    current_thread_id_ = 0;
    dispatch_enabled_ = true;
    
    g_logFile << "Scheduler: Reset complete" << std::endl;
}

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================
int PS2Scheduler::AllocateThreadSlot() {
    for (int i = 1; i < MAX_THREADS; i++) {
        if (!threads_[i].active) {
            threads_[i].active = true;
            threads_[i].id = i;
            return i;
        }
    }
    return -1;
}

int PS2Scheduler::AllocateSemaSlot() {
    for (int i = 1; i < MAX_SEMAPHORES; i++) {
        if (!semaphores_[i].active) {
            return i;
        }
    }
    return -1;
}

void PS2Scheduler::AddToReadyQueue(int tid) {
    if (tid < 0 || tid >= MAX_THREADS || !threads_[tid].active) return;
    
    PS2Thread& t = threads_[tid];
    int prio = t.current_priority;
    if (prio < 0) prio = 0;
    if (prio >= MAX_PRIORITIES) prio = MAX_PRIORITIES - 1;
    
    PriorityQueue& q = ready_queues_[prio];
    
    t.prev_id = q.tail;
    t.next_id = -1;
    
    if (q.tail >= 0) {
        threads_[q.tail].next_id = tid;
    } else {
        q.head = tid;
    }
    q.tail = tid;
    
    g_logFile << "Scheduler: Thread " << tid << " added to priority " << prio << " queue" << std::endl;
}

void PS2Scheduler::RemoveFromReadyQueue(int tid) {
    if (tid < 0 || tid >= MAX_THREADS || !threads_[tid].active) return;
    
    PS2Thread& t = threads_[tid];
    int prio = t.current_priority;
    if (prio < 0 || prio >= MAX_PRIORITIES) return;
    
    PriorityQueue& q = ready_queues_[prio];
    
    if (t.prev_id >= 0) {
        threads_[t.prev_id].next_id = t.next_id;
    } else {
        q.head = t.next_id;
    }
    
    if (t.next_id >= 0) {
        threads_[t.next_id].prev_id = t.prev_id;
    } else {
        q.tail = t.prev_id;
    }
    
    t.prev_id = -1;
    t.next_id = -1;
}

void PS2Scheduler::AddToSemaWaitQueue(int sid, int tid) {
    if (sid < 0 || sid >= MAX_SEMAPHORES) return;
    if (tid < 0 || tid >= MAX_THREADS) return;
    
    PS2Semaphore& s = semaphores_[sid];
    PS2Thread& t = threads_[tid];
    
    t.sema_wait_prev = s.wait_tail;
    t.sema_wait_next = -1;
    
    if (s.wait_tail >= 0) {
        threads_[s.wait_tail].sema_wait_next = tid;
    } else {
        s.wait_head = tid;
    }
    s.wait_tail = tid;
    s.wait_threads++;
}

void PS2Scheduler::RemoveFromSemaWaitQueue(int sid, int tid) {
    if (sid < 0 || sid >= MAX_SEMAPHORES) return;
    if (tid < 0 || tid >= MAX_THREADS) return;
    
    PS2Semaphore& s = semaphores_[sid];
    PS2Thread& t = threads_[tid];
    
    if (t.sema_wait_prev >= 0) {
        threads_[t.sema_wait_prev].sema_wait_next = t.sema_wait_next;
    } else {
        s.wait_head = t.sema_wait_next;
    }
    
    if (t.sema_wait_next >= 0) {
        threads_[t.sema_wait_next].sema_wait_prev = t.sema_wait_prev;
    } else {
        s.wait_tail = t.sema_wait_prev;
    }
    
    t.sema_wait_prev = -1;
    t.sema_wait_next = -1;
    s.wait_threads--;
}

int PS2Scheduler::FindHighestPriorityThread() {
    for (int prio = 0; prio < MAX_PRIORITIES; prio++) {
        if (ready_queues_[prio].head >= 0) {
            return ready_queues_[prio].head;
        }
    }
    return -1;
}

void PS2Scheduler::SaveContext(int tid, const CpuContext& ctx) {
    if (tid < 0 || tid >= MAX_THREADS || !threads_[tid].active) return;
    threads_[tid].ctx = ctx;
}

void PS2Scheduler::LoadContext(int tid, CpuContext& ctx) {
    if (tid < 0 || tid >= MAX_THREADS || !threads_[tid].active) return;
    ctx = threads_[tid].ctx;
}




// ============================================================================
// CORE SCHEDULING
// ============================================================================
void PS2Scheduler::Reschedule(CpuContext& ctx) {
    if (!dispatch_enabled_) {
        g_logFile << "Scheduler: Dispatch disabled, skipping reschedule" << std::endl;
        return;
    }


    int old_tid = current_thread_id_;

    
    // Save current thread
    if (old_tid > 0 && threads_[old_tid].active) {
        PS2Thread& current = threads_[old_tid];
        SaveContext(old_tid, ctx);
        
        if (current.status == THS_RUN) {
            current.status = THS_READY;
            AddToReadyQueue(old_tid);
        }
    }
    
    g_logFile << "Scheduler: Thread " << old_tid << " yielded, looking for next thread to run..." << std::endl;

    SwitchToFiber(scheduler_fiber_);
    ctx = threads_[current_thread_id_].ctx;
}

#ifdef _WIN32
void CALLBACK PS2Scheduler::FiberEntry(void* param) {
#else
void PS2Scheduler::FiberEntry(void* param) {
#endif
    int tid = reinterpret_cast<intptr_t>(param);
    PS2Thread& t = g_scheduler.threads_[tid];

    g_logFile << "FiberEntry: Thread " << tid << " starting execution at 0x" 
              << std::hex << t.ctx.cpuRegs.pc << std::dec << std::endl;

    uint32_t entry_pc = t.ctx.cpuRegs.pc;


    auto it = recompiled_functions.find(entry_pc);
    if (it == recompiled_functions.end()) {
        g_logFile << "FiberEntry: No recompiled function for entry PC 0x" << std::hex << entry_pc << std::dec << std::endl;
        t.status = THS_DORMANT;
        t.needs_fiber_cleanup = true;
        SwitchToFiber(g_scheduler.scheduler_fiber_);
        return;
    }

    it->second(t.ctx, entry_pc);
    g_logFile << "FiberEntry: Thread " << tid << " finished execution, exiting thread" << std::endl;
    g_scheduler.ExitThread(t.ctx);

}

CpuContext& PS2Scheduler::GetCurrentContext() {
    if (current_thread_id_ > 0 && threads_[current_thread_id_].active) {
        return threads_[current_thread_id_].ctx;
    }
    static CpuContext dummy;
    return dummy;
}

PS2Thread* PS2Scheduler::GetCurrentThread() {
    if (current_thread_id_ > 0 && threads_[current_thread_id_].active) {
        return &threads_[current_thread_id_];
    }
    return nullptr;
}

PS2Thread* PS2Scheduler::GetThread(int tid) {
    if (tid > 0 && tid < MAX_THREADS && threads_[tid].active) {
        return &threads_[tid];
    }
    return nullptr;
}

// ============================================================================
// INITIALIZATION
// ============================================================================
uint32_t PS2Scheduler::InitMainThread(uint32_t gp, uint32_t stack, int stack_size,
                                       uint32_t args, int root, CpuContext& caller_ctx) {
    g_logFile << "Scheduler: InitMainThread gp=0x" << std::hex << gp
              << " stack=0x" << stack << " size=" << std::dec << stack_size << std::endl;
    
    uint32_t sp;
    if (stack == 0xFFFFFFFF) {
        sp = 0x02000000 - stack_size;
    } else {
        sp = stack + stack_size;
    }
    sp &= ~0xF;
    
    int tid = current_thread_id_;
    if (tid <= 0 || !threads_[tid].active){
        g_logFile << "Scheduler: Invalid thread ID or inactive thread. Allocating as fallback thread." << std::endl;
        tid = AllocateThreadSlot();
        if (tid < 0) {
            g_logFile << "Scheduler: Failed to allocate thread slot for main thread initialization." << std::endl;
            return 0;
        }
    }
    
    PS2Thread& t = threads_[tid];
    t.status = THS_RUN;
    t.entry_func = caller_ctx.cpuRegs.pc;
    t.stack_base = (stack == 0xFFFFFFFF) ? (0x02000000 - stack_size) : stack;
    t.stack_size = stack_size;
    t.gp_reg = gp;
    t.current_priority = 0;
    t.init_priority = 0;
    
    t.ctx = caller_ctx;
    t.ctx.cpuRegs.GPR.r[28].UL[0] = gp;
    t.ctx.cpuRegs.GPR.r[29].UL[0] = sp;
    
    caller_ctx.cpuRegs.GPR.r[28].UL[0] = gp;
    caller_ctx.cpuRegs.GPR.r[29].UL[0] = sp;

    t.heap_base = 0x00400000; 
    t.heap_end = sp;
    
    g_logFile << "Scheduler: Main thread " << tid << " initialized, SP=0x" 
              << std::hex << sp << std::dec << std::endl;
    
    return sp;
}

uint32_t PS2Scheduler::InitHeap(uint32_t heap, int heap_size, CpuContext& ctx) {
    uint32_t heap_limit;
    PS2Thread* t = GetCurrentThread();
    if (!t) return 0;

    // 1. Determine Heap START
    if (heap != 0xFFFFFFFF) {
        t->heap_base = heap;
    } 
    else if (t->heap_base == 0) {
        t->heap_base = 0x00200000; // Default safe start
    }

    // 2. Determine Heap END (Limit)
    if (heap_size == -1) {
        uint32_t stack_ptr = ctx.cpuRegs.GPR.r[29].UL[0];
        
        // --- FIX STARTS HERE ---
        
        // 1. Sanity Check: If SP is wild (Host Ptr), clamp to physical max
        if (stack_ptr > 0x02000000) {
            stack_ptr = 0x02000000; 
        }

        // 2. Alignment Check: The heap end usually needs to be 16-byte aligned.
        // We do NOT subtract 0x1000 anymore. The game likely wants every byte.
        // We just ensure it doesn't align *up* past 32MB.
        heap_limit = stack_ptr & ~0xF; 
        
        // -----------------------
    } else {
        heap_limit = t->heap_base + heap_size;
    }

    // 3. Final Hard Clamp
    // Allow up to 0x02000000 (Exact 32MB). 
    // Do NOT stop at 0x1FFFFF0.
    if (heap_limit > 0x02000000) {
        heap_limit = 0x02000000;
    }
    
    t->heap_end = heap_limit;
    
    // 4. Initialize Allocator
    if (heap_limit > t->heap_base) {
        uint32_t actual_size = heap_limit - t->heap_base;
        g_logFile << "InitHeap: Start=0x" << std::hex << t->heap_base 
                  << " End=0x" << heap_limit << " Size=0x" << actual_size << std::dec << std::endl;
        
        g_heap.initialize(t->heap_base, actual_size);
    } else {
        return 0;
    }
              
    return heap_limit;
}

uint32_t PS2Scheduler::EndOfHeap() {
    g_logFile << "EndOfHeap called, returning 0x02000000 as the end of heap." << std::endl;
    printf("EndOfHeap called, returning 0x02000000 as the end of heap.\n");
    // Correct: 32MB exactly (0x02000000)
    // This allows the size calculation (End - Start) to result in the full 32MB.
    return 0x02000000;
}

// ============================================================================
// THREAD LIFECYCLE
// ============================================================================
int PS2Scheduler::CreateThread(uint32_t param_addr) {
    // Read ThreadParam from guest memory
    uint32_t func = memory::read<uint32_t>(param_addr + 0x04);
    uint32_t stack = memory::read<uint32_t>(param_addr + 0x08);
    int32_t stack_size = memory::read<int32_t>(param_addr + 0x0C);
    uint32_t gp = memory::read<uint32_t>(param_addr + 0x10);
    int32_t priority = memory::read<int32_t>(param_addr + 0x14);
    uint32_t attr = memory::read<uint32_t>(param_addr + 0x1C);
    
    int tid = AllocateThreadSlot();
    if (tid < 0) {
        g_logFile << "Scheduler: CreateThread failed - no slots" << std::endl;
        return -1;
    }
    
    PS2Thread& t = threads_[tid];
    t.status = THS_DORMANT;
    t.entry_func = func;
    t.stack_base = stack;
    t.stack_size = stack_size;
    t.gp_reg = gp;
    t.init_priority = priority;
    t.current_priority = priority;
    
    // Initialize context
    std::memset(&t.ctx, 0, sizeof(CpuContext));
    t.ctx.cpuRegs.pc = func;
    t.ctx.cpuRegs.GPR.r[28].UL[0] = gp;
    t.ctx.cpuRegs.GPR.r[29].UL[0] = (stack + stack_size) & ~0xF;
    
    // Inherit heap from parent
    if (current_thread_id_ > 0) {
        t.heap_base = threads_[current_thread_id_].heap_base;
    }
    
    g_logFile << "Scheduler: CreateThread id=" << tid 
              << " func=0x" << std::hex << func
              << " prio=" << std::dec << priority << std::endl;
    
    return tid;
}

int PS2Scheduler::DeleteThread(int tid) {
    if (tid <= 0 || tid >= MAX_THREADS || !threads_[tid].active) return -1;
    if (tid == current_thread_id_) return -1;
    
    PS2Thread& t = threads_[tid];
    if (t.status != THS_DORMANT) return -1;
    
    t.active = false;
    g_logFile << "Scheduler: DeleteThread id=" << tid << std::endl;
    return tid;
}

int PS2Scheduler::StartThread(int tid, uint32_t arg, CpuContext& ctx) {
    if (tid <= 0 || tid >= MAX_THREADS || !threads_[tid].active) return -1;
    if (tid == current_thread_id_) return -1;
    
    PS2Thread& t = threads_[tid];
    if (t.status != THS_DORMANT) return -1;
    
    // Reset context
    t.ctx.cpuRegs.pc = t.entry_func;
    t.ctx.cpuRegs.GPR.r[4].UL[0] = arg;  // $a0
    t.ctx.cpuRegs.GPR.r[28].UL[0] = t.gp_reg;
    t.ctx.cpuRegs.GPR.r[29].UL[0] = (t.stack_base + t.stack_size) & ~0xF;
    

    if (!t.fiber_created) {
#ifdef _WIN32
        t.fiber = CreateFiber(1024 * 1024, FiberEntry, reinterpret_cast<void*>(tid));
#else
        // LINUX STUFF
#endif
        t.fiber_created = true;
    }

    t.status = THS_READY;
    AddToReadyQueue(tid);
    
    g_logFile << "Scheduler: StartThread id=" << tid << std::endl;
    Reschedule(ctx);
    return tid;
}

void PS2Scheduler::ExitThread(CpuContext& ctx) {
    if (current_thread_id_ <= 0) return;
    
    int tid = current_thread_id_;
    g_logFile << "Scheduler: ExitThread id=" << tid << std::endl;
    
    PS2Thread& t = threads_[tid];
    RemoveFromReadyQueue(tid);
    t.status = THS_DORMANT;
    t.wait_type = WAIT_NONE;
    t.needs_fiber_cleanup = true; // Mark fiber for cleanup after switching out

    SwitchToFiber(scheduler_fiber_);
}

void PS2Scheduler::ExitDeleteThread(CpuContext& ctx) {
    if (current_thread_id_ <= 0) return;
    
    int tid = current_thread_id_;
    g_logFile << "Scheduler: ExitDeleteThread id=" << tid << std::endl;
    
    RemoveFromReadyQueue(tid);
    threads_[tid].active = false;
    threads_[tid].status = THS_DORMANT;
    threads_[tid].wait_type = WAIT_NONE;
    threads_[tid].needs_fiber_cleanup = true;
    
    SwitchToFiber(scheduler_fiber_);
}

int PS2Scheduler::TerminateThread(int tid, CpuContext& ctx) {
    int result = iTerminateThread(tid);
    if (result >= 0) {
        Reschedule(ctx);
    }
    return result;
}

int PS2Scheduler::iTerminateThread(int tid) {
    if (tid <= 0 || tid >= MAX_THREADS || !threads_[tid].active) return -1;
    
    PS2Thread& t = threads_[tid];
    
    g_logFile << "Scheduler: iTerminateThread id=" << tid 
              << " status=" << t.status << std::endl;
    
    if (t.status == 0 || t.status == THS_RUN || t.status == THS_DORMANT) {
        return -1;
    }
    
    if (t.status == THS_READY) {
        RemoveFromReadyQueue(tid);
    }
    
    if ((t.status & THS_WAIT) && t.wait_type == WAIT_SEMA && t.sema_id > 0) {
        RemoveFromSemaWaitQueue(t.sema_id, tid);
    }
    
    t.status = THS_DORMANT;
    t.wait_type = WAIT_NONE;
    return tid;
}

// ============================================================================
// PRIORITY
// ============================================================================
int PS2Scheduler::ChangeThreadPriority(int tid, int priority, CpuContext& ctx) {
    int result = iChangeThreadPriority(tid, priority);
    if (result >= 0) {
        Reschedule(ctx);
    }
    return result;
}

int PS2Scheduler::iChangeThreadPriority(int tid, int priority) {
    if (tid == 0) tid = current_thread_id_;
    if (tid <= 0 || tid >= MAX_THREADS || !threads_[tid].active) return -1;
    
    PS2Thread& t = threads_[tid];
    if (t.status == THS_DORMANT) return -1;
    
    int old_priority = t.current_priority;
    
    if (t.status == THS_READY) {
        RemoveFromReadyQueue(tid);
        t.current_priority = priority;
        AddToReadyQueue(tid);
    } else {
        t.current_priority = priority;
    }
    
    g_logFile << "Scheduler: iChangeThreadPriority id=" << tid 
              << " " << old_priority << " -> " << priority << std::endl;
    
    return old_priority;
}

int PS2Scheduler::RotateThreadReadyQueue(int priority, CpuContext& ctx) {
    int result = iRotateThreadReadyQueue(priority);
    if (result >= 0) {
        Reschedule(ctx);
    }
    return result;
}

int PS2Scheduler::iRotateThreadReadyQueue(int priority) {
    if (priority < 0 || priority >= MAX_PRIORITIES) return -1;
    
    PriorityQueue& q = ready_queues_[priority];
    if (q.head < 0 || q.head == q.tail) return priority;
    
    int old_head = q.head;
    PS2Thread& t = threads_[old_head];
    
    q.head = t.next_id;
    if (q.head >= 0) {
        threads_[q.head].prev_id = -1;
    }
    
    t.prev_id = q.tail;
    t.next_id = -1;
    threads_[q.tail].next_id = old_head;
    q.tail = old_head;
    
    g_logFile << "Scheduler: iRotateThreadReadyQueue priority=" << priority << std::endl;
    return priority;
}

// ============================================================================
// WAIT RELEASE
// ============================================================================
int PS2Scheduler::ReleaseWaitThread(int tid, CpuContext& ctx) {
    int result = iReleaseWaitThread(tid);
    if (result >= 0) {
        Reschedule(ctx);
    }
    return result;
}

int PS2Scheduler::iReleaseWaitThread(int tid) {
    if (tid <= 0 || tid >= MAX_THREADS || !threads_[tid].active) return -1;
    
    PS2Thread& t = threads_[tid];
    
    if (t.status == THS_WAIT) {
        if (t.wait_type == WAIT_SEMA && t.sema_id > 0) {
            RemoveFromSemaWaitQueue(t.sema_id, tid);
        }
        t.wait_type = WAIT_NONE;
        t.sema_id = -1;
        t.status = THS_READY;
        AddToReadyQueue(tid);
        return tid;
    }
    else if (t.status == THS_WAITSUSPEND) {
        if (t.wait_type == WAIT_SEMA && t.sema_id > 0) {
            RemoveFromSemaWaitQueue(t.sema_id, tid);
        }
        t.wait_type = WAIT_NONE;
        t.sema_id = -1;
        t.status = THS_SUSPEND;
        return tid;
    }
    
    return -1;
}

// ============================================================================
// THREAD INFO
// ============================================================================
int PS2Scheduler::GetThreadId() {
    return current_thread_id_;
}

int PS2Scheduler::ReferThreadStatus(int tid, uint32_t status_addr) {
    if (tid == 0) tid = current_thread_id_;
    if (tid <= 0 || tid >= MAX_THREADS || !threads_[tid].active) return -1;
    
    PS2Thread& t = threads_[tid];
    
    memory::write<int32_t>(status_addr + 0x00, t.status);
    memory::write<uint32_t>(status_addr + 0x04, t.entry_func);
    memory::write<uint32_t>(status_addr + 0x08, t.stack_base);
    memory::write<int32_t>(status_addr + 0x0C, t.stack_size);
    memory::write<uint32_t>(status_addr + 0x10, t.gp_reg);
    memory::write<int32_t>(status_addr + 0x14, t.init_priority);
    memory::write<int32_t>(status_addr + 0x18, t.current_priority);
    
    return tid;
}


void PS2Scheduler::RunSchedulerLoop() {
    g_logFile << "Scheduler: Starting main loop" << std::endl;

    while (true) {

        // Clean
        for (int i = 1; i < MAX_THREADS; i++) {
            if ( threads_[i].needs_fiber_cleanup && threads_[i].fiber_created) {
                g_logFile << "Cleaning up fiber for thread " << i << std::endl;
                DeleteFiber(threads_[i].fiber);
                threads_[i].fiber = nullptr;
                threads_[i].fiber_created = false;
                threads_[i].needs_fiber_cleanup = false;
            }
        }

        int next_tid = FindHighestPriorityThread();
        if (next_tid < 0) {
            g_logFile << "Scheduler: No ready threads, idling..." << std::endl;
            Sleep(1);
            continue;
        }

        RemoveFromReadyQueue(next_tid);
        threads_[next_tid].status = THS_RUN;
        current_thread_id_ = next_tid;
        g_logFile << "Scheduler: Switching to thread " << next_tid << std::endl;
        SwitchToFiber(threads_[next_tid].fiber);
    }
}

// ============================================================================
// SLEEP / WAKEUP
// ============================================================================
void PS2Scheduler::SleepThread(CpuContext& ctx) {
    if (current_thread_id_ <= 0) return;
    
    PS2Thread& t = threads_[current_thread_id_];
    
    g_logFile << "Scheduler: SleepThread id=" << current_thread_id_ 
              << " wakeup_count=" << t.wakeup_count << std::endl;
    
    if (t.wakeup_count > 0) {
        t.wakeup_count--;
        return; 
    }

    RemoveFromReadyQueue(current_thread_id_);
    t.status = THS_WAIT;
    t.wait_type = WAIT_SLEEP;
    Reschedule(ctx);

}

int PS2Scheduler::WakeupThread(int tid, CpuContext& ctx) {
    int result = iWakeupThread(tid);
    if (result >= 0) {
        Reschedule(ctx);
    }
    return result;
}

int PS2Scheduler::iWakeupThread(int tid) {
    if (tid <= 0 || tid >= MAX_THREADS || !threads_[tid].active) return -1;
    
    PS2Thread& t = threads_[tid];
    
    g_logFile << "Scheduler: iWakeupThread id=" << tid 
              << " status=" << t.status << " wait_type=" << t.wait_type << std::endl;
    
    if (t.status == THS_WAIT && t.wait_type == WAIT_SLEEP) {
        t.status = THS_READY;
        t.wait_type = WAIT_NONE;
        AddToReadyQueue(tid);
        return tid;
    }
    else if (t.status == THS_WAITSUSPEND && t.wait_type == WAIT_SLEEP) {
        t.status = THS_SUSPEND;
        t.wait_type = WAIT_NONE;
        return tid;
    }
    else if (t.status == THS_READY || t.status == THS_SUSPEND ||
             ((t.status & THS_WAIT) && t.wait_type == WAIT_SEMA)) {
        t.wakeup_count++;
        return tid;
    }
    
    return -1;
}

int PS2Scheduler::CancelWakeupThread(int tid) {
    return iCancelWakeupThread(tid);
}

int PS2Scheduler::iCancelWakeupThread(int tid) {
    if (tid == 0) tid = current_thread_id_;
    if (tid <= 0 || tid >= MAX_THREADS || !threads_[tid].active) return -1;
    
    PS2Thread& t = threads_[tid];
    int old_count = t.wakeup_count;
    t.wakeup_count = 0;
    
    return old_count;
}

// ============================================================================
// SUSPEND / RESUME
// ============================================================================
int PS2Scheduler::SuspendThread(int tid) {
    return iSuspendThread(tid);
    // BUG: No reschedule! This is documented PS2 BIOS behavior!
}

int PS2Scheduler::iSuspendThread(int tid) {
    if (tid == 0) tid = current_thread_id_;
    if (tid <= 0 || tid >= MAX_THREADS || !threads_[tid].active) return -1;
    
    PS2Thread& t = threads_[tid];
    
    g_logFile << "Scheduler: iSuspendThread id=" << tid << " status=" << t.status << std::endl;
    
    if (t.status == THS_READY || t.status == THS_RUN) {
        RemoveFromReadyQueue(tid);
        t.status = THS_SUSPEND;
        return tid;
    }
    else if (t.status == THS_WAIT) {
        t.status = THS_WAITSUSPEND;
        return tid;
    }
    
    return -1;
}

int PS2Scheduler::ResumeThread(int tid, CpuContext& ctx) {
    int result = iResumeThread(tid);
    if (result >= 0) {
        Reschedule(ctx);
    }
    return result;
}

int PS2Scheduler::iResumeThread(int tid) {
    if (tid <= 0 || tid >= MAX_THREADS || !threads_[tid].active) return -1;
    
    PS2Thread& t = threads_[tid];
    
    g_logFile << "Scheduler: iResumeThread id=" << tid << " status=" << t.status << std::endl;
    
    if (t.status == THS_SUSPEND) {
        t.status = THS_READY;
        AddToReadyQueue(tid);
        return tid;
    }
    else if (t.status == THS_WAITSUSPEND) {
        t.status = THS_WAIT;
        return tid;
    }
    
    return -1;
}

// ============================================================================
// DISPATCH CONTROL
// ============================================================================
int PS2Scheduler::DisableDispatchThread() {
    g_logFile << "Scheduler: DisableDispatchThread" << std::endl;
    dispatch_enabled_ = false;
    return 1;
}

int PS2Scheduler::EnableDispatchThread(CpuContext& ctx) {
    g_logFile << "Scheduler: EnableDispatchThread" << std::endl;
    dispatch_enabled_ = true;
    Reschedule(ctx);
    return 0;
}

// ============================================================================
// SEMAPHORES
// ============================================================================
int PS2Scheduler::CreateSema(uint32_t param_addr) {
    int32_t init_count = memory::read<int32_t>(param_addr + 0x08);
    int32_t max_count = memory::read<int32_t>(param_addr + 0x04);
    uint32_t attr = memory::read<uint32_t>(param_addr + 0x00);
    uint32_t option = memory::read<uint32_t>(param_addr + 0x0C);
    
    int sid = AllocateSemaSlot();
    if (sid < 0) {
        g_logFile << "Scheduler: CreateSema failed - no slots" << std::endl;
        return -1;
    }
    
    PS2Semaphore& s = semaphores_[sid];
    s.active = true;
    s.init_count = init_count;
    s.count = init_count;
    s.max_count = max_count;
    s.attr = attr;
    s.option = option;
    s.wait_threads = 0;
    s.wait_head = -1;
    s.wait_tail = -1;
    
    g_logFile << "Scheduler: CreateSema id=" << sid 
              << " init=" << init_count << " max=" << max_count << std::endl;
    
    return sid;
}

int PS2Scheduler::DeleteSema(int sid, CpuContext& ctx) {
    if (sid <= 0 || sid >= MAX_SEMAPHORES || !semaphores_[sid].active) return -1;
    
    PS2Semaphore& s = semaphores_[sid];
    
    g_logFile << "Scheduler: DeleteSema id=" << sid 
              << " wait_threads=" << s.wait_threads << std::endl;
    
    // Wake all waiting threads
    int tid = s.wait_head;
    while (tid >= 0) {
        PS2Thread& t = threads_[tid];
        int next = t.sema_wait_next;
        
        t.sema_id = -1;
        t.wait_type = WAIT_NONE;
        
        if (t.status == THS_WAIT) {
            t.status = THS_READY;
            AddToReadyQueue(tid);
        } else if (t.status == THS_WAITSUSPEND) {
            t.status = THS_SUSPEND;
        }
        
        t.sema_wait_prev = -1;
        t.sema_wait_next = -1;
        tid = next;
    }
    
    s.active = false;
    Reschedule(ctx);
    return sid;
}

int PS2Scheduler::SignalSema(int sid, CpuContext& ctx) {
    int result = iSignalSema(sid);
    if (result == -2) {
        Reschedule(ctx);
        return sid;
    }
    return result;
}

int PS2Scheduler::iSignalSema(int sid) {
    if (sid <= 0 || sid >= MAX_SEMAPHORES || !semaphores_[sid].active) return -1;
    
    PS2Semaphore& s = semaphores_[sid];
    
    g_logFile << "Scheduler: iSignalSema id=" << sid 
              << " count=" << s.count << " waiters=" << s.wait_threads << std::endl;
    
    if (s.wait_head >= 0) {
        int tid = s.wait_head;
        PS2Thread& t = threads_[tid];
        
        // Remove from wait queue
        s.wait_head = t.sema_wait_next;
        if (s.wait_head >= 0) {
            threads_[s.wait_head].sema_wait_prev = -1;
        } else {
            s.wait_tail = -1;
        }
        t.sema_wait_prev = -1;
        t.sema_wait_next = -1;
        s.wait_threads--;
        
        t.sema_id = -1;
        t.wait_type = WAIT_NONE;
        
        if (t.status == THS_WAIT) {
            t.status = THS_READY;
            AddToReadyQueue(tid);
        } else if (t.status == THS_WAITSUSPEND) {
            t.status = THS_SUSPEND;
        }
        
        g_logFile << "Scheduler: iSignalSema woke thread " << tid << std::endl;
        return -2;  // Thread released
    }
    
    s.count++;
    if (s.count > s.max_count) {
        s.count = s.max_count;
    }
    
    return sid;
}

int PS2Scheduler::WaitSema(int sid, CpuContext& ctx) {
    if (sid <= 0 || sid >= MAX_SEMAPHORES || !semaphores_[sid].active) return -1;
    
    PS2Semaphore& s = semaphores_[sid];
    
    g_logFile << "Scheduler: WaitSema id=" << sid << " count=" << s.count << std::endl;
    
    if (s.count > 0) {
        s.count--;
        return sid;
    }
    
    // =========================================================
    // HLE WORKAROUND: If this is the only thread and we'd block,
    // check if this looks like a SIF RPC wait and auto-complete
    // =========================================================
    int next_thread = FindHighestPriorityThread();
    if (next_thread < 0) {
        // No other threads to run - we're waiting for IOP which doesn't exist
        g_logFile << "Scheduler: WaitSema would deadlock - auto-signaling (SIF HLE)" << std::endl;
        
        // Just pretend the semaphore was signaled immediately
        // This simulates instant IOP response
        return sid;
    }
    
    // Normal blocking path - there are other threads to run
    PS2Thread& t = threads_[current_thread_id_];
    RemoveFromReadyQueue(current_thread_id_);
    t.status = THS_WAIT;
    t.wait_type = WAIT_SEMA;
    t.sema_id = sid;
    AddToSemaWaitQueue(sid, current_thread_id_);
    
    Reschedule(ctx);
    
    return sid;
}

int PS2Scheduler::PollSema(int sid) {
    return iPollSema(sid);
}

int PS2Scheduler::iPollSema(int sid) {
    if (sid <= 0 || sid >= MAX_SEMAPHORES || !semaphores_[sid].active) return -1;
    
    PS2Semaphore& s = semaphores_[sid];
    
    if (s.count > 0) {
        s.count--;
        return sid;
    }
    
    return -1;  // Would block
}

// ============================================================================
// SYSCALL WRAPPERS
// ============================================================================

void Syscall_CreateThread(CpuContext& ctx) {
    uint32_t param_addr = ctx.cpuRegs.GPR.r[4].UL[0];
    int tid = g_scheduler.CreateThread(param_addr);
    ctx.cpuRegs.GPR.r[2].SL[0] = tid;
}

void Syscall_DeleteThread(CpuContext& ctx) {
    int tid = ctx.cpuRegs.GPR.r[4].SL[0];
    int result = g_scheduler.DeleteThread(tid);
    ctx.cpuRegs.GPR.r[2].SL[0] = result;
}

void Syscall_StartThread(CpuContext& ctx) {
    int tid = ctx.cpuRegs.GPR.r[4].SL[0];
    uint32_t arg = ctx.cpuRegs.GPR.r[5].UL[0];
    int result = g_scheduler.StartThread(tid, arg, ctx);
    ctx.cpuRegs.GPR.r[2].SL[0] = result;
}

void Syscall_ExitThread(CpuContext& ctx) {
    g_scheduler.ExitThread(ctx);
}

void Syscall_ExitDeleteThread(CpuContext& ctx) {
    g_scheduler.ExitDeleteThread(ctx);
}

void Syscall_TerminateThread(CpuContext& ctx) {
    int tid = ctx.cpuRegs.GPR.r[4].SL[0];
    int result = g_scheduler.TerminateThread(tid, ctx);
    ctx.cpuRegs.GPR.r[2].SL[0] = result;
}

void Syscall_iTerminateThread(CpuContext& ctx) {
    int tid = ctx.cpuRegs.GPR.r[4].SL[0];
    int result = g_scheduler.iTerminateThread(tid);
    ctx.cpuRegs.GPR.r[2].SL[0] = result;
}

void Syscall_DisableDispatchThread(CpuContext& ctx) {
    int result = g_scheduler.DisableDispatchThread();
    ctx.cpuRegs.GPR.r[2].SL[0] = result;
}

void Syscall_EnableDispatchThread(CpuContext& ctx) {
    int result = g_scheduler.EnableDispatchThread(ctx);
    ctx.cpuRegs.GPR.r[2].SL[0] = result;
}

void Syscall_ChangeThreadPriority(CpuContext& ctx) {
    int tid = ctx.cpuRegs.GPR.r[4].SL[0];
    int priority = ctx.cpuRegs.GPR.r[5].SL[0];
    int result = g_scheduler.ChangeThreadPriority(tid, priority, ctx);
    ctx.cpuRegs.GPR.r[2].SL[0] = result;
}

void Syscall_iChangeThreadPriority(CpuContext& ctx) {
    int tid = ctx.cpuRegs.GPR.r[4].SL[0];
    int priority = ctx.cpuRegs.GPR.r[5].SL[0];
    int result = g_scheduler.iChangeThreadPriority(tid, priority);
    ctx.cpuRegs.GPR.r[2].SL[0] = result;
}

void Syscall_RotateThreadReadyQueue(CpuContext& ctx) {
    int priority = ctx.cpuRegs.GPR.r[4].SL[0];
    int result = g_scheduler.RotateThreadReadyQueue(priority, ctx);
    ctx.cpuRegs.GPR.r[2].SL[0] = result;
}

void Syscall_iRotateThreadReadyQueue(CpuContext& ctx) {
    int priority = ctx.cpuRegs.GPR.r[4].SL[0];
    int result = g_scheduler.iRotateThreadReadyQueue(priority);
    ctx.cpuRegs.GPR.r[2].SL[0] = result;
}

void Syscall_ReleaseWaitThread(CpuContext& ctx) {
    int tid = ctx.cpuRegs.GPR.r[4].SL[0];
    int result = g_scheduler.ReleaseWaitThread(tid, ctx);
    ctx.cpuRegs.GPR.r[2].SL[0] = result;
}

void Syscall_iReleaseWaitThread(CpuContext& ctx) {
    int tid = ctx.cpuRegs.GPR.r[4].SL[0];
    int result = g_scheduler.iReleaseWaitThread(tid);
    ctx.cpuRegs.GPR.r[2].SL[0] = result;
}

void Syscall_GetThreadId(CpuContext& ctx) {
    ctx.cpuRegs.GPR.r[2].SL[0] = g_scheduler.GetThreadId();
}

void Syscall_ReferThreadStatus(CpuContext& ctx) {
    int tid = ctx.cpuRegs.GPR.r[4].SL[0];
    uint32_t status_addr = ctx.cpuRegs.GPR.r[5].UL[0];
    int result = g_scheduler.ReferThreadStatus(tid, status_addr);
    ctx.cpuRegs.GPR.r[2].SL[0] = result;
}

void Syscall_iReferThreadStatus(CpuContext& ctx) {
    Syscall_ReferThreadStatus(ctx);
}

void Syscall_SleepThread(CpuContext& ctx) {
    g_scheduler.SleepThread(ctx);
}

void Syscall_WakeupThread(CpuContext& ctx) {
    int tid = ctx.cpuRegs.GPR.r[4].SL[0];
    int result = g_scheduler.WakeupThread(tid, ctx);
    ctx.cpuRegs.GPR.r[2].SL[0] = result;
}

void Syscall_iWakeupThread(CpuContext& ctx) {
    int tid = ctx.cpuRegs.GPR.r[4].SL[0];
    int result = g_scheduler.iWakeupThread(tid);
    ctx.cpuRegs.GPR.r[2].SL[0] = result;
}

void Syscall_CancelWakeupThread(CpuContext& ctx) {
    int tid = ctx.cpuRegs.GPR.r[4].SL[0];
    int result = g_scheduler.CancelWakeupThread(tid);
    ctx.cpuRegs.GPR.r[2].SL[0] = result;
}

void Syscall_iCancelWakeupThread(CpuContext& ctx) {
    int tid = ctx.cpuRegs.GPR.r[4].SL[0];
    int result = g_scheduler.iCancelWakeupThread(tid);
    ctx.cpuRegs.GPR.r[2].SL[0] = result;
}

void Syscall_SuspendThread(CpuContext& ctx) {
    int tid = ctx.cpuRegs.GPR.r[4].SL[0];
    int result = g_scheduler.SuspendThread(tid);
    ctx.cpuRegs.GPR.r[2].SL[0] = result;
    // NOTE: No reschedule - this is a documented PS2 BIOS BUG!
}

void Syscall_iSuspendThread(CpuContext& ctx) {
    int tid = ctx.cpuRegs.GPR.r[4].SL[0];
    int result = g_scheduler.iSuspendThread(tid);
    ctx.cpuRegs.GPR.r[2].SL[0] = result;
}

void Syscall_ResumeThread(CpuContext& ctx) {
    int tid = ctx.cpuRegs.GPR.r[4].SL[0];
    int result = g_scheduler.ResumeThread(tid, ctx);
    ctx.cpuRegs.GPR.r[2].SL[0] = result;
}

void Syscall_iResumeThread(CpuContext& ctx) {
    int tid = ctx.cpuRegs.GPR.r[4].SL[0];
    int result = g_scheduler.iResumeThread(tid);
    ctx.cpuRegs.GPR.r[2].SL[0] = result;
}

void Syscall_InitMainThread(CpuContext& ctx) {
    uint32_t gp = ctx.cpuRegs.GPR.r[4].UL[0];
    uint32_t stack = ctx.cpuRegs.GPR.r[5].UL[0];
    int32_t stack_size = ctx.cpuRegs.GPR.r[6].SL[0];
    uint32_t args = ctx.cpuRegs.GPR.r[7].UL[0];
    int32_t root = ctx.cpuRegs.GPR.r[8].SL[0];
    
    uint32_t sp = g_scheduler.InitMainThread(gp, stack, stack_size, args, root, ctx);
    ctx.cpuRegs.GPR.r[2].UL[0] = sp;
}

void Syscall_InitHeap(CpuContext& ctx) {
    g_logFile << "Syscall: InitHeap()" << std::endl;
    printf("Syscall: InitHeap()\n");
    uint32_t heap = ctx.cpuRegs.GPR.r[4].UL[0];
    int32_t heap_size = ctx.cpuRegs.GPR.r[5].SL[0];
    
    uint32_t heap_end = g_scheduler.InitHeap(heap, heap_size, ctx);
    ctx.cpuRegs.GPR.r[2].UL[0] = heap_end;
}
void Syscall_EndOfHeap(CpuContext& ctx) {
    uint32_t current_heap = g_scheduler.EndOfHeap();
    g_logFile << "Syscall: EndOfHeap() returning 0x" << std::hex << current_heap << std::dec << std::endl;
    printf("Syscall: EndOfHeap() returning 0x%X\n", current_heap);
    ctx.cpuRegs.GPR.r[2].UL[0] = current_heap;
}

void Syscall_CreateSema(CpuContext& ctx) {
    uint32_t param_addr = ctx.cpuRegs.GPR.r[4].UL[0];
    int sid = g_scheduler.CreateSema(param_addr);
    ctx.cpuRegs.GPR.r[2].SL[0] = sid;
}

void Syscall_DeleteSema(CpuContext& ctx) {
    int sid = ctx.cpuRegs.GPR.r[4].SL[0];
    int result = g_scheduler.DeleteSema(sid, ctx);
    ctx.cpuRegs.GPR.r[2].SL[0] = result;
}

void Syscall_SignalSema(CpuContext& ctx) {
    int sid = ctx.cpuRegs.GPR.r[4].SL[0];
    int result = g_scheduler.SignalSema(sid, ctx);
    ctx.cpuRegs.GPR.r[2].SL[0] = result;
}

void Syscall_iSignalSema(CpuContext& ctx) {
    int sid = ctx.cpuRegs.GPR.r[4].SL[0];
    int result = g_scheduler.iSignalSema(sid);
    ctx.cpuRegs.GPR.r[2].SL[0] = result;
}

void Syscall_WaitSema(CpuContext& ctx) {
    int sid = ctx.cpuRegs.GPR.r[4].SL[0];
    int result = g_scheduler.WaitSema(sid, ctx);
    ctx.cpuRegs.GPR.r[2].SL[0] = result;
}

void Syscall_PollSema(CpuContext& ctx) {
    int sid = ctx.cpuRegs.GPR.r[4].SL[0];
    int result = g_scheduler.PollSema(sid);
    ctx.cpuRegs.GPR.r[2].SL[0] = result;
}

void Syscall_iPollSema(CpuContext& ctx) {
    int sid = ctx.cpuRegs.GPR.r[4].SL[0];
    int result = g_scheduler.iPollSema(sid);
    ctx.cpuRegs.GPR.r[2].SL[0] = result;
}