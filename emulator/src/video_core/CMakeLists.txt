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
    renderer_opengl/gl_software_batch_visibility_self_test.cpp
    renderer_opengl/gl_lut_effective_change_self_test.cpp
    renderer_opengl/gl_native_remap_self_test.cpp
    renderer_opengl/gl_native_required_copy_self_test.cpp
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
    renderer_opengl/gl_host_scissor.h
    renderer_opengl/gl_state.h
    renderer_opengl/gl_command_queue.cpp
    renderer_opengl/gl_pipeline_capture.h
    renderer_opengl/gl_host_latency.h
    renderer_opengl/gl_command_queue.h
    renderer_opengl/gl_driver_transactions.h
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

# The file-backed observers belong only to the explicit native atlas lab path.
# Other products retain unavailable/no-op observers and no journal mmap linkage.
if (CARA_MAC_ATLAS_PIPELINE)
    target_sources(video_core PRIVATE renderer_opengl/gl_pipeline_capture.cpp
        renderer_opengl/gl_host_latency.cpp)
else()
    target_sources(video_core PRIVATE renderer_opengl/gl_observer_stubs.cpp)
endif()

if(CARA_ENABLE_JIT AND ARCHITECTURE_x86_64)
    target_sources(video_core
        PRIVATE
            shader/shader_jit_x64.cpp
            shader/shader_jit_x64_compiler.cpp

            shader/shader_jit_x64.h
            shader/shader_jit_x64_compiler.h
    )
endif()

