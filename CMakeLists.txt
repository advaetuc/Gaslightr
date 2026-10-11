# =============================================================================
# Gaslightr — Headless DLSS 5 Neural Rendering Plugin for Adobe Photoshop
# CMakeLists.txt — Phase 1: Scaffolding & Headless DX12 Environment
# =============================================================================
cmake_minimum_required(VERSION 3.20)
project(Gaslightr
    VERSION   0.1.0
    LANGUAGES CXX
)

# ── Target platform guard ────────────────────────────────────────────────────
if(NOT WIN32)
    message(FATAL_ERROR "Gaslightr targets Windows x64 exclusively.")
endif()

# ── Compiler requirements ────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD          20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

# Force x64 architecture on MSVC generators
if(MSVC)
    set(CMAKE_GENERATOR_PLATFORM "x64" CACHE STRING "" FORCE)
endif()

# ── SDK paths (user-configurable) ────────────────────────────────────────────
# Point this at the root of your local NVIDIA NGX SDK installation.
# The SDK is expected to contain:
#   <NGX_SDK_PATH>/include/nvsdk_ngx.h
#   <NGX_SDK_PATH>/lib/x64/nvsdk_ngx_d.lib   (debug)  — optional
#   <NGX_SDK_PATH>/lib/x64/nvsdk_ngx_s.lib   (static) — optional
# NOTE: In Phase 4 we dynamically load nvngx.dll via GetProcAddress,
# so a static link library is NOT strictly required for runtime.  The
# include path is needed for type definitions only.
set(NGX_SDK_PATH "" CACHE PATH
    "Path to the NVIDIA NGX SDK root (contains include/ and lib/)")

# ── Source files ─────────────────────────────────────────────────────────────
set(GASLIGHTR_SOURCES
    src/main.cpp
    src/DX12Context.cpp
)

set(GASLIGHTR_HEADERS
    src/DX12Context.h
    include/nvngx_dlssnr_core.h
)

# ── Executable (Phase 1 is a headless console test harness) ──────────────────
add_executable(${PROJECT_NAME}
    ${GASLIGHTR_SOURCES}
    ${GASLIGHTR_HEADERS}
)

# ── Include directories ─────────────────────────────────────────────────────
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(NGX_SDK_PATH)
    target_include_directories(${PROJECT_NAME} PRIVATE
        ${NGX_SDK_PATH}/include
    )
endif()

# ── Link libraries ───────────────────────────────────────────────────────────
# DirectX 12 / DXGI — provided by the Windows SDK, no find_package needed.
target_link_libraries(${PROJECT_NAME} PRIVATE
    d3d12.lib
    dxgi.lib
    dxguid.lib        # GUIDs for debug / info queue interfaces
    Advapi32.lib      # RegSetKeyValue / RegCreateKeyEx for registry bypass
)

# ── Compiler flags (MSVC) ────────────────────────────────────────────────────
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        /W4             # High warning level
        /WX             # Treat warnings as errors
        /permissive-    # Strict standards conformance
        /Zc:__cplusplus # Report correct __cplusplus value
        /MP             # Multi-process compilation
        /GR-            # Disable RTTI — not needed; saves binary size
        /EHsc           # Standard C++ exception handling
        /utf-8          # Source and execution charset = UTF-8
    )

    # Static CRT linkage — avoids shipping vcruntime140.dll alongside the plugin.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

    target_compile_definitions(${PROJECT_NAME} PRIVATE
        WIN32_LEAN_AND_MEAN     # Trim rarely-used Win32 headers
        NOMINMAX                # Prevent <windows.h> min/max macros
        UNICODE
        _UNICODE
        _CRT_SECURE_NO_WARNINGS # Allow standard CRT functions
    )

    # Release optimizations
    target_compile_options(${PROJECT_NAME} PRIVATE
        $<$<CONFIG:Release>:/O2 /Ob2 /Oi /Ot /GL>
    )
    target_link_options(${PROJECT_NAME} PRIVATE
        $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF>
    )
endif()

# ── Installation (future phases) ─────────────────────────────────────────────
# install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)
