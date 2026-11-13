# Public hardware source audit

Verified 2026-11-13. This record distinguishes vendor architecture material,
physical board photographs, reverse-engineered register behavior, and new
measurements still needed. No retail-console timing experiment was performed
for this audit. Downloaded source PDFs and photographs are not repository assets.

## Architecture and physical evidence

| Source and inspected location | What it establishes | What it does not establish |
| --- | --- | --- |
| [DMP adoption announcement, 2010-06-21](https://www.dmprof.com/en/news/dmp-3d-graphics-ip-core-pica200-is-adopted-by-nintendo-3ds/) | The IP vendor identifies PICA200 and MAESTRO technology as adopted by Nintendo 3DS. | The exact CTR implementation, enabled options, bus width, operating clock, or completion latency. |
| [DMP PICA200 brochure, archived 2010-07-04](https://web.archive.org/web/20100704204015id_/http://www.dmprof.com/release/leaflet_PICA200_en.pdf), one-page sheet, lower specification box | Vendor-advertised IP maxima of 15.3 million polygons/s and 800 million pixels/s at 200 MHz; render-to-texture, depth/stencil and single/double/triple buffering are listed. | Sustained retail CTR throughput, a universal cost per draw, or a three-job hardware queue. These are IP marketing maxima without a reproduced workload. |
| [Kazakov and Ohbuchi, DMP, HPG 2011 presentation](https://www.highperformancegraphics.org/previous/www_2011/media/Papers/HPG2011_Papers_Kazakov.pdf), PDF pp. 5-6 and 10 | The figures show programmable vertex processing, a post-transform vertex cache, Primitive Engine, rasterizer, texture access, fixed fragment circuitry, and SoC memory-bus interfaces. Page 10 locates on-chip lighting LUTs beside texture reads and color-buffer read/write traffic. | The exact shipping CTR configuration, memory arbitration, command admission limits, IRQ visibility point, or host-GPU execution time. |
| [Eisaku Ohbuchi, DMP hardware engineer, 2007-07-13](https://www.eetimes.com/3d-graphics-hardware-ip-uses-ocp-bus-interface/), "Hardware block diagram", Figure 7 and Table 1 discussion | DMP describes OCP bus interfaces and customer-dependent vertex/texture-pipeline counts, cache parameters, bandwidth and functionality. This directly cautions against treating the IP family as one fixed configuration. | Proof that Nintendo's internal bus uses a particular OCP configuration. The article text was inspected; its linked Figure 7/Table 1 image downloads were unavailable. |
| [iFixit Nintendo 3DS teardown, 2011-03-03](https://www.ifixit.com/Teardown/Nintendo+3DS+Teardown/5029), step 14 board photograph and package identifications | The photographed CTR board has separate Nintendo-branded processor and Fujitsu memory packages. The photograph shows board routing and physical integration; the teardown identifies Nintendo 1048 0H and MB82M8080-07L. | Internal SoC topology, BGA pin connectivity, memory-bus width, GPU clock, cache policy, or completion timing. A board photograph is not a timing schematic. |
| [Nintendo FCC KTR-001 internal-photo exhibit 2399322](https://fccid.io/BKEKTR001/Internal-Photos/Short-Term-Confidential-Internal-Photo-2399322.pdf), PDF pp. 2-3, printed 2/9-3/9 | Nintendo-submitted photographs show the KTR mainboard's two sides, package placement, scale rulers and Wi-Fi/NFC annotations. This is a different model from the original CTR teardown. | A GPU pipeline diagram or electrical schematic. The [exhibit record](https://fccid.io/BKEKTR001/Internal-Photos/Short-Term-Confidential-Internal-Photo-2399322) exposes photographs but lists schematics and block diagrams as confidential metadata only. No public Nintendo GPU timing schematic was obtained. |

The DMP HPG presentation's page 10 describes a long fixed fragment pipeline and
configuration-dependent clocks per fragment/light. That is evidence for
work-dependent pipeline cost, not a calibrated CTR completion equation. Its
statement about avoiding external memory traffic concerns on-chip material LUTs;
the same figure explicitly shows texture reads and color read/write traffic.

## Clock domains and observable interfaces

[libctru os.h at revision 62b7c42c88b1ee837f513358409d2e5c8f9001a9](https://github.com/devkitPro/libctru/blob/62b7c42c88b1ee837f513358409d2e5c8f9001a9/libctru/include/3ds/os.h#L8-L26)
defines a 16,756,991 Hz external SoC clock, the CTR ARM11/system-tick rate as
16 times that value (268,111,856 Hz), and distinct ARM11 LGR multipliers. Its
comment specifically associates `svcGetSystemTick` with the base rate. This is
public homebrew interface documentation, not a newly measured crystal frequency
for every board. Record system ticks and CPU execution cycles separately when
changing CPU mode; do not multiply GPU completion deadlines by the New 3DS CPU
multiplier merely because the application cores run faster.

[3dbrew PDN research, pinned mirror revision 56ca8f176c59e52291b80f1fb4d5bac4a9455137](https://github.com/mikage-emu/3dbrew/blob/56ca8f176c59e52291b80f1fb4d5bac4a9455137/content/PDN_Registers.md)
([readable rendering](https://docs.mikage.app/PDN_Registers/), "Clock and reset
registers") describes GPU/VRAM/LCD clock gating and separate FCRAM controls;
some reset-bit interpretations remain tentative. This supports distinguishing
subsystems, but does not prove that every block has an independent oscillator,
clock rate, or execution queue.

The [3dbrew external-register research](https://docs.mikage.app/GPU/External_Registers/)
separately identifies PSC memory-fill units, PPF transfers, P3D commands, PDC
display controllers, busy bits, framebuffer-selection registers and LCD timing.
Its PDC timing description derives refresh from pixel-clock and horizontal and
vertical totals, and describes framebuffer selection after VBlank. Display
refresh therefore is not the throughput or completion period of individual PICA
jobs. Multiple render passes, transfers and submissions can occur within one
scanout interval. Nothing inspected implies that work older than 16.7 ms may be
discarded.

## IRQ semantics and limits of the evidence

[GBATEK](https://problemkaputt.de/gbatek.htm#3dsgpuinternalregistersfinalizeinterruptregisters),
"3DS GPU Internal Registers - Finalize Interrupt registers", documents
REQ/ACK/CMP byte comparisons, IRQ masks and AUTOSTOP. It reports that command
lists write REQ, that CPU reads of REQ wait while the GPU is busy, and that
AUTOSTOP can stop a list on a matching enabled request. Its command-list section
explicitly leaves the purpose/concurrency of the two command-buffer banks
uncertain. These are reverse-engineering findings to test, not a vendor timing
specification. They establish no numeric IRQ latency or hardware FIFO capacity.

A command-list IRQ request can be an explicit command-stream operation. Its
meaning must include the mask/comparison/autostop state and ordering against
prior rendering and transfers. Do not equate command parsing, IRQ enqueue,
guest-event signaling, host draw submission, host GPU completion and presentation
without evidence joining those stages.

## Hardware tests and next measurements

[JayFoxRox/3ds-tests, revision 9728f7e2da6c4ff4e4d0f850af9d5f41795235b4](https://github.com/JayFoxRox/3ds-tests/tree/9728f7e2da6c4ff4e4d0f850af9d5f41795235b4)
is first-party test code intended to validate 3DS hardware findings. The inspected
[buffer-allow test](https://github.com/JayFoxRox/3ds-tests/blob/9728f7e2da6c4ff4e4d0f850af9d5f41795235b4/buffer-allow/source/main.c)
varies color/depth/stencil buffer permissions, and its
[frame helper](https://github.com/JayFoxRox/3ds-tests/blob/9728f7e2da6c4ff4e4d0f850af9d5f41795235b4/frame.h)
dumps GPU buffer contents. Those files are useful correctness-test prior art;
they do not supply calibrated submission-to-IRQ latency distributions or a
hardware queue-capacity result.

The following tests would constrain a timing model without using game-specific
rules:

1. On retail hardware, measure minimal list, vertex-heavy, fragment-heavy,
   light/LUT-heavy and transfer-heavy workloads separately. Vary one dimension
   at a time; retain shader, format, stride, tiling and memory-placement identity.
   Capture submit, busy-state observations, IRQ/event arrival and first safe
   CPU observation of output bytes using a named tick domain.
2. Vary IRQ request placement, mask/comparison values and AUTOSTOP with two
   distinguishable output markers around the request. Determine which work must
   be visible at IRQ and whether execution continues. A count of command banks
   or display buffers cannot substitute for this test.
3. Test consecutive submissions and P3D-to-PPF dependencies; observe admission,
   busy states, completion order and output correctness before inferring a queue
   limit. Keep GSP software command-ring slots separate from hardware capacity.
4. For the current emulator, first collect a bounded ordered trace joining GX
   admission, command-list execution, IRQ relay, guest wait, scheduler resumption
   and PDC/VBlank. This can show whether extra Stories submissions are completion
   driven. The paused scene's lower draw count alone cannot show which rendering
   work is redundant or dispensable.

Any candidate must preserve required reads, writes, alias ownership, interrupt
ordering and guest CPU execution at the requested clock. The host target's M4
GLES driver costs and no-JIT interpreter costs are independent measurements;
physical PICA pipeline diagrams cannot predict them or guarantee full speed.

## Download identity

The inspected PDFs have these SHA-256 hashes, allowing a future audit to confirm
that the figure/page references identify the same files:

| Source | SHA-256 |
| --- | --- |
| DMP HPG 2011 presentation | `6cab39a4a65ff150b8b3cbcecda1f80a4b8abbc2360f8b0f29fe3d18551ab1a8` |
| Archived DMP PICA200 brochure | `36176d23856a43a3a36ff963aeaa0614a9c849ec6dea66f622d3fbce4dc215a4` |
| Nintendo FCC exhibit 2399322 | `0977ef9234e658933cbc924a569b10b8a2e754ecf7b182dfc9e5a5faf3db3660` |
