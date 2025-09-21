# Add the source files for the host application
add_executable(host_app
    main.cpp
    memory.cpp
    syscalls.cpp
)

# Set the GENERATED property on the source file.
# This tells CMake not to check for its existence at configure time.
set_source_files_properties(
    "${CMAKE_BINARY_DIR}/recompiled_functions.cpp"
    PROPERTIES GENERATED TRUE
)

# Now, add the generated source to the target
target_sources(host_app PRIVATE
    "${CMAKE_BINARY_DIR}/recompiled_functions.cpp"
)