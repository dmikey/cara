add_library(network STATIC
    network.cpp
    network.h
    relay_client.cpp
    relay_client.h
    relay_session.h
    wifi_envelope.h
    uds_relay_admission.h
    packet.cpp
    packet.h
    room.cpp
    room.h
    room_member.cpp
    room_member.h
    verify_user.cpp
    verify_user.h
)

create_target_directory_groups(network)

target_link_libraries(network PRIVATE common enet)

if (APPLE)
    enable_language(OBJCXX)
    target_sources(network PRIVATE relay_client_apple.mm)
    set_target_properties(network PROPERTIES OBJCXX_STANDARD 17 OBJCXX_STANDARD_REQUIRED YES)
    set_source_files_properties(relay_client_apple.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    target_link_libraries(network PRIVATE "-framework Foundation" "-framework Security")
endif()

# Standalone native witnesses. Live targets require explicit stdin credentials;
# they are never invoked by the shipping UI or automatically by CI.
if (APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    foreach(witness IN ITEMS relay_client_live_test relay_client_reconnect_test relay_client_configuration_self_test)
        string(REPLACE "_" "-" witness_target "cara-${witness}")
        add_executable(${witness_target} "${PROJECT_SOURCE_DIR}/../tests/${witness}.mm")
        target_include_directories(${witness_target} PRIVATE "${PROJECT_SOURCE_DIR}/src")
        set_target_properties(${witness_target} PROPERTIES OBJCXX_STANDARD 17 OBJCXX_STANDARD_REQUIRED YES)
        target_compile_options(${witness_target} PRIVATE -fobjc-arc)
        target_link_libraries(${witness_target} PRIVATE network)
    endforeach()
    if (CARA_SOFTWARE_PRESENTATION)
        add_executable(cara-nwm-relay-hle-live-test "${PROJECT_SOURCE_DIR}/../tests/nwm_relay_hle_live_test.mm")
        target_include_directories(cara-nwm-relay-hle-live-test PRIVATE "${PROJECT_SOURCE_DIR}/src")
        set_target_properties(cara-nwm-relay-hle-live-test PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES)
        target_compile_options(cara-nwm-relay-hle-live-test PRIVATE -fobjc-arc)
        target_link_libraries(cara-nwm-relay-hle-live-test PRIVATE core network)
    endif()
endif()
