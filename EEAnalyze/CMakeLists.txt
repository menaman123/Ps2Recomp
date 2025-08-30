# --- Project Setup ---
cmake_minimum_required(VERSION 3.16)
project(PS2Tools CXX)
set(CMAKE_CXX_STANDARD 17)

include(FetchContent)

# --- Dependencies ---

# Fetch Capstone automatically
FetchContent_Declare(
  capstone
  URL https://github.com/capstone-engine/capstone/archive/5.0.1.zip
)
FetchContent_MakeAvailable(capstone)

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

add_library(EEAnalyze STATIC analyze.cpp)
target_include_directories(EEAnalyze PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(EEAnalyze PRIVATE capstone)

add_executable(analyze_test analyze_test.cpp)
target_link_libraries(analyze_test PRIVATE EEAnalyze gtest_main capstone)

include(GoogleTest)
gtest_discover_tests(analyze_test)

# --- Disassembler Executable ---

add_executable(disasm main.cpp)
target_link_libraries(disasm PRIVATE rabbitizer capstone)
target_include_directories(disasm PRIVATE "C:\\Users\\Owner\\Desktop\\PS2_Recomp\\ELFIO")