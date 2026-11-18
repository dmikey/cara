# Building Cara

Apple builds require macOS, Xcode, Python 3, CMake 3.31.10, and Ninja 1.13.2.
Emulator and dependency source is bundled in the repository.

```sh
python3 tools/prepare.py
python3 tools/build.py ios --renderer gles3
python3 tools/audit_no_jit.py ios --renderer gles3
```

The unsigned app is written to `work/build-ios-gles3/bin/cara-ios.app`.
Device installation requires development signing.

See the guides for [save states](save-states.md) and [validation](validation.md).


## Linux OpenGL

The **Linux OpenGL** GitHub Actions workflow builds an x86_64 SDL frontend on
Ubuntu 24.04 and uploads `Cara-Linux-x86_64-OpenGL.tar.gz`. Download it from the
workflow's `Cara-Linux-x86_64-OpenGL` artifact, extract it, and run:

```sh
./Cara-Linux-x86_64-OpenGL/cara /path/to/game.3ds
```

It requires desktop OpenGL 4.3 and the distribution's SDL2, libpng and OpenGL
runtime packages. On Ubuntu 24.04:

```sh
sudo apt install libsdl2-2.0-0 libpng16-16t64 libgl1 libgl1-mesa-dri
```

This initial Linux developer frontend uses FastInterp without JIT, SDL audio,
native resolution and a 100% speed cap. FFmpeg/AAC decoding and the native Apple
library UI are not included. The archive includes controls, licenses, its exact
source revision, dependency linkage and a checksum. `CARA_USER_DIR` selects a
separate save directory; otherwise Cara uses its normal Linux data location.

Build from source with SDL2, libpng and OpenGL development packages installed:

```sh
python3 tools/prepare.py
cmake -S emulator -B work/build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBOOST_ROOT="$PWD/emulator/externals/boost" -DBoost_NO_SYSTEM_PATHS=ON \
  -DENABLE_SDL2=ON -DCARA_LINUX_SDL_HOST=ON -DENABLE_QT=OFF \
  -DCARA_ENABLE_JIT=OFF -DCARA_SOFTWARE_PRESENTATION=OFF
cmake --build work/build-linux --target cara -j3
work/build-linux/bin/cara --self-test
```

CI uses Mesa software rendering under Xvfb to verify real OpenGL pixel readback,
then runs an original ARM test ELF through the core and renderer for four display
refreshes. This checks Linux initialization and shutdown without commercial ROMs;
it does not establish game compatibility or hardware GPU performance.
