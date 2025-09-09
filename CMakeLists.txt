# --- Single, All-in-One Build Script ---
cmake_minimum_required(VERSION 3.16)
project(Ps2RecompProject CXX)
set(CMAKE_CXX_STANDARD 17)

# --- Define Project Root ---
set(PROJECT_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

# --- ELFIO Dependency ---
# Define an INTERFACE library for ELFIO, which is header-only.
add_library(elfio INTERFACE)
target_include_directories(elfio INTERFACE "${PROJECT_ROOT}/include/ELFIO")

# --- Rabbitizer Dependency ---
set(RABBITIZER_ROOT "${PROJECT_ROOT}/include/rabbitizer")
file(GLOB_RECURSE RABBITIZER_SOURCES
    "${RABBITIZER_ROOT}/cplusplus/src/*.cpp"
    "${RABBITIZER_ROOT}/src/*.c"
)
add_library(rabbitizer STATIC ${RABBITIZER_SOURCES})
target_include_directories(rabbitizer PUBLIC
    "${RABBITIZER_ROOT}/cplusplus/include"
    "${RABBITIZER_ROOT}/include"
    "${RABBITIZER_ROOT}/include/generated"
    "${RABBITIZER_ROOT}/tables"
)

# --- EEAnalyze Library ---
# Define the EEAnalyze library and its source files explicitly.
add_library(EEAnalyze STATIC
    "${PROJECT_ROOT}/EEAnalyze/Function.cpp"
    "${PROJECT_ROOT}/EEAnalyze/analyze.cpp"
    "${PROJECT_ROOT}/EEAnalyze/DataFlowEngine.cpp"
)
# Add its own include directory so its headers can be found.
target_include_directories(EEAnalyze PUBLIC "${PROJECT_ROOT}/EEAnalyze")
# Link EEAnalyze to the dependencies it needs.
target_link_libraries(EEAnalyze PUBLIC rabbitizer elfio)


# --- Recompiler Tool Executable ---
# Define the final executable and its source files.
add_executable(recompiler_tool
    "${PROJECT_ROOT}/recompiler_tool/main.cpp"
    "${PROJECT_ROOT}/recompiler_tool/Recompiler.cpp"
)
# Link the executable to the EEAnalyze library we just defined.
target_link_libraries(recompiler_tool PRIVATE EEAnalyze)
# Add the include path for host_app (for cpu_state.h)
target_include_directories(recompiler_tool PRIVATE "${PROJECT_ROOT}/host_app")