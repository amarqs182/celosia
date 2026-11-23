cmake_minimum_required(VERSION 3.20)

project(PS2Runtime VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(PS2X_ENABLE_RUNNER_UNITY_BUILD "Build ps2EntryRunner with CMake unity build" ON)
set(PS2X_RUNNER_UNITY_BUILD_BATCH_SIZE 32 CACHE STRING "Unity build batch size for ps2EntryRunner")
option(PS2X_ENABLE_RUNNER_PCH "Precompile the heavy runtime headers for ps2EntryRunner" ON)
option(PS2X_ENABLE_SCCACHE "Use sccache as compiler launcher when available" ON)

option(PS2X_ENABLE_RUNTIME_LOGS "Enable PS2 runtime logs" ON)
option(PS2X_ENABLE_AGRESSIVE_LOGS "Enable very verbose/agressive PS2 runtime logs" ON)
option(PS2X_ENABLE_IOP_RPC_TRACE "Log unhandled IOP/SIF RPC trace suggestions" ON)
option(PS2X_ENABLE_GUEST_READ_WATCH "Enable guest RAM load watch map checks" ON)
option(PS2X_ENABLE_GUEST_WRITE_TRACE "Enable guest RAM write trace checks" ON)
option(PS2X_LINUX_RELEASE "Linux/aarch64: release build (SOTC_RELEASE=1 diagnostics compiled out)" OFF)
option(PS2X_LINUX_SELECTIVE_LTO "Linux/aarch64: apply -flto=auto -fno-plt to runtime + overrides while keeping lean_part non-LTO" OFF)
option(PS2X_LINUX_FASTMEM "Linux/aarch64: 4 GiB mmap fastmem for PS2 guest reads/writes" OFF)
option(PS2X_STRICT_RETURN_DIAGNOSTICS "Route generated JR $ra returns through runtime branch diagnostics" OFF)
option(PS2X_SHOW_WINDOWS_CONSOLE "Show a console window for ps2EntryRunner on Windows release builds" ON)
option(PS2X_ENABLE_DEBUG_UI "Build the desktop runtime debug UI" ON)

option(PS2X_ENABLE_CCACHE "Use ccache as compiler launcher when available (preferred over sccache)" ON)
if(PS2X_ENABLE_CCACHE)
    find_program(PS2X_CCACHE_PROGRAM ccache)
    if(PS2X_CCACHE_PROGRAM)
        # Content-hashed: unchanged sources (e.g. a header touched but not
        # changed) come back from the cache instead of being recompiled.
        set(CMAKE_C_COMPILER_LAUNCHER "${PS2X_CCACHE_PROGRAM}")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${PS2X_CCACHE_PROGRAM}")
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            # Lets ccache cache sources built with the precompiled header.
            add_compile_options("SHELL:-Xclang -fno-pch-timestamp")
        endif()
        set(PS2X_ENABLE_SCCACHE OFF)
        message(STATUS "Using ccache: ${PS2X_CCACHE_PROGRAM}")
    endif()
endif()

if(PS2X_ENABLE_SCCACHE)
    find_program(PS2X_SCCACHE_PROGRAM sccache)

    if(PS2X_SCCACHE_PROGRAM)
        if(CMAKE_C_COMPILER)
            set(CMAKE_C_COMPILER_LAUNCHER "${PS2X_SCCACHE_PROGRAM}")
        endif()

        set(CMAKE_CXX_COMPILER_LAUNCHER "${PS2X_SCCACHE_PROGRAM}")
        message(STATUS "Using sccache: ${PS2X_SCCACHE_PROGRAM}")
    else()
        message(STATUS "sccache not found; continuing without compiler launcher")
    endif()
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/ReleaseMode.cmake")

set(PS2X_IS_VITA OFF)
set(PS2X_IS_ANDROID OFF)

if(ANDROID)
    set(PS2X_IS_ANDROID ON)
endif()

set(PS2X_IS_LINUX OFF)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(PS2X_IS_LINUX ON)
endif()

set(PS2X_DEFAULT_BOOT_ELF "" CACHE STRING "Guest ELF path passed directly to main() when argv is unavailable")

include(FetchContent)
set(FETCHCONTENT_QUIET FALSE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES OFF CACHE BOOL "" FORCE)

if(PS2X_IS_ANDROID)
    set(PLATFORM "Android" CACHE STRING "" FORCE)
endif()

FetchContent_Declare(
    raylib
    GIT_REPOSITORY "https://github.com/raysan5/raylib.git"
    GIT_TAG "5.5" # we will migrate to 4.2.0 later, trust me it will be better
    GIT_PROGRESS TRUE
)
FetchContent_MakeAvailable(raylib)

if(PS2X_ENABLE_DEBUG_UI AND NOT PS2X_IS_ANDROID AND NOT PS2X_IS_LINUX)
    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG "v1.92.7-docking"
        GIT_SHALLOW TRUE
    )
    FetchContent_GetProperties(imgui)

    if(NOT imgui_POPULATED)
        FetchContent_Populate(imgui)
    endif()

    set(PS2X_IMGUI_SOURCE_DIR "${imgui_SOURCE_DIR}")

    add_library(imgui STATIC
        "${PS2X_IMGUI_SOURCE_DIR}/imgui.cpp"
        "${PS2X_IMGUI_SOURCE_DIR}/imgui_draw.cpp"
        "${PS2X_IMGUI_SOURCE_DIR}/imgui_tables.cpp"
        "${PS2X_IMGUI_SOURCE_DIR}/imgui_widgets.cpp"
        "${PS2X_IMGUI_SOURCE_DIR}/imgui_demo.cpp"
    )
    target_include_directories(imgui PUBLIC "${PS2X_IMGUI_SOURCE_DIR}")

    FetchContent_Declare(
        rlImGui
        GIT_REPOSITORY https://github.com/raylib-extras/rlImGui.git
        GIT_TAG "Raylib_5_5"
        GIT_SHALLOW TRUE
    )
    FetchContent_GetProperties(rlImGui)

    if(NOT rlimgui_POPULATED)
        FetchContent_Populate(rlImGui)
    endif()

    set(PS2X_RLIMGUI_SOURCE_DIR "${rlimgui_SOURCE_DIR}")

    add_library(rlImGui STATIC
        "${PS2X_RLIMGUI_SOURCE_DIR}/rlImGui.cpp"
    )
    target_include_directories(rlImGui PUBLIC "${PS2X_RLIMGUI_SOURCE_DIR}")
    target_link_libraries(rlImGui PUBLIC raylib imgui)
