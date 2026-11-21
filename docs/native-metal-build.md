# Native Metal developer builds

The Apple Silicon macOS runner and the physical-device iOS app build the same
native Metal renderer. These opt-in commands leave the normal OpenGL Mac and
GLES iOS build commands available.

Use full Xcode with the Metal tools selected, Python 3.9 or newer, Git, CMake
3.31.10 and Ninja 1.13.2. For Mac builds, install SDL2 development files and
libpng, for example with `brew install sdl2 libpng`. Use `--prefix-path` for
an alternate installation prefix.

```sh
python3 tools/build_native_metal.py mac --cpu-jit
python3 tools/build_native_metal.py ios
```

The Mac command includes the optional ARM CPU JIT, with FastInterp still
available. Omit the flag for a FastInterp-only build. Both commands compile
the native Metal renderer and the same PICA shader resources. The Mac output
is a locally ad-hoc-signed SDL developer runner with dynamic SDL dependencies
and the iOS output is an app without provisioning for installation. The audit
records the architecture inputs, native Metal build inputs, compiler identities,
bundled shader resources and linked JIT policy. CI verifies compilation and
the Metal transfer planner. It does not run an emulator scene.

To run the Mac compatibility frontend with a local ROM and isolated app data,
pass an absolute user directory, the ROM and a refresh limit:

```sh
mkdir -p "$PWD/work/metal-profile"
CARA_USER_DIR="$PWD/work/metal-profile" CARA_CPU_BACKEND=jit \
  work/build-native-metal-mac/bin/cara-metal-rom.app/Contents/MacOS/cara-metal-rom \
  "/absolute/path/to/game.3ds" 2400
```

Use `CARA_CPU_BACKEND=fastinterp` for the interpreter. The iOS build includes
FastInterp and retains its GLES option. The existing release and signing
workflows continue to package their current configurations.

## Runtime shader compiler sources

The build helper fetches two pinned Khronos source revisions as needed.
`--shader-sources PATH` reuses clean offline checkouts named `glslang` and
`spirv-cross` inside that directory. Both dependencies are built as static
libraries for the selected SDK and translate shaders at runtime on each target.

- glslang 16.6.0, commit
  [e1b562a](https://github.com/KhronosGroup/glslang/tree/e1b562a8bed273a02f30b59b66a5d499793cede5).
- SPIRV-Cross Vulkan SDK 1.4.350.0, commit
  [1a61695](https://github.com/KhronosGroup/SPIRV-Cross/tree/1a6169566c73d3da552748fc372fe2bbb856e46e).

No per-title shader capture, ROM contents, prepared Gate catalog, private
signing identity or device provisioning is required to compile. The build
includes the unmodified upstream license texts and notices in the app's
`Licenses` directory. Preserve the included Khronos notices and the emulator's
corresponding-source material when packaging these developer builds. glslang's
notices cover several licenses and SPIRV-Cross includes the Apache 2.0 license
and a separate `LICENSES` directory for components.
