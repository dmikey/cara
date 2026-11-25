# Validated RG Slide reuse defaults

The existing packed-layout uniform cache, tracked raster-state comparison and packed-fetch decoder are enabled automatically only for the tested **RG Slide / Unisoc T820 / Mali-G57** direct GLES Atlas Performance path. The exact device/vendor/renderer predicate is shared with the mainlined persistent-UBO policy. UBO support and resolved UBO enablement are required. Automatic state reuse requires layout reuse, and automatic packed fetch requires both. Each existing Debug true/false override remains available; malformed explicit values fail closed. Accuracy mode, ordered-worker paths and other GPUs retain their previous defaults.

The implementations were already in main. This change selects their tested combination and corrects process-lifetime policy caching: the layout flag belongs to each renderer, and the raster flag is reset explicitly when constructing a renderer. Ordinary raster invalidation does not reread environment variables, so per-draw/copy invalidations do not introduce per-draw getenv work. Shader archive identity already includes packed-fetch policy before decode. Program-specific layout entries are renderer-owned and cannot survive reconstruction.

Same-seed physical results, measured separately and **not additive**:

- Layout cache: graphics CPU 24.088153 → 23.695931 ms/ref, −0.392222 ms (1.63%); tails mixed.
- Raster state, with UBO/layout on: graphics CPU 23.645806 → 22.232708 ms/ref, −1.413097 ms (5.98%); late median did not clear drift.
- Packed fetch, with UBO/state/layout on: late median 33.915 → 32.570 ms, −1.345 ms (3.97%); early/middle medians did not improve. Graphics CPU did not fall. Separate GPU active cycles per primitive improved 2.57–3.49% in that exact workload.

These are instrumented Gadd lab windows at JIT, fixed CLK100%, 1× and frame skip off. Both panels were visually checked; exact frame-aligned intermediate pixel parity is not established. Different genuine seeds and policy stages must never be compared as an overall 13–15% improvement. None establishes full speed or a universal title gain.

The **Unisoc T820** is the SoC, with **Mali-G57** GPU; this is not the older Mali-T820 GPU. Logitech G Cloud / Adreno is a separate physical target. No Mali performance verdict or driver cache is transplanted to Adreno. Hardware counters reported against 850 MHz are rated-clock normalization, not live GPU-clock telemetry; inaccessible live clocks remain disclosed in the original reports.

Rejected or private-only work remains preserved without activation: native attributes with compact quaternion geometry (warm regression), full-color fill coalescing (no consistent warm gain), FBO lookup, retained bindings, VS uniform reuse and zero-tail comparison. The zero-tail comparison had a small CPU result on a distinct genuine seed but is a new renderer algorithm still default-off/private; it is not silently bundled with these existing reuse defaults. Full-fill source is preserved as an investigation patch and is not compiled into this mainline change.

The companion historical reports retain exact scopes, source/binary/state identities, controls, failed attempts and limitations. Local full raw outputs remain in `outputs/cara-slide-*-20261125`. `gadd-validated-defaults-receipt.json` binds the copied reports and candidate patches. No save files, ROM contents, shader dump assets or private signing material are committed.
