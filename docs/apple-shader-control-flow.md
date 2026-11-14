# Apple OpenGL shader control flow

Dragon Quest VIII exposed a native macOS driver crash while linking a generated PICA vertex shader. The reported stack enters `glpIsCaseStatementNode` from `glpLLVMCGSwitchStatement` inside Apple's OpenGL linker, before Cara's draw completes. This is distinct from a guest ARM execution fault, a shader-link error return, or a measured performance regression.

Cara already emitted an equivalent `do`/`if` dispatch form for iOS GLES to avoid this compiler pattern. The same lowering now applies on Apple desktop OpenGL. Ordered case activation preserves fallthrough; a dispatch break exits the single-iteration loop and returns to the outer dispatch loop. Guest instructions, arithmetic, clock and event timing are unchanged. Other desktop platforms keep the switch form.

Apple desktop persistent shader caches advance to version 0xA so old source/reference tables cannot reload the crashing form. Other platforms retain version 0x9. This cache change does not alter save data.

## Evidence

On the local M1, the captured 819-line vertex shader compiled successfully but crashed the driver at link time in a standalone OpenGL program, without the emulator core or ROM loader running. The same source with only the existing dispatch lowering applied linked successfully. This single-variable reproduction isolates the compiler pattern more strongly than a full-game restart alone.

A fresh no-JIT Dragon Quest VIII run with 100% guest CPU, a 100% speed limit and SDL sound passed the original startup crash and rendered the title screen. This proves the reported linker failure is avoided; first-battle completion and full-game compatibility require separate gameplay checks. Mac desktop GL evidence does not establish iPad GLES behavior or a speed gain.

Crash reports, generated game shaders and captures remain private test artifacts outside the repository. The maintained regression fixtures use original synthetic PICA programs.
