# Android GLES mailbox and checked remap on the RG Slide

The Android ordered GLES worker can now publish a frame directly after creating and flushing its render fence. The rendering context uses a GPU-side wait before reusing a presented buffer. The producer still observes bounded mailbox backpressure when it runs out of buffers. Access to the presentation queue is locked, and resizing advances the generation so late completions from before the resize return to the free queue. The new path is available through the Android debug setting `mailbox_async_publish`. [APK and EGL fixture](https://github.com/dmikey/cara/actions/runs/45277898425)

Four normal boots used the same signed APK, distinct lighting/fog texture generations, Performance Mode, Dynarmic JIT, ordered GLES at 1×, and fixed guest CLK 100%. Each boot restored all 128 cached shader binaries. A and R are bracketing controls, M enables async publication, and Q adds the existing checked remap. The checked remap moves active texture and other state queries onto the worker, then reports the real framebuffer and draw result before cache metadata marks the destination valid.

| Boot | Matched smoky title p99, ms | Worst title frame, ms | Matched warm Gadd p99, ms | Worst Gadd frame, ms |
| --- | ---: | ---: | ---: | ---: |
| A, control | 39.96–40.28 | 46.80 | 41.01–46.36 | 81.99 |
| M, async publication | 39.34–40.89 | 46.99 | 39.95–44.16 | 86.35 |
| Q, async publication and checked remap | 39.39–42.29 | 47.07 | 40.31–44.80 | 85.82 |
| R, repeat control | 40.13–41.28 | 44.20 | 42.00–47.59 | 81.97 |

The matched title windows end at 961, 1081 and 1201. The warm Gadd windows end at 3121, 3241 and 3361. The combined run's warm Gadd medians were 30.01–30.28 ms. The entry window ending at 3001 is listed separately in the raw timings. Recent SurfaceFlinger timestamps after the warm windows showed a 33.25–33.27 ms median actual presentation gap for the first control and both candidates. This is a snapshot of the latest 126 gaps, separate from emulation refresh timing. There was no clear pacing regression or steady-state speed gain.

The first control spent 1.473 ms per warm refresh queued before `glClientWaitSync` and 2.069 ms before `glFenceSync`. Async publication removed both producer calls. The pre-call wait at `glGetIntegerv` rose from 5.836 to 9.777 ms per refresh. With checked remap enabled, that state query left the producer and the queue still spent 9.635 ms per refresh before the checked draw. The check also spent 2.263 ms per refresh on the worker, including submission of the remap. These aggregates include complete lab windows with 348–365 draws per refresh, so they can include a different number of windows from the matched timing table.

The EGL fixture verified that the worker could publish a flushed fence to another context and retire the returned fence with a server-side wait. It also verified both remap framebuffer outcomes and matched 80 remap cases at every pixel. The candidate remains opt-in because moving both waits was insufficient to reach the frame budget.

The separate queue-attribution boot with both changes recorded 9.866 ms per refresh of producer pre-call wait. The remap check overlapped 9.113 ms of earlier work. Entry 327's indexed draws accounted for 3.978 ms of that overlap. The guest display-transfer blit accounted for 0.095 ms of overlap, down from 1.062 ms in the earlier distinct-LUT diagnostic boot. This records less blocking on a producer result during the blit. It does not time the blit on the GPU. Rendering the mailbox still overlapped 1.865 ms during its clear, while publishing its fence overlapped 0.103 ms. [Queue attribution](dark-moon-mailbox-backlog.txt) · [Per-window frame times](dark-moon-mailbox-windows.tsv) · [Synchronous call timings](dark-moon-mailbox-queries.tsv)

The Slide's original signed APK was restored and the app configuration and shader archive checksums matched.
