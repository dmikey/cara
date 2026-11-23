# FCRAM watch retirement experiments

Native Metal marks physical memory pages so guest CPU accesses enter the normal
rasterizer coherency callbacks. Historically, these watches survived partial
invalidations until a global clear or shutdown, even if all page dependencies
had disappeared. Saving state performs a global rasterizer clear; comparisons
that save within a normal-boot measurement can consequently change watch history.
This establishes a possible measurement confound, not its performance magnitude.

Two independent flags default off:

- `CARA_METAL_RELEASE_INVALIDATED_FCRAM_PAGES=1` releases watched FCRAM pages
  wholly covered by a successful invalidation. It keeps the final page if its
  following page outside the invalidation is still watched.
- `CARA_METAL_RELEASE_OWNERLESS_FCRAM_PAGES=1` examines only touched FCRAM pages,
  including partial scalar invalidations. `CanReleaseFcramWatchPage` requires no
  native authority, resident color/depth attachment, or pending draw attachment
  anywhere in that page or its successor. Any deferred texture-copy batch blocks
  release conservatively. Authority lookup validates surviving producer status.
  The query performs no GPU wait or additional submission retirement poll.

Both paths run only after `InvalidateGuest` succeeds. GPU-written buffers are
independent of guest RAM, so retiring an invalidated producer does not permit a
later GPU command to overwrite the guest's new CPU bytes. Native command/resource
lifetimes remain unchanged. Partial unmarking clears all covered-range shortcuts
so a future GPU write restores the page-table barrier. VRAM watches, including
tracked decoded-texture watches, remain unchanged. Flush alone does not retire
watches because materialized native ownership remains useful to GPU consumers.

`AppendNativeMetalWatchProfile(stream, json)` exports checked/released/proven
ownerless query counts and current watch counts. Cost profiling enables read-only
candidate queries even with both release flags off; these diagnostic runs include
that extra query work and must not be represented as clean performance controls.
Normal flag-off operation does not walk the watch set; current counts are scanned
only when explicitly requesting a snapshot.

## Regression fixture

```sh
python3 tests/run_metal_fcram_watch_release_test.py
```

The fixture includes the production NativeRasterizer and installs it through the
normal VideoCore factory, using an actual MemorySystem/page table, native Metal
fills, CPU reads/writes and gated GPU producers. Four fresh processes exercise
both flags independently and together under Metal API Validation. Cases cover
partial surviving ownership, ownerless scalar invalidation, next-page guard,
restored direct CPU pointers, later GPU re-marking, pending producer invalidation,
read-only resident attachment outside the overwritten bytes, unsubmitted draw
attachments, deferred-copy batch, VRAM preservation and global clear. It does not
claim to reproduce real driver failure or prove scene performance. Results are
recorded under `work/portable-metal-checks/fcram-watch-release/`.
