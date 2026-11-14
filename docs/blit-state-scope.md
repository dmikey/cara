# Optional blit state scope

`CARA_BLIT_STATE_SCOPE=1` enables an experimental state scope around the raw
surface-cache framebuffer blit. It is disabled by default and independent of
`CARA_GL_STATE_REUSE`. No performance improvement has been established.

The existing path applies and restores the clear/blit `SubApply` state subset.
That changes color, depth and stencil write masks, viewport and scissor rectangle,
although those settings do not affect a framebuffer blit. OpenGL 4.1 core
section 4.3.2 specifies that blits bypass the fragment pipeline except for pixel
ownership and scissor tests:
<https://registry.khronos.org/OpenGL/specs/gl/glspec41.core.pdf>.
The experiment leaves these settings untouched and changes only read/draw
framebuffer bindings and scissor enable. It does not change sRGB state.

The narrow helper operates only when the existing full raster-state snapshot is
valid. Otherwise both entry and exit use the original `SubApply` path. Neither
operation establishes full validity. Attachment setup, copy rectangles, filtering,
framebuffer completeness checks, GL-error checks and exception ordering remain
unchanged. Existing optional `StateApply` timing scopes remain present. No copies,
guest instructions, synchronization or guest timing are skipped.

## Native correctness oracle

Build `cara-mac-blit-state-self-test` in a native macOS engine build with the
OpenGL renderer enabled. This target includes the production cache implementation
in its test translation unit so it can call the actual static raw blit helper;
it introduces no production test-access API. The linker resolves those cache
symbols from the test object rather than extracting the archive's duplicate
object. No ROM, save data, window, simulator or device is required.

Run the same executable in four separate processes, because both options are
cached on first use:

```sh
CARA_BLIT_STATE_SCOPE=0 CARA_GL_STATE_REUSE=0 build/bin/cara-mac-blit-state-self-test
CARA_BLIT_STATE_SCOPE=1 CARA_GL_STATE_REUSE=0 build/bin/cara-mac-blit-state-self-test
CARA_BLIT_STATE_SCOPE=0 CARA_GL_STATE_REUSE=1 build/bin/cara-mac-blit-state-self-test
CARA_BLIT_STATE_SCOPE=1 CARA_GL_STATE_REUSE=1 build/bin/cara-mac-blit-state-self-test
```

Each invocation checks actual RGBA8, D16 and D24S8 copies; all 256 stencil values;
scissor enabled/disabled; unusual write masks, viewport and scissor rectangle;
actual GL state and tracked state restoration; a subsequent draw without state
reapplication against fixed expected pixels; and invalid-tracker fallback.
Failure cases cover a pending GL error, unsupported attachment category,
incomplete framebuffer and a GL error from the blit itself. Failure messages
verify the intended rejection stage. The test also observes real GL entry points
to verify that the narrow route avoids mask, viewport and scissor-box calls.

The native Apple M1 OpenGL 4.1 fixture passed 6,839 checks in each configuration.
A private negative-control build omitted the actual scissor disable and was
rejected by the pixel oracle. These are targeted fixture compile/link results
against preserved native engine archives, not a fresh full-engine build or CI
receipt. Private commands, source/archive/binary hashes and logs are under
`profiles/blit-state-scope-20261114` on the development volume.

The D24 fixture deliberately uses exact zero/full-depth endpoints. Arbitrary
D24 values uploaded through the existing native GL path showed a one-code
rounding difference with the experiment disabled; this fixture does not claim
to validate all depth-format conversions. The separate depth oracle covers that
work. There is no gameplay, GLES, physical-iPad or speed claim from this test.
A guarded, matched gameplay comparison remains necessary before promotion.

A later fresh full-engine build passed the same four native oracle configurations.
The subsequent [quiet Stories Secret Path comparison](stories-surface-copies-20261114.md)
found no speedup; the option remains off. Its measured outcome must not be
replaced by the smaller API-call count from the correctness fixture.
