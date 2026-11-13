# Used vertex input staging

The default-off `CARA_GPU_USED_INPUTS=1` experiment derives a conservative input
register read mask from the existing PICA shader decompiler. It intersects that
mask with configured input registers before staging vertex payloads. Shader
branches and subroutine calls are analyzed independently of current uniform
values; failed analysis retains all configured inputs.

The bounded mask cache uses the existing dirty-tracked shader/swizzle identities,
entry point, output mapping and multiplication-sanitization configuration.
Selected masks participate in generated shader keys and layouts for normal,
direct, lit-pair and transform-feedback paths. Duplicate input mappings retain
the last configured attribute, and defaults retain their existing semantics.

This targets CPU staging, packing and reused-byte comparison. The existing shader
generator already avoids fetching unread registers on the GPU. Interleaved
attribute streams can still require nearly the same byte range because selected
attributes span the intervening stride gaps. Reduced register counts alone do
not prove reduced copied bytes or improved performance.

Frame reports expose staging attempts, configured/selected register totals and
draws with pruned inputs. These counters describe staging attempts; a later
shader rejection can still fall back to another path. The benchmark runner
selects the experiment with `used_input_attributes` and verifies the launched
mode, fixed guest clock, sound, backend, timing policy and final pixels.

## Validation

- Production-decompiler fixtures cover zero-input shaders, both uniform-branch
  arms, subroutines, indirect uniform addressing and analysis failure.
- Cache identity tests and 8,192 filtered last-wins mapping cases pass.
- The iPad GPU fixture packs a reduced payload while retaining nonzero omitted
  inputs in the full interpreter reference. Dense, indexed, packed, direct and
  feedback modes pass; lit-pair coverage is available with its existing flag.
- Native bounded upload ranges pass 256 byte-coverage cases, capacity fallback
  and address-edge checks. The iOS build and no-JIT symbol audit pass.

The original Stories Slot 2 and its matching app are preserved. Its source-build
fingerprint differs from this candidate, so the candidate comparison must use a
replayed, newly saved checkpoint. It is not an exact original Slot 2 replay.
Performance results are recorded separately from these correctness checks.

## Stories iPad result

Build `313b3a4b6bd80cc25f9eaeae667a6d5d859c29a3ce7c802972ef1dedb55c4385`, recreated Secret Path checkpoint, 1,200 refreshes per run, first 300 discarded. Fixed 100% guest CPU, sound enabled, no JIT, existing hardware timing unchanged. Order: baseline, candidate, candidate, baseline.

| Run | ms/refresh | Emulation thread CPU ms/refresh |
| --- | ---: | ---: |
| baseline_a | 20.719 | 18.906 |
| used_inputs_a | 20.744 | 18.933 |
| used_inputs_b | 20.763 | 18.943 |
| baseline_b | 20.764 | 18.936 |

Baseline mean 20.742 ms versus candidate 20.753 ms: **no measured gain**. All four final RGBA hashes and GPU draw/vertex workload totals match. Configured input register totals fall by 14.01%, but staged bytes (uploaded plus byte-identical reused payloads) remain 18,232,717 bytes per refresh. Existing interleaved range/row layout prevents the register reduction from reducing payload size in this scene. The experiment remains off by default.

Audio stayed enabled and running, with no output error, but underrun callbacks remain; this is not a claim of smooth audio at the measured sub-full speed. The new checkpoint visually matches the original Slot 2 position/camera but differs in saved execution state and final pixels from that older-build replay. It cannot establish a before/after gain for unrelated changes between builds.
