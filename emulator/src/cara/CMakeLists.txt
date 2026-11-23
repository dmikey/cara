# Modified for Cara on 2026-11-12; see NOTICE for attribution.
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${PROJECT_SOURCE_DIR}/CMakeModules)

option(CARA_LINUX_SDL_HOST "Build the native Linux OpenGL SDL frontend" OFF)
option(CARA_MAC_SDL_DEV_HOST "Build the separate native macOS SDL developer frontend" OFF)
if (CARA_MAC_SDL_DEV_HOST)
    if (NOT APPLE OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
        message(FATAL_ERROR "CARA_MAC_SDL_DEV_HOST requires native macOS")
    endif()
    add_executable(cara
        native_mac/controller_play_test.cpp
        native_mac/controller_play_test.h
        native_mac/main.cpp
        native_mac/mac_preferences_ui.mm
        native_mac/mac_preferences_ui.h
        native_mac/mac_preferences.h
        native_mac/game_menu.mm
        native_mac/game_menu.h
        native_mac/native_command_policy.h
        native_mac/gl_query_census.cpp
        native_mac/gl_query_census.h
        native_mac/profile_gpu_boundary.h
        native_mac/behavior_fixture.cpp
        native_mac/required_copy_fixture.cpp
        native_mac/required_copy_fixture.h
        native_mac/behavior_fixture.h
        native_mac/relay_config.mm
        native_mac/relay_config.h
        native_mac/config.cpp
        native_mac/emu_window.cpp
        native_mac/emu_window.h
        native_mac/image.cpp
        config.h
        default_ini.h
        lodepng_image_interface.h
    )
elseif(CARA_LINUX_SDL_HOST)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CARA_ENABLE_JIT)
        message(FATAL_ERROR "CARA_LINUX_SDL_HOST requires Linux and CARA_ENABLE_JIT=OFF")
    endif()
    add_executable(cara native_linux/main.cpp native_mac/image.cpp)
else()
    add_executable(cara
        cara.cpp
        cara.rc
        config.cpp
        config.h
        default_ini.h
        emu_window/emu_window_sdl2.cpp
        emu_window/emu_window_sdl2.h
        lodepng_image_interface.cpp
        lodepng_image_interface.h
        resource.h
    )
endif()

create_target_directory_groups(cara)

target_link_libraries(cara PRIVATE common core input_common network)
target_link_libraries(cara PRIVATE inih glad)
if (CARA_MAC_SDL_DEV_HOST)
    if(CARA_MAC_CPU_JIT)
        target_sources(cara PRIVATE native_mac/arm_dynarmic_adapter_test.cpp)
        target_link_libraries(cara PRIVATE dynarmic)
    endif()
    add_executable(cara-mac-preferences-self-test "${PROJECT_SOURCE_DIR}/../tests/mac_preferences_test.cpp")
    target_include_directories(cara-mac-preferences-self-test PRIVATE "${PROJECT_SOURCE_DIR}/src" ${SDL2_INCLUDE_DIR})
    target_compile_options(cara-mac-preferences-self-test PRIVATE -UNDEBUG)
    target_link_libraries(cara-mac-preferences-self-test PRIVATE common)
    enable_language(OBJCXX)
    set_target_properties(cara PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES)
    set_source_files_properties(native_mac/relay_config.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    set_source_files_properties(native_mac/game_menu.mm native_mac/mac_preferences_ui.mm PROPERTIES COMPILE_FLAGS "-fno-objc-arc")
    target_link_libraries(cara PRIVATE "-framework AppKit")
    find_package(PNG REQUIRED)
    target_link_libraries(cara PRIVATE PNG::PNG nihstro-headers)
    add_executable(cara-native-relay-config-self-test
        "${PROJECT_SOURCE_DIR}/../tests/native_relay_config_test.mm"
        native_mac/relay_config.mm)
    target_include_directories(cara-native-relay-config-self-test PRIVATE "${PROJECT_SOURCE_DIR}/src")
    set_target_properties(cara-native-relay-config-self-test PROPERTIES
        OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES)
    target_compile_options(cara-native-relay-config-self-test PRIVATE -fobjc-arc -UNDEBUG)
    target_link_libraries(cara-native-relay-config-self-test PRIVATE "-framework Foundation")
    add_executable(cara-native-touch-command-self-test
        "${PROJECT_SOURCE_DIR}/../tests/native_touch_command_test.cpp")
    target_include_directories(cara-native-touch-command-self-test PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_compile_options(cara-native-touch-command-self-test PRIVATE -UNDEBUG)

elseif(CARA_LINUX_SDL_HOST)
    find_package(PNG REQUIRED)
    target_link_libraries(cara PRIVATE PNG::PNG)
else()
    target_link_libraries(cara PRIVATE lodepng)
endif()
if (MSVC)
    target_link_libraries(cara PRIVATE getopt)
endif()
target_link_libraries(cara PRIVATE ${PLATFORM_LIBRARIES} SDL2 Threads::Threads)

if(UNIX AND NOT APPLE)
    install(TARGETS cara RUNTIME DESTINATION "${CMAKE_INSTALL_PREFIX}/bin")
endif()

if (MSVC)
    include(CopyCaraSDLDeps)
    copy_cara_SDL_deps(cara)
endif()

if (CARA_PRIVATE_MAC_METAL_HOST)
    if (NOT CARA_MAC_SDL_DEV_HOST OR NOT CARA_PRIVATE_METAL_DRAWS OR
        NOT CARA_PRIVATE_METAL_TRANSFERS)
        message(FATAL_ERROR "The native Mac Metal host requires the Mac SDL host and the Metal renderer")
    endif()
    add_executable(cara-metal-rom MACOSX_BUNDLE native_mac/metal_rom.mm native_mac/config.cpp)
    target_link_libraries(cara-metal-rom PRIVATE common core video_core input_common inih glad
        nihstro-headers SDL2 Threads::Threads "-framework Metal" "-framework QuartzCore"
        "-framework Foundation")
    set_target_properties(cara-metal-rom PROPERTIES
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.cara.mac.metal-runtime"
        MACOSX_BUNDLE_BUNDLE_NAME "Cara Metal"
        OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES)
    target_compile_options(cara-metal-rom PRIVATE -fobjc-arc)
    if (CARA_MAC_CPU_JIT)
        target_link_libraries(cara-metal-rom PRIVATE dynarmic)
    endif()

    add_dependencies(cara-metal-rom cara-pica-metallib)
    get_target_property(pica_library video_core CARA_PICA_METAL_LIBRARY)
    get_target_property(scaled_crop_library video_core CARA_PICA_SCALED_CROP_LIBRARY)
    set(metal_assets "${pica_library}" "${scaled_crop_library}")
    if (CARA_PRIVATE_GATE_METAL_CATALOG)
        list(APPEND metal_assets "${CARA_PRIVATE_GATE_METAL_CATALOG}/cara-gate.metallib")
    endif()
    foreach(asset IN LISTS metal_assets)
        set_property(TARGET cara-metal-rom APPEND PROPERTY LINK_DEPENDS "${asset}")
        add_custom_command(TARGET cara-metal-rom POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_BUNDLE_DIR:cara-metal-rom>/Contents/Resources"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${asset}"
                "$<TARGET_BUNDLE_DIR:cara-metal-rom>/Contents/Resources"
            VERBATIM)
    endforeach()
endif()
