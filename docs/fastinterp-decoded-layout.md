# Optional compact interpreter records

`CARA_FASTINTERP_COMPACT_DECODED` is a **default-off** compile option. It reduces host decoded-instruction storage from 20 to 16 bytes. It changes no guest instruction, tick, event, memory operation, block boundary, or serialized guest field. A small native M1 Stories improvement is documented below; iPad performance remains unverified. The smaller record also has code-generation costs.

## Representation and invariants

The unchanged twelve-byte operand union stays at offset four with four-byte alignment. Its ARM/VFP operands, immediate sentinels and retained fusion records remain intact. A compact four-byte header holds:

| Field | Bits | Required range |
| --- | ---: | --- |
| Dispatch index | 10 | Current 299 semantic opcodes × two checked/AL banks: 0–597 |
| Effective byte size | 5 | Raw 0, 2, 4, 8, 12, 16; no dictionary or escape |
| Reserved | 1 | Zero on fresh decoded records |
| Condition | 4 | 0–15 |
| ARM destination register | 4 | 0–15; wider VFP indices remain in the operand union |
| Tick cost | 8 | Complete original byte range; canonical VFP transfers can charge 128 |

`GetOpcode()` returns semantic identity from either dispatch bank. Every semantic write uses `SetOpcode()` immediately; it resets compact records to the semantic base index without reading prior contents. `PrepareDispatch` selects the effective AL bank after cold decode, Step and hot rewrites. Intermediate PC folding, fusion and flag analysis therefore see the current semantic opcode. Hot computed-goto dispatch reads the encoded index directly, without calling the semantic accessor.

Both formats remain trivially copyable and preserve zeroing/copy behavior. No current header-address-taking or positional aggregate-initializer dependency was found. Compile-time checks retain the union offset, record size/alignment, and dispatch capacity. Future opcodes or fusions must remain within the explicit bounds. Blocks still contain 31 real slots and one reserved EndBlock slot; fusion skips the same retained slots and charges the same ticks.

On the verified 64-bit ABIs, `BasicBlock` shrinks from 672 to 544 bytes. The 128-byte reduction saves 4 MiB per core at 32K entries. Entry count, set mapping, replacement, linking and invalidation policy do not change.

## Build and ongoing checks

Use separate build directories. `tools/build.py mac` explicitly selects the ordinary 20-byte format; add `--compact-decoded` for 16 bytes. Direct CMake users can set `-DCARA_FASTINTERP_COMPACT_DECODED=OFF` or `ON`. A generated `build/src/fastinterp_decoded_layout_config.h` consistently selects the format for actual consumers, rejects a conflicting explicit compiler definition, and avoids changing every vendor compilation command when toggled.

`cara-fastinterp-decoded-record-self-test` checks field boundaries, semantic setters, dispatch banks, copies, union preservation and sentinel records. `CARA_FASTINTERP_DECODED_METADATA_TEST=1 cara-fastinterp-self-test` walks every Thumb16 encoding, 131,072 deterministic ARM words, 131,072 deterministic Thumb32 words and 768 VFP boundary words: 328,448 records with a layout-independent semantic/operand trace.

The separate `compact-interpreter` macOS CI job builds with pinned CMake/Ninja, the explicit compact option and no JIT, audits the linked probe, and requires the actual record witness to report `record=16 block=544`. It runs the full interpreter oracle, metadata walk, and independent AL/BLX/PC, VFP/VLDR, cold-cache, conditional-return, SVC/unroll and clock/event-budget checks. It also genuinely reconfigures the same build to the reference format, requires its actual 20/672 layout, and compares the complete metadata trace before restoring and re-auditing compact mode. It does not compare unrelated executions across jobs. The ordinary build matrix retains default-off coverage. The workflow is configured to check the integrated source; current run results remain available in GitHub Actions.

## Completed evidence and its boundaries