endif()

add_library(ps2_host_backend INTERFACE)

set(PS2X_DEFAULT_BOOT_ELF_DEFINE "")

if(PS2X_DEFAULT_BOOT_ELF)
    set(PS2X_DEFAULT_BOOT_ELF_DEFINE "${PS2X_DEFAULT_BOOT_ELF}")
    string(REPLACE "\\" "\\\\" PS2X_DEFAULT_BOOT_ELF_DEFINE "${PS2X_DEFAULT_BOOT_ELF_DEFINE}")
    string(REPLACE "\"" "\\\"" PS2X_DEFAULT_BOOT_ELF_DEFINE "${PS2X_DEFAULT_BOOT_ELF_DEFINE}")
endif()

if(PS2X_IS_LINUX)
    # Linux (incl. AArch64): the raylib subset on SDL2 (window, input,
    # audio) and GLES3 (drawing, textures) — platform/linux, mirrored from
    # the Vita's SDL2 shim. The raylib FetchContent above still runs (it is
    # the non-Vita branch); the shim resolves the subset instead of stock
    # raylib, so the stock library is not linked here.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PS2X_LINUX_SDL2 REQUIRED IMPORTED_TARGET GLOBAL sdl2)

    find_library(PS2X_LINUX_GLES_LIBRARY NAMES GLESv3 GLESv2 GLES REQUIRED)
    find_library(PS2X_LINUX_EGL_LIBRARY NAMES EGL REQUIRED)

    add_library(ps2x_linux_raylib STATIC "${CMAKE_CURRENT_SOURCE_DIR}/platform/linux/raylib_linux.cpp")
    target_include_directories(ps2x_linux_raylib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/platform/linux")
    target_link_libraries(ps2x_linux_raylib PUBLIC
        PkgConfig::PS2X_LINUX_SDL2
        ${PS2X_LINUX_GLES_LIBRARY}
        ${PS2X_LINUX_EGL_LIBRARY}
        m
    )
    target_compile_definitions(ps2_host_backend INTERFACE PLATFORM_LINUX PS2X_LINUX_SDL2_SHIM=1)
    target_link_libraries(ps2_host_backend INTERFACE ps2x_linux_raylib)
else()
    target_link_libraries(ps2_host_backend INTERFACE raylib)

    if(PS2X_IS_ANDROID)
        target_link_libraries(ps2_host_backend INTERFACE log android)
    endif()
endif()

if(PS2X_IS_ANDROID)
    set(PS2X_ENABLE_FFMPEG_DEFAULT OFF)
else()
    set(PS2X_ENABLE_FFMPEG_DEFAULT ON)
endif()

option(PS2X_ENABLE_FFMPEG "Enable FFmpeg-backed MPEG video decoding" ${PS2X_ENABLE_FFMPEG_DEFAULT})

if(NOT PS2X_ENABLE_FFMPEG)
    message(STATUS "FFmpeg disabled; MPEG video decode falls back to stub frames")
    add_library(ffmpeg INTERFACE)
elseif(WIN32)
    include(ExternalProject)

    set(FFMPEG_PREBUILT_TAG "n7.1-241205")
    set(FFMPEG_PREBUILT_URL
        "https://github.com/System233/ffmpeg-msvc-prebuilt/releases/download/${FFMPEG_PREBUILT_TAG}/ffmpeg-${FFMPEG_PREBUILT_TAG}-lgpl-amd64-shared.zip")

    set(FFMPEG_PREFIX_DIR "${CMAKE_BINARY_DIR}/ThirdParty/ffmpeg-prefix")
    set(FFMPEG_SOURCE_DIR "${FFMPEG_PREFIX_DIR}/src/ffmpeg_external")
    set(FFMPEG_INCLUDE_DIR "${FFMPEG_SOURCE_DIR}/include")
    set(FFMPEG_LIB_DIR "${FFMPEG_SOURCE_DIR}/bin")
    set(FFMPEG_BIN_DIR "${FFMPEG_SOURCE_DIR}/bin")

    ExternalProject_Add(ffmpeg_external
        URL "${FFMPEG_PREBUILT_URL}"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        PREFIX "${FFMPEG_PREFIX_DIR}"
        CONFIGURE_COMMAND ""
        BUILD_COMMAND ""
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS
        "${FFMPEG_LIB_DIR}/avcodec.lib"
        "${FFMPEG_LIB_DIR}/avformat.lib"
        "${FFMPEG_LIB_DIR}/avutil.lib"
        "${FFMPEG_LIB_DIR}/swresample.lib"
        "${FFMPEG_LIB_DIR}/swscale.lib"
    )

    # Link against the import libraries shipped with the Windows DLLs.
    add_library(ffmpeg_avcodec STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg_avcodec PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/avcodec.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )
    add_dependencies(ffmpeg_avcodec ffmpeg_external)

    add_library(ffmpeg_avformat STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg_avformat PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/avformat.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )
    add_dependencies(ffmpeg_avformat ffmpeg_external)

    add_library(ffmpeg_avutil STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg_avutil PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/avutil.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )
    add_dependencies(ffmpeg_avutil ffmpeg_external)

    add_library(ffmpeg_swresample STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg_swresample PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/swresample.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )
    add_dependencies(ffmpeg_swresample ffmpeg_external)

    add_library(ffmpeg_swscale STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg_swscale PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/swscale.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )
    add_dependencies(ffmpeg_swscale ffmpeg_external)

    add_library(ffmpeg INTERFACE)
    target_link_libraries(ffmpeg INTERFACE
        ffmpeg_avcodec
        ffmpeg_avformat
        ffmpeg_avutil
        ffmpeg_swresample
        ffmpeg_swscale
        bcrypt
        secur32
        ws2_32
        user32
        advapi32
        ole32
        shell32
    )

    set(PS2X_FFMPEG_BIN_DIR "${FFMPEG_BIN_DIR}" CACHE INTERNAL "Directory containing FFmpeg runtime DLLs")
    set(PS2X_COPY_FFMPEG_DLLS_SCRIPT
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyFfmpegDlls.cmake"
        CACHE INTERNAL
        "Script used to stage FFmpeg runtime DLLs beside executables")
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET GLOBAL
        libavcodec
        libavformat
        libavutil
        libswresample
        libswscale
    )

    add_library(ffmpeg INTERFACE)
    target_link_libraries(ffmpeg INTERFACE PkgConfig::FFMPEG)
