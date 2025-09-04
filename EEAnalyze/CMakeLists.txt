# --- Project Setup ---
cmake_minimum_required(VERSION 3.16)
project(PS2Tools CXX)
set(CMAKE_CXX_STANDARD 17)

include(FetchContent)

# Add Rabbitizer from the include directory
set(RABBITIZER_ROOT "C:/Users/Owner/Desktop/PS2_Recomp/Ps2Recomp/include/rabbitizer")

# Add the root of rabbitizer to the include path for all subsequent targets
include_directories("${RABBITIZER_ROOT}/tables")

# Find all the C++ and C source files for the library
file(GLOB_RECURSE RABBITIZER_SOURCES 
    "${RABBITIZER_ROOT}/cplusplus/src/*.cpp"
    "${RABBITIZER_ROOT}/src/*.c"
)

# Create a library target for rabbitizer
add_library(rabbitizer STATIC ${RABBITIZER_SOURCES})

# Tell CMake where to find the rabbitizer library's public header files
target_include_directories(rabbitizer PUBLIC
    "${RABBITIZER_ROOT}/cplusplus/include"
    "${RABBITIZER_ROOT}/include"
    "${RABBITIZER_ROOT}/include/generated"
    
)

# Fetch GoogleTest automatically
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/03597a01ee50ed33e9dfd640b249b4be3799d395.zip
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# --- EEAnalyze Library and Tests ---

# Create a library for our analysis source files
add_library(EEAnalyze STATIC 
    Function.cpp
    analyze.cpp
)
# The EEAnalyze library needs to link to rabbitizer to use its functions
target_link_libraries(EEAnalyze PUBLIC rabbitizer)

# The EEAnalyze library needs access to its own headers and rabbitizer's headers
target_include_directories(EEAnalyze PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR} 
    "${RABBITIZER_ROOT}/include"
    "C:/Users/Owner/Desktop/PS2_Recomp/Ps2Recomp/include/ELFIO"
)

# Create the test executable
add_executable(analyze_test analyze_test.cpp)
# The test executable needs to link to our library and gtest
target_link_libraries(analyze_test PRIVATE EEAnalyze gtest_main)

include(GoogleTest)
gtest_add_tests(TARGET analyze_test AUTO)

# --- Disassembler Executable ---

# Create the main executable
add_executable(disasm main.cpp)

# The main executable needs to link to our analysis library and rabbitizer
target_link_libraries(disasm PRIVATE EEAnalyze rabbitizer)

# The main executable needs to know where the ELFIO headers are
target_include_directories(disasm PRIVATE "C:/Users/Owner/Desktop/PS2_Recomp/Ps2Recomp/include/ELFIO")

# --- Jump Table Analysis Script ---

# Create the jump table analysis executable
add_executable(jump_table_analysis jump_table_analysis.cpp)

# The jump table analysis executable needs to link to our analysis library and rabbitizer
target_link_libraries(jump_table_analysis PRIVATE EEAnalyze rabbitizer)

# The jump table analysis executable needs to know where the ELFIO headers are
target_include_directories(jump_table_analysis PRIVATE "C:/Users/Owner/Desktop/PS2_Recomp/Ps2Recomp/include/ELFIO")
