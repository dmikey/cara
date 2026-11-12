# Modified for Cara on 2026-11-12; see NOTICE for attribution.
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${PROJECT_SOURCE_DIR}/CMakeModules)

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

create_target_directory_groups(cara)

target_link_libraries(cara PRIVATE common core input_common network)
target_link_libraries(cara PRIVATE inih glad lodepng)
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
