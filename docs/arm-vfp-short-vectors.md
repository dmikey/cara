# ARM11 VFP short-vector execution

## Hardware behavior

The ARM11 VFP extension uses FPSCR LEN and STRIDE to execute short vectors over
register banks. Scalar-bank destinations still execute once. Vector source
traversal, bank wrap and scalar-source broadcast follow the instruction's bank
rules; four independent scalar instructions cannot be substituted blindly.
Guest rounding, default-NaN, flush-to-zero and cumulative exception state also
apply while this mode is active.

Primary references: [Arm VFP11 TRM, DDI0274G, Table 3-7](https://documentation-service.arm.com/static/5e8e21eb88295d1e18d375b0)
and [Arm assembler register-bank guide](https://support.arm.com/documentation/dui0473/c/neon-and-vfp-programming/vectors-in-the-vfp-extension-register-bank).
Reserved or register-repeating length/stride combinations are outside the
validated hardware cases.

## Sonic Boom: Fire & Ice finding

A native M1 trace over 2,400 refreshes observed 212,286 executed VMSR writes;
106,137 enabled LEN=3 (four elements), STRIDE=0, with nearest rounding. These
writes also enabled default-NaN and flush-to-zero. A second bounded witness
confirmed actual vector-bank arithmetic, including VMUL into S8/S12/S16/S20
from S24 and bank-zero sources, followed by VMLA from S28. This was an executed
CPU feature, not an inference from a title-specific setting.

FastInterp previously stored FPSCR but its arithmetic handlers updated only a
single register. Native software output collapsed into a narrow strip or black
framebuffers. Both CPU-vertex and GPU-vertex iOS GLES tests ended black after
2,400 refreshes, with the same 71,283 draw requests. Native and simulator RTC
settings differed in the initial runs, so their workload totals are not a
matched performance comparison.

## Correction

When LEN is nonzero, arithmetic and register FP moves use the existing SkyEye
software VFP engine for bank traversal and FP controls. It receives a fresh copy
of all FP registers and FPSCR on each call. Its thread-local scratch is not
serialized and does not perform guest memory or system operations. Normal LEN=0
arithmetic retains the existing fast handlers and their existing accuracy limits.
Comparisons, conversions, CPU transfers and memory transfers remain scalar.

Optional four-instruction SIMD fusions execute their constituent instructions
in order while LEN is active. Expanded vectors can overlap registers even when
scalar-only fusion alias checks would permit batching. Scalar-bank arithmetic
under active LEN also uses software FP controls: a negative-control test found
that retaining host arithmetic there preserved a NaN payload despite guest DN.

No title switch, guest clock, instruction charge, scheduled-event policy, draw
skip or interpreter record-layout change is used. Build fingerprints change;
older build-specific states remain protected by the normal compatibility check.

## Validation and limits

Each SIMD mode (0, 1, 2 and 3) passes 460 independent fixed-answer, condition,
Thumb and matrix cases; 11,648 software differential FP-control cases; 20 cached
and fused transition cases; and 128 scheduled-event slices. Independent cases
cover bank-zero DN/FZ/rounding answers, bank wrap and overlap, and the matrix
instruction shapes with independently chosen inputs. Existing 22,000-instruction,
AL-dispatch and scalar-double/RAM regressions also pass. CI invokes the maintained
short-vector test explicitly in all four SIMD modes.

A corrected native M1 GPU run restored Sonic's full title screen. The controlled
iOS GLES CPU-vertex run restored bottom-screen title graphics, with a separate
top-screen difference still under investigation. These are title-screen results;
full gameplay, physical-iPad compatibility and performance gains are separate
validation requirements. Software-rendered simulator speed is not an M4 GPU
benchmark. The native desktop host also needs GL 4.1 image-copy fallbacks and is
not the identical iPad graphics backend.
