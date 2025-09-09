cmake_minimum_required(VERSION 3.16)

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

add_executable(recompiler_tool
    main.cpp
    Recompiler.cpp
)

# Link recompiler_tool against the EEAnalyze library.
target_link_libraries(recompiler_tool PRIVATE EEAnalyze)

target_link_libraries(recompiler_tool PUBLIC
    rabbitizer
    elfio
)

# --- EXPLICIT INCLUDES (NEW) ---
# Directly add the include paths needed by the headers used in main.cpp
# and the headers included by EEAnalyze. This is less clean but more robust
# if property propagation is failing.
target_include_directories(recompiler_tool PRIVATE
    ../host_app
    "${CMAKE_SOURCE_DIR}/include/ELFIO"
    "${CMAKE_SOURCE_DIR}/include/rabbitizer/include"
    "${CMAKE_SOURCE_DIR}/include/rabbitizer/include/generated"
)