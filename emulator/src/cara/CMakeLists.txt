# Modified for Cara on 2026-11-12; see NOTICE for attribution.
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${PROJECT_SOURCE_DIR}/CMakeModules)

option(CARA_MAC_SDL_DEV_HOST "Build the separate native macOS SDL developer frontend" OFF)
if (CARA_MAC_SDL_DEV_HOST)
    if (NOT APPLE OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
        message(FATAL_ERROR "CARA_MAC_SDL_DEV_HOST requires native macOS")
    endif()
    add_executable(cara
        native_mac/main.cpp
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
    enable_language(OBJCXX)
    set_target_properties(cara PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES)
    set_source_files_properties(native_mac/relay_config.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    find_package(PNG REQUIRED)
    target_link_libraries(cara PRIVATE PNG::PNG)
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
