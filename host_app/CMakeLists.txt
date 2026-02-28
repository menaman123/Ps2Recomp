# Add the source files for the host application
add_library(GLM INTERFACE)

# CHANGE: Use CMAKE_SOURCE_DIR to guarantee the path is correct
target_include_directories(GLM INTERFACE
    "${CMAKE_SOURCE_DIR}/include/glm"
)

add_executable(host_app
    main.cpp
    memory.cpp
    syscalls.cpp
    ps2_scheduler.h
    ps2_scheduler.cpp
    sif.cpp
    sif.h
    dmac.h
    dmac.cpp
    gs.h
    gs.cpp
    intc.h
    intc.cpp
    sif_hle.cpp
    sif_hle.h
    gs_hle.h
    gs_hle.cpp
    gif.h
    gif.cpp
    graphics_thread.cpp
    gs_state.cpp
    gs_state.h
    vif.cpp
    vif.h
    vu.h
    vu.cpp
    hle_heap.cpp
    hle_heap.h
    sif_rpc_handlers.h
    sif_rpc_handlers.cpp
    texture_decoder.h
    texture_decoder.cpp
    texture_cache.h
    texture_cache.cpp

)
# Set the GENERATED property on the source file.
# This tells CMake not to check for its existence at configure time.
set_source_files_properties(
    "${CMAKE_BINARY_DIR}/recompiled.cpp"
    PROPERTIES GENERATED TRUE
)

# Now, add the generated source to the target
target_sources(host_app PRIVATE
    "${CMAKE_BINARY_DIR}/recompiled.cpp"
)

target_link_libraries(host_app PRIVATE
    EEAnalyze
    rabbitizer
    GLM
    SDL2::SDL2main
    SDL2::SDL2
)

if(MSVC)
    # /FS allows multiple CL.EXE processes to write to the same .PDB file
    target_compile_options(host_app PRIVATE "/FS")
endif()