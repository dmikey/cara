# Modified for Cara on 2026-11-12; see NOTICE for attribution.
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    enable_language(OBJCXX)
endif()

# Enable modules to include each other's files
include_directories(.)

# CMake seems to only define _DEBUG on Windows
set_property(DIRECTORY APPEND PROPERTY
    COMPILE_DEFINITIONS $<$<CONFIG:Debug>:_DEBUG> $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)

# Set compilation flags
if (MSVC)
    set(CMAKE_CONFIGURATION_TYPES Debug Release CACHE STRING "" FORCE)

    # Silence "deprecation" warnings
    add_definitions(-D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_DEPRECATE -D_SCL_SECURE_NO_WARNINGS)

    # Avoid windows.h junk
    add_definitions(-DNOMINMAX)

    # Avoid windows.h from including some usually unused libs like winsocks.h, since this might cause some redefinition errors.
    add_definitions(-DWIN32_LEAN_AND_MEAN)

    # Ensure that projects build with Unicode support.
    add_definitions(-DUNICODE -D_UNICODE)

    # /W3                 - Level 3 warnings
    # /MP                 - Multi-threaded compilation
    # /Zi                 - Output debugging information
    # /Zo                 - Enhanced debug info for optimized builds
    # /permissive-        - Enables stricter C++ standards conformance checks
    # /EHsc               - C++-only exception handling semantics
    # /volatile:iso       - Use strict standards-compliant volatile semantics.
    # /Zc:externConstexpr - Allow extern constexpr variables to have external linkage, like the standard mandates
    # /Zc:inline          - Let codegen omit inline functions in object files
    # /Zc:throwingNew     - Let codegen assume `operator new` (without std::nothrow) will never return null
    add_compile_options(
        /W3
        /MP
        /Zi
        /Zo
        /permissive-
        /EHsc
        /std:c++latest
        /volatile:iso
        /Zc:externConstexpr
        /Zc:inline
        /Zc:throwingNew
    )

    # /GS- - No stack buffer overflow checks
    add_compile_options("$<$<CONFIG:Release>:/GS->")

    set(CMAKE_EXE_LINKER_FLAGS_DEBUG   "/DEBUG /MANIFEST:NO" CACHE STRING "" FORCE)
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "/DEBUG /MANIFEST:NO /INCREMENTAL:NO /OPT:REF,ICF" CACHE STRING "" FORCE)
else()
    add_compile_options(
        -Wall
        -Wno-attributes
    )

    if (APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL Clang)
        add_compile_options("-stdlib=libc++")
    endif()

    # Set file offset size to 64 bits.
    #
    # On modern Unixes, this is typically already the case. The lone exception is
    # glibc, which may default to 32 bits. glibc allows this to be configured
    # by setting _FILE_OFFSET_BITS.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR MINGW)
        add_definitions(-D_FILE_OFFSET_BITS=64)
    endif()

    if (MINGW)
        add_definitions(-DMINGW_HAS_SECURE_API)
        if (COMPILE_WITH_DWARF)
            add_compile_options("-gdwarf")
        endif()

        if (MINGW_STATIC_BUILD)
            add_definitions(-DQT_STATICPLUGIN)
            add_compile_options("-static")
        endif()
    endif()

    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR MINGW)
        # GNU ar: Create thin archive files.
        # Requires binutils-2.19 or later.
        set(CMAKE_C_ARCHIVE_CREATE   "<CMAKE_AR> qcTP <TARGET> <LINK_FLAGS> <OBJECTS>")
        set(CMAKE_C_ARCHIVE_APPEND   "<CMAKE_AR> qTP  <TARGET> <LINK_FLAGS> <OBJECTS>")
        set(CMAKE_CXX_ARCHIVE_CREATE "<CMAKE_AR> qcTP <TARGET> <LINK_FLAGS> <OBJECTS>")
        set(CMAKE_CXX_ARCHIVE_APPEND "<CMAKE_AR> qTP  <TARGET> <LINK_FLAGS> <OBJECTS>")
    endif()
endif()

add_subdirectory(common)
add_subdirectory(core)
add_subdirectory(video_core)
add_subdirectory(audio_core)
add_subdirectory(network)
add_subdirectory(input_common)

