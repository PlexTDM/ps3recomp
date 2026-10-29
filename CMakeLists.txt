cmake_minimum_required(VERSION 3.20)
# Keep in step with the changelog in README.md. This sat at 0.3.0 until August
# 2026 while the project shipped 0.6.x through 0.8.0, so every build printed
# "ps3recomp runtime v0.3.0" and was believed. v0.9.0 is the first TAGGED one.
project(ps3recomp_runtime
    VERSION 0.11.0
    LANGUAGES C CXX
    DESCRIPTION "ps3recomp - Static recompilation runtime for PS3 games"
)

# ---------------------------------------------------------------------------
# Standards
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(PS3RECOMP_BUILD_TESTS "Build unit tests" OFF)

# ---------------------------------------------------------------------------
# Collect source files
# ---------------------------------------------------------------------------

# HLE library sources
file(GLOB_RECURSE HLE_SOURCES
    "libs/system/*.c"
    "libs/filesystem/*.c"
    "libs/input/*.c"
    "libs/audio/*.c"
    "libs/video/*.c"
    "libs/network/*.c"
    "libs/spurs/*.c"
    "libs/sync/*.c"
    "libs/codec/*.c"
    "libs/font/*.c"
    "libs/misc/*.c"
    "libs/hardware/*.c"
    "runtime/diag/*.c"
)
# Exclude test harnesses (e.g. libs/video/tests/*): they define their own main().
list(FILTER HLE_SOURCES EXCLUDE REGEX "/tests/")

