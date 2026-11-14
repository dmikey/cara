# Private compact decoded-record prototype

This branch preserves the existing 20-byte format by default. `CARA_FASTINTERP_COMPACT_DECODED=ON` is an experimental compile option, not a measured speed improvement or a proposed default. It changes host decoded-code storage only; it does not change guest instructions, ticks, events, block limits, memory operations, or save-state fields.

The current enum has 299 semantic opcodes and 598 checked/AL dispatch entries. A compact record contains a four-byte header and the unchanged twelve-byte operand union:

| Field | Bits | Current requirements |
| --- | ---: | --- |
| Dispatch index | 10 | 0–597; compile-time bound permits at most 512 semantic opcodes |
| Original/effective byte size | 5 | Raw values 0, 2, 4, 8, 12, 16; no encoded-size dictionary or escape |
| Reserved | 1 | Zero on fresh decoded records |
| Condition | 4 | Complete original 0–15 range |
| ARM destination register | 4 | 0–15; VFP register numbers and operand sentinels remain in the union |
| Tick cost | 8 | Full original byte range; canonical VFP multiple transfers can charge 128 ticks |

The operand union remains at offset four with four-byte alignment. `DecodedInst` is 20/16 bytes and `BasicBlock` is 672/544 bytes on the tested ARM64 ABI. Blocks still have 31 real slots plus an EndBlock slot. Saving 128 bytes per block saves 4 MiB per core at 32K entries, with the same entry count, tag mapping, replacement policy, linking, and validation.

`GetOpcode()` returns semantic identity independently of the selected checked/AL dispatch bank. Every semantic write uses `SetOpcode()` immediately. In compact mode the setter resets the index to the semantic base bank without reading prior contents; existing `PrepareDispatch` calls finalize AL selection after cold decode, Step, and each hot-unroll rewrite. Intermediate decode, PC folding, fusion and flag analysis therefore never read a stale semantic opcode. No handler uses this accessor for hot dispatch: computed goto reads the encoded index directly. Fresh-record memset/copy behavior and union operand offsets remain intact. No header-address-taking or positional aggregate-initializer dependency was found in current consumers. Future fields/opcodes/fusions must respect the explicit bit bounds.

## Build and checks

Use a separate build directory for each layout. `tools/build.py mac --build-dir /absolute/Fast/path` explicitly selects the default 20-byte format; add `--compact-decoded` for 16 bytes. Direct CMake users can set `-DCARA_FASTINTERP_COMPACT_DECODED=OFF` or `ON`. A generated header under build/src applies the layout consistently to every consumer; conflicting explicit compiler definitions are rejected. Toggling the option rebuilds actual header consumers rather than changing every vendor compilation command. The standalone `cara-fastinterp-decoded-record-self-test` target checks field boundaries, semantic setters, dispatch banks, copies, union preservation and sentinel records. `CARA_FASTINTERP_DECODED_METADATA_TEST=1 cara-fastinterp-self-test` walks all 65,536 Thumb16 encodings, 131,072 deterministic ARM words, 131,072 deterministic Thumb32 words and 768 VFP boundary words, reporting a layout-independent semantic/operand trace.

Private ARM64 macOS O3 validation on 2026-11-13 used current interpreter/decoder/tick sources and copied native dependency/core archives. These are interpreter oracle executables, not complete native SDL or iOS applications. Both layouts compiled and linked. Address/undefined-behavior sanitizer field witnesses passed 1,396,931 checks for 20 bytes and 1,416,067 for 16 bytes (compact includes additional base-bank assertions). The new metadata walk reports 328,448 records, maximum tick cost 128, and the same trace `10461467696425395394` in both layouts.

The full paired native matrix passed 185/185 configurations (370 process runs), with byte-identical output and exit status for each pair. It includes the existing full interpreter oracle and every non-benchmark interpreter test CLI, 16K/32K capacities, checked/AL dispatch, secondary-link combinations, 4/8 ways, budget modes, clear/invalidation, Step/PC boundaries, BLX LR ordering, SVC/context-switch boundaries, conditional LDM-PC, short vectors, scalar-double arithmetic, retained load/fusion slots, unroll behavior and VFP fusion variants. Existing capacity/secondary tests retain their independent 75/100/133-percent timer/event checks. The initial new field fixture incorrectly treated the 20-byte dispatch index as its independent semantic opcode; correcting that fixture resolved its failure before the passing matrix. No production compile failure was observed.

Evidence is under `/Volumes/Fast/CaraBuild/profiles/decoded16-20261113`: `results.json`, `run.log`, per-layout field/compiler logs, `codegen.json`, and full assembly. The original-source 20-byte ExecuteBlock and accessor-based 20-byte control have identical normalized ARM64 O3 assembly: 11,976 instructions, 47,904 instruction bytes. Compact assembly has 12,427 instructions / 49,708 bytes, approximately 3.77% static growth, and 181 packed-index masks. It emits no semantic-getter calls in dispatch. Some individual tails combine tick extraction with addition and pointer post-increment, so static growth does not establish a dynamic cost or benefit. This is macOS non-LTO code generation, not iOS ThinLTO evidence.

## Save-state and performance gates

Decoded blocks are host cache objects and are not serialized. This prototype does not modify fingerprint generation or acceptance checks. A genuinely regenerated fingerprint may match between the two compile options for the exact same source/compiler/platform because current identity hashes source and platform, not this option; this must be observed from generated headers, never forced. Earlier-source checkpoints must continue to reject normally.

Fresh-process, generated-counter ELF cross-layout save/load (20→16 and 16→20), exact continued state/ticks, and actual complete-host compatibility are pending at this commit. No app or checkpoint was produced using a substituted fingerprint. Only after these gates and review should a fresh native checkpoint be created from the owner's ordinary game save for a matched game comparison. Existing game/user data and commercial assets are not part of this branch. No game speedup, iPad result, or default promotion is established.
