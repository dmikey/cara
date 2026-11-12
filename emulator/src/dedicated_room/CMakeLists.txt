# Modified for Cara on 2026-11-12; see NOTICE for attribution.
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${PROJECT_SOURCE_DIR}/CMakeModules)

add_executable(cara-room
    cara-room.cpp
    cara-room.rc
)

create_target_directory_groups(cara-room)

target_link_libraries(cara-room PRIVATE common core network)
if (ENABLE_WEB_SERVICE)
    target_compile_definitions(cara-room PRIVATE -DENABLE_WEB_SERVICE)
    target_link_libraries(cara-room PRIVATE web_service)
endif()

target_link_libraries(cara-room PRIVATE cryptopp glad)
if (MSVC)
    target_link_libraries(cara-room PRIVATE getopt)
endif()
target_link_libraries(cara-room PRIVATE ${PLATFORM_LIBRARIES} Threads::Threads)

if(UNIX AND NOT APPLE)
    install(TARGETS cara-room RUNTIME DESTINATION "${CMAKE_INSTALL_PREFIX}/bin")
endif()
