# --- Single, All-in-One Build Script ---
cmake_minimum_required(VERSION 3.16)
# FIX: Explicitly enable both C++ and C languages for the project.
project(Ps2RecompProject CXX C)
set(CMAKE_CXX_STANDARD 17)

# Define the project root directory to make paths easier
set(PROJECT_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

# --- ELFIO Dependency (Header-Only) ---
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

# FIX: Add a compile definition to enable static linking.
# This tells the rabbitizer headers to not use __declspec(dllimport/dllexport).
target_compile_definitions(rabbitizer PUBLIC RABBITIZER_STATIC)

# --- EEAnalyze Library ---
add_library(EEAnalyze STATIC
    "${PROJECT_ROOT}/EEAnalyze/Function.cpp"
    "${PROJECT_ROOT}/EEAnalyze/analyze.cpp"
    "${PROJECT_ROOT}/EEAnalyze/DataFlowEngine.cpp"
)
# Add the EEAnalyze directory to its own include paths
target_include_directories(EEAnalyze PUBLIC "${PROJECT_ROOT}")



# --- Recompiler Tool Executable ---
add_executable(recompiler_tool
    "${PROJECT_ROOT}/recompiler_tool/main.cpp"
    "${PROJECT_ROOT}/recompiler_tool/Recompiler.cpp"
)

target_link_libraries(recompiler_tool PRIVATE EEAnalyze)
# --- Host App Executable ---
add_executable(host_app
    "${PROJECT_ROOT}/host_app/main.cpp"
    "${PROJECT_ROOT}/host_app/memory.cpp"
    "${PROJECT_ROOT}/host_app/recomp_test.cpp"
)

# --- Linking ---
# Link EEAnalyze to its dependencies.
# Because it's PUBLIC, any target linking EEAnalyze gets these automatically.
target_link_libraries(EEAnalyze PUBLIC rabbitizer elfio)

# Link the final executables to the EEAnalyze library.
target_link_libraries(recompiler_tool PRIVATE EEAnalyze)
target_link_libraries(host_app PRIVATE EEAnalyze)
