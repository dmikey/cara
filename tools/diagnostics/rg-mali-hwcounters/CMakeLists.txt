cmake_minimum_required(VERSION 3.16)
project(cara_mali_sample LANGUAGES CXX)

add_subdirectory("${LIBGPUCOUNTERS_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}/libGPUCounters")
add_executable(cara-mali-sample main.cpp)
target_link_libraries(cara-mali-sample PRIVATE hwcpipe)
target_compile_features(cara-mali-sample PRIVATE cxx_std_14)