if(NOT ANDROID AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    add_subdirectory(tests)
endif()

if (ENABLE_SDL2)
    add_subdirectory(cara)
endif()

if (ENABLE_QT)
    add_subdirectory(cara_qt)
endif()
if (ANDROID)
    add_subdirectory(android/jni)
elseif(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    add_subdirectory(dedicated_room)
endif()

if (ENABLE_WEB_SERVICE)
    add_subdirectory(web_service)
endif()

if (CARA_SOFTWARE_PRESENTATION AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    add_executable(cara-arm-tick-self-test arm_tick_self_test.cpp)
    target_include_directories(cara-arm-tick-self-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_executable(cara-texture-lookup-self-test texture_lookup_self_test.cpp)
    target_include_directories(cara-texture-lookup-self-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_executable(cara-render-bookkeeping-self-test render_bookkeeping_self_test.cpp)
    target_include_directories(cara-render-bookkeeping-self-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_executable(cara-vertex-upload-bank-self-test vertex_upload_bank_self_test.cpp)
    target_include_directories(cara-vertex-upload-bank-self-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_executable(cara-frame-pacer-self-test frame_pacer_self_test.cpp)
    target_include_directories(cara-frame-pacer-self-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_executable(cara-cpu-category-self-test cpu_category_self_test.cpp)
    target_include_directories(cara-cpu-category-self-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(cara-cpu-category-self-test PRIVATE Threads::Threads)
    add_executable(cara-clock-self-test clock_self_test.cpp)
    target_link_libraries(cara-clock-self-test PRIVATE core common ${PLATFORM_LIBRARIES} Threads::Threads)
    add_executable(cara-input-self-test input_self_test.cpp)
    add_executable(cara-audio-buffer-self-test audio_buffer_self_test.cpp)
    add_executable(cara-interval-cache-self-test interval_cache_self_test.cpp)
    target_link_libraries(cara-interval-cache-self-test PRIVATE Boost::boost)
    add_executable(cara-extdata-state-self-test extdata_state_self_test.cpp)
    target_link_libraries(cara-extdata-state-self-test PRIVATE core common ${PLATFORM_LIBRARIES} Threads::Threads)
    add_executable(cara-save-state-self-test save_state_self_test.cpp)
    target_link_libraries(cara-save-state-self-test PRIVATE core common nihstro-headers cryptopp libzstd_static ${PLATFORM_LIBRARIES} Threads::Threads)
    add_executable(cara-fastinterp-self-test fastinterp_self_test.cpp)
    target_link_libraries(cara-fastinterp-self-test PRIVATE core common ${PLATFORM_LIBRARIES} Threads::Threads)
    add_executable(cara-core-cache-self-test core_cache_self_test.cpp)
    target_link_libraries(cara-core-cache-self-test PRIVATE core common ${PLATFORM_LIBRARIES} Threads::Threads)
    add_executable(cara-raster-self-test raster_self_test.cpp)
    target_link_libraries(cara-raster-self-test PRIVATE core common nihstro-headers ${PLATFORM_LIBRARIES} Threads::Threads)
    add_executable(cara-no-jit-probe no_jit_probe.cpp)
    target_link_libraries(cara-no-jit-probe PRIVATE core common ${PLATFORM_LIBRARIES} Threads::Threads)
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "iOS")
    enable_language(OBJCXX)
    add_executable(cara-ios MACOSX_BUNDLE ios/main.mm ios/library_store.mm ios/shell_controllers.mm ios/input_hub.mm ios/state_catalog.mm ios/feature_controllers.mm ios/configuration_store.mm ios/save_sync.mm ios/skin_layout.mm ios/skin_controls.mm ios/skins/CarbonDuo.deltaskin)
    set_source_files_properties(ios/skins/CarbonDuo.deltaskin PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    file(GLOB CARA_MENU_SOUNDS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ios/MenuSounds/*.caf")
    target_sources(cara-ios PRIVATE ${CARA_MENU_SOUNDS} ios/MenuSounds/NOTICE)
    set_source_files_properties(${CARA_MENU_SOUNDS} ios/MenuSounds/NOTICE PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/MenuSounds")
    target_compile_options(cara-ios PRIVATE -fobjc-arc)
    if (CARA_IOS_HARDWARE_RENDERER)
        target_sources(cara-ios PRIVATE ios/gpu_vertex_self_test.cpp ios/gpu_depth_self_test.cpp ios/gpu_state_self_test.cpp)
        target_link_libraries(cara-ios PRIVATE nihstro-headers)
        target_link_libraries(cara-ios PRIVATE glad "-framework OpenGLES")
    endif()
    target_link_libraries(cara-ios PRIVATE core common Threads::Threads
        "-framework Foundation" "-lobjc" "-framework UIKit" "-framework CoreGraphics" "-framework UniformTypeIdentifiers" "-framework ImageIO" "-framework QuartzCore" "-framework GameController" "-framework AudioToolbox" z)
    set_target_properties(cara-ios PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/ios/Info.plist"
        XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED NO
        XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2")

    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    find_program(CARA_XCRUN xcrun REQUIRED)
    set(CARA_ICON_PLATFORM iphoneos)
    if (CMAKE_OSX_SYSROOT MATCHES "iphonesimulator|iPhoneSimulator")
        set(CARA_ICON_PLATFORM iphonesimulator)
    endif()
    set(CARA_ICON_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/cara-icons")
    set(CARA_ICON_INFO "${CMAKE_CURRENT_BINARY_DIR}/cara-icon-info.plist")
    set(CARA_ICON_STAMP "${CMAKE_CURRENT_BINARY_DIR}/cara-icons.stamp")
    file(GLOB_RECURSE CARA_ICON_INPUTS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/ios/Assets.xcassets/*")
    add_custom_command(OUTPUT "${CARA_ICON_STAMP}" "${CARA_ICON_INFO}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CARA_ICON_OUTPUT}"
        COMMAND "${CARA_XCRUN}" actool
            --compile "${CARA_ICON_OUTPUT}"
            --platform "${CARA_ICON_PLATFORM}"
            --minimum-deployment-target 15.0
            --app-icon AppIcon --target-device iphone --target-device ipad
            --output-partial-info-plist "${CARA_ICON_INFO}"
            --output-format human-readable-text
            "${CMAKE_CURRENT_SOURCE_DIR}/ios/Assets.xcassets"
        COMMAND ${CMAKE_COMMAND} -E touch "${CARA_ICON_STAMP}"
        DEPENDS ${CARA_ICON_INPUTS}
        VERBATIM)
    add_custom_target(cara-ios-icons DEPENDS "${CARA_ICON_STAMP}")
    add_dependencies(cara-ios cara-ios-icons)
    set_property(TARGET cara-ios APPEND PROPERTY LINK_DEPENDS "${CARA_ICON_STAMP}")
    add_custom_command(TARGET cara-ios POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CARA_ICON_OUTPUT}" "$<TARGET_BUNDLE_DIR:cara-ios>"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/../tools/merge_icon_plist.py"
            "$<TARGET_BUNDLE_DIR:cara-ios>/Info.plist" "${CARA_ICON_INFO}"
        VERBATIM)
endif()

if (APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    enable_language(OBJCXX)
    add_executable(cara-save-sync-self-test ios/save_sync_self_test.mm ios/save_sync.mm)
    target_compile_options(cara-save-sync-self-test PRIVATE -fobjc-arc -Wno-deprecated-declarations)
    target_include_directories(cara-save-sync-self-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(cara-save-sync-self-test PRIVATE "-framework Foundation")
    add_executable(cara-play-profile-self-test ios/play_profile_self_test.cpp)
    target_include_directories(cara-play-profile-self-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_executable(cara-input-hub-self-test ios/input_hub_self_test.mm ios/input_hub.mm)
    target_compile_options(cara-input-hub-self-test PRIVATE -fobjc-arc)
    target_include_directories(cara-input-hub-self-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(cara-input-hub-self-test PRIVATE "-framework Foundation" "-framework GameController" "-framework CoreGraphics")
    add_executable(cara-state-catalog-self-test ios/state_catalog_self_test.mm ios/state_catalog.mm ios/save_sync.mm)
    target_compile_options(cara-state-catalog-self-test PRIVATE -fobjc-arc -Wno-deprecated-declarations)
    target_include_directories(cara-state-catalog-self-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(cara-state-catalog-self-test PRIVATE "-framework Foundation")
    add_executable(cara-configuration-self-test ios/configuration_store_self_test.mm ios/configuration_store.mm ios/save_sync.mm ios/skin_layout.mm)
    target_compile_options(cara-configuration-self-test PRIVATE -fobjc-arc -Wno-deprecated-declarations)
    target_include_directories(cara-configuration-self-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(cara-configuration-self-test PRIVATE "-framework Foundation" "-framework CoreGraphics" z)
    add_executable(cara-library-self-test ios/library_store_self_test.mm ios/library_store.mm)
    target_compile_options(cara-library-self-test PRIVATE -fobjc-arc)
    target_include_directories(cara-library-self-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(cara-library-self-test PRIVATE "-framework Foundation")
    add_executable(cara-skin-self-test ios/skin_self_test.mm ios/skin_layout.mm)
    target_compile_options(cara-skin-self-test PRIVATE -fobjc-arc)
    target_include_directories(cara-skin-self-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(cara-skin-self-test PRIVATE CARA_BUILTIN_SKIN="${CMAKE_CURRENT_SOURCE_DIR}/ios/skins/CarbonDuo.deltaskin")
    target_link_libraries(cara-skin-self-test PRIVATE "-framework Foundation" "-framework CoreGraphics" z)
endif()
