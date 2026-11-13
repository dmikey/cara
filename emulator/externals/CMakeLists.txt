# Definitions for all external bundled libraries

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${PROJECT_SOURCE_DIR}/CMakeModules)
include(DownloadExternals)
include(ExternalProject)

# Catch
add_library(catch-single-include INTERFACE)
target_include_directories(catch-single-include INTERFACE catch/single_include)

# Crypto++
add_subdirectory(cryptopp)

# Define the shared fmt target before optional Dynarmic dependencies inspect it.
add_subdirectory(fmt)
add_library(fmt::fmt ALIAS fmt)

# Android's optional Dynarmic dependency is fetched at an exact source revision.
if(CARA_ENABLE_JIT AND (ARCHITECTURE_x86_64 OR ARCHITECTURE_ARM64))
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/dynarmic/CMakeLists.txt")
        message(FATAL_ERROR "Run tools/prepare_android_dynarmic.py before enabling JIT")
    endif()
    function(cara_add_dynarmic)
        if(ARCHITECTURE_ARM64)
            set(ARCHITECTURE arm64)
        elseif(ARCHITECTURE_x86_64)
            set(ARCHITECTURE x86_64)
        endif()
        set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
        set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
        add_subdirectory(dynarmic)
    endfunction()
    cara_add_dynarmic()
endif()

# getopt
if (MSVC)
    add_subdirectory(getopt)
endif()

# Glad
add_subdirectory(glad)

# libyuv
add_subdirectory(libyuv)
target_include_directories(yuv INTERFACE ./libyuv/include)

if (NOT ANDROID)
    # inih
    add_subdirectory(inih)
endif()

# MicroProfile
add_library(microprofile INTERFACE)
target_include_directories(microprofile INTERFACE ./microprofile)

# Nihstro
add_library(nihstro-headers INTERFACE)
target_include_directories(nihstro-headers INTERFACE ./nihstro/include)
# Legacy nihstro customizes make_unsigned for its packed register types.
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(nihstro-headers INTERFACE -Wno-invalid-specialization)
endif()

# Open Source Archives
add_subdirectory(open_source_archives)

# lzo compress library
add_subdirectory(minilzo)

# SoundTouch
add_subdirectory(soundtouch)
# The SoundTouch target doesn't export the necessary include paths as properties by default
target_include_directories(SoundTouch INTERFACE ./soundtouch/include)

# Teakra
add_subdirectory(teakra)

# Xbyak
if (ARCHITECTURE_x86_64)
    # Defined before "dynarmic" above
    if (NOT TARGET xbyak)
        add_library(xbyak INTERFACE)
    endif()
    target_include_directories(xbyak INTERFACE ./xbyak/xbyak)
    target_compile_definitions(xbyak INTERFACE XBYAK_NO_OP_NAMES)
endif()

# ENet
add_subdirectory(enet)
target_include_directories(enet INTERFACE ./enet/include)

# Cubeb
if (ENABLE_CUBEB)
    set(BUILD_TESTS OFF CACHE BOOL "")
    add_subdirectory(cubeb EXCLUDE_FROM_ALL)
endif()

# DiscordRPC
if (USE_DISCORD_PRESENCE)
    add_subdirectory(discord-rpc EXCLUDE_FROM_ALL)
    target_include_directories(discord-rpc INTERFACE ./discord-rpc/include)
endif()

if (ENABLE_WEB_SERVICE)
    # LibreSSL
    set(LIBRESSL_SKIP_INSTALL ON CACHE BOOL "")
    add_subdirectory(libressl EXCLUDE_FROM_ALL)
    target_include_directories(ssl INTERFACE ./libressl/include)
    target_compile_definitions(ssl PRIVATE -DHAVE_INET_NTOP)

    # JSON
    add_library(json-headers INTERFACE)
    target_include_directories(json-headers INTERFACE ./json)

    # lurlparser
    add_subdirectory(lurlparser EXCLUDE_FROM_ALL)

    # httplib
    add_library(httplib INTERFACE)
    target_include_directories(httplib INTERFACE ./httplib)

    # cpp-jwt
    add_library(cpp-jwt INTERFACE)
    target_include_directories(cpp-jwt INTERFACE ./cpp-jwt/include)
endif()

# Save-state serialization and compression (bundled source).
file(GLOB cara_boost_serialization_sources "${CMAKE_CURRENT_SOURCE_DIR}/boost/libs/serialization/src/*.cpp")
add_library(boost_serialization STATIC ${cara_boost_serialization_sources})
target_include_directories(boost_serialization PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/boost")
set_property(TARGET boost_serialization PROPERTY CXX_STANDARD 17)
add_library(Boost::serialization ALIAS boost_serialization)
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)
add_subdirectory(zstd/build/cmake EXCLUDE_FROM_ALL)
target_include_directories(libzstd_static INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/zstd/lib>")
