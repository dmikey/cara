add_executable(tests
    common/bit_field.cpp
    common/param_package.cpp
    core/arm/arm_test_common.cpp
    core/arm/arm_test_common.h
    core/arm/dyncom/arm_dyncom_vfp_tests.cpp
    core/core_timing.cpp
    core/file_sys/path_parser.cpp
    core/hle/kernel/hle_ipc.cpp
    core/memory/memory.cpp
    core/memory/vm_manager.cpp
    audio_core/audio_fixures.h
    audio_core/decoder_tests.cpp
    tests.cpp
)

if (CARA_ENABLE_JIT AND ARCHITECTURE_x86_64)
    target_sources(tests
        PRIVATE
            video_core/shader/shader_jit_x64_compiler.cpp
    )
endif()

create_target_directory_groups(tests)

target_link_libraries(tests PRIVATE common core video_core audio_core)
target_link_libraries(tests PRIVATE ${PLATFORM_LIBRARIES} catch-single-include nihstro-headers Threads::Threads)

add_test(NAME tests COMMAND tests)

# Original synthetic AAC, no game audio; also usable independently of the emulator test binary.
if(APPLE AND NOT ENABLE_MF AND NOT ENABLE_FFMPEG_AUDIO_DECODER)
    add_executable(cara-apple-aac-test audio_core/apple_aac_test.cpp
        ../audio_core/hle/apple_aac_codec.cpp)
    target_include_directories(cara-apple-aac-test PRIVATE ..)
    target_link_libraries(cara-apple-aac-test PRIVATE "-framework AudioToolbox" "-framework CoreFoundation")
    add_test(NAME cara-apple-aac-test COMMAND cara-apple-aac-test)
endif()
