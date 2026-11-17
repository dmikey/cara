# Cara on Mac

Open Cara to choose a game. Opening the app does not start a previous game or restore a state. Use **Open Game**, **File > Open** (Command-O), or pass an explicit game path to the local launcher.

**Cara > Settings** (Command-comma) offers:

- **FastInterp**, the default CPU engine.
- **JIT (Dynarmic, experimental)** in builds made with `--cpu-jit` on Apple Silicon.
- The compatibility interpreter.
- Keyboard and controller bindings for the game buttons, Circle Pad and C-stick. Select an action and choose **Change binding**. For a controller stick, move right and then down. Escape cancels capture. Save keeps the settings; Cancel leaves the file unchanged.

Changes apply when the next game is opened. Opening Settings during a game does not replace its running CPU or input devices. Guest CPU speed stays at 100%, sound stays enabled, and the hardware renderer retains its complete reference and actual vertex work. CPU JIT is independent of shader JIT, which remains disabled in this Mac product.

Settings live in `config/sdl2-config.ini` inside the app's game data directory. Editing CPU and input choices preserves the other sections. A packaged local app uses `~/Library/Application Support/Cara` unless packaged with an explicit data directory. Game saves are separate from CPU-specific save states. State headers continue to reject another backend or incompatible engine build; the app never rewrites state headers to make them load.

## Experimental M1 CPU JIT

The optional backend recovers the previously prepared and locally exercised Dynarmic ARM64 integration. Its pinned dependency and exact local compatibility changes are recorded in [the dependency manifest](evidence/m1-dynarmic-source-manifest.json). It does not enable the global CPU/shader JIT build flag or change the iPad build.

The earlier diagnostic build executed a real game and passed 49 actual adapter/factory/archive checks. Those historical receipts are distinct from validation of this Mac settings integration. The earlier game comparison also found different SVC tick visibility and workload behavior between engines. It is not a controlled speedup or universal compatibility proof. FastInterp remains the default, and JIT remains experimental; no guest tick adjustment was introduced to conceal the difference.

Build and package locally:

```sh
python3 tools/build_mac_sdl.py --atlas --cpu-jit --build-dir /absolute/path/mac-build
python3 tools/package_mac_app.py --build-dir /absolute/path/mac-build --output /absolute/path/Cara.app
```

Omit `--cpu-jit` for a build without Dynarmic. Its JIT menu item is unavailable. The packaging helper verifies the build audit, signs the local app, and grants the JIT entitlement only to a CPU JIT build. This is a local developer app using the installed SDL and libpng libraries, not a notarized redistributable release.
