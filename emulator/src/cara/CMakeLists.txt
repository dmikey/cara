# Modified for Cara on 2026-11-12; see NOTICE for attribution.
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${PROJECT_SOURCE_DIR}/CMakeModules)

option(CARA_MAC_SDL_DEV_HOST "Build the separate native macOS SDL developer frontend" OFF)
if (CARA_MAC_SDL_DEV_HOST)
    if (NOT APPLE OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
        message(FATAL_ERROR "CARA_MAC_SDL_DEV_HOST requires native macOS")
    endif()
    add_executable(cara
        native_mac/main.cpp
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
