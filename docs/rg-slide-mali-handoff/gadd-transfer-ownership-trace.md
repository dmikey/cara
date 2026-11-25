> Historical physical experiment receipt, preserved 2026-11-25. Original private/default statements describe that experiment. See `gadd-validated-defaults.md` for the mainline policy and current scope.

# Original Gadd attachment and transfer observation

The trace identifies a concrete required-fill dependency and a safe-looking materialization opportunity. It does **not** count internal Mali render passes or establish a new speedup.

## Actual build and run

Private JNI-only build7e84146b, APK SHA1b58958c8f6f0b3268f74fc0628ea5a9d6bd9a7faab41d723014a21709300e72. Renderer/core source bytes equal retained62b648c2. Actual original engine fingerprinta0f5ed remains in ELF and the unchanged genuine original4e3e2041 save loaded successfully. No header modification or forced fingerprint acceptance. Guarded build42.48s,78proper-host app-quiet checks,2workers, process reaped. Prior source254849df/APK preserved.

Physical RG Slide, JIT, CLK100%,1×, UBO/state/layout/packed-fetch reuse ON, frame skip OFF. Existing draw mask trace, synchronous GLES debug and bounded copy/GPU observers enabled. This is an intrusive diagnostic, not a timed comparison. The original guest/staging/draw/LUT/index work counters match all selected original control windows exactly (`WORK-CHECK.json` PASS). Synchronous GLES observers installed on boot and restored renderer contexts; no GL error messages were emitted. Both actual screens were opened and visually reviewed (lit ghost lab, expected maroon lower panel), not exact guest-aligned pixel parity. Normal Exit Game was selected from the observed private menu; that ran the rasterizer destructor and wrote the post-load copy trace before owned process cleanup. GPU trace and copy JSON were copied out. Main player unchanged; prior private active profile/cache preserved under `renderpass-diagnostic-active-before` while the next candidate is prepared.

## Admission and limits

GPU trace4110records, complete/untruncated, foreign events0, capture generation2. Copy JSON21returned operations,5,255,168guest bytes across VideoCore frames719–720. No copy detail/bucket overflow, unknown identities, counter saturation or timing failures. Framebuffer census821requests has bounded128detail slots, so its693overflow calls are aggregate-only. Feedback-copy census recorded0calls. The copy observer records returned ValidateSurface CopySurface operations, not every backend transfer.

The frame counters differ: all valid copy-frame720 draw anchors point into GPU trace-frame719. First copy-frame719 events predate the active GPU capture and have unknown joins. Thus only the second copy frame has complete in-window draw/GX joins; never merge the frame clocks by identical number. Execution-owner attribution is unavailable on these JIT joins even where draw/GX IDs are valid. No regenerated capture or fabricated join. See `COPY-DEPENDENCIES.json`.

The original private build compiles CARA_GL_CONTEXT_DISPATCH off. Remap-owner hooks are inside that conditional, so exposing their JNI logging does not create actual remap-owner data. No claims about remap format coverage or texel-pair eligibility follow. A new wrapper build solely for those counts was deferred in favor of the actual observed fill opportunity.

## Attachments versus passes

Frame720 logs461accelerated draw records and55renderer attachment-identity changes. Guest output addresses have only3segments:1draw to main color/depth,24draws to offscreen color/depth, then436draws to main again. Different host views of the same guest memory can change identity. These are not55measured driver render passes. Separable shader stage changes are not fully captured by this old mask's program/pipeline field; zero shader mask changes must not be read as one shader. The conservative feedback bit is likewise not evidence of actual feedback copies (the copy census was zero).

Framebuffer census found821compatible proposals and zero alternate selections, so extending the old paired-FBO cache is not justified from this capture. See `DRAW-ATTACHMENTS.json`.

## Required fill dependency

Every one of the21returned operations is a fill materialization. Aggregate host-return scope3.065ms and thread CPU1.347ms over2frames include setup, attachment changes, clear and any inherited GPU/driver backlog. They are not GPU completion times or isolated glClearBuffer timing.

The largest individual scope is1.774mswall/.081msCPU clearing sampled texture0x183e8000 before the first lit offscreen draw. Valid draw anchor295 (GPU frame719 /VideoCore720), VS entry366, writes color0x183a8000/depth0x18368000 and immediately samples0x183e8000. That texture's new contents must exist before the draw. Moving its wait downstream would not remove the dependency.

Seven fills cover complete attachments; all complete-fill scopes sum2.181mswall/.496msCPU over2frames. Other main color/depth fills cover400rows of a256×416view. Two later operations per refresh materialize the16-row color tail separately.

## Candidate derived from ownership, not guesswork

At each initial400-row color materialization, existing independent observer fields say the selected Fill contains the entire destination interval and `IsRegionValid(full)` is true. The same source no longer validates the full destination after actual draws. The opportunity is to materialize/publish the authoritative full color interval at that initial point, preserving every value and eliminating its later redundant tail clear. This is stronger than merely moving a wait or reusing an FBO name.

The source proposal restricts this to the exact same source/destination interval, registered noncustom mip0 1×RGBA8 color, fully invalid destination, normal CanFill(full) and source-full-valid proofs, and a full-width initial rectangle. It uses the existing real FillSurface implementation, unchanged scissor behavior, and ordinary notify_validated on exactly the interval actually cleared. No guest draw, fill, effect, byte ownership or timing charge is dropped. Unsupported or stale cases retain the old path.

Source and a complete-buffer/later-write oracle are staged separately under `outputs/cara-slide-whole-fill-20261125`. This is still **unbuilt/unmeasured**; actual activation, whole-workload performance and screens remain to be tested with a genuine matching new fingerprint seed. All failures and original artifacts are preserved.
