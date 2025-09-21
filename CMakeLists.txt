# --- Project Setup ---
cmake_minimum_required(VERSION 3.16)
project(Ps2RecompProject CXX C)
set(CMAKE_CXX_STANDARD 17)
set(PROJECT_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

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
    "${PROJECT_ROOT}/EEAnalyze/Function.cpp"
    "${PROJECT_ROOT}/EEAnalyze/analyze.cpp"
    "${PROJECT_ROOT}/EEAnalyze/DataFlowEngine.cpp"
)
target_include_directories(EEAnalyze PUBLIC "${PROJECT_ROOT}")
target_link_libraries(EEAnalyze PUBLIC rabbitizer elfio)

# --- Recompiler Tool ---
add_executable(recompiler_tool
    "${PROJECT_ROOT}/recompiler_tool/main.cpp"
    "${PROJECT_ROOT}/recompiler_tool/Recompiler.cpp"
)
target_link_libraries(recompiler_tool PRIVATE EEAnalyze)

# --- Generated Code ---
set(ELF_FILE_PATH "C:/Users/Owner/Desktop/PS2_Recomp/Crash_TwinSanity.rep/SLUS_209.09")
set(GENERATED_HEADER "${CMAKE_BINARY_DIR}/recompiled_functions.h")
set(GENERATED_CPP "${CMAKE_BINARY_DIR}/recompiled_functions.cpp")

# This command runs the recompiler after it's built
add_custom_command(
    OUTPUT ${GENERATED_HEADER} ${GENERATED_CPP}
    COMMAND recompiler_tool --input ${ELF_FILE_PATH} --output-header ${GENERATED_HEADER} --output-cpp ${GENERATED_CPP}
    DEPENDS recompiler_tool
    COMMENT "Generating recompiled C++ code from ELF file..."
)

# This target ensures the generated files exist before dependents are built
add_custom_target(generate_recompiled_code DEPENDS ${GENERATED_HEADER} ${GENERATED_CPP})

# --- Host App ---
add_executable(host_app
    "${PROJECT_ROOT}/host_app/main.cpp"
    "${PROJECT_ROOT}/host_app/memory.cpp"
    "${PROJECT_ROOT}/host_app/syscalls.cpp"
    ${GENERATED_CPP} 
)
target_include_directories(host_app PUBLIC
    "${PROJECT_ROOT}"
    "${CMAKE_BINARY_DIR}"
)

# This is the crucial link: host_app depends on the generated code
add_dependencies(host_app generate_recompiled_code)
target_link_libraries(host_app PRIVATE EEAnalyze)