option(CARA_PRIVATE_METAL_TRANSFERS "Build Apple Metal transfers" OFF)
if (CARA_PRIVATE_METAL_TRANSFERS)
    if (NOT APPLE)
        message(FATAL_ERROR "CARA_PRIVATE_METAL_TRANSFERS requires an Apple target")
    endif()
    enable_language(OBJCXX)
    target_sources(video_core PRIVATE renderer_metal/pica_metal_transfers.mm)
    set_property(TARGET video_core PROPERTY OBJCXX_STANDARD 20)
    set_property(TARGET video_core PROPERTY OBJCXX_STANDARD_REQUIRED ON)
    target_compile_options(video_core PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
    target_compile_definitions(video_core PRIVATE CARA_PRIVATE_METAL_TRANSFERS=1)
    find_library(CARA_TRANSFER_METAL Metal REQUIRED)
    find_library(CARA_TRANSFER_FOUNDATION Foundation REQUIRED)
    target_link_libraries(video_core PRIVATE ${CARA_TRANSFER_METAL} ${CARA_TRANSFER_FOUNDATION})
endif()
option(CARA_PRIVATE_METAL_DRAWS "Build the native Metal renderer" OFF)
set(CARA_PRIVATE_GATE_METAL_CATALOG "" CACHE PATH
    "Private iPad Gate Metal catalog directory")
option(CARA_PRIVATE_MAC_METAL_HOST "Build the macOS Metal compatibility runner" OFF)
option(CARA_PRIVATE_IOS_METAL_LCD "Build the UIKit Metal host" OFF)
if (CARA_PRIVATE_IOS_METAL_LCD AND (NOT CMAKE_SYSTEM_NAME STREQUAL "iOS" OR
    NOT CARA_PRIVATE_METAL_DRAWS OR NOT CARA_PRIVATE_METAL_TRANSFERS OR
    NOT CARA_IOS_HARDWARE_RENDERER))
    message(FATAL_ERROR "The Metal LCD host requires the private physical iOS draw and transfer paths")
endif()
if (CARA_PRIVATE_IOS_METAL_LCD)
    target_compile_definitions(video_core PUBLIC CARA_PRIVATE_IOS_METAL_LCD=1)
endif()
if (CARA_PRIVATE_METAL_DRAWS)
    if (NOT APPLE OR NOT CARA_PRIVATE_METAL_TRANSFERS)
        message(FATAL_ERROR "Native Metal draws require Apple Metal transfers")
    endif()
    if (CMAKE_SYSTEM_NAME STREQUAL "iOS")
        if (NOT CARA_IOS_HARDWARE_RENDERER OR CMAKE_OSX_SYSROOT MATCHES "iphonesimulator|iPhoneSimulator")
            message(FATAL_ERROR "The private Metal draw bridge requires a physical iOS hardware-renderer build")
        endif()
        if (CARA_ENABLE_JIT OR CARA_MAC_CPU_JIT)
            message(FATAL_ERROR "The current private iOS hardware build is FastInterp-only")
        endif()
    endif()
    enable_language(OBJCXX)
    target_sources(video_core PRIVATE
        renderer_metal/pica_metal_draw.mm
        renderer_metal/batched_vertex_source.cpp
        renderer_metal/simd_vertex_source.cpp
        renderer_metal/pica_metal_resolution.mm
        renderer_metal/pica_metal_draw_group.mm
        renderer_metal/pica_metal_upload.mm
        renderer_metal/pica_metal_submission_queue.mm
        renderer_metal/pica_metal_guest_authority.cpp
        renderer_metal/pica_metal_native_draw_lane.mm
        renderer_metal/pica_metal_packed_inputs.cpp
        renderer_metal/pica_metal_pair_indices.mm
        renderer_metal/pica_metal_lut.cpp
        renderer_metal/pica_metal_lighting_cache.mm
        renderer_metal/pica_metal_pipelines.mm
        renderer_metal/pica_metal_resources.mm
        renderer_metal/pica_metal_mixed_alias.mm
        renderer_metal/pica_metal_lcd_native.mm
        renderer_metal/pica_metal_lcd_compose.mm
        renderer_metal/pica_metal_depth_import.mm
        renderer_metal/pica_metal_texture_decode.mm
        renderer_metal/pica_tiled_export.mm
        renderer_metal/exporter-adapter/pica_metal_visibility_export.mm)
    target_sources(video_core PRIVATE
        renderer_metal/pica_metal_native_bindings.mm
        renderer_metal/pica_metal_native_renderer.mm
        renderer_metal/pica_metal_native_uniforms.cpp)
    if (CARA_PRIVATE_GATE_METAL_CATALOG)
        if ((NOT CMAKE_SYSTEM_NAME STREQUAL "iOS" AND NOT CARA_PRIVATE_MAC_METAL_HOST) OR
            NOT EXISTS "${CARA_PRIVATE_GATE_METAL_CATALOG}/pica_metal_gate_catalog_data.inc" OR
            NOT EXISTS "${CARA_PRIVATE_GATE_METAL_CATALOG}/cara-gate.metallib")
            message(FATAL_ERROR "Private Gate catalog requires matching iPad metadata and library")
        endif()
        target_include_directories(video_core PRIVATE "${CARA_PRIVATE_GATE_METAL_CATALOG}")
        target_compile_definitions(video_core PRIVATE CARA_PRIVATE_GATE_METAL_CATALOG=1)
    endif()
    include(renderer_metal/runtime_shader_dependencies.cmake)
    cara_link_runtime_shader_libraries(video_core)
    include(renderer_metal/metal_resources.cmake)
    add_dependencies(video_core cara-pica-metallib)
    set_property(TARGET video_core PROPERTY CARA_PICA_METAL_LIBRARY "${CARA_PICA_METAL_LIBRARY}")
    set_property(TARGET video_core PROPERTY CARA_PICA_SCALED_CROP_LIBRARY
        "${CARA_PICA_SCALED_CROP_LIBRARY}")
    target_compile_definitions(video_core PUBLIC CARA_EXPERIMENTAL_METAL_DRAW_BRIDGE=1)
    set_target_properties(video_core PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON)
    target_compile_options(video_core PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
    find_library(CARA_DRAW_METAL Metal REQUIRED)
    find_library(CARA_DRAW_FOUNDATION Foundation REQUIRED)
    find_library(CARA_DRAW_QUARTZCORE QuartzCore REQUIRED)
    target_link_libraries(video_core PRIVATE
        ${CARA_DRAW_METAL} ${CARA_DRAW_FOUNDATION} ${CARA_DRAW_QUARTZCORE})
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
if (CARA_MAC_ATLAS_PIPELINE)
    string(APPEND CARA_SHADER_CACHE_SIGNATURE ";native-mac-atlas-gl41-v1")
endif()
foreach(INPUT IN LISTS CARA_SHADER_CACHE_INPUTS)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${INPUT}")
    file(SHA256 "${INPUT}" INPUT_HASH)
    string(APPEND CARA_SHADER_CACHE_SIGNATURE "${INPUT_HASH}")
endforeach()
string(SHA256 CARA_SHADER_CACHE_SIGNATURE "${CARA_SHADER_CACHE_SIGNATURE}")
target_compile_definitions(video_core PRIVATE
    CARA_SHADER_CACHE_SIGNATURE="${CARA_SHADER_CACHE_SIGNATURE}")
