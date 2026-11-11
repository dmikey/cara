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
    add_subdirectory(citra)
endif()

if (ENABLE_QT)
    add_subdirectory(citra_qt)
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
    add_executable(cara-input-self-test input_self_test.cpp)
    add_executable(cara-audio-buffer-self-test audio_buffer_self_test.cpp)
    add_executable(cara-interval-cache-self-test interval_cache_self_test.cpp)
    target_link_libraries(cara-interval-cache-self-test PRIVATE Boost::boost)
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
    add_executable(cara-ios MACOSX_BUNDLE ios/main.mm ios/save_sync.mm ios/skin_layout.mm ios/skin_controls.mm ios/skins/CarbonDuo.deltaskin)
    set_source_files_properties(ios/skins/CarbonDuo.deltaskin PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    target_compile_options(cara-ios PRIVATE -fobjc-arc)
    if (CARA_IOS_HARDWARE_RENDERER)
        target_sources(cara-ios PRIVATE ios/gpu_vertex_self_test.cpp ios/gpu_depth_self_test.cpp)
        target_link_libraries(cara-ios PRIVATE nihstro-headers)
        target_link_libraries(cara-ios PRIVATE glad "-framework OpenGLES")
    endif()
    target_link_libraries(cara-ios PRIVATE core common Threads::Threads
        "-framework Foundation" "-lobjc" "-framework UIKit" "-framework CoreGraphics" "-framework UniformTypeIdentifiers" "-framework ImageIO" "-framework QuartzCore" z)
    set_target_properties(cara-ios PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/ios/Info.plist"
        XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED NO
        XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2")
endif()

if (APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    enable_language(OBJCXX)
    add_executable(cara-save-sync-self-test ios/save_sync_self_test.mm ios/save_sync.mm)
    target_compile_options(cara-save-sync-self-test PRIVATE -fobjc-arc -Wno-deprecated-declarations)
    target_include_directories(cara-save-sync-self-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(cara-save-sync-self-test PRIVATE "-framework Foundation")
    add_executable(cara-skin-self-test ios/skin_self_test.mm ios/skin_layout.mm)
    target_compile_options(cara-skin-self-test PRIVATE -fobjc-arc)
    target_include_directories(cara-skin-self-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(cara-skin-self-test PRIVATE CARA_BUILTIN_SKIN="${CMAKE_CURRENT_SOURCE_DIR}/ios/skins/CarbonDuo.deltaskin")
    target_link_libraries(cara-skin-self-test PRIVATE "-framework Foundation" "-framework CoreGraphics" z)
endif()
