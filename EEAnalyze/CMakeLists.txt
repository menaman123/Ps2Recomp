cmake_minimum_required(VERSION 3.16)

# Note: project() is now in the top-level CMakeLists.txt

# --- ELFIO Dependency ---
# Define an INTERFACE library for ELFIO. This is the modern CMake way
# to handle header-only libraries. It lets us treat it like a target.
add_library(elfio INTERFACE)
target_include_directories(elfio INTERFACE
    # CMAKE_SOURCE_DIR points to the root of the project (Ps2Recomp)
    "${CMAKE_SOURCE_DIR}/include/ELFIO"
)

# --- Rabbitizer Dependency ---
set(RABBITIZER_ROOT "${CMAKE_SOURCE_DIR}/include/rabbitizer")
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
add_library(EEAnalyze STATIC
    Function.cpp
    analyze.cpp
    DataFlowEngine.cpp
)

# The EEAnalyze library needs to know where its own headers are.
target_include_directories(EEAnalyze PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Link EEAnalyze to its dependencies. Because these are PUBLIC,
# any target that links to EEAnalyze (like recompiler_tool)
# will also get these include paths automatically.
target_link_libraries(EEAnalyze PUBLIC
    rabbitizer
    elfio
)


# --- Utility Executables (for testing, etc.) ---
add_executable(disasm main.cpp)
target_link_libraries(disasm PRIVATE EEAnalyze)

add_executable(jump_table_analysis jump_table_analysis.cpp)
target_link_libraries(jump_table_analysis PRIVATE EEAnalyze)