endif()

function(ps2x_stage_ffmpeg_runtime_dlls target_name)
    if(NOT WIN32)
        return()
    endif()

    if(NOT PS2X_FFMPEG_BIN_DIR OR NOT PS2X_COPY_FFMPEG_DLLS_SCRIPT)
        return()
    endif()

    if(TARGET ffmpeg_external)
        add_dependencies(${target_name} ffmpeg_external)
    endif()

    add_custom_command(
        TARGET ${target_name} POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
        "-Dsource_dir=${PS2X_FFMPEG_BIN_DIR}"
        "-Ddest_dir=$<TARGET_FILE_DIR:${target_name}>"
        -P "${PS2X_COPY_FFMPEG_DLLS_SCRIPT}"
        VERBATIM
    )
endfunction()

add_library(ps2_runtime STATIC
    src/lib/game_overrides.cpp
    src/lib/gs/ps2_gif_arbiter.cpp
    src/lib/ps2_audio.cpp
    src/lib/ps2_audio_vag.cpp
    src/lib/gs/ps2_gs_memory.cpp
    src/lib/gs/gs_frontend.cpp
    src/lib/gs/gs_cpu_backend.cpp
    src/lib/gs/gs_async_backend.cpp
    src/lib/gs/gs_hw_backend.cpp
    src/lib/ps2_iop_host.cpp
    src/lib/ps2_memory.cpp
    src/lib/ps2_pad.cpp
    src/lib/host_menu.cpp
    src/lib/ps2_rom_device.cpp
    src/lib/ps2_vfs.cpp
    src/lib/ps2_runtime.cpp
    src/lib/ps2_macro_selftest.cpp
    src/lib/ps2_vif1_interpreter.cpp
    src/lib/vu/ps2_vu1_core.cpp
    src/lib/vu/ps2_vu1_upper.cpp
    src/lib/vu/ps2_vu1_lower.cpp
    src/lib/vu/ps2_vu1_rec.cpp
    src/lib/games_database.cpp
)

# Native renderer: the front picks Metal (macOS) or OpenGL (macOS with
# SOTC_NATIVE_GL=1, the Vita through vitaGL).
target_sources(ps2_runtime PRIVATE src/lib/native/native_renderer.cpp src/lib/native/native_renderer_gl.cpp src/lib/native/native_recorder.cpp src/lib/native/native_renderer_mali.cpp src/lib/native/r36s_boot_menu.cpp)
# The scene recorder's blocks are zlib-compressed (the Vita SDK has libz).
find_package(ZLIB QUIET)
if(TARGET ZLIB::ZLIB)
    target_link_libraries(ps2_runtime PUBLIC ZLIB::ZLIB ${CMAKE_DL_LIBS})
else()
    target_link_libraries(ps2_runtime PUBLIC z ${CMAKE_DL_LIBS})
endif()

if(PS2X_ENABLE_RUNTIME_LOGS AND NOT PS2X_LINUX_RELEASE)
    target_compile_definitions(ps2_runtime PUBLIC
        PS2_RUNTIME_LOGS=1
    )
endif()

if(PS2X_ENABLE_AGRESSIVE_LOGS AND NOT PS2X_LINUX_RELEASE)
    target_compile_definitions(ps2_runtime PUBLIC
        AGRESSIVE_LOGS=1
        PS2_FUNCTION_LOG_TRACKER=1
    )
endif()

if(PS2X_ENABLE_IOP_RPC_TRACE AND NOT PS2X_LINUX_RELEASE)
    target_compile_definitions(ps2_runtime PUBLIC
        PS2X_ENABLE_IOP_RPC_TRACE=1
    )
else()
    target_compile_definitions(ps2_runtime PUBLIC
        PS2X_ENABLE_IOP_RPC_TRACE=0
    )
endif()

if(PS2X_ENABLE_GUEST_READ_WATCH AND NOT PS2X_LINUX_RELEASE)
    target_compile_definitions(ps2_runtime PUBLIC
        PS2X_GUEST_READ_WATCH=1
    )
else()
    target_compile_definitions(ps2_runtime PUBLIC
        PS2X_GUEST_READ_WATCH=0
    )
endif()

if(PS2X_ENABLE_GUEST_WRITE_TRACE AND NOT PS2X_LINUX_RELEASE)
    target_compile_definitions(ps2_runtime PUBLIC
        PS2X_GUEST_WRITE_TRACE=1
    )
else()
    target_compile_definitions(ps2_runtime PUBLIC
        PS2X_GUEST_WRITE_TRACE=0
    )
endif()

if(PS2X_STRICT_RETURN_DIAGNOSTICS)
    target_compile_definitions(ps2_runtime PUBLIC
        PS2X_STRICT_RETURN_DIAGNOSTICS=1
    )
endif()

target_compile_definitions(ps2_runtime PRIVATE
    PS2X_HAS_FFMPEG=$<BOOL:${PS2X_ENABLE_FFMPEG}>
)

