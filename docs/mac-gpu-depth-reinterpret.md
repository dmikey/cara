# Native Mac GPU depth reinterpretation

`CARA_MAC_GPU_DEPTH_REINTERPRET=1` selects an experimental GPU-only D24S8-to-RGBA8
converter on native Apple desktop OpenGL. It is disabled by default. iOS already
uses this converter; this experiment does not establish an iPad speed gain.

Apple OpenGL 4.1 lacks stencil texturing. The existing fallback reads depth/stencil
into a GPU pixel buffer and samples that buffer; it does not map those bytes to
the CPU. Native M1 gameplay samples nevertheless showed synchronization inside
that `glReadPixels` call. The candidate instead samples depth into RGBA8, then
recovers eight stencil bits using read-only fixed-function tests and additive
color passes. It uses nine draws and a final nearest blit, with no pixel-buffer
read or CPU readback during normal conversion.

The implementation retains the existing GLES converter, extent checks and
source ownership. Native GLSL 330 omits the ES precision preamble, which names an
unsupported image type. Embedded `highp` qualifiers remain legal in GLSL 330.
Desktop color logic operation and clip enables are preserved explicitly,
including raw state that differs from the renderer cache. Other reinterpretation
routes and native RAM upload/download paths remain unchanged.

## Correctness test

```sh
cmake --build /path/to/native-build --target cara-mac-depth-self-test
CARA_MAC_GPU_DEPTH_REINTERPRET=1 /path/to/native-build/bin/cara-mac-depth-self-test
```

The target requires an actual native Mac OpenGL context. Missing context support
is a failure, not a pass. Running with the experiment disabled also fails, so the
old desktop no-op oracle cannot masquerade as GPU execution.

On Apple M1, OpenGL 4.1 Metal 91.7 with stencil texturing unavailable, validation
passed every 16,777,216 D24 code for both D24 and D24S8, all 65,536 D16 codes,
stencil patterns, offset/scaled rectangles, partial uploads, untouched source and
destination regions, and raster-state restoration. An additional 65,536-texel
native integer upload compares the legacy PBO and candidate outputs byte for
byte, including offset/scaled and repeated conversions. This complements the
exhaustive oracle's fragment-written exact depth seeds.

The tests perform explicit readbacks; normal conversion does not. No gameplay
speed gain has been measured yet. Promotion requires one-binary, same-checkpoint
comparisons with unchanged guest clocks, workload, output and progression.

References: [GLSL 3.30 precision qualifiers](https://registry.khronos.org/OpenGL/specs/gl/GLSLangSpec.3.30.pdf)
and [OpenGL stencil texture behavior](https://wikis.khronos.org/opengl/Stencil_Image_Format).
