# Shader control-flow regression tests

`cara-shader-flow-self-test` sends 20 original, ROM-free PICA programs through the production decompiler and shader interpreter. Expected outputs are independently chosen small integers, so all four output lanes compare exactly without floating-point tolerances.

Coverage includes straight-line instructions; uniform forward jumps, their inverted predicate and both outcomes; comparison-based taken/untaken jumps; a terminating backward jump; multiple dispatch labels with fallthrough; jumps inside called subroutines and counted loops; early END propagation from a call; and an outer label inside an IF body that forces the generator to insert a `compile_end` redispatch. Both IF outcomes are checked. The fixture assembly uses the vendored nihstro assembler and bitfield instruction definitions, not game instructions or assets.

The target compiles the real `gl_shader_decompiler.cpp` with `CARA_IOS_GLES=1`, ensuring the existing Apple do/if lowering is exercised even on hosts whose production renderer uses switch dispatch. It checks that every dispatch fixture reaches this lowering, emits no `switch (jmp_to)`, preserves the single-iteration break scope, and covers the interior-label path. It does not contain a second implementation of the decompiler or a test-only rewrite of its output.

Build with the existing desktop self-test configuration (`CARA_SOFTWARE_PRESENTATION=ON`):

```sh
cmake --build work/build-mac --target cara-shader-flow-self-test
work/build-mac/bin/cara-shader-flow-self-test
```

The regular build script includes this target, and the macOS CI job runs this deterministic source/interpreter mode. That mode does **not** execute generated GLSL or establish GPU correctness.

On macOS with graphics-session access, also run:

```sh
work/build-mac/bin/cara-shader-flow-self-test --gpu
```

This creates a private CGL context, complete 4×4 framebuffer, vertex array and transform-feedback buffer. It compiles and links each generated vertex shader, executes one point with rasterization discarded, and checks the captured four floats against both the fixed answer and interpreter result. Input vectors, bool uniforms and loop uniforms are supplied through actual GL uniforms after linking, preventing constant folding from replacing the control flow under test. The buffer starts with a nonzero sentinel and GL errors fail the test. No window, emulator instance, ROM, save data or device is used. A missing CGL context is a failure, not a silently skipped GPU pass.

Local validation on 2026-11-13: all 20 fixtures passed source/interpreter mode and native transform feedback on Apple M1, OpenGL 4.1 Metal - 91.7. This is macOS GPU evidence, not physical iPad, GLES runtime, broad game compatibility, performance, or full floating-point accuracy evidence. It does not certify arbitrary malformed/nonterminating PICA programs; invalid dispatch targets and unsupported BREAK opcodes are not supplied to the interpreter.