file(GLOB_RECURSE KERNEL_SRC_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/Kernel/*.cpp"
)

target_sources(ps2_runtime PRIVATE
    ${KERNEL_SRC_FILES}
)

# Directories holding the recompiled game code (and any hand-written game
# overrides). Defaults to src/runner; projects can keep generated code out of
# the source tree by pointing this at their own directories.
set(PS2X_RUNNER_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src/runner" CACHE STRING "Directories with ps2EntryRunner sources")
set(RUNNER_SRC_FILES)
foreach(runner_dir IN LISTS PS2X_RUNNER_DIRS)
    file(GLOB runner_dir_files CONFIGURE_DEPENDS "${runner_dir}/*.cpp")
    list(APPEND RUNNER_SRC_FILES ${runner_dir_files})
endforeach()

# Game overrides (game/overrides, next to PS2Recomp): the Vita and Windows
# builds pass this directory through PS2X_RUNNER_DIRS, but the
# linux-aarch64 (R36S) build was configured without it, so none of the
# *_overrides.cpp files -- including sotc_overrides.cpp (the idle wait, the
# host clock sync) -- were compiled there. Add the directory to the runner
# sources (and to the include path below) whenever a non-Vita build did
# not already list it. Vita always passes it, so it is left alone.
if(NOT PS2X_IS_VITA)
    set(PS2X_GAME_OVERRIDES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../game/overrides")
    if(EXISTS "${PS2X_GAME_OVERRIDES_DIR}")
        # PS2X_RUNNER_DIRS may spell the directory differently (e.g.
        # /d/celosia_upstream/game/overrides); normalise before testing so
        # the files are not added twice.
        set(PS2X_RUNNER_DIRS_NORM "${PS2X_RUNNER_DIRS}")
        string(REPLACE "\\" "/" PS2X_RUNNER_DIRS_NORM "${PS2X_RUNNER_DIRS_NORM}")
        if(NOT PS2X_RUNNER_DIRS_NORM MATCHES "game/overrides")
            file(GLOB PS2X_GAME_OVERRIDES_SRC
                 CONFIGURE_DEPENDS "${PS2X_GAME_OVERRIDES_DIR}/*.cpp")
            list(APPEND RUNNER_SRC_FILES ${PS2X_GAME_OVERRIDES_SRC})
            list(REMOVE_DUPLICATES RUNNER_SRC_FILES)
            list(APPEND PS2X_RUNNER_DIRS "${PS2X_GAME_OVERRIDES_DIR}")
        endif()
    endif()
endif()

set(RUNNER_MAIN_CPP "${CMAKE_CURRENT_SOURCE_DIR}/src/runner/main.cpp")
set(ROOT_MAIN_CPP "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")

if(EXISTS "${RUNNER_MAIN_CPP}")
    list(APPEND RUNNER_SRC_FILES "${RUNNER_MAIN_CPP}")
elseif(EXISTS "${ROOT_MAIN_CPP}")
    list(APPEND RUNNER_SRC_FILES "${ROOT_MAIN_CPP}")
endif()

if(PS2X_IS_ANDROID)
    add_library(ps2EntryRunner SHARED
        ${RUNNER_SRC_FILES}
    )

    target_link_options(ps2EntryRunner PRIVATE "-Wl,--undefined=ANativeActivity_onCreate")
else()
    add_executable(ps2EntryRunner
        ${RUNNER_SRC_FILES}
    )
endif()

if(PS2X_ENABLE_RUNNER_UNITY_BUILD)
    set_target_properties(ps2EntryRunner PROPERTIES
        UNITY_BUILD ON
        UNITY_BUILD_BATCH_SIZE "${PS2X_RUNNER_UNITY_BUILD_BATCH_SIZE}"
    )
endif()

# Lean copies of hot recompiled functions (game/overrides/lean_*.cpp): they
# rename the functions they hold (lean/renames.inc), so each is a
# translation unit of its own.
foreach(PS2X_SOURCE IN LISTS RUNNER_SRC_FILES)
    if(PS2X_SOURCE MATCHES "/overrides/lean_[^/]*\\.cpp$")
        set_source_files_properties(${PS2X_SOURCE} PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
    endif()
    # Decomp D2: the lifted batches (decomp_liftd*_NN.cpp, D4: decomp_liftalt_NN.cpp; tools/lift_d1.py) are large and share namespace lift_d1:
    # each a translation unit of its own (a guest function named like a libm one, D1's fabsf, stays out of the
    # other batches; they build in parallel).
    if(PS2X_SOURCE MATCHES "/overrides/decomp_lift(d[0-9]+|alt)_[0-9]+\\.cpp$")
        set_source_files_properties(${PS2X_SOURCE} PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
    endif()
    # The second set (lean_part_tN.cpp) defines PS2X_EE_TRUST_FINITE before
    # ps2_runtime_macros.h: not with the precompiled header, which has it 0.
    if(PS2X_SOURCE MATCHES "/overrides/lean_part_t[0-9]+\\.cpp$")
        set_source_files_properties(${PS2X_SOURCE} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    endif()
endforeach()

if(PS2X_ENABLE_RUNNER_PCH)
    set(PS2X_RUNNER_PCH_HEADERS
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_runtime_macros.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_runtime.h"
    )

    foreach(PS2X_RUNNER_GENERATED_HEADER IN ITEMS
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_recompiled_functions.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_recompiled_stubs.h")
        if(EXISTS "${PS2X_RUNNER_GENERATED_HEADER}")
            list(APPEND PS2X_RUNNER_PCH_HEADERS "${PS2X_RUNNER_GENERATED_HEADER}")
        endif()
    endforeach()

    list(APPEND PS2X_RUNNER_PCH_HEADERS
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_syscalls.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_stubs.h"
    )

    target_precompile_headers(ps2EntryRunner PRIVATE ${PS2X_RUNNER_PCH_HEADERS})
endif()

if(PS2X_DEFAULT_BOOT_ELF_DEFINE)
    target_compile_definitions(ps2EntryRunner PRIVATE
        PS2X_DEFAULT_BOOT_ELF="${PS2X_DEFAULT_BOOT_ELF_DEFINE}"
    )
endif()

if(MSVC)
    target_compile_options(ps2EntryRunner PRIVATE /FS)
endif()

target_include_directories(ps2_runtime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lib/Kernel
)

target_link_libraries(ps2_runtime PUBLIC ps2_host_backend ffmpeg ps2_iop)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/meshoptimizer/CMakeLists.txt")
    set(MESHOPT_BUILD_DEMO OFF CACHE BOOL "" FORCE)
    set(MESHOPT_BUILD_GLTFPACK OFF CACHE BOOL "" FORCE)
    set(MESHOPT_INSTALL OFF CACHE BOOL "" FORCE)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/meshoptimizer" "${CMAKE_CURRENT_BINARY_DIR}/meshoptimizer" EXCLUDE_FROM_ALL)
    target_link_libraries(ps2_runtime PUBLIC meshoptimizer)
    target_include_directories(ps2_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/meshoptimizer/src")
endif()
target_link_libraries(ps2EntryRunner
    PRIVATE
    ps2_runtime
)
target_include_directories(ps2EntryRunner PRIVATE ${PS2X_RUNNER_DIRS})

# Linux: the debug UI needs the stock raylib (rlImGui), which the SDL2/GLES
# shim does not provide yet (PLATFORM_LINUX; TODO r36s).
if(PS2X_ENABLE_DEBUG_UI AND NOT PS2X_IS_VITA AND NOT PS2X_IS_ANDROID AND NOT PS2X_IS_LINUX)
    target_sources(ps2EntryRunner PRIVATE
        src/lib/ps2_debug_panel.cpp
    )
    target_link_libraries(ps2EntryRunner PRIVATE rlImGui imgui)
    target_compile_definitions(ps2EntryRunner PRIVATE PS2X_ENABLE_DEBUG_UI=1)
endif()

ps2x_stage_ffmpeg_runtime_dlls(ps2EntryRunner)

if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    EnableFastReleaseMode(ps2_runtime)
    EnableFastReleaseMode(ps2EntryRunner)

    if(WIN32 AND NOT PS2X_SHOW_WINDOWS_CONSOLE)
        if(MSVC)
            target_link_options(ps2EntryRunner PRIVATE
                $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:/SUBSYSTEM:WINDOWS>
                $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:/ENTRY:mainCRTStartup>
            )
        elseif(MINGW)
            target_link_options(ps2EntryRunner PRIVATE
                $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:-mwindows>
            )
        endif()
    endif()
endif()

# Work around WinAPI vs raylib symbol clash for CloseWindow on x64
if(MSVC)
    target_link_options(ps2EntryRunner PRIVATE "/FORCE:MULTIPLE")
endif()

# Generated/native VU1 code trusts the FPU mode instead of normalising every
# float in software (see vu1rec::norm).
option(PS2X_VU1_TRUST_FPU "VU1 native/generated code: no software float normalisation" ON)
if(PS2X_VU1_TRUST_FPU)
    target_compile_definitions(ps2EntryRunner PRIVATE PS2X_VU1_TRUST_FPU=1)
endif()

if(PS2X_IS_LINUX AND PS2X_IS_AARCH64_TARGET)
    set(PS2X_LINUX_GAME_OPT "-Os" CACHE STRING "Optimization level of cold recompiled game code on Linux/aarch64 (-Os, -O2, or -O3)")
    target_compile_options(ps2_runtime PRIVATE -O3)
    if(TARGET ps2_iop)
        target_compile_options(ps2_iop PRIVATE -O3 -fno-math-errno -fno-trapping-math)
    endif()
    set_source_files_properties(src/lib/native/native_renderer_gl.cpp PROPERTIES COMPILE_OPTIONS "-fno-math-errno;-fno-trapping-math")
    set_source_files_properties(src/lib/native/native_renderer_mali.cpp PROPERTIES COMPILE_OPTIONS "-fno-math-errno;-fno-trapping-math;-mcpu=cortex-a35")
    target_compile_options(ps2EntryRunner PRIVATE ${PS2X_LINUX_GAME_OPT})

    set(PS2X_LINUX_OVERRIDE_OPTS -O3)
    if(PS2X_LINUX_SELECTIVE_LTO)
        set(PS2X_LINUX_LTO_FLAG "-flto=auto")
        if(CMAKE_HOST_WIN32)
            cmake_host_system_information(RESULT PS2X_HOST_CORES QUERY NUMBER_OF_LOGICAL_CORES)
            if(PS2X_HOST_CORES GREATER 1)
                set(PS2X_LINUX_LTO_FLAG "-flto=${PS2X_HOST_CORES}")
            endif()
        endif()
        message(STATUS "Linux/aarch64: selective LTO on ps2_runtime + overrides (${PS2X_LINUX_LTO_FLAG} -ffat-lto-objects -fno-plt)")
        list(APPEND PS2X_LINUX_OVERRIDE_OPTS -flto=auto -ffat-lto-objects -fno-plt)
        foreach(PS2X_LINUX_TARGET IN ITEMS ps2_runtime ps2_iop)
            if(TARGET ${PS2X_LINUX_TARGET})
                target_compile_options(${PS2X_LINUX_TARGET} PRIVATE -flto=auto -ffat-lto-objects -fno-plt)
            endif()
        endforeach()
        target_link_options(ps2EntryRunner PRIVATE ${PS2X_LINUX_LTO_FLAG} -fno-plt
            $<$<BOOL:${CMAKE_HOST_WIN32}>:--param=lto-partitions=16>)
    endif()
    if(PS2X_LINUX_RELEASE)
        message(STATUS "Linux/aarch64: release build (SOTC_RELEASE=1)")
        list(APPEND PS2X_LINUX_OVERRIDE_OPTS -DSOTC_RELEASE=1)
        foreach(PS2X_LINUX_TARGET IN ITEMS ps2_runtime ps2_iop)
            if(TARGET ${PS2X_LINUX_TARGET})
                target_compile_definitions(${PS2X_LINUX_TARGET} PRIVATE SOTC_RELEASE=1)
            endif()
        endforeach()
    endif()
    if(PS2X_LINUX_FASTMEM)
        message(STATUS "Linux/aarch64: 4 GiB mmap fastmem (PS2X_LINUX_FASTMEM=1)")
        foreach(PS2X_LINUX_TARGET IN ITEMS ps2_runtime ps2_iop ps2EntryRunner)
            if(TARGET ${PS2X_LINUX_TARGET})
                target_compile_definitions(${PS2X_LINUX_TARGET} PRIVATE PS2X_LINUX_FASTMEM=1)
            endif()
        endforeach()
    endif()

    set(PS2X_LINUX_PGO "OFF" CACHE STRING "Linux/aarch64 PGO mode: OFF, gen, or collected profile directory")
    set(PS2X_LINUX_PGO_OPTS "")
    file(TO_NATIVE_PATH "${CMAKE_BINARY_DIR}" PS2X_LINUX_PGO_PREFIX)
    if(PS2X_LINUX_PGO STREQUAL "gen")
        message(STATUS "Linux/aarch64: PGO generate (runtime + overrides + lean_part_t*, prefix=${PS2X_LINUX_PGO_PREFIX})")
        set(PS2X_LINUX_PGO_OPTS
            -fprofile-generate=/tmp/sotc_pgo/
            -fprofile-prefix-path=${PS2X_LINUX_PGO_PREFIX}
            -fprofile-update=single
            -fno-profile-values
            -fno-profile-reorder-functions)
        list(APPEND PS2X_LINUX_OVERRIDE_OPTS ${PS2X_LINUX_PGO_OPTS})
        target_compile_options(ps2_runtime PRIVATE ${PS2X_LINUX_PGO_OPTS})
        target_link_options(ps2EntryRunner PRIVATE -fprofile-generate=/tmp/sotc_pgo/ -Wl,-u,__gcov_dump)
    elseif(NOT PS2X_LINUX_PGO STREQUAL "" AND NOT PS2X_LINUX_PGO STREQUAL "OFF")
        get_filename_component(PS2X_LINUX_PGO_USE_DIR "${PS2X_LINUX_PGO}" ABSOLUTE)
        message(STATUS "Linux/aarch64: PGO use from ${PS2X_LINUX_PGO_USE_DIR} (prefix=${PS2X_LINUX_PGO_PREFIX})")
        set(PS2X_LINUX_PGO_OPTS
            -fprofile-use=${PS2X_LINUX_PGO_USE_DIR}
            -fprofile-prefix-path=${PS2X_LINUX_PGO_PREFIX}
            -fprofile-partial-training
            -fprofile-correction
            -fno-profile-values
            -fno-profile-reorder-functions
            -fno-vpt
            -Wno-missing-profile
            -Wno-coverage-mismatch)
        list(APPEND PS2X_LINUX_OVERRIDE_OPTS ${PS2X_LINUX_PGO_OPTS})
        target_compile_options(ps2_runtime PRIVATE ${PS2X_LINUX_PGO_OPTS})
    endif()

    get_target_property(PS2X_RUNNER_SOURCES ps2EntryRunner SOURCES)
    foreach(PS2X_SOURCE IN LISTS PS2X_RUNNER_SOURCES)
        if(PS2X_SOURCE MATCHES "/overrides/lean_part[0-9]+\\.cpp$")
            set_source_files_properties(${PS2X_SOURCE} PROPERTIES COMPILE_OPTIONS "${PS2X_LINUX_GAME_OPT}")
        elseif(PS2X_SOURCE MATCHES "/overrides/[^/]*\\.cpp$")
            set_source_files_properties(${PS2X_SOURCE} PROPERTIES COMPILE_OPTIONS "${PS2X_LINUX_OVERRIDE_OPTS}")
        elseif(PS2X_SOURCE MATCHES "vu1rec_[^/]*\\.cpp$")
            set_source_files_properties(${PS2X_SOURCE} PROPERTIES COMPILE_OPTIONS "-O2")
        endif()
    endforeach()

    option(PS2X_LINUX_FUNCTION_SECTIONS "Linux/aarch64: -ffunction-sections and hot-code order file" ON)
    set(PS2X_LINUX_HOT_ORDER "" CACHE FILEPATH "Linux/aarch64: ld --section-ordering-file (empty: linux_hot_order.ld)")
    if(PS2X_LINUX_FUNCTION_SECTIONS)
        foreach(PS2X_LINUX_TARGET IN ITEMS ps2_runtime ps2EntryRunner ps2_iop)
            if(TARGET ${PS2X_LINUX_TARGET})
                target_compile_options(${PS2X_LINUX_TARGET} PRIVATE -ffunction-sections)
            endif()
        endforeach()
        set(PS2X_LINUX_HOT_ORDER_FILE "${PS2X_LINUX_HOT_ORDER}")
        if(NOT PS2X_LINUX_HOT_ORDER_FILE)
            foreach(PS2X_RUNNER_DIR IN LISTS PS2X_RUNNER_DIRS)
                if(EXISTS "${PS2X_RUNNER_DIR}/linux_hot_order.ld")
                    set(PS2X_LINUX_HOT_ORDER_FILE "${PS2X_RUNNER_DIR}/linux_hot_order.ld")
                endif()
            endforeach()
        endif()
        if(NOT PS2X_LINUX_HOT_ORDER_FILE AND EXISTS "${CMAKE_SOURCE_DIR}/../game/overrides/linux_hot_order.ld")
            set(PS2X_LINUX_HOT_ORDER_FILE "${CMAKE_SOURCE_DIR}/../game/overrides/linux_hot_order.ld")
        endif()
        if(PS2X_LINUX_HOT_ORDER_FILE AND EXISTS "${PS2X_LINUX_HOT_ORDER_FILE}")
            message(STATUS "Linux/aarch64: hot-code order from ${PS2X_LINUX_HOT_ORDER_FILE}")
            target_link_options(ps2EntryRunner PRIVATE "-Wl,--section-ordering-file=${PS2X_LINUX_HOT_ORDER_FILE}")
            set_property(TARGET ps2EntryRunner APPEND PROPERTY LINK_DEPENDS "${PS2X_LINUX_HOT_ORDER_FILE}")
        endif()
    endif()
endif()

if(PS2X_IS_ANDROID)
    target_sources(ps2EntryRunner PRIVATE src/lib/ps2_android_runtime.cpp)
endif()

install(TARGETS ps2_runtime ps2EntryRunner
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

if(WIN32 AND PS2X_ENABLE_FFMPEG)
    install(DIRECTORY "${FFMPEG_BIN_DIR}/"
        DESTINATION bin
        FILES_MATCHING
        PATTERN "*.dll"
    )
endif()

install(DIRECTORY include/
    DESTINATION include
)
