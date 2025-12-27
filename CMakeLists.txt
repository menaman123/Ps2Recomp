# --- Project Setup ---
cmake_minimum_required(VERSION 3.21)
project(Ps2RecompProject CXX C)
set(CMAKE_CXX_STANDARD 17)
set(PROJECT_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

find_package(SDL2 CONFIG REQUIRED)

# --- Dependencies (elfio and rabbitizer) ---
add_library(elfio INTERFACE)
target_include_directories(elfio INTERFACE "${PROJECT_ROOT}/include/ELFIO")

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
target_compile_definitions(rabbitizer PUBLIC RABBITIZER_STATIC)

# --- EEAnalyze Library ---
add_library(EEAnalyze STATIC
    "${PROJECT_ROOT}/EEAnalyze/analyze.cpp"
)
target_include_directories(EEAnalyze PUBLIC "${PROJECT_ROOT}")
target_link_libraries(EEAnalyze PUBLIC rabbitizer elfio)

# --- Recompiler Tool ---
add_executable(recompiler_tool
    "${PROJECT_ROOT}/recompiler_tool/main.cpp"
    "${PROJECT_ROOT}/recompiler_tool/recompiler.cpp"
    "${PROJECT_ROOT}/EEAnalyze/analyze.cpp"
    
)

target_link_libraries(recompiler_tool PRIVATE
    EEAnalyze
    SDL2::SDL2
)

# --- Generated Code ---
set(ELF_FILE_PATH "C:/Users/Owner/Desktop/PS2_Recomp/Crash_TwinSanity.rep/SLUS_209.09")
set(GHIDRA_FILE_PATH "C:/Users/Owner/Desktop/PS2_Recomp/ghidra_functions/extracted_functions.json")
set(GENERATED_HEADER "${CMAKE_BINARY_DIR}/recompiled.h")
set(GENERATED_CPP "${CMAKE_BINARY_DIR}/recompiled.cpp")

add_custom_command(
    OUTPUT ${GENERATED_HEADER} ${GENERATED_CPP}
    COMMAND recompiler_tool --input ${ELF_FILE_PATH} --ghidra-input ${GHIDRA_FILE_PATH} --output-header ${GENERATED_HEADER} --output-cpp ${GENERATED_CPP}
    DEPENDS recompiler_tool
    COMMENT "Generating recompiled C++ code from ELF file..."
)

add_custom_target(generate_recompiled_code DEPENDS ${GENERATED_HEADER} ${GENERATED_CPP})

# --- Host App ---
add_subdirectory(host_app)

target_compile_options(host_app PRIVATE 
    /bigobj
    /Od          # No optimization
    /Zm500       # More memory
)

# Single-threaded build
set_property(GLOBAL PROPERTY JOB_POOLS one_job=1)
set(CMAKE_JOB_POOL_COMPILE one_job)

set(CMAKE_BUILD_PARALLEL_LEVEL 1)

# Crucially, tell host_app where to find the generated header
target_include_directories(host_app PUBLIC
    "${PROJECT_ROOT}"
    "${CMAKE_BINARY_DIR}" # This is the key!
)

# Link host_app with necessary libraries
target_link_libraries(host_app PRIVATE EEAnalyze)

# And finally, ensure the code is generated before host_app is built
add_dependencies(host_app generate_recompiled_code)