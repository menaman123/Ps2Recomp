cmake_minimum_required(VERSION 3.10)
project(host_app)

# Define the paths for the generated files in the main build directory
set(GENERATED_HEADER "${CMAKE_BINARY_DIR}/recompiled_functions.h")
set(GENERATED_CPP "${CMAKE_BINARY_DIR}/recompiled_functions.cpp")

# This custom command runs the recompiler tool to generate the necessary files.
# It makes sure recompiler_tool is built before this command is run.
add_custom_command(
    OUTPUT ${GENERATED_HEADER} ${GENERATED_CPP}
    COMMAND recompiler_tool --input ${CMAKE_SOURCE_DIR}/crashtw.elf --output-header ${GENERATED_HEADER} --output-cpp ${GENERATED_CPP}
    DEPENDS recompiler_tool
    COMMENT "Generating recompiled functions from ELF file..."
)

# Create a custom target for the generated files
add_custom_target(generate_recompiled_code DEPENDS ${GENERATED_HEADER} ${GENERATED_CPP})

# Add the source files for the host application
add_executable(host_app
    main.cpp
    memory.cpp
    syscalls.cpp
    ${GENERATED_CPP} # Include the generated C++ file
)

# This is crucial: it tells CMake that host_app depends on the files being generated
add_dependencies(host_app generate_recompiled_code)

# Specify the include directories, including the build directory for the generated header
target_include_directories(host_app PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_BINARY_DIR}"
)