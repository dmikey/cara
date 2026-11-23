# Untracked CPU upload sources (experimental)

`CARA_METAL_UNTRACKED_UPLOAD_SOURCES=1` opts the shared native Metal upload
arena's backing buffers and linear texture views, and the immutable vertex-input
cache's backing buffers and views, out of automatic GPU hazard tracking. The
flag defaults off and changes no public header or serialized state. Compute
outputs, argument buffers, render attachments, material textures and transfers
keep their existing tracking modes.

The current production consumers only read these uploads on the GPU. Arena
allocations remain disjoint within a command lease. CPU reuse still requires the
previous command to complete; failed commands, cancellation and submission keep
the original checks. Cached input textures are independent, immutable snapshots;
full source equality and retained command references remain required. This is a
call-graph contract: the public Range buffer itself is not type-enforced read-only.

Apple permits concurrent resource reads without a write conflict, and explains
that untracked mode transfers synchronization responsibility to the application.
The existing CPU/command lease synchronization is therefore essential:

- [Resource synchronization](https://developer.apple.com/documentation/metal/resource-synchronization)
- [Untracked resources](https://developer.apple.com/documentation/metal/mtlhazardtrackingmode/untracked)

This is a bookkeeping experiment, with no performance claim. Apple also documents
serialization between nonoverlapping texture views sharing one buffer. It does
not say untracked mode removes that serialization. Argument-buffer resource
access already has different tracking rules:

- [Buffer-backed textures](https://developer.apple.com/documentation/metal/mtlbuffer/maketexture(descriptor:offset:bytesperrow:))
- [Resource fundamentals](https://developer.apple.com/documentation/metal/resource-fundamentals)

## Validation

Run outside timing measurements:

```sh
python3 tests/run_metal_untracked_upload_test.py
```

On the local Apple M3 Pro, all four fresh processes (tracked/untracked × immutable
input reuse off/on) passed with Metal API Validation enabled. The fixture links
the production upload implementation and checks actual resource modes, exact GPU
compute results for R16Uint/RGBA32Uint/RGBA32Float with padded source rows, direct
vertex-buffer raster output, twelve command leases with three initially gated
pending reads, page rotation, cache eviction of already-encoded sources before
pending GPU reads, cancellation and source mutation. GPU output buffers remain
tracked. Results are recorded in
`work/portable-metal-checks/untracked-upload/results.json`.

The fixture does not establish speed, cross-device performance, driver-fault
recovery or every production deferred-compute shader path. Real-scene output and
matched performance controls remain necessary before any shared-policy promotion.
