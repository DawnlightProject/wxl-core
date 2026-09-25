# wxl-graphics-extend's built-in HLSL library (shaders/wxl/gfx), declared to the root extension
# loop (included with OPTIONAL, wxl_ext_dir set). The files are embedded in the DLL as the table
# wxl::gfx::shaders::kEmbeddedFiles by cmake/EmbedFiles.cmake and served to every consumer's
# #include "wxl/..." at runtime. Nothing is compiled offline: consumers compile through the
# service, which caches the bytecode in memory and on disk, so no fxc or Python is needed here.

set(WXL_GFX_DIR "${wxl_ext_dir}")
set(WXL_GFX_OUT "${CMAKE_CURRENT_BINARY_DIR}/wxl-graphics-extend")
file(GLOB_RECURSE WXL_GFX_FILES CONFIGURE_DEPENDS
     "${WXL_GFX_DIR}/shaders/wxl/*.hlsl" "${WXL_GFX_DIR}/shaders/wxl/*.hlsli")

set(WXL_GFX_EMBED "${WXL_GFX_OUT}/EmbeddedFiles.gen.cpp")
add_custom_command(
    OUTPUT "${WXL_GFX_EMBED}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${WXL_GFX_OUT}"
    COMMAND ${CMAKE_COMMAND} "-DROOT=${WXL_GFX_DIR}/shaders" "-DOUT=${WXL_GFX_EMBED}"
            -P "${WXL_GFX_DIR}/cmake/EmbedFiles.cmake"
    DEPENDS ${WXL_GFX_FILES} "${WXL_GFX_DIR}/cmake/EmbedFiles.cmake"
    COMMENT "wxl-graphics-extend: embed the shader library"
    VERBATIM)
list(APPEND WXL_EXT_SHARED_SRC "${WXL_GFX_EMBED}")
