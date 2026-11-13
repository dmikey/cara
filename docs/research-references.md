# Research references

Cara builds on Citra, Citra MMJ and Azahar FastInterp. Their code provenance and
licenses are recorded in [NOTICE](../NOTICE) and
[source provenance](../emulator/VENDORED_SOURCE.json). The following additional
projects and publications inform development research. A research reference is
not a claim that its implementation was imported or its performance reproduced.

| Reference and contributors | Contribution to the investigation | Source review |
| --- | --- | --- |
| Dolphin Emulator Project | Cached interpretation, proven idle recognition, GPU staging and synchronization. | [Performance prior art](performance-prior-art.md) |
| PPSSPP / Henrik Rydgård and contributors | Interpreter dispatch and optimization passes, deferred draws, upload buffering and batching boundaries. | [Performance prior art](performance-prior-art.md) |
| Panda3DS contributors | Native vertex-attribute transport, index rebasing and accelerated renderer comparisons. | [Performance prior art](performance-prior-art.md) |
| Mikage contributors | Explicit resource ownership, guest memory hooks and fence handling, including documented incomplete paths. | [GPU residency review](gpu-residency-prior-art.md) |
| V8 authors / DrumBrake | Static interpreter handlers, superinstructions and register modes. | [CPU interpreter prior art](performance-prior-art.md#cpu-interpreter-prior-art-and-bounded-candidates) |
| fearkov / 3dsrecomp and Zakuro | Guest-image inspection, verification approaches, vertex input planning and GPU command research. | [Pinned source comparison](zakuro-recomp-prior-art.md) |
| Nguyen Anh Quynh, COSEINC and Capstone contributors | Offline ARM/Thumb disassembly for private guest-code inspection. | [Tool versions and credits](inspection-tool-attributions.md) |
| DMP / PICA200 publications, including Kazakov and Ohbuchi | Vertex/primitive pipelines, caches, lighting circuitry and memory interfaces. | [Hardware source audit](hardware-source-audit.md) |
| 3dbrew community, GBATEK / Martin Korth, JayFoxRox and 3ds-tests contributors | Reverse-engineered registers, interrupt behavior and hardware correctness-test approaches. | [Hardware source audit](hardware-source-audit.md) |
| Apple documentation | GLES context ownership, buffered resources and synchronization guidance. | [Performance prior art](performance-prior-art.md) |
| iFixit and Nintendo-submitted FCC photographs | Physical board evidence, with explicit limits on what photographs establish. | [Hardware source audit](hardware-source-audit.md) |

The linked reviews carry primary-source links, pinned revisions where recorded,
measurement conditions and limitations. Public hardware material has not supplied
a calibrated retail-console GPU completion model. Emulator source review, physical
board evidence, interpreter-reference tests and real-hardware measurements remain
distinct forms of evidence.

Cara's current findings are collected in the
[Stories dependency trace](stories-gpu-trace-2026-11-13.md),
[tick-poll audit](stories-tick-poll-audit.md),
[GPU alias results](gpu-surface-aliases.md),
[host pacing results](frame-pacing.md), and
[used-input staging experiment](vertex-input-staging.md).
