#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "cpu_state.h"
#include "memory.h"
#include <array>
#include <fstream>

extern std::ofstream g_logFile;

// ============================================================================
// PS2 THREAD STATES
// ============================================================================
#define THS_RUN         0x01
#define THS_READY       0x02
#define THS_WAIT        0x04
#define THS_SUSPEND     0x08
#define THS_WAITSUSPEND 0x0C  // THS_WAIT | THS_SUSPEND
#define THS_DORMANT     0x10

// Wait types
#define WAIT_NONE       0
#define WAIT_SLEEP      1
#define WAIT_SEMA       2

// Limits
#define MAX_THREADS     256
#define MAX_SEMAPHORES  256
#define MAX_PRIORITIES  128
// ============================================================================
// THREAD CONTROL BLOCK
// ============================================================================
struct PS2Thread {
    bool active = false;
    int id = 0;
    int status = THS_DORMANT;
    
    int16_t init_priority = 0;
    int16_t current_priority = 0;
    
    // Full CPU context - uses YOUR CpuContext type
    CpuContext ctx;
    
    // Thread info
    uint32_t entry_func = 0;
    uint32_t stack_base = 0;
    uint32_t stack_size = 0;
    uint32_t gp_reg = 0;
    uint32_t heap_base = 0;
    uint32_t heap_end = 0;  // The END of the heap (Stack Pointer/Limit)
    
    // Wait state
    int wait_type = WAIT_NONE;
    int sema_id = -1;
    int wakeup_count = 0;
    
    // Linked list for ready queue
    int prev_id = -1;
    int next_id = -1;
    
    // Linked list for semaphore wait queue
    int sema_wait_prev = -1;
    int sema_wait_next = -1;


    void* fiber = nullptr; // For Windows Fiber implementation
    bool fiber_created = false; // Track if fiber has been created for this thread
    bool needs_fiber_cleanup = false; // Track if fiber needs to be cleaned up on thread exit
};

// ============================================================================
// SEMAPHORE (replaces your HostSemaphore)
// ============================================================================
struct PS2Semaphore {
    bool active = false;
    int count = 0;
    int max_count = 0;
    int init_count = 0;
    uint32_t attr = 0;
    uint32_t option = 0;
    int wait_threads = 0;
    int wait_head = -1;
    int wait_tail = -1;
};

// ============================================================================
// PRIORITY QUEUE
// ============================================================================
struct PriorityQueue {
    int head = -1;
    int tail = -1;
};

// ============================================================================
// PS2 SCHEDULER CLASS
// ============================================================================
class PS2Scheduler {

public:
    PS2Scheduler();
    void Reset();
    

    void InitFibers();
    void RunSchedulerLoop();
    
    // ===== Initialization =====
    uint32_t InitMainThread(uint32_t gp, uint32_t stack, int stack_size,
                            uint32_t args, int root, CpuContext& caller_ctx);
    uint32_t InitHeap(uint32_t heap, int heap_size, CpuContext& ctx);
    uint32_t EndOfHeap(); // Add this
    
    // ===== Thread Lifecycle =====
    int CreateThread(uint32_t param_addr);
    int DeleteThread(int tid);
    int StartThread(int tid, uint32_t arg, CpuContext& ctx);
    void ExitThread(CpuContext& ctx);
    void ExitDeleteThread(CpuContext& ctx);
    int TerminateThread(int tid, CpuContext& ctx);
    int iTerminateThread(int tid);
    
    // ===== Priority =====
    int ChangeThreadPriority(int tid, int priority, CpuContext& ctx);
    int iChangeThreadPriority(int tid, int priority);
    int RotateThreadReadyQueue(int priority, CpuContext& ctx);
    int iRotateThreadReadyQueue(int priority);
    
    // ===== Wait Release =====
    int ReleaseWaitThread(int tid, CpuContext& ctx);
    int iReleaseWaitThread(int tid);
    
    // ===== Thread Info =====
    int GetThreadId();
    int ReferThreadStatus(int tid, uint32_t status_addr);
    
    // ===== Sleep/Wake =====
    void SleepThread(CpuContext& ctx);
    int WakeupThread(int tid, CpuContext& ctx);
    int iWakeupThread(int tid);
    int CancelWakeupThread(int tid);
    int iCancelWakeupThread(int tid);
    
    // ===== Suspend/Resume =====
    int SuspendThread(int tid);  // NOTE: Has documented BUG - no reschedule!
    int iSuspendThread(int tid);
    int ResumeThread(int tid, CpuContext& ctx);
    int iResumeThread(int tid);
    
