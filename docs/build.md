# Building Cara

Requires macOS, Xcode, Python 3, CMake 3.31.10, and Ninja 1.13.2.
Emulator and dependency source is bundled in the repository.

```sh
python3 tools/prepare.py
python3 tools/build.py ios --renderer gles3
python3 tools/audit_no_jit.py ios --renderer gles3
```

The unsigned app is written to `work/build-ios-gles3/bin/cara-ios.app`.
Device installation requires development signing.

See the guides for [save states](save-states.md) and [validation](validation.md).

