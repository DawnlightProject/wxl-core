# WarcraftXL. A Win32 configure builds everything injected into the 32-bit client (WarcraftXL.dll, the
# d3d9.dll proxy, wxl-patcher.exe, the extensions); an x64 configure builds wxl-host.exe, the 64-bit
# companion process (docs/host.md). build.ps1 drives both.
# Copyright (C) 2026 WarcraftXL. GPLv3 (see source headers).

cmake_minimum_required(VERSION 3.20)
# C is enabled for the vendored C libraries pulled in via add_subdirectory (MinHook).
project(WarcraftXL LANGUAGES CXX C)

# Static CRT: the DLLs load inside the player's Wow.exe, which has no reason to carry the toolset's
# brand-new VC++ redistributable. A dynamic CRT turns a missing/stale redist into a loader failure at
# process start (0xc0000142) with no log at all.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # feeds the IDE the real C++20 flags

set(WXL_DEFS WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)

# Client directory to deploy the built artifacts into. Passed by build.ps1 (-DCLIENT_PATH=...) and cached.
set(CLIENT_PATH "" CACHE PATH "Client folder the built artifacts are copied into after each build")

# Tracy zones in the host and its client (third_party/tracy, BSD-3). Off by default: it adds a profiler
# thread and a listening socket to both processes.
option(WXL_TRACY "Build the Tracy client into WarcraftXL.dll and wxl-host.exe" OFF)
set(WXL_TRACY_SRC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tracy/public/TracyClient.cpp")

# The shared-memory contract is header-only (src/ipc); TLSF manages the transfer window on the client
# and the store on the host.
set(WXL_TLSF_SRC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tlsf/tlsf.c")

# --- SDK boundary --------------------------------------------------------------------------------
# Core (src/client, src/engine, src/game, src/runtime) legitimately touches offsets/ directly -- a
# detour needs the real address, no amount of indirection removes that. The boundary that actually
# holds is narrower: an EXTENSION should never need offsets/ itself, only wxl::game / wxl::events /
# WXL_Api -- an extension reaching for a raw offset is reaching past the SDK surface meant to
# insulate it from a client-version rebase.
option(WXL_STRICT_SDK_BOUNDARY "Fail the configure when an extension includes offsets/ directly" OFF)

file(GLOB_RECURSE WXL_BOUNDARY_SCAN CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/extensions/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/extensions/*.hpp")

set(WXL_BOUNDARY_VIOLATIONS "")
foreach(wxl_file IN LISTS WXL_BOUNDARY_SCAN)
    file(STRINGS "${wxl_file}" wxl_hit REGEX "^[ \t]*#include[ \t]+\"offsets/")
    if(wxl_hit)
        file(RELATIVE_PATH wxl_rel "${CMAKE_CURRENT_SOURCE_DIR}" "${wxl_file}")
        list(APPEND WXL_BOUNDARY_VIOLATIONS "${wxl_rel}")
    endif()
endforeach()

list(LENGTH WXL_BOUNDARY_VIOLATIONS WXL_BOUNDARY_COUNT)
if(WXL_BOUNDARY_COUNT GREATER 0)
    string(REPLACE ";" "\n  " WXL_BOUNDARY_PRETTY "${WXL_BOUNDARY_VIOLATIONS}")
    if(WXL_STRICT_SDK_BOUNDARY)
        message(FATAL_ERROR
            "SDK boundary: ${WXL_BOUNDARY_COUNT} extension file(s) include offsets/ directly:\n  ${WXL_BOUNDARY_PRETTY}")
    endif()
    message(STATUS "SDK boundary: ${WXL_BOUNDARY_COUNT} extension file(s) include offsets/ directly")
endif()

# =============================================================================
# 32-bit family (injected into the client): WarcraftXL.dll + wxl-patcher.exe + d3d9.dll
# =============================================================================
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    # --- Automatic, recursive source discovery (no hand-written paths) -----------------------------
    # CONFIGURE_DEPENDS re-globs at build time, so dropping a file (or a whole script) needs no edit here.
    # The DLL excludes the patcher tree and the proxy (engine/gpu builds its own target below).
    file(GLOB_RECURSE WXL_CORE_SRC CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

    # models/m2, models/m3, textures/blp and textures/dds are each owned by exactly one extension target
    # (wxl-m2, wxl-modern-m3, wxl-modern-blp -- added to their own target below); none of them has a
    # consumer in the plain DLL, so excluding them here is what keeps that true instead of shipping
    # four unused cross-target .cpp files inside WarcraftXL.dll by glob accident.
    list(FILTER WXL_CORE_SRC EXCLUDE REGEX
         "/src/patcher/|/src/host/|/src/probe/|/src/engine/gpu/|/src/engine/assets/shared/(models/m2|models/m3|textures/blp|textures/dds)/")

    # =========================================================================
    # Target 1: WarcraftXL.dll  (32-bit, injected) = engine + features + assets pipelines
    #   The modern-assets DLL + cross-target-shared halves now live in src/engine/assets/ and are caught
    #   by the src/*.cpp glob above.
    # =========================================================================
    # Dear ImGui: the in-game tuning panels (engine/ui). Listed explicitly rather than globbed -- only
    # the core plus the DX9 and Win32 backends are vendored, and the demo window is deliberately not.
    set(WXL_IMGUI
        "${CMAKE_CURRENT_SOURCE_DIR}/deps/imgui/imgui.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/deps/imgui/imgui_draw.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/deps/imgui/imgui_tables.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/deps/imgui/imgui_widgets.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/deps/imgui/backends/imgui_impl_dx9.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/deps/imgui/backends/imgui_impl_win32.cpp")

    add_library(WarcraftXL SHARED ${WXL_CORE_SRC} ${WXL_IMGUI} ${WXL_TLSF_SRC})
    set_target_properties(WarcraftXL PROPERTIES OUTPUT_NAME "WarcraftXL" PREFIX "")
    target_include_directories(WarcraftXL PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/include"   # the extension ABI, shared verbatim with extensions
        "${CMAKE_CURRENT_SOURCE_DIR}/deps/minhook/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/deps/imgui"
        "${CMAKE_CURRENT_SOURCE_DIR}/deps/imgui/backends"
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tlsf")

    add_subdirectory(deps/minhook)
    target_link_libraries(WarcraftXL PRIVATE minhook d3d9 shell32)
    target_compile_definitions(WarcraftXL PRIVATE ${WXL_DEFS})
    if(WXL_TRACY)
        target_sources(WarcraftXL PRIVATE ${WXL_TRACY_SRC})
        target_include_directories(WarcraftXL PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tracy/public")
        target_compile_definitions(WarcraftXL PRIVATE TRACY_ENABLE TRACY_ON_DEMAND)
        target_link_libraries(WarcraftXL PRIVATE ws2_32 dbghelp)
    endif()

    source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${WXL_CORE_SRC})

    # Deploy: copy the freshly linked DLL into the client folder.
    if(CLIENT_PATH)
        add_custom_command(TARGET WarcraftXL POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:WarcraftXL>" "${CLIENT_PATH}/WarcraftXL.dll"
            COMMENT "Deploy WarcraftXL.dll -> ${CLIENT_PATH}")
    endif()

    # =========================================================================
    # Target 2: wxl-patcher.exe  = the offline PE patcher (incl. the ex-wxl-no-gluexml example under
    #   src/patcher/, now caught by the src/patcher glob below).
    # =========================================================================
    file(GLOB_RECURSE WXL_PATCHER_CORE CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/patcher/*.cpp")

    # src/common/ compiles into every binary (leveled logger, config, shared helpers): the patcher and
    # proxy do not link the engine, so the shared infrastructure is object-level, not a link dependency.
    file(GLOB WXL_COMMON_SRC CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/common/*.cpp")

    add_executable(wxl-patcher ${WXL_PATCHER_CORE} ${WXL_COMMON_SRC})
    set_target_properties(wxl-patcher PROPERTIES OUTPUT_NAME "wxl-patcher")
    target_include_directories(wxl-patcher PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_definitions(wxl-patcher PRIVATE ${WXL_DEFS})

    # wxl-host-probe.exe and wxl-gpu-probe.exe: development checks of wxl-host from a 32-bit process and of the
    # proxy's managed-pool emulation. Never deployed.
    add_executable(wxl-host-probe "${CMAKE_CURRENT_SOURCE_DIR}/src/probe/ProbeMain.cpp"
                   "${CMAKE_CURRENT_SOURCE_DIR}/src/probe/TextureProbe.cpp"
                   "${CMAKE_CURRENT_SOURCE_DIR}/src/host/Blp.cpp"
                   "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/host/HostClient.cpp" ${WXL_COMMON_SRC} ${WXL_TLSF_SRC})
    target_include_directories(wxl-host-probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
                                                      "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tlsf")
    target_compile_definitions(wxl-host-probe PRIVATE ${WXL_DEFS})
    # The texture check maps Wow.exe's image at its own base (0x400000) to run the client's decoder: the probe
    # itself sits well above it.
    target_link_options(wxl-host-probe PRIVATE "/BASE:0x62000000" "/DYNAMICBASE:NO")

    add_executable(wxl-gpu-probe "${CMAKE_CURRENT_SOURCE_DIR}/src/probe/GpuProbeMain.cpp")
    target_include_directories(wxl-gpu-probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_definitions(wxl-gpu-probe PRIVATE ${WXL_DEFS})

    # =========================================================================
    # Target 3: d3d9.dll  = the native proxy (system-d3d9 passthrough + runtime loader)
    # =========================================================================
    file(GLOB WXL_GPU_SRC CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/engine/gpu/*.cpp")

    add_library(d3d9 SHARED ${WXL_GPU_SRC} ${WXL_COMMON_SRC})
    set_target_properties(d3d9 PROPERTIES OUTPUT_NAME "d3d9" PREFIX "")
    # include/ for wxl/HostApi.h: the managed-pool emulation reaches wxl-host through WarcraftXL.dll.
    target_include_directories(d3d9 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/include")
    # The proxy shadows a static import of Wow.exe, so a hard dependency of the proxy is a hard
    # dependency of the game. It resolves the system d3d9 at runtime and links nothing beyond the CRT.
    target_link_options(d3d9 PRIVATE "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/src/engine/gpu/Proxy.def")
    target_compile_definitions(d3d9 PRIVATE ${WXL_DEFS})

    # Deploy: copy the freshly linked proxy into the client folder after every build.
    if(CLIENT_PATH)
        add_custom_command(TARGET d3d9 POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:d3d9>" "${CLIENT_PATH}/d3d9.dll"
            COMMENT "Deploy d3d9.dll -> ${CLIENT_PATH}")
    endif()

    # =========================================================================
    # Target 4+: one DLL per folder under extensions/, deployed to Extensions/<Name>/<Name>.dll and
    #   discovered by the core at runtime. They link nothing of the core: the SDK is header-only and
    #   compiles in, and the rest arrives through the API table.
    # =========================================================================
    # The SDK's out-of-line half. Everything else under src/game is inline, but the gx facade creates
    # D3D resources and tracks them for the device reset, so each binary compiles and owns its own --
    # an extension releases its targets from OnDeviceLost rather than through the core's sweep.
    file(GLOB WXL_SDK_SRC CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/game/*.cpp")

    file(GLOB WXL_EXTENSION_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/extensions/*")
    foreach(wxl_ext_dir IN LISTS WXL_EXTENSION_DIRS)
        if(NOT IS_DIRECTORY "${wxl_ext_dir}")
            continue()
        endif()
        get_filename_component(wxl_ext_name "${wxl_ext_dir}" NAME)
        file(GLOB_RECURSE WXL_EXT_SRC CONFIGURE_DEPENDS "${wxl_ext_dir}/*.cpp")
        if(NOT WXL_EXT_SRC)
            continue()
        endif()

        # An extension that needs cross-target-shared source (src/engine/assets/shared/...) declares it
        # in its own extensions/<name>/shared.cmake (populates WXL_EXT_SHARED_SRC) instead of this loop
        # hardcoding extension names -- drop a new extension in, give it a shared.cmake if it needs
        # one, and it's picked up with no edit here. Reset before each include: file(GLOB) in the
        # included script would otherwise accumulate the previous iteration's stale value.
        # wxl.json "deploy": { "mode": "none" } builds the extension but deploys nothing: CLIENT_PATH is
        # hidden from its shared.cmake and its DLL copy, then restored for the next one.
        set(wxl_ext_client "${CLIENT_PATH}")
        if(EXISTS "${wxl_ext_dir}/wxl.json")
            file(READ "${wxl_ext_dir}/wxl.json" wxl_ext_manifest)
            string(JSON wxl_ext_deploy_mode ERROR_VARIABLE wxl_ext_json_error GET "${wxl_ext_manifest}" deploy mode)
            if(wxl_ext_deploy_mode STREQUAL "none")
                set(CLIENT_PATH "")
                message(STATUS "${wxl_ext_name}: built, not deployed (wxl.json deploy mode none)")
            endif()
        endif()

        set(WXL_EXT_SHARED_SRC "")
        include("${wxl_ext_dir}/shared.cmake" OPTIONAL)
        list(APPEND WXL_EXT_SRC ${WXL_EXT_SHARED_SRC})

        add_library(${wxl_ext_name} SHARED ${WXL_EXT_SRC} ${WXL_SDK_SRC})
        set_target_properties(${wxl_ext_name} PROPERTIES OUTPUT_NAME "${wxl_ext_name}" PREFIX "")
        # deps/vulkan-headers: the official Vulkan headers, for the services that expose DXVK's Vulkan
        # device (include/wxl/GraphicsVulkanApi.h). Headers only: nothing links vulkan-1.
        target_include_directories(${wxl_ext_name} PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
            "${CMAKE_CURRENT_SOURCE_DIR}/deps/vulkan-headers/include")
        # WXL_EXTENSION is what makes PluginApi.h declare the two entry points as exports. It belongs on
        # the target rather than in a source file: an extension that forgets it still builds, and fails
        # only at load with "exports no WXL_Query/WXL_Load".
        target_compile_definitions(${wxl_ext_name} PRIVATE ${WXL_DEFS} WXL_EXTENSION)

        if(CLIENT_PATH)
            add_custom_command(TARGET ${wxl_ext_name} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "${CLIENT_PATH}/Extensions/${wxl_ext_name}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${wxl_ext_name}>"
                        "${CLIENT_PATH}/Extensions/${wxl_ext_name}/${wxl_ext_name}.dll"
                COMMENT "Deploy ${wxl_ext_name}.dll -> ${CLIENT_PATH}/Extensions/${wxl_ext_name}")
        endif()
        set(CLIENT_PATH "${wxl_ext_client}")
    endforeach()
endif()

# =============================================================================
# 64-bit family: wxl-host.exe, the companion process (src/host + src/ipc + src/common)
# =============================================================================
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    # StormLib declares an old minimum version: have it honour the static CRT chosen above.
    set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
    set(STORM_USE_BUNDLED_LIBRARIES ON CACHE BOOL "" FORCE)
    set(STORM_SKIP_INSTALL ON CACHE BOOL "" FORCE)
    set(STORM_UNICODE ON CACHE BOOL "" FORCE)
    set(STORM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    add_subdirectory(deps/stormlib)

    # LZ4, zstd (compression, decompression) and enkiTS, compiled into the host only.
    file(GLOB WXL_ZSTD_SRC CONFIGURE_DEPENDS
         "${CMAKE_CURRENT_SOURCE_DIR}/third_party/zstd/lib/common/*.c"
         "${CMAKE_CURRENT_SOURCE_DIR}/third_party/zstd/lib/compress/*.c"
         "${CMAKE_CURRENT_SOURCE_DIR}/third_party/zstd/lib/decompress/*.c")
    add_library(wxl_zstd STATIC ${WXL_ZSTD_SRC})
    target_include_directories(wxl_zstd PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/zstd/lib")
    # The assembly Huffman decoder targets GCC and Clang; MSVC takes the C one.
    target_compile_definitions(wxl_zstd PRIVATE ZSTD_DISABLE_ASM ZSTD_MULTITHREAD=0)

    add_library(wxl_lz4 STATIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lz4/lz4.c"
                               "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lz4/lz4hc.c")
    target_include_directories(wxl_lz4 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lz4")

    add_library(wxl_enkits STATIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/enkiTS/TaskScheduler.cpp")
    target_include_directories(wxl_enkits PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/enkiTS")

    file(GLOB_RECURSE WXL_HOST_SRC CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/host/*.cpp")
    file(GLOB WXL_HOST_COMMON_SRC CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/common/*.cpp")

    add_executable(wxl-host ${WXL_HOST_SRC} ${WXL_HOST_COMMON_SRC} ${WXL_TLSF_SRC})
    set_target_properties(wxl-host PROPERTIES OUTPUT_NAME "wxl-host")
    target_include_directories(wxl-host PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tlsf")
    # UNICODE matches the STORM_UNICODE build of StormLib, so its TCHAR paths are wide on both sides.
    target_compile_definitions(wxl-host PRIVATE ${WXL_DEFS} UNICODE _UNICODE)
    target_link_libraries(wxl-host PRIVATE storm wxl_zstd wxl_lz4 wxl_enkits psapi)
    if(WXL_TRACY)
        target_sources(wxl-host PRIVATE ${WXL_TRACY_SRC})
        target_include_directories(wxl-host PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tracy/public")
        target_compile_definitions(wxl-host PRIVATE TRACY_ENABLE TRACY_ON_DEMAND)
        target_link_libraries(wxl-host PRIVATE ws2_32 dbghelp)
    endif()

    if(CLIENT_PATH)
        add_custom_command(TARGET wxl-host POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:wxl-host>" "${CLIENT_PATH}/wxl-host.exe"
            COMMENT "Deploy wxl-host.exe -> ${CLIENT_PATH}")
    endif()
endif()
