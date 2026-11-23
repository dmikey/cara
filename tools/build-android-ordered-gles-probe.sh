#!/usr/bin/env bash
# Build an isolated ARM64/EGL executable with the same GLAD and submission code as Cara.
set -euo pipefail
out=${1:?expected output directory}
compiler=${2:?expected NDK clang++, e.g. aarch64-linux-android30-clang++}
dispatch=${3:-tls}
mkdir -p "$out"
root="$(cd "$(dirname "$0")/.." && pwd)"
deps="$root/emulator/externals"
src="$root/emulator/src"
renderer="$src/video_core/renderer_opengl"
defines=()
case "$dispatch" in
    tls) defines+=(-DCARA_GL_CONTEXT_DISPATCH=1);;
    original) ;;
    *) exit 2;;
esac
"${compiler%++}" -O2 -I"$deps/glad/include" -c "$deps/glad/src/glad.c" -o "$out/glad.o"
"$compiler" -std=c++20 -O2 -ffunction-sections -fdata-sections -static-libstdc++ \
    -I"$src" -I"$deps/glad/include" -I"$deps/boost" -I"$deps/fmt/include" \
    "${defines[@]}" -DCARA_IOS_GLES=1 -DCARA_ANDROID_GLES_ATLAS=1 -DFMT_HEADER_ONLY=1 \
    "$src/android/jni/tests/ordered_gles_probe.cpp" "$src/android/jni/tests/probe_log.cpp" \
    "$renderer/gl_command_queue.cpp" "$renderer/gl_observer_stubs.cpp" "$renderer/gl_state.cpp" \
    "$out/glad.o" -Wl,--gc-sections -lEGL -ldl -llog -o "$out/cara_ordered_gles_probe"
