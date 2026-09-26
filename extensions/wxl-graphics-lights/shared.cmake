# wxl-graphics-lights' data and shader build, declared to the root extension loop (included with OPTIONAL,
# wxl_ext_dir set).
#
# The model table is deployed loose beside the DLL, where src/lights/ModelTable.hpp reads it
# (Extensions/wxl-graphics-lights/model-lights.csv), with the example settings.
#
# The compute shaders (shaders/compute/*.cs.hlsl) are compiled offline by DXC into SPIR-V and committed
# as src/gpu/Shaders.gen.cpp with the D3D9 shaders' sources, so a build needs no shader compiler. When
# DXC is found -- the WXL_DXC cache entry, then deps/dxc, the Vulkan SDK, PATH -- the table is
# regenerated whenever a shader changes, and rewritten only when its content differs. The field's
# point-shadow variant compiles against wxl-graphics-shadow's shaders/wxl/shadow/shadow.hlsli when it
# is there.

set(WXL_GL_DIR "${wxl_ext_dir}")
set(WXL_GL_GEN "${WXL_GL_DIR}/src/gpu/Shaders.gen.cpp")
set(WXL_GL_GFX "${CMAKE_CURRENT_SOURCE_DIR}/extensions/wxl-graphics-extend/shaders")
set(WXL_GL_SHADOW "${CMAKE_CURRENT_SOURCE_DIR}/extensions/wxl-graphics-shadow/shaders")

if(CLIENT_PATH)
    set(WXL_GL_TABLE "${CLIENT_PATH}/Extensions/wxl-graphics-lights/model-lights.csv")
    add_custom_command(
        OUTPUT "${WXL_GL_TABLE}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CLIENT_PATH}/Extensions/wxl-graphics-lights"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${WXL_GL_DIR}/data/model-lights.csv" "${WXL_GL_TABLE}"
        DEPENDS "${WXL_GL_DIR}/data/model-lights.csv"
        COMMENT "wxl-graphics-lights: deploy model-lights.csv"
        VERBATIM)
    list(APPEND WXL_EXT_SHARED_SRC "${WXL_GL_TABLE}")

    # The example settings beside the DLL (the user's own .cfg is never touched).
    set(WXL_GL_EXAMPLE "${CLIENT_PATH}/Extensions/wxl-graphics-lights/wxl-graphics-lights.cfg.example")
    add_custom_command(
        OUTPUT "${WXL_GL_EXAMPLE}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CLIENT_PATH}/Extensions/wxl-graphics-lights"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${WXL_GL_DIR}/wxl-graphics-lights.cfg.example" "${WXL_GL_EXAMPLE}"
        DEPENDS "${WXL_GL_DIR}/wxl-graphics-lights.cfg.example"
        COMMENT "wxl-graphics-lights: deploy wxl-graphics-lights.cfg.example"
        VERBATIM)
    list(APPEND WXL_EXT_SHARED_SRC "${WXL_GL_EXAMPLE}")
endif()

set(WXL_DXC "" CACHE FILEPATH
    "DirectX Shader Compiler (dxc) that rebuilds SPIR-V; empty: deps/dxc, the Vulkan SDK's, then PATH")
set(WXL_GL_DXC "${WXL_DXC}")
if(NOT WXL_GL_DXC AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/deps/dxc/bin/x64/dxc.exe")
    set(WXL_GL_DXC "${CMAKE_CURRENT_SOURCE_DIR}/deps/dxc/bin/x64/dxc.exe")
endif()
if(NOT WXL_GL_DXC AND DEFINED ENV{VULKAN_SDK} AND EXISTS "$ENV{VULKAN_SDK}/Bin/dxc.exe")
    set(WXL_GL_DXC "$ENV{VULKAN_SDK}/Bin/dxc.exe")
endif()
if(NOT WXL_GL_DXC)
    find_program(WXL_GL_DXC_ON_PATH dxc)
    if(WXL_GL_DXC_ON_PATH)
        set(WXL_GL_DXC "${WXL_GL_DXC_ON_PATH}")
    endif()
endif()

if(WXL_GL_DXC)
    # The Windows SDK's dxc has no SPIR-V code generation: a compiler that cannot emit it is ignored.
    set(WXL_GL_PROBE "${CMAKE_CURRENT_BINARY_DIR}/wxl-graphics-lights")
    file(MAKE_DIRECTORY "${WXL_GL_PROBE}")
    file(WRITE "${WXL_GL_PROBE}/probe.hlsl"
         "RWTexture2D<float4> o;\n[numthreads(1, 1, 1)] void main(uint3 i : SV_DispatchThreadID) { o[i.xy] = 1; }\n")
    execute_process(
        COMMAND "${WXL_GL_DXC}" -spirv -T cs_6_0 -E main "${WXL_GL_PROBE}/probe.hlsl" -Fo "${WXL_GL_PROBE}/probe.spv"
        RESULT_VARIABLE WXL_GL_PROBE_RC OUTPUT_QUIET ERROR_QUIET)
    if(NOT WXL_GL_PROBE_RC EQUAL 0)
        message(STATUS "wxl-graphics-lights: ${WXL_GL_DXC} cannot emit SPIR-V; using the committed src/gpu/Shaders.gen.cpp")
        set(WXL_GL_DXC "")
    endif()
endif()

if(WXL_GL_DXC)
    file(GLOB_RECURSE WXL_GL_SHADERS CONFIGURE_DEPENDS
         "${WXL_GL_DIR}/shaders/*.hlsl" "${WXL_GL_DIR}/shaders/*.hlsli" "${WXL_GL_DIR}/shaders/*.h"
         "${WXL_GL_SHADOW}/wxl/shadow/*.hlsli")
    set(WXL_GL_OUT "${CMAKE_CURRENT_BINARY_DIR}/wxl-graphics-lights")
    # The stamp is the command's output, not the .cpp: an unchanged table is left untouched (no rebuild
    # of its object), and the command runs again only when a shader changed.
    set(WXL_GL_STAMP "${WXL_GL_OUT}/Shaders.stamp")
    add_custom_command(
        OUTPUT "${WXL_GL_STAMP}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${WXL_GL_OUT}"
        COMMAND ${CMAKE_COMMAND} "-DDXC=${WXL_GL_DXC}" "-DSRC=${WXL_GL_DIR}/shaders" "-DOUT=${WXL_GL_GEN}"
                "-DWORK=${WXL_GL_OUT}" "-DGFX=${WXL_GL_GFX}" "-DSHADOW=${WXL_GL_SHADOW}"
                -P "${WXL_GL_DIR}/cmake/CompileShaders.cmake"
        COMMAND ${CMAKE_COMMAND} -E touch "${WXL_GL_STAMP}"
        DEPENDS ${WXL_GL_SHADERS} "${WXL_GL_DIR}/cmake/CompileShaders.cmake"
        COMMENT "wxl-graphics-lights: compile the compute shaders to SPIR-V"
        VERBATIM)
    list(APPEND WXL_EXT_SHARED_SRC "${WXL_GL_STAMP}")
    message(STATUS "wxl-graphics-lights: SPIR-V regenerated from shaders/ by ${WXL_GL_DXC}")
else()
    message(STATUS "wxl-graphics-lights: no dxc (WXL_DXC, deps/dxc, VULKAN_SDK, PATH); using the committed src/gpu/Shaders.gen.cpp")
endif()
