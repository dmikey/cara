# Cara on Mac

Open Cara to choose a game. Opening the app does not start a previous game or restore a state. Use **Open Game**, **File > Open** (Command-O), or pass an explicit game path to the local launcher.

**Cara > Settings** (Command-comma) offers:

- **FastInterp**, the default CPU engine.
- **JIT (Dynarmic, experimental)** in builds made with `--cpu-jit` on Apple Silicon.
- The compatibility interpreter.
- Keyboard and controller bindings for the game buttons, Circle Pad and C-stick. Select an action and choose **Change binding**. For a controller stick, move right and then down. Escape cancels capture. Save keeps the settings; Cancel leaves the file unchanged.

Saving control mappings queues a reload at the game’s next normal input update, so the current game uses the new controls. CPU engine changes apply when the next game is opened; saving Settings does not replace a running CPU. Guest CPU speed stays at 100%, sound stays enabled, and the hardware renderer retains its complete reference and actual vertex work. CPU JIT is independent of shader JIT, which remains disabled in this Mac product.

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

## Release packages

GitHub's **Release packages** workflow builds the arm64 Mac app with the optional experimental CPU JIT included and an unsigned arm64 iOS GLES3 IPA. The Mac app starts with FastInterp and uses arrow keys for the D-pad and Circle Pad defaults. Custom bindings remain intact.

The Mac ZIP contains a native launcher, private copies of its non-system libraries (including SDL3 when using sdl2-compat), and their licenses. Python, Homebrew and Xcode are build-time tools only. The package audit checks every linked library, verifies signatures and executable hashes, and actually runs the native launcher/core with an empty home directory and a system-only PATH. The supported macOS minimum is calculated from the packaged binaries rather than assumed from the launcher.

Publish a GitHub release from a tag containing this workflow to build and attach `Cara-macOS-arm64-JIT.zip`, `Cara-iOS-arm64-GLES3-unsigned.ipa`, audit reports and `SHA256SUMS.txt`. Both builds must pass before the upload job runs. A published prerelease follows the same flow. A manual run without `release_tag` produces Actions artifacts only; supplying an existing release tag builds that exact tag and updates only these generated assets. Tags predating the packaging tools must be replaced by a new release tag containing them. Existing Android and signed iOS assets are not renamed or deleted.

There are no Apple distribution credentials configured. The Mac app is ad-hoc signed and is not notarized. The unsigned IPA needs signing with the user's own credentials or sideloading service; it is not an App Store or immediately installable distribution-signed IPA. This pipeline does not contact a physical iPad.

## Save states and keyboard input

With a game open, use **File > Save State > Slot 1–10** and **File > Load State > Slot 1–10**. Command-Shift-S saves slot 1; Command-Shift-L loads slot 1. The window title confirms the slot; errors appear in a dialog without silently closing the game. States are separate from a game's normal save files and require the same compatible engine build, CPU backend, and system mode. Build updates can invalidate states; keep normal in-game saves.

Keyboard input releases on focus loss and reconciles held keys with SDL's current keyboard state after event polling. A key still physically held stays held across a scene; a missed release no longer leaves a direction latched. Script-owned diagnostic input is excluded.
