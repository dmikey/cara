# Build public source distributions for every Apple SDK, including iPhoneOS.
cmake_policy(SET CMP0079 NEW)
include(FetchContent)
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
# Probe libarchive symbols with a linker even when the iOS parent uses static try-compiles.
set(CMAKE_TRY_COMPILE_TARGET_TYPE EXECUTABLE)
set(BUILD_SHARED_LIBS OFF)
set(BUILD_TESTING OFF)
set(XZ_THREADS no CACHE STRING "Threading in vendored liblzma" FORCE)
foreach(option TOOL_XZDEC TOOL_LZMADEC TOOL_LZMAINFO TOOL_XZ TOOL_SCRIPTS DOC NLS)
    set(XZ_${option} OFF)
endforeach()
FetchContent_Declare(cara_xz URL "${CMAKE_CURRENT_SOURCE_DIR}/xz-5.8.4.tar.gz"
    URL_HASH SHA256=b6b34c52e672e810d339db14e0c10b5b6f5f67a30d8d05cdac5d8cb4d82f78d5)
FetchContent_MakeAvailable(cara_xz)
foreach(option MBEDTLS NETTLE OPENSSL MD LIBB2 LZ4 LZO LZMA ZSTD LIBXML2 BSDXML EXPAT
        WIN32_XMLLITE PCREPOSIX PCRE2POSIX LIBGCC CNG TAR CPIO CAT UNZIP XATTR ACL TEST INSTALL WERROR)
    set(ENABLE_${option} OFF)
endforeach()
FetchContent_Declare(cara_archive URL "${CMAKE_CURRENT_SOURCE_DIR}/libarchive-3.8.11.tar.gz"
    URL_HASH SHA256=6e9560b2e0659ff688c5efb7717a68bb06b28ef65a69addcd6c424eaf9c319b1)
FetchContent_MakeAvailable(cara_archive)
target_compile_definitions(archive_static PRIVATE HAVE_LZMA_H=1 HAVE_LIBLZMA=1)
target_link_libraries(archive_static liblzma)
target_include_directories(archive_static PRIVATE "${cara_xz_SOURCE_DIR}/src/liblzma/api")

function(cara_bundle_sevenzip_licenses target)
    set(notices "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/licenses/COPYING-libarchive.txt"
                "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/licenses/COPYING-liblzma.txt")
    set_source_files_properties(${notices} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/Licenses)
    target_sources(${target} PRIVATE ${notices})
endfunction()
