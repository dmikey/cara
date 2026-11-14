add_library(video_core STATIC
    command_processor.cpp
    command_processor.h
    debug_utils/debug_utils.cpp
    debug_utils/debug_utils.h
    geometry_pipeline.cpp
    geometry_pipeline.h
    gpu_debugger.h
    pica.cpp
    pica.h
    pica_state.h
    pica_types.h
    primitive_assembly.cpp
    primitive_assembly.h
    rasterizer_interface.h
    regs.cpp
    regs.h
    regs_framebuffer.h
    regs_lighting.h
    regs_pipeline.h
    regs_rasterizer.h
    regs_shader.h
    regs_texturing.h
    renderer_software.cpp
    renderer_software.h
    renderer_base.cpp
    renderer_base.h
    renderer_opengl/gl_ios_compat.cpp
    renderer_opengl/gl_ios_compat.h
    renderer_opengl/gl_rasterizer.cpp
    renderer_opengl/gl_rasterizer.h
    renderer_opengl/gl_rasterizer_cache.cpp
    renderer_opengl/gl_surface_coherence_self_test.cpp
    renderer_opengl/gl_surface_remap.cpp
    renderer_opengl/gl_surface_remap.h
    renderer_opengl/gl_rasterizer_cache.h
    renderer_opengl/gl_resource_manager.cpp
    renderer_opengl/gl_resource_manager.h
    renderer_opengl/gl_shader_decompiler.cpp
    renderer_opengl/gl_shader_decompiler.h
    renderer_opengl/gl_shader_gen.cpp
    renderer_opengl/gl_shader_gen.h
    renderer_opengl/gl_shader_manager.cpp
    renderer_opengl/gl_shader_manager.h
    renderer_opengl/gl_shader_util.cpp
    renderer_opengl/gl_shader_util.h
    renderer_opengl/gl_state.cpp
    renderer_opengl/gl_state.h
    renderer_opengl/gl_stream_buffer.cpp
    renderer_opengl/gl_stream_buffer.h
    renderer_opengl/gl_surface_params.cpp
    renderer_opengl/gl_surface_params.h
    renderer_opengl/gl_vars.cpp
    renderer_opengl/gl_vars.h
    renderer_opengl/pica_to_gl.h
    renderer_opengl/renderer_opengl.cpp
    renderer_opengl/renderer_opengl.h
    renderer_opengl/on_screen_display.cpp
    renderer_opengl/on_screen_display.h
    renderer_opengl/gl_format_reinterpreter.cpp
    renderer_opengl/gl_format_reinterpreter.h
    shader/debug_data.h
    shader/shader.cpp
    shader/shader.h
    shader/shader_interpreter.cpp
    shader/shader_interpreter.h
    swrasterizer/clipper.cpp
    swrasterizer/clipper.h
    swrasterizer/framebuffer.cpp
    swrasterizer/framebuffer.h
    swrasterizer/lighting.cpp
    swrasterizer/lighting.h
    swrasterizer/proctex.cpp
    swrasterizer/proctex.h
    swrasterizer/rasterizer.cpp
    swrasterizer/rasterizer.h
    swrasterizer/swrasterizer.cpp
    swrasterizer/swrasterizer.h
    swrasterizer/texturing.cpp
    swrasterizer/texturing.h
    texture/etc1.cpp
    texture/etc1.h
    texture/texture_decode.cpp
    texture/texture_decode.h
    utils.h
    vertex_loader.cpp
    vertex_loader.h
    video_core.cpp
    video_core.h
)

if(CARA_ENABLE_JIT AND ARCHITECTURE_x86_64)
    target_sources(video_core
        PRIVATE
            shader/shader_jit_x64.cpp
            shader/shader_jit_x64_compiler.cpp

            shader/shader_jit_x64.h
            shader/shader_jit_x64_compiler.h
    )
endif()

# Reuse the production GLES fixtures on Mali before selecting optional vertex paths.
if(CARA_ANDROID_GLES_EXPERIMENT)
    target_sources(video_core PRIVATE ../ios/gpu_state_self_test.cpp ../ios/gpu_vertex_self_test.cpp)
endif()

create_target_directory_groups(video_core)

target_link_libraries(video_core PUBLIC common core)
target_link_libraries(video_core PRIVATE glad nihstro-headers)

if (CARA_ENABLE_JIT AND ARCHITECTURE_x86_64)
    target_link_libraries(video_core PUBLIC xbyak)
endif()

# Cache ABI covers shader generation, binding/layout and GL source preparation. Reconfigure
# on edits so an iOS cache can never silently outlive a changed packed vertex layout.
file(GLOB CARA_SHADER_CACHE_INPUTS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/renderer_opengl/gl_shader*"
    "${CMAKE_CURRENT_SOURCE_DIR}/renderer_opengl/gl_packed_vertex.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/renderer_opengl/gl_rasterizer.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/renderer_opengl/gl_vars.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/regs*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/renderer_opengl/gl_vertex_input_layout.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/renderer_opengl/gl_resource_manager*"
    "${CMAKE_CURRENT_SOURCE_DIR}/renderer_opengl/ios_pipeline_cache.h")
set(CARA_SHADER_CACHE_SIGNATURE "cara-ios-pipeline-v1")
foreach(INPUT IN LISTS CARA_SHADER_CACHE_INPUTS)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${INPUT}")
    file(SHA256 "${INPUT}" INPUT_HASH)
    string(APPEND CARA_SHADER_CACHE_SIGNATURE "${INPUT_HASH}")
endforeach()
string(SHA256 CARA_SHADER_CACHE_SIGNATURE "${CARA_SHADER_CACHE_SIGNATURE}")
target_compile_definitions(video_core PRIVATE
    CARA_SHADER_CACHE_SIGNATURE="${CARA_SHADER_CACHE_SIGNATURE}")