# The Vulkan RSX backend is opt-in. It loads libvulkan at run time, so building
# needs only the Vulkan headers -- no target-architecture libvulkan, which keeps
# cross-compiling simple. It shares the HLSL -> SPIR-V translator and its
# binding plan (libs/video/rsx_shader_spirv.*) with the draw engine.
option(PS3RECOMP_RSX_VULKAN "Build the Vulkan RSX backend (Linux)" OFF)
list(FILTER HLE_SOURCES EXCLUDE REGEX "rsx_vulkan_backend\\.c$")
if(PS3RECOMP_RSX_VULKAN)
    find_path(PS3RECOMP_VULKAN_INCLUDE_DIR vulkan/vulkan.h)
    if(NOT PS3RECOMP_VULKAN_INCLUDE_DIR)
        message(FATAL_ERROR "Vulkan headers not found. Debian/Ubuntu: "
            "libvulkan-dev, Fedora: vulkan-headers -- or pass "
            "-DPS3RECOMP_VULKAN_INCLUDE_DIR=<dir containing vulkan/vulkan.h>")
    endif()
    list(APPEND HLE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/libs/video/rsx_vulkan_backend.c")
endif()

# HLSL -> SPIR-V for the Vulkan backend; a stub without glslang.
list(APPEND HLE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/libs/video/rsx_shader_spirv.cpp")
# cellAtrac decodes with FFmpeg's ATRAC3plus decoder as extracted by PPSSPP
# (LGPL-2.1, kept separate in third_party/at3_standalone -- see its README).
file(GLOB AT3_SOURCES "third_party/at3_standalone/*.cpp")
set_source_files_properties(${AT3_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")  # upstream code
list(APPEND HLE_SOURCES ${AT3_SOURCES})

# Runtime core (syscalls, PPU, SPU, memory)
file(GLOB_RECURSE RUNTIME_SOURCES
    "runtime/*.c"
    "runtime/*.cpp"
)
# Exclude:
#  - test harnesses (runtime/*/tests/*): they have their own main() and #include
#    per-game generated headers (spu_recomp.h) that a plain library build lacks.
#  - the PPU boot scaffold (runtime/ppu/, runtime/host/): these are compiled
#    per-game against the lifter-generated ppu_recomp.h, not as part of the
#    game-agnostic runtime library. See docs/PPU_RECOMP.md.
#  - spu_dispatch_mt.c: the musttail SPU indirect-branch fast path. It uses
#    __attribute__((musttail)), and it must be compiled *inside the game build*
#    by clang so the tail call is guaranteed -- a plain call leaks one resolver
#    frame per guest loop iteration. A port adds it to its own target; see
#    templates/project/CMakeLists.txt.
list(FILTER RUNTIME_SOURCES EXCLUDE REGEX "/tests/|/runtime/ppu/|/runtime/host/|/spu_dispatch_mt\.c$")

# ---------------------------------------------------------------------------
# Static library
# ---------------------------------------------------------------------------
add_library(ps3recomp_runtime STATIC
    ${HLE_SOURCES}
    ${RUNTIME_SOURCES}
)

target_include_directories(ps3recomp_runtime PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# HLE modules include their own headers
target_include_directories(ps3recomp_runtime PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/system"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/filesystem"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/input"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/audio"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/video"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/network"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/spurs"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/sync"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/codec"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/font"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/misc"
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime/spu"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# cellPad and cellAudio use SDL2 as their backend on Linux.
find_package(SDL2 QUIET)
if(SDL2_FOUND)
    if(TARGET SDL2::SDL2)
        target_link_libraries(ps3recomp_runtime PRIVATE SDL2::SDL2)
        # SDL2::SDL2 exports <prefix>/include/SDL2, but the sources include
        # <SDL2/SDL.h>. /usr/include hides that on Linux.
        get_target_property(_sdl2_incs SDL2::SDL2 INTERFACE_INCLUDE_DIRECTORIES)
        foreach(_inc IN LISTS _sdl2_incs)
            get_filename_component(_parent "${_inc}" DIRECTORY)
            target_include_directories(ps3recomp_runtime PRIVATE "${_parent}")
        endforeach()
    else()
        target_include_directories(ps3recomp_runtime PRIVATE ${SDL2_INCLUDE_DIRS})
        foreach(_inc IN LISTS SDL2_INCLUDE_DIRS)
            get_filename_component(_parent "${_inc}" DIRECTORY)
            target_include_directories(ps3recomp_runtime PRIVATE "${_parent}")
        endforeach()
        target_link_libraries(ps3recomp_runtime PRIVATE ${SDL2_LIBRARIES})
    endif()
    message(STATUS "  SDL2:         found (input + audio backends enabled)")
else()
    message(FATAL_ERROR
        "SDL2 not found. It provides the cellPad and cellAudio backends.\n"
        "  Debian:  apt install libsdl2-dev\n"
        "  Fedora:  dnf install SDL2-devel\n"
        "If SDL2 is installed in a non-standard prefix, pass "
        "-DCMAKE_PREFIX_PATH=<prefix>.")
endif()

find_package(Threads REQUIRED)
target_link_libraries(ps3recomp_runtime PRIVATE Threads::Threads)

# Frame capture (libs/video/rsx_capture.c) writes gzip. Without zlib the
# capture is compiled out; the game is unaffected.
find_package(ZLIB)
if(ZLIB_FOUND)
    target_link_libraries(ps3recomp_runtime PUBLIC ZLIB::ZLIB)
    target_compile_definitions(ps3recomp_runtime PUBLIC PS3RECOMP_HAVE_ZLIB=1)
else()
    message(STATUS "  zlib:         not found; frame capture off")
endif()

# PS3RECOMP_DEBUG_INFO: debug records in the runtime's objects so a host
# profiler or crash handler can name its functions. Codegen is unchanged.
option(PS3RECOMP_DEBUG_INFO "Compile the runtime with debug info" OFF)
if(PS3RECOMP_DEBUG_INFO)
    target_compile_options(ps3recomp_runtime PRIVATE -g)
    message(STATUS "  Debug info:   on")
endif()

if(PS3RECOMP_RSX_VULKAN)
    target_include_directories(ps3recomp_runtime PRIVATE "${PS3RECOMP_VULKAN_INCLUDE_DIR}")
    # PUBLIC: the host selects its backend on this definition.
    target_compile_definitions(ps3recomp_runtime PUBLIC PS3RECOMP_RSX_VULKAN)
    target_link_libraries(ps3recomp_runtime PUBLIC ${CMAKE_DL_LIBS})
    message(STATUS "  Vulkan:       RSX backend enabled (rsx_vulkan_backend.c, headers in ${PS3RECOMP_VULKAN_INCLUDE_DIR})")
endif()

# cellVdec decodes H.264 / MPEG-2 through FFmpeg when this is on. libavcodec
# is loaded at run time (dlopen), never linked, so building needs only the
# FFmpeg headers, and a binary run where FFmpeg is absent -- or with
# PS3RECOMP_FFMPEG=0 -- keeps the callback-only decoders. FFmpeg stays a
# separate, user-supplied LGPL library. See libs/codec/ffmpeg_runtime.h.
option(PS3RECOMP_FFMPEG_RUNTIME "Decode media through FFmpeg loaded at run time (needs the FFmpeg headers)" OFF)
if(PS3RECOMP_FFMPEG_RUNTIME)
    find_path(PS3RECOMP_FFMPEG_INCLUDE_DIR libavcodec/avcodec.h)
    if(NOT PS3RECOMP_FFMPEG_INCLUDE_DIR)
        message(FATAL_ERROR "FFmpeg headers not found. Debian/Ubuntu: "
            "libavcodec-dev, Fedora: ffmpeg-free-devel "
            "-- or pass -DPS3RECOMP_FFMPEG_INCLUDE_DIR=<dir containing libavcodec/avcodec.h>")
    endif()
    target_include_directories(ps3recomp_runtime PRIVATE "${PS3RECOMP_FFMPEG_INCLUDE_DIR}")
    target_compile_definitions(ps3recomp_runtime PRIVATE PS3RECOMP_FFMPEG_RUNTIME)
    target_link_libraries(ps3recomp_runtime PUBLIC ${CMAKE_DL_LIBS})
    message(STATUS "  FFmpeg:       run-time loaded (headers in ${PS3RECOMP_FFMPEG_INCLUDE_DIR})")
endif()

# ---------------------------------------------------------------------------
# Guest shaders on Vulkan: HLSL -> SPIR-V
# ---------------------------------------------------------------------------
# glslang only -- Vulkan consumes SPIR-V. Without glslang
# libs/video/rsx_shader_spirv.cpp is a stub and the Vulkan backend stays on its
# fixed-function path.
if(PS3RECOMP_RSX_VULKAN)
    find_package(glslang CONFIG QUIET)
    if(glslang_FOUND AND TARGET glslang::glslang AND TARGET glslang::SPIRV)
        set_source_files_properties(
            "${CMAKE_CURRENT_SOURCE_DIR}/libs/video/rsx_shader_spirv.cpp"
            PROPERTIES COMPILE_DEFINITIONS PS3RECOMP_HAVE_GLSLANG_SPIRV=1)
        target_link_libraries(ps3recomp_runtime PRIVATE glslang::glslang glslang::SPIRV)
        if(TARGET glslang::glslang-default-resource-limits)
            target_link_libraries(ps3recomp_runtime PRIVATE
                glslang::glslang-default-resource-limits)
        endif()
        message(STATUS "  Vulkan:       guest programs available (glslang ${glslang_VERSION}); "
                       "opt in with PS3RECOMP_VK_GUEST_PROGRAMS=1")
    else()
        message(STATUS "  Vulkan:       glslang not found; guest programs unavailable")
    endif()
endif()

# ---------------------------------------------------------------------------
# Compiler flags
# ---------------------------------------------------------------------------
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(ps3recomp_runtime PRIVATE
        -Wall -Wextra -Wno-unused-parameter
        -fno-strict-aliasing
        # No spontaneous fused multiply-add. A PPC fmadd is spelt as one
        # where the lifter means one.
        -ffp-contract=off
        # Explicit, because the per-config defaults cannot be trusted: this
        # tree's caches carry CMAKE_C_FLAGS_RELEASE="". Duplicating a default
        # -O2 is harmless.
        $<$<CONFIG:Release,RelWithDebInfo>:-O2>
    )
    # SSE4.1 (any x86-64 since ~2008): pshufb/blendv for spu_shufb, the
    # hottest SPU op in lifted audio code. Probed rather than assumed.
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64|i[3-6]86")
        include(CheckCCompilerFlag)
        check_c_compiler_flag(-msse4.1 PS3RECOMP_HAS_MSSE41)
        if(PS3RECOMP_HAS_MSSE41)
            target_compile_options(ps3recomp_runtime PRIVATE -msse4.1)
        endif()
    endif()
endif()

# dladdr/Dl_info (runtime/platform/win32_backtrace.h) are GNU extensions
# that glibc hides unless _GNU_SOURCE is set. g++ defines it implicitly, so
# the C++ boot scaffold never noticed; a C file including the same shim does
# not get it for free.
target_compile_definitions(ps3recomp_runtime PRIVATE _GNU_SOURCE)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(PS3RECOMP_BUILD_TESTS)
    include(CTest)
    add_subdirectory(tests/sync_stress)
    add_subdirectory(tests/fiber_switch)
    # cellVdec through its guest API: the callback-only decoder always, and
    # real H.264 / MPEG-2 decode when FFmpeg is on (libs/codec/tests).
    add_subdirectory(libs/codec/tests)
    # cellGcm's FIFO walker without a backend: where GET_REPORT writes for
    # each report DMA context. Links the runtime like the POSIX host below.
    add_subdirectory(tests/gcm_reports)

    find_package(Python3 3.10 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME spurs_task_exit_code_initialize
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_spurs_task_exit_code_initialize.py")
        add_test(NAME spurs_taskset_size
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_spurs_taskset_size.py")
        add_test(NAME spu_sdcrz
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_spu_sdcrz.py")
        add_test(NAME spu_fnms_zero_extended
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_spu_fnms_zero_extended.py")
    endif()
endif()

# ---------------------------------------------------------------------------
# Linux host harness
# ---------------------------------------------------------------------------
# Drives the cellGcm -> RSX -> backend bridge with no lifted game, so a backend
# can be brought up and regression-tested without a game binary or a recompiler
# run. It picks the Vulkan backend when PS3RECOMP_RSX_VULKAN is on, and the
# null backend's headless software path otherwise.
add_executable(ps3recomp_host
    runtime/host/host_posix.c
    runtime/host/host_stubs.c
)
target_link_libraries(ps3recomp_host PRIVATE ps3recomp_runtime m)
target_include_directories(ps3recomp_host PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/video"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
# The runtime carries C++, so the C++ driver must do the final link or the
# standard library goes missing.
set_target_properties(ps3recomp_host PROPERTIES LINKER_LANGUAGE CXX)

# ---------------------------------------------------------------------------
# PPU boot smoke title
# ---------------------------------------------------------------------------
# The one target that LINKS AND RUNS the PPU boot scaffold. runtime/ppu/ is the
# game-agnostic boot path every port is built on. runtime/ppu/tests/smoke/
# supplies a hand-written program in the lifter's ABI, and
# tools/make_smoke_elf.py writes both the PPU ELF it is loaded from and the
# ppu_recomp.h it compiles against -- the header from ppu_lifter.py's own
# HEADER_PREAMBLE, so the synthetic title cannot drift away from the ABI a real
# one gets.
#
# The HLE NID table is generated exactly as a port generates it, so the smoke
# title's firmware imports resolve through the same bridge a game's do.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(NOT Python3_Interpreter_FOUND)
    message(STATUS "  Boot smoke:   no python3; ps3recomp_boot_smoke not built")
else()
    set(SMOKE_GEN "${CMAKE_CURRENT_BINARY_DIR}/smoke")

    add_custom_command(
        OUTPUT "${SMOKE_GEN}/smoke.elf" "${SMOKE_GEN}/ppu_recomp.h"
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_smoke_elf.py"
                --out    "${SMOKE_GEN}/smoke.elf"
                --header "${SMOKE_GEN}/ppu_recomp.h"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_smoke_elf.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/ppu_lifter.py"
        COMMENT "Generating the smoke title's image and ppu_recomp.h")

    add_custom_command(
        OUTPUT "${SMOKE_GEN}/ppu_hle_nids.cpp"
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_hle_nids.py"
                --out "${SMOKE_GEN}/ppu_hle_nids.cpp" --all
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_hle_nids.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/nid_database.py"
        COMMENT "Generating the HLE NID -> handler table")

    add_executable(ps3recomp_boot_smoke
        runtime/ppu/tests/boot_main.cpp
        runtime/ppu/tests/smoke/smoke_recomp.c
        runtime/ppu/ppu_loader.cpp
        runtime/ppu/ppu_hle.cpp
        runtime/ppu/ppu_fs.cpp
        runtime/ppu/ppu_sysprx.cpp
        "${SMOKE_GEN}/ppu_hle_nids.cpp"
        "${SMOKE_GEN}/ppu_recomp.h"
    )
    target_include_directories(ps3recomp_boot_smoke PRIVATE
        "${SMOKE_GEN}"                                  # the generated ppu_recomp.h
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/runtime/ppu"
        "${CMAKE_CURRENT_SOURCE_DIR}/runtime/spu"
        "${CMAKE_CURRENT_SOURCE_DIR}/runtime/memory"
        "${CMAKE_CURRENT_SOURCE_DIR}/runtime/syscalls"
        "${CMAKE_CURRENT_SOURCE_DIR}/runtime/platform"
        "${CMAKE_CURRENT_SOURCE_DIR}/libs/video"
    )
    target_link_libraries(ps3recomp_boot_smoke PRIVATE ps3recomp_runtime)
    set_target_properties(ps3recomp_boot_smoke PROPERTIES LINKER_LANGUAGE CXX)

    # Where the runnable image lands, so CI and a developer both say
    #   ./build/ps3recomp_boot_smoke build/smoke/smoke.elf
    message(STATUS "  Boot smoke:   ps3recomp_boot_smoke + smoke/smoke.elf")

    # The same lift again, arranged the way a port's own tree arranges one:
    # the generated ppu_recomp.h and the lifted source in one directory, which
    # is what templates/project wants for -DRECOMP_DIR.
    set(SMOKE_LIFT "${SMOKE_GEN}/lift")
    set(SMOKE_LIFT_SRC
        "${CMAKE_CURRENT_SOURCE_DIR}/runtime/ppu/tests/smoke/smoke_recomp.c")
    add_custom_command(
        OUTPUT "${SMOKE_LIFT}/ppu_recomp.h" "${SMOKE_LIFT}/smoke_recomp.c"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${SMOKE_LIFT}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${SMOKE_GEN}/ppu_recomp.h" "${SMOKE_LIFT}/ppu_recomp.h"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${SMOKE_LIFT_SRC}" "${SMOKE_LIFT}/smoke_recomp.c"
        DEPENDS "${SMOKE_GEN}/ppu_recomp.h" "${SMOKE_LIFT_SRC}"
        COMMENT "Staging the smoke title's lift as a RECOMP_DIR")
    add_custom_target(ps3recomp_smoke_lift ALL
        DEPENDS "${SMOKE_LIFT}/ppu_recomp.h" "${SMOKE_LIFT}/smoke_recomp.c")
    message(STATUS "  Smoke lift:   smoke/lift (a RECOMP_DIR for templates/project)")
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS ps3recomp_runtime
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)

install(DIRECTORY include/ps3emu
    DESTINATION include
    FILES_MATCHING PATTERN "*.h"
)

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "ps3recomp runtime v${PROJECT_VERSION}")
message(STATUS "  C compiler:   ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
message(STATUS "  C++ compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Build type:   ${CMAKE_BUILD_TYPE}")
message(STATUS "")
