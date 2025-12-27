#ifndef INTC_H
#define INTC_H

#include <vector>
#include <map>

struct IntcHandler {
    int id;
    int cause;
    uint32_t handler_pc; // Guest MIPS address of the handler function
    uint32_t gp;         // Global pointer context for the handler
    uint32_t arg;        // Argument passed to handler
    int flag;            // Flag value (stored but typically unused)
    bool active;         // Whether this handler is active
};

// Map cause ID to a list of handlers (ordered by priority)
std::map<int, std::vector<IntcHandler>> g_intc_queues;
int g_nextIntcId = 1;

#endif // INTC_H