The original implementation was frozen as `d77304e`. Private ARM64 macOS O3 builds passed sanitizer field witnesses: 1,396,931 checks for 20 bytes and 1,416,067 for 16 bytes. The extra compact checks assert immediate base-bank identity. The metadata walk matched trace `10461467696425395394`, with maximum tick cost 128. An initial new fixture incorrectly treated the ordinary dispatch index as its independently stored semantic opcode; that fixture was corrected before the passing runs.

A 185-pair native matrix passed with identical output and exit status for every pair. It covered the full interpreter oracle, all non-benchmark interpreter test CLIs, 16K/32K capacities, checked/AL and secondary-link combinations, 4/8 ways, clear/invalidation, Step, PC/BLX boundaries, conditional LDM-PC, SVC/context switches, short vectors, scalar-double arithmetic, retained slots and fusion/unroll variants. Existing capacity/secondary tests retain independent 75/100/133-percent timer/event expectations. The matrix passed again against complete genuine CMake native builds on source `33a87a1`, rather than only the earlier copied-archive oracle executables.

Mac O3 assembly also establishes a countereffect: the original-source and accessor-based 20-byte ExecuteBlock have identical normalized assembly (11,976 instructions / 47,904 instruction bytes), while compact has 12,427 instructions / 49,708 bytes, approximately 3.77% static growth. It adds packed-index masks and no semantic-getter calls in dispatch. Some individual tails combine header extraction, addition and pointer advancement differently. These static counts establish neither dynamic cost nor a win, and are not iOS ThinLTO evidence.

Fresh-process counter checkpoint testing exposed a pre-existing guest DSP padding bug, including nondeterminism in same-layout repeats. The independently reviewed [DSP status initialization correction](dsp-status-determinism.md) was applied equally to both configurations. On common source `6e86769`, both genuinely generated fingerprint `cd68ca8a6e5567d402e7c0642d9bf4441e496b9deb8c1fa894c001605841db24`.

Six real SDL processes then created fresh original-counter checkpoints and tested both cross-layout directions with same-layout controls. Actual settings receipts show **guest software rasterization** (`use_hw_renderer=false`, `use_hw_shader=false`), with CPU/shader JIT disabled. The SDL hosts had their normal GL context and presentation path; this was not hardware-rendered game validation. Registers, clocks and ticks matched after load and thirty-refresh continuation. Each pair's entire 156,063,547-byte decompressed archive, including guest RAM, DSP buffers and scheduled state, compared byte-for-byte equal; compressed payloads also matched. No internal bytes were ignored or normalized. Save requests ran after RunLoop returned. Host refresh numbering resets during state-load reinitialization, while guest ticks restore exactly. Both applications normally rejected untouched earlier-fingerprint checkpoints.

Private M1 receipts are under `profiles/decoded16-20261113/`: `results-native.json`, `codegen.json`, `native-build-receipt.json`, and `cross-state-dsp-initialized/{results,full-state-byte-parity,old-state-rejection}.json`. The standalone DSP note retains the typed diagnosis and its separate regression evidence.

## Integration and performance gate

Decoded blocks are host cache objects and are not serialized. Fingerprint generation and acceptance checks are unchanged. The same source/compiler/platform can naturally generate matching identities across these options under the current identity scheme; this must be observed from generated headers, never forced. Older-source checkpoints continue to reject normally.

The integration combines the optional implementation with main `0ef5f1c`, retaining its LUT and DSP fixes. The native counter and matched Stories source did not include the later LUT residency correction. The CI workflow checks both formats in the combined source; those earlier local receipts do not independently establish the combined source's hardware-rendered game behavior.

A matched M1 Stories bracket measured a small reduction of 0.5591 ms of emulation-thread CPU per refresh (1.13%), with matching captured state and pixels; see [the complete measurement and its limits](native-decoded-layout-measurement-20261113.md). The checkpoint came from normal New Game progression, not an old emulator state or a usable in-game save.

Physical Apple GLES validation remains necessary before default promotion. Game comparisons must use verified effective renderer/clock settings, matching actual executables and a fresh normally-created checkpoint. No physical-iPad result or broader compatibility claim is established here.
