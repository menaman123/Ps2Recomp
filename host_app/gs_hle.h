#include <SDL.h>
#include "cpu_state.h"
#include <fstream>


// Global graphics handles
extern SDL_Window* g_window;
extern SDL_Renderer* g_renderer;
extern std::ofstream g_logFile;

// HLE Function Declaration
void hle_InitGraphics(CpuContext& ctx);
void hle_RenderLoop(CpuContext& ctx);