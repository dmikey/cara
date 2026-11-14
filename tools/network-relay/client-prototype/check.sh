#!/bin/sh
# Copyright 2026 Cara contributors. Licensed under GPL-2.0-or-later.
# Standalone C++17 check. No ROM, network, emulator build, or extra library.
set -eu
source_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if [ "$#" -gt 1 ]; then
    echo "usage: $0 [build-directory]" >&2
    exit 2
fi
if [ "$#" -eq 1 ]; then
    mkdir -p "$1"
    build_dir=$(CDPATH= cd -- "$1" && pwd)
else
    build_dir=$(mktemp -d "${TMPDIR:-/tmp}/cara-relay-codec.XXXXXX")
    trap 'rm -rf "$build_dir"' EXIT HUP INT TERM
fi
mkdir -p "$build_dir/corpus"
"${CXX:-c++}" -std=c++17 -O1 -g -Wall -Wextra -Werror \
    -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all \
    "$source_dir/wifi_envelope_test.cpp" -o "$build_dir/wifi_envelope_test"
"${CXX:-c++}" -std=c++17 -O1 -g -Wall -Wextra -Werror \
    -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all \
    "$source_dir/wifi_envelope_fuzz.cpp" "$source_dir/fuzz_driver.cpp" \
    -o "$build_dir/wifi_envelope_mutation"
(cd "$build_dir" && CORPUS_DIR="$build_dir/corpus" ./wifi_envelope_test && ./wifi_envelope_mutation)
