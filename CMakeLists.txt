cmake_minimum_required(VERSION 3.20)
project(FiveMFrameGen VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Windows SDK
set(CMAKE_SYSTEM_VERSION 10.0.19041.0)

# Compiler flags
if(MSVC)
    add_compile_options(/W3 /MP)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    add_compile_definitions(NOMINMAX)
    add_compile_definitions(WIN32_LEAN_AND_MEAN)
    add_compile_definitions(FIVEM_FRAMEGEN_EXPORTS)
    
    # Static runtime linking (/MT) to avoid missing VCRUNTIME140.dll
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Dependencies
add_subdirectory(deps/minhook)
add_subdirectory(deps/imgui)

# Source files
set(SOURCES
    src/main.cpp
    src/core/hooks.cpp
    src/core/hooks_d3d12.cpp
    src/core/d3d11_wrapper.cpp
    src/core/swap_chain_hook.cpp
    src/frame_gen/frame_generator.cpp
    src/frame_gen/fsr3_backend.cpp
    src/frame_gen/optical_flow.cpp
    src/frame_gen/frame_buffer.cpp
    src/overlay/imgui_overlay.cpp
    src/overlay/config_ui.cpp
    src/utils/logger.cpp
    src/utils/config.cpp
    src/utils/performance.cpp
    src/resource.rc
)

# Main library target
add_library(FiveMFrameGen SHARED ${SOURCES})

target_include_directories(FiveMFrameGen PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/deps/minhook/include
    ${CMAKE_SOURCE_DIR}/deps/imgui
    ${CMAKE_SOURCE_DIR}/deps/imgui/backends
)

target_link_libraries(FiveMFrameGen PRIVATE
    minhook
    imgui
    d3d11
    d3d12
    dxgi
    d3dcompiler
)

# Post-build: Rename DLL to ASI
add_custom_command(TARGET FiveMFrameGen POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:FiveMFrameGen>
        ${CMAKE_BINARY_DIR}/bin/FiveMFrameGen.asi
    COMMENT "Creating ASI plugin..."
)

# Install rules
install(FILES ${CMAKE_BINARY_DIR}/bin/FiveMFrameGen.asi
    DESTINATION plugins
)

# Verification tool
add_executable(verify_resource src/verify_resource.cpp)

# Package configuration
set(CPACK_GENERATOR "ZIP")
set(CPACK_PACKAGE_NAME "FiveMFrameGen")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
include(CPack)
