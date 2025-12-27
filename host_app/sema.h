#ifndef SEMA_H
#define SEMA_H
// This matches the struct the PS2 game uses in its memory
#include <cstdint>
#include <vector>
#include <iostream>

struct ee_sema_t {
    uint32_t attr;       // Attributes
    uint32_t option;     // Options
    int32_t  init_count; // Initial count
    int32_t  max_count;  // Maximum count
    uint32_t wait_threads; // Internal kernel use (ignore in HLE)
};

// This is your host-side representation
struct HostSemaphore {
    int32_t count;
    int32_t max_count;
    bool active = false;
    HostSemaphore() : count(0), max_count(0), active(false) {}
};

// Global or static manager for semaphores
extern std::vector<HostSemaphore> g_semaphores;

#endif // SEMA_H