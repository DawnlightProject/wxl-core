# wxl-forever's shaders, declared to the root extension loop (included with OPTIONAL).
# Every src/<feature>/shaders/<stem>.ps.hlsl and .vs.hlsl is compiled offline with fxc, wrapped in
# the client's BLS format (tools/bls.py) as "<feature>.<stem>.bls" and deployed to the client patch
# folder, mirroring the client's own layout:
#   Data/Patch-4.MPQ/Shaders/pixel/ps_3_0/Forever/<feature>.<stem>.bls
#   Data/Patch-4.MPQ/Shaders/vertex/vs_3_0/Forever/<feature>.<stem>.bls
# The sources are deployed too (Data/Patch-4.MPQ/Shaders/Forever/src, for WXL_FOREVER_SHADER_DEV) and
# embedded in the DLL, which compiles them at runtime when a bytecode file is missing.

set(WXL_FV_DIR "${wxl_ext_dir}")
set(WXL_FV_OUT "${CMAKE_CURRENT_BINARY_DIR}/wxl-forever-shaders")
file(GLOB_RECURSE WXL_FV_PROGRAMS CONFIGURE_DEPENDS
     "${WXL_FV_DIR}/src/*/shaders/*.ps.hlsl" "${WXL_FV_DIR}/src/*/shaders/*.vs.hlsl")
file(GLOB_RECURSE WXL_FV_FILES CONFIGURE_DEPENDS
     "${WXL_FV_DIR}/src/*/shaders/*.hlsl" "${WXL_FV_DIR}/src/*/shaders/*.hlsli")

# fxc from the newest Windows 10/11 SDK present.
file(GLOB WXL_FV_FXC_CANDIDATES "C:/Program Files (x86)/Windows Kits/10/bin/*/x64/fxc.exe")
set(WXL_FV_FXC "")
if(WXL_FV_FXC_CANDIDATES)
    list(SORT WXL_FV_FXC_CANDIDATES)
    list(REVERSE WXL_FV_FXC_CANDIDATES)
    list(GET WXL_FV_FXC_CANDIDATES 0 WXL_FV_FXC)
else()
    message(WARNING "wxl-forever: fxc not found; shaders fall back to runtime compilation of the embedded sources")
endif()

set(WXL_FV_PATCH "")
if(CLIENT_PATH)
    set(WXL_FV_PATCH "${CLIENT_PATH}/Data/Patch-4.MPQ")
endif()

find_program(WXL_FV_PYTHON NAMES python3 python py)
if(NOT WXL_FV_PYTHON)
    message(WARNING "wxl-forever: Python not found; shaders fall back to runtime compilation of the embedded sources")
endif()

if(WXL_FV_FXC AND WXL_FV_PYTHON)
    foreach(wxl_fv_program IN LISTS WXL_FV_PROGRAMS)
        get_filename_component(wxl_fv_file "${wxl_fv_program}" NAME)
        get_filename_component(wxl_fv_shaders_dir "${wxl_fv_program}" DIRECTORY)
        get_filename_component(wxl_fv_feature_dir "${wxl_fv_shaders_dir}" DIRECTORY)
        get_filename_component(wxl_fv_feature "${wxl_fv_feature_dir}" NAME)
        if(wxl_fv_file MATCHES "^(.+)\\.ps\\.hlsl$")
            set(wxl_fv_profile "ps_3_0")
            set(wxl_fv_stage "pixel")
        else()
            set(wxl_fv_profile "vs_3_0")
            set(wxl_fv_stage "vertex")
        endif()
        string(REGEX REPLACE "\\.(ps|vs)\\.hlsl$" "" wxl_fv_stem "${wxl_fv_file}")
        set(wxl_fv_name "${wxl_fv_feature}.${wxl_fv_stem}")
        set(wxl_fv_cso "${WXL_FV_OUT}/cso/${wxl_fv_name}.cso")
        set(wxl_fv_bls "${WXL_FV_OUT}/${wxl_fv_stage}/${wxl_fv_profile}/Forever/${wxl_fv_name}.bls")

        set(wxl_fv_deploy "")
        if(WXL_FV_PATCH)
            set(wxl_fv_deploy
                COMMAND ${CMAKE_COMMAND} -E make_directory "${WXL_FV_PATCH}/Shaders/${wxl_fv_stage}/${wxl_fv_profile}/Forever"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${wxl_fv_bls}"
                        "${WXL_FV_PATCH}/Shaders/${wxl_fv_stage}/${wxl_fv_profile}/Forever/${wxl_fv_name}.bls")
        endif()

        # fxc, then the client's BLS container around the bytecode (tools/bls.py).
        add_custom_command(
            OUTPUT "${wxl_fv_bls}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${WXL_FV_OUT}/cso"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${WXL_FV_OUT}/${wxl_fv_stage}/${wxl_fv_profile}/Forever"
            COMMAND "${WXL_FV_FXC}" /nologo /T ${wxl_fv_profile} /O3 /E main /I "${WXL_FV_DIR}/src"
                    /Fo "${wxl_fv_cso}" "${wxl_fv_program}"
            COMMAND "${WXL_FV_PYTHON}" "${WXL_FV_DIR}/tools/bls.py" write "${wxl_fv_bls}" "${wxl_fv_cso}"
            ${wxl_fv_deploy}
            DEPENDS ${WXL_FV_FILES} "${WXL_FV_DIR}/tools/bls.py"
            COMMENT "fxc ${wxl_fv_name} (${wxl_fv_profile}) -> BLS"
            VERBATIM)
        list(APPEND WXL_EXT_SHARED_SRC "${wxl_fv_bls}")
    endforeach()
endif()

# The embedded sources, and the deployed copy of them for the dev loop.
set(WXL_FV_EMBED "${WXL_FV_OUT}/EmbeddedShaders.gen.cpp")
add_custom_command(
    OUTPUT "${WXL_FV_EMBED}"
    COMMAND ${CMAKE_COMMAND} "-DROOT=${WXL_FV_DIR}/src" "-DOUT=${WXL_FV_EMBED}" "-DDEPLOY=${WXL_FV_PATCH}"
            -P "${WXL_FV_DIR}/cmake/EmbedShaders.cmake"
    DEPENDS ${WXL_FV_FILES} "${WXL_FV_DIR}/cmake/EmbedShaders.cmake"
    COMMENT "wxl-forever: embed shader sources"
    VERBATIM)
list(APPEND WXL_EXT_SHARED_SRC "${WXL_FV_EMBED}")

# The light service's model table, deployed beside the DLL (src/lights/ModelTable.hpp reads it).
if(CLIENT_PATH)
    set(WXL_FV_TABLE "${CLIENT_PATH}/Extensions/wxl-forever/model-lights.csv")
    add_custom_command(
        OUTPUT "${WXL_FV_TABLE}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CLIENT_PATH}/Extensions/wxl-forever"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${WXL_FV_DIR}/data/model-lights.csv" "${WXL_FV_TABLE}"
        DEPENDS "${WXL_FV_DIR}/data/model-lights.csv"
        COMMENT "wxl-forever: deploy model-lights.csv"
        VERBATIM)
    list(APPEND WXL_EXT_SHARED_SRC "${WXL_FV_TABLE}")
endif()
