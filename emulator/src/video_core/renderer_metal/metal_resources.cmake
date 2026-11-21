# SPDX-License-Identifier: GPL-2.0-or-later
# Included explicitly by the private Metal renderer build.
find_program(CARA_XCRUN xcrun REQUIRED)
if (CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(CARA_PICA_METAL_SDK iphoneos)
else()
    set(CARA_PICA_METAL_SDK macosx)
endif()

set(CARA_PICA_METAL_SOURCES
    pica_depth_transfer
    pica_fused_depth_export
    pica_fused_depth_import
    pica_metal_lcd
    pica_metal_mixed_alias
    pica_metal_surface_import
    pica_metal_transfers
    pica_texture_decode
    pica_tiled_export)
set(CARA_PICA_METAL_OBJECTS)
foreach(shader IN LISTS CARA_PICA_METAL_SOURCES)
    set(source "${CMAKE_CURRENT_LIST_DIR}/${shader}.metal")
    set(object "${CMAKE_CURRENT_BINARY_DIR}/${shader}.air")
    set(dependencies "${source}")
    if (shader MATCHES "^pica_(depth_transfer|fused_depth_)")
        list(APPEND dependencies "${CMAKE_CURRENT_LIST_DIR}/depth_conversion.h")
    endif()
    add_custom_command(OUTPUT "${object}"
        COMMAND "${CARA_XCRUN}" --sdk "${CARA_PICA_METAL_SDK}" metal -c "${source}" -o "${object}"
        DEPENDS ${dependencies}
        VERBATIM)
    list(APPEND CARA_PICA_METAL_OBJECTS "${object}")
endforeach()
set(CARA_PICA_METAL_LIBRARY "${CMAKE_CURRENT_BINARY_DIR}/cara-pica.metallib")
add_custom_command(OUTPUT "${CARA_PICA_METAL_LIBRARY}"
    COMMAND "${CARA_XCRUN}" --sdk "${CARA_PICA_METAL_SDK}" metallib ${CARA_PICA_METAL_OBJECTS}
        -o "${CARA_PICA_METAL_LIBRARY}"
    DEPENDS ${CARA_PICA_METAL_OBJECTS}
    VERBATIM)
set(CARA_PICA_SCALED_CROP_AIR "${CMAKE_CURRENT_BINARY_DIR}/pica_metal_scaled_crop_rgb8.air")
set(CARA_PICA_SCALED_CROP_LIBRARY "${CMAKE_CURRENT_BINARY_DIR}/cara-scaled-crop-rgb8.metallib")
set(scaled_crop_source "${CMAKE_CURRENT_LIST_DIR}/pica_metal_scaled_crop_rgb8.metal")
add_custom_command(OUTPUT "${CARA_PICA_SCALED_CROP_AIR}"
    COMMAND "${CARA_XCRUN}" --sdk "${CARA_PICA_METAL_SDK}" metal -c
        "${scaled_crop_source}" -o "${CARA_PICA_SCALED_CROP_AIR}"
    DEPENDS "${scaled_crop_source}"
    VERBATIM)
add_custom_command(OUTPUT "${CARA_PICA_SCALED_CROP_LIBRARY}"
    COMMAND "${CARA_XCRUN}" --sdk "${CARA_PICA_METAL_SDK}" metallib
        "${CARA_PICA_SCALED_CROP_AIR}" -o "${CARA_PICA_SCALED_CROP_LIBRARY}"
    DEPENDS "${CARA_PICA_SCALED_CROP_AIR}"
    VERBATIM)
add_custom_target(cara-pica-metallib
    DEPENDS "${CARA_PICA_METAL_LIBRARY}" "${CARA_PICA_SCALED_CROP_LIBRARY}")
