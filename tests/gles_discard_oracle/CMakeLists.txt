cmake_minimum_required(VERSION 3.20)
project(CaraGLESDiscardOracle LANGUAGES CXX OBJCXX)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    message(FATAL_ERROR "This standalone oracle requires the actual iOS OpenGLES platform")
endif()
add_executable(cara-gles-discard-oracle MACOSX_BUNDLE main.mm)
set_target_properties(cara-gles-discard-oracle PROPERTIES
    OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist"
    XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED NO
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2")
target_compile_options(cara-gles-discard-oracle PRIVATE -fobjc-arc -Wno-deprecated-declarations)
target_link_libraries(cara-gles-discard-oracle PRIVATE "-framework UIKit" "-framework Foundation" "-framework OpenGLES")
