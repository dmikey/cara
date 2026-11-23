# Luigi vertex reuse on iPhone SE 3 and Apple M1

Cara’s desktop OpenGL atlas configuration already enables prepared index
caching and byte-exact vertex-upload reuse with order-aware lookup. The
iOS frontend enables these paths with packed inputs on devices reporting
at least 6 GiB of RAM. The SE 3 falls below that threshold. These tests
leave both defaults in place.

## M1 startup and title screen

Three fresh private runs used the Mac RC core at `9a99f46`, desktop
OpenGL 4.1, FastInterp, SDL audio and all four guest timers at 100%. They
used the same script and measured 600 refresh intervals from 120 to 720
with the application shader cache disabled. Both optimizations changed
together. The last capture showed the title screen.

| Vertex reuse and index cache | Vertex prep (ms/refresh) | Vertex upload (ms/refresh) | Emulation-thread CPU (ms/refresh) | Wall (ms/refresh) | Draw submission (s total) |
| --- | ---: | ---: | ---: | ---: | ---: |
| Off, first | 1.975 | 1.221 | 23.051 | 80.235 | 30.346 |
| On | 0.298 | 0.184 | 19.445 | 29.270 | 1.854 |
| Off, final | 1.852 | 1.070 | 20.951 | 29.287 | 0.967 |

The enabled run avoided 1,536,648,960 of 1,641,983,488 vertex upload bytes
and hit the prepared index cache 225,607 times in 226,496 lookups. The
three runs matched 226,496 draw requests, 220,274 hardware vertex draws,
guest timer ticks and the final two-screen compositor bytes. Five of six
guest files matched. The fresh NAND configuration differed. Each core
shut down cleanly.

Compared with the final control, the enabled run used less CPU and had
essentially identical whole-refresh wall time. The first control’s draw
submission took 30.35 s across the window, while explicit compile and
link scopes accounted for 3.66 s. Driver cache state was uncontrolled.
The aggregate counters establish a draw-side delay, with insufficient
detail to identify the specific call or its cause.

## SE 3 dialogue checkpoint

The private iOS diagnostic replayed 720 refreshes of the E. Gadd dialogue
on OpenGL ES 3 with FastInterp, fixed 100% guest CPU, native rendering
resolution and audio enabled. The pairs below completed without a
foreground pause. They matched the terminal compositor bytes, guest
ticks, 350,770 draw requests and 347,815 GPU vertex draws.

- Upload reuse avoided 98.93% of upload bytes. The uninterrupted adjacent
  runs averaged 23.74 ms with reuse off and 21.72 ms with it on. Terminal
  memory footprint increased by about 52 MB. Other runs had foreground
  pauses and are unsuitable for a whole-refresh speed comparison.
- Holding upload reuse enabled, two index-cache passes reduced mean
  vertex preparation from 6.81 to 0.73 ms and overall mean refresh time
  from 21.73 to 20.21 ms. Mean p95 across the two passes rose from 31.59
  to 39.63 ms. The host presentation scope grew from 0.59 to 4.02 ms
  per refresh.
- Holding both caches enabled, an EAGL worker pass increased mean refresh
  time from 20.18 to 20.38 ms and p95 from 39.11 to 42.51 ms against the
  nearest preceding control. Time shifted from presentation and draw
  submission into stream mapping. Stream mapping was nested within
  uniform upload in that profile.

A separate 720-refresh SE 3 trace with both caches enabled split the
presentation scope into an average of 2.64 ms for state queries and
framebuffer clear, 1.39 ms for presenting the renderbuffer, and 0.05 ms
for blitting. The iPhone also exhibited a separate 7.74-second startup
stall, of which 6.97 s was inside GLES draw submission. The available
host spans cannot distinguish deferred shader compilation from other
driver waits or give physical GPU time.

The Mac draws the two LCDs through its desktop OpenGL compositor and uses
`SDL_GL_SwapWindow`. The iOS EAGL worker switch and drawable-clear block
are specific to the iOS frontend. Future work should measure the Mac
compositor and SDL swap independently and bind iOS opening stalls to
their actual draw calls before changing shader or presentation policy.
