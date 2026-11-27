# Finer M1 Metal owner CPU attribution

Status: **source-only, not compiled or executed**. This observer does not
establish a performance result or a graphics fix.

Apply only to the exact private instrumented source at
`cde0aa06fd1081290f43551820ace838fd1960af`, retained on
`archive/m1-metal-profile-20261127`. Verify every input hash in SOURCE.json
before applying observer.patch; build/audit a new private product afterward.
Do not overwrite or relabel the existing tested binaries or closed receipts.

The observer subdivides the previous binding category into fragment source,
vertex source, vertex capture, pipeline selection, surface/LUT setup, texture
decode and uniforms. It separates fill, texture copy, display transfer, byte
resolution and byte publication. Existing nested CPU accounting subtracts
child spans; do not add these to the enclosing elapsed driver spans or GPU
command envelopes. No guest timing, GPU work, renderer policy or synchronization
change is intended. Run the observer separately from quiet timing comparisons.

Before a new timing run, require sustained low background activity and check
installed Cara applications/gameplay locks, competing compilers and exact
binary/resource/source/ROM identities. New replay artifacts belong outside
Documents. Stop only owned jobs if contention or user gameplay starts.

The prepared matched schedule is OpenGL / Metal OFF / Metal ON / Metal ON /
Metal OFF / OpenGL, with JIT, fixed guest CLK100, cap100, native 1x, SDL sound,
all GPU vertex/draw/transfer work, no skipped draws and four native LCD captures.
Compare both OpenGL controls exactly, both Metal controls and candidate images
exactly, every retained guest state/work row and guest SD files. Preserve and
report the existing cross-renderer upper-screen differences; no tolerance or
channel permutation authorizes an exact-parity claim.

No scored run was started while APFS/background service admission was failing.
The source-only staging script originally assumed a single PublishBytesHost
scope; review found the copy-batch publication is a second scope. The preserved
original staging failure did not mutate runtime source or launch any native job.
Both publication sites are included in this reviewed source patch.
