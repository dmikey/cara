#!/usr/bin/env sh
set -eu
ndk=${1:?usage: build-android.sh NDK_DIR LIBGPUCOUNTERS_SOURCE_DIR OUTPUT_DIR}
source=${2:?required libGPUCounters source directory}
output=${3:?required output directory}
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cmake -S "$here" -B "$output" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=Release -DHWCPIPE_ENABLE_LTO=OFF \
    -DLIBGPUCOUNTERS_SOURCE="$source"
cmake --build "$output" --target cara-mali-sample --parallel 4
