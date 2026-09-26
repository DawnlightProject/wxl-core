# wxl-graphics-fog's shader build, declared to the root extension loop (included with OPTIONAL,
# wxl_ext_dir set). The compute shaders (shaders/*.cs.hlsl) are compiled offline by DXC into SPIR-V and
# committed as src/gpu/Spirv.gen.cpp with the D3D9 composite's source, so a build needs no shader
# compiler. When DXC is found -- the WXL_DXC cache entry, then deps/dxc (the official release, see
# deps/README), the Vulkan SDK, PATH -- the table is regenerated whenever a shader changes, and
# rewritten only when its content differs.

set(WXL_FOG_DIR "${wxl_ext_dir}")
set(WXL_FOG_GEN "${WXL_FOG_DIR}/src/gpu/Spirv.gen.cpp")

set(WXL_DXC "" CACHE FILEPATH
    "DirectX Shader Compiler (dxc) that rebuilds SPIR-V; empty: deps/dxc, the Vulkan SDK's, then PATH")
set(WXL_FOG_DXC "${WXL_DXC}")
if(NOT WXL_FOG_DXC AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/deps/dxc/bin/x64/dxc.exe")
    set(WXL_FOG_DXC "${CMAKE_CURRENT_SOURCE_DIR}/deps/dxc/bin/x64/dxc.exe")
endif()
if(NOT WXL_FOG_DXC AND DEFINED ENV{VULKAN_SDK} AND EXISTS "$ENV{VULKAN_SDK}/Bin/dxc.exe")
    set(WXL_FOG_DXC "$ENV{VULKAN_SDK}/Bin/dxc.exe")
endif()
if(NOT WXL_FOG_DXC)
    find_program(WXL_FOG_DXC_ON_PATH dxc)
    if(WXL_FOG_DXC_ON_PATH)
        set(WXL_FOG_DXC "${WXL_FOG_DXC_ON_PATH}")
    endif()
endif()

if(WXL_FOG_DXC)
    # The Windows SDK's dxc has no SPIR-V code generation: a compiler that cannot emit it is ignored.
    set(WXL_FOG_PROBE "${CMAKE_CURRENT_BINARY_DIR}/wxl-graphics-fog")
    file(MAKE_DIRECTORY "${WXL_FOG_PROBE}")
    file(WRITE "${WXL_FOG_PROBE}/probe.hlsl"
         "RWTexture2D<float4> o;\n[numthreads(1, 1, 1)] void main(uint3 i : SV_DispatchThreadID) { o[i.xy] = 1; }\n")
    execute_process(
        COMMAND "${WXL_FOG_DXC}" -spirv -T cs_6_0 -E main "${WXL_FOG_PROBE}/probe.hlsl" -Fo "${WXL_FOG_PROBE}/probe.spv"
        RESULT_VARIABLE WXL_FOG_PROBE_RC OUTPUT_QUIET ERROR_QUIET)
    if(NOT WXL_FOG_PROBE_RC EQUAL 0)
        message(STATUS "wxl-graphics-fog: ${WXL_FOG_DXC} cannot emit SPIR-V; using the committed src/gpu/Spirv.gen.cpp")
        set(WXL_FOG_DXC "")
    endif()
endif()

if(WXL_FOG_DXC)
    file(GLOB_RECURSE WXL_FOG_SHADERS CONFIGURE_DEPENDS
         "${WXL_FOG_DIR}/shaders/*.hlsl" "${WXL_FOG_DIR}/shaders/*.hlsli" "${WXL_FOG_DIR}/shaders/*.h")
    set(WXL_FOG_OUT "${CMAKE_CURRENT_BINARY_DIR}/wxl-graphics-fog")
    # The stamp is the command's output, not the .cpp: an unchanged table is left untouched (no
    # rebuild of its object), and the command runs again only when a shader changed.
    set(WXL_FOG_STAMP "${WXL_FOG_OUT}/Spirv.stamp")
    add_custom_command(
        OUTPUT "${WXL_FOG_STAMP}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${WXL_FOG_OUT}"
        COMMAND ${CMAKE_COMMAND} "-DDXC=${WXL_FOG_DXC}" "-DSRC=${WXL_FOG_DIR}/shaders" "-DOUT=${WXL_FOG_GEN}"
                "-DWORK=${WXL_FOG_OUT}" -P "${WXL_FOG_DIR}/cmake/CompileShaders.cmake"
        COMMAND ${CMAKE_COMMAND} -E touch "${WXL_FOG_STAMP}"
        DEPENDS ${WXL_FOG_SHADERS} "${WXL_FOG_DIR}/cmake/CompileShaders.cmake"
        COMMENT "wxl-graphics-fog: compile the compute shaders to SPIR-V"
        VERBATIM)
    list(APPEND WXL_EXT_SHARED_SRC "${WXL_FOG_STAMP}")
    message(STATUS "wxl-graphics-fog: SPIR-V regenerated from shaders/ by ${WXL_FOG_DXC}")
else()
    message(STATUS "wxl-graphics-fog: no dxc (WXL_DXC, deps/dxc, VULKAN_SDK, PATH); using the committed src/gpu/Spirv.gen.cpp")
endif()

# The example settings are deployed beside the DLL (the user's own .cfg is never touched).
if(CLIENT_PATH)
    set(WXL_FOG_EXAMPLE "${CLIENT_PATH}/Extensions/wxl-graphics-fog/wxl-graphics-fog.cfg.example")
    add_custom_command(
        OUTPUT "${WXL_FOG_EXAMPLE}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CLIENT_PATH}/Extensions/wxl-graphics-fog"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${WXL_FOG_DIR}/wxl-graphics-fog.cfg.example" "${WXL_FOG_EXAMPLE}"
        DEPENDS "${WXL_FOG_DIR}/wxl-graphics-fog.cfg.example"
        COMMENT "wxl-graphics-fog: deploy wxl-graphics-fog.cfg.example"
        VERBATIM)
    list(APPEND WXL_EXT_SHARED_SRC "${WXL_FOG_EXAMPLE}")
endif()