    // ===== Semaphores =====
    int CreateSema(uint32_t param_addr);
    int DeleteSema(int sid, CpuContext& ctx);
    int SignalSema(int sid, CpuContext& ctx);
    int iSignalSema(int sid);
    int WaitSema(int sid, CpuContext& ctx);
    int PollSema(int sid);
    int iPollSema(int sid);
    
    // ===== Dispatch Control =====
    int DisableDispatchThread();
    int EnableDispatchThread(CpuContext& ctx);
    
    // ===== Core Scheduling =====
    void Reschedule(CpuContext& ctx);
    CpuContext& GetCurrentContext();
    PS2Thread* GetCurrentThread();
    PS2Thread* GetThread(int tid);
    bool HasCurrentThread() const { return current_thread_id_ > 0; }
    int GetCurrentThreadId() const { return current_thread_id_; }

    void* scheduler_fiber_ = nullptr;
    std::array<PS2Thread, MAX_THREADS> threads_;
    int next_thread_id_ = 1;
    int current_thread_id_ = 0;

    static void CALLBACK FiberEntry(void* param);
    
private:
    
    std::array<PriorityQueue, MAX_PRIORITIES> ready_queues_;
    std::array<PS2Semaphore, MAX_SEMAPHORES> semaphores_;
    
    
    bool dispatch_enabled_ = true;
    
    int AllocateThreadSlot();
    int AllocateSemaSlot();
    void AddToReadyQueue(int tid);
    void RemoveFromReadyQueue(int tid);
    void AddToSemaWaitQueue(int sid, int tid);
    void RemoveFromSemaWaitQueue(int sid, int tid);
    int FindHighestPriorityThread();
    void SaveContext(int tid, const CpuContext& ctx);
    void LoadContext(int tid, CpuContext& ctx);


};

// ============================================================================
// GLOBAL INSTANCE
// ============================================================================
extern PS2Scheduler g_scheduler;

// ============================================================================
// SYSCALL WRAPPERS - Call these from runtime_syscall_dispatcher
// ============================================================================

// Thread Management (32-38)
void Syscall_CreateThread(CpuContext& ctx);
void Syscall_DeleteThread(CpuContext& ctx);
void Syscall_StartThread(CpuContext& ctx);
void Syscall_ExitThread(CpuContext& ctx);
void Syscall_ExitDeleteThread(CpuContext& ctx);
void Syscall_TerminateThread(CpuContext& ctx);
void Syscall_iTerminateThread(CpuContext& ctx);

// Dispatch Control (39-40)
void Syscall_DisableDispatchThread(CpuContext& ctx);
void Syscall_EnableDispatchThread(CpuContext& ctx);

// Priority (41-44)
void Syscall_ChangeThreadPriority(CpuContext& ctx);
void Syscall_iChangeThreadPriority(CpuContext& ctx);
void Syscall_RotateThreadReadyQueue(CpuContext& ctx);
void Syscall_iRotateThreadReadyQueue(CpuContext& ctx);

// Wait Release (45-46)
void Syscall_ReleaseWaitThread(CpuContext& ctx);
void Syscall_iReleaseWaitThread(CpuContext& ctx);

// Thread Info (47-49)
void Syscall_GetThreadId(CpuContext& ctx);
void Syscall_ReferThreadStatus(CpuContext& ctx);
void Syscall_iReferThreadStatus(CpuContext& ctx);

// Sleep/Wake (50-54)
void Syscall_SleepThread(CpuContext& ctx);
void Syscall_WakeupThread(CpuContext& ctx);
void Syscall_iWakeupThread(CpuContext& ctx);
void Syscall_CancelWakeupThread(CpuContext& ctx);
void Syscall_iCancelWakeupThread(CpuContext& ctx);

// Suspend/Resume (55-58)
void Syscall_SuspendThread(CpuContext& ctx);
void Syscall_iSuspendThread(CpuContext& ctx);
void Syscall_ResumeThread(CpuContext& ctx);
void Syscall_iResumeThread(CpuContext& ctx);

// Thread Init (60-61)
void Syscall_InitMainThread(CpuContext& ctx);
void Syscall_InitHeap(CpuContext& ctx);
void Syscall_EndOfHeap(CpuContext& ctx);

// Semaphores (64-70)
void Syscall_CreateSema(CpuContext& ctx);
void Syscall_DeleteSema(CpuContext& ctx);
void Syscall_SignalSema(CpuContext& ctx);
void Syscall_iSignalSema(CpuContext& ctx);
void Syscall_WaitSema(CpuContext& ctx);
void Syscall_PollSema(CpuContext& ctx);
void Syscall_iPollSema(CpuContext& ctx);