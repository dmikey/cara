# RG Slide ordered GLES submission

The private Android experiment moves GLES driver calls onto a thread that owns the emulator’s EGL pbuffer. PICA parsing, guest timing, vertex preparation, layout decisions and renderer metadata stay on the emulation thread. The presentation thread keeps its window context. The shader compiler keeps a third, shared context. The package and runtime switches both default off.

The original Mac recorder replaced process-wide GLAD function pointers. On the Slide it rejected the presenter’s first framebuffer bind and the compiler’s first shader creation. The Android experiment overrides GLAD dispatch on the producer thread only. It constructs the optional shader compiler from the worker’s current EGL context, preserves guest query and readback ordering, and flushes a mailbox fence before publishing it.

## Device and measurement

We used a physical RG Slide (Unisoc T820, Mali-G57), Dark Moon USA, Performance Mode, JIT, GLES atlas, 1×, fixed guest clock 100%, presentation thread, indexed draws and async shader stages. Both arms enabled packed layout uniform reuse. Each boot ran through the title and file selector into Gadd’s lab, using a temporary app profile. Timing windows contain 120 system refresh intervals. P99 ranges below summarize the separately recorded window p99s. Gadd figures use the four qualifying windows on each boot (39–58k draws, at most 3 ms compile time and under 100 ms LUT time). Graphics CPU is the producer’s GPU-command CPU time per refresh. Worker CPU is measured with the worker’s thread clock. Either thread can overlap the other, so their CPU times need to be interpreted separately from wall time.

## First pair, scalar GLES calls

Build `6730ba0` compared the direct renderer and the worker on the same APK. The older supplied binary cache had a different renderer signature. Time from launch to the title checkpoint was 101.53 s direct and 103.79 s with the worker. At lab entry, direct had a 40,962.21 ms refresh (40,767.71 ms compiling in that window), and the worker had a 41,900.53 ms refresh. The worker’s worst startup refresh was 50,102.74 ms at refresh 515. These are first encounters for the trial cache. [APK and build](https://github.com/dmikey/cara/actions/runs/45245718130)

| Scene and path | Graphics CPU ms/refresh | Worker CPU ms/refresh | Window p50 range, ms | Window p99 range, ms | Worst measured refresh, ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| Title, direct | 23.19 | — | 49.75–52.12 | 56.86–58.53 | 59.28 |
| Title, worker | 13.72 | 15.46 | 45.11–46.55 | 50.90–53.48 | 56.79 |
| Gadd, direct | 19.72 | — | 28.78–29.62 | 34.18–52.37 | 168.81 |
| Gadd, worker | 13.40 | 13.13 | 29.65–33.55 | 35.83–118.64 | 138.95 |

The four Gadd windows contained 169,266 direct and 169,407 worker draws. The worker recorded an average of 3,001,584 commands, 47,946 batches and 2,785 synchronous calls per 120-refresh Gadd window. The measured map, unmap, uniform-location lookup, shader creation and program creation RPCs were all zero during those windows. Owned stream uploads can map on the worker without an RPC. The title averaged 5,530 synchronous calls per window. A 4-second `simpleperf` capture included 1,415 samples. Its largest producer symbol was `memcmp` (6.78% of whole-process samples), and the driver worker showed both Mali GLES driver work and pthread locks.

At title window end 1441, 480 LUT uploads spent 2,549.36 ms in setup and 3.61 ms issuing texture updates. Setup includes seven synchronous binding/unpack state queries per upload. The next trial captured the owned bytes once and reused Cara’s Mac LUT command so the worker executes the full setup, upload and restoration in order. The other renderer helpers remain on the producer.

## Shared binary-cache pair

Build `2bd61fa` ran direct first and the worker second from normal boots. Each copied the same cache captured after the first trial reached Gadd. Both logged `cache_restore entries=128 binary_hits=128 source_links=0 bytes=29954062` and reached the title checkpoint in 21.90 seconds. This pair also enabled the shader census on both arms. [APK and build](https://github.com/dmikey/cara/actions/runs/45248433508)

| Scene and path | Graphics CPU ms/refresh | Worker CPU ms/refresh | Window p50 range, ms | Window p99 range, ms | Worst measured refresh, ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| Title, direct | 22.65 | — | 50.09–50.46 | 57.00–58.87 | 65.78 |
| Title, grouped worker | 13.03 | 15.62 | 43.10–44.47 | 48.94–55.00 | 55.68 |
| Gadd, direct | 19.17 | — | 28.77–29.63 | 35.51–50.82 | 70.33 |
| Gadd, grouped worker | 12.57 | 12.61 | 29.38–33.09 | 35.05–57.74 | 110.85 |

| Gadd window ending at refresh | Direct draws | Worker draws | Direct p99 / worst, ms | Worker p99 / worst, ms |
| ---: | ---: | ---: | ---: | ---: |
| 2641 | 42,230 | 42,254 | 35.79 / 51.86 | 57.74 / 110.85 |
| 2761 | 41,880 | 42,926 | 35.51 / 35.54 | 48.38 / 65.18 |
| 2881 | 42,228 | 41,880 | 43.64 / 53.84 | 35.05 / 35.16 |
| 3001 | 42,927 | 42,230 | 50.82 / 70.33 | 38.26 / 46.70 |

The worker moved 6.60 ms of graphics CPU per refresh off the guest thread in Gadd. That exceeded the 3DS CPU execution cost in these windows, yet the heaviest worker refresh took 110.85 ms at frame 2591 versus the direct maximum of 70.33 ms at frame 2969. A graphics-thread CPU reduction alone did not improve Gadd’s tail or reach the 16.7 ms budget in this pair.

Title averaged 480 owned LUT transactions and 2,165 synchronous calls per 120 refreshes, down from 5,530 calls in the earlier worker trial. At title window end 841, direct spent 2,147.76 ms inside the texture-transfer call across 480 uploads. The grouped worker recorded the same 5,354,496 bytes and its producer lighting/fog region took 11.42 ms. The worker still executed the transfers. The producer-side transfer timer no longer measures their driver latency.

Gadd averaged 3,006,825 recorded GLES commands, 47,884 batches, 2,453 synchronous calls and 30 owned LUT jobs per 120 refreshes. Of the synchronous calls, the measured averages included 364 `glGetIntegerv`, four uniform-location queries, zero buffer maps/unmaps and zero program/shader creations. The last two Gadd windows showed no LUT bytes. The measured throughput cost persisted at roughly 71 recorded GLES commands per emulated draw. The data support keeping this implementation experimental and testing a coarser representation of draw and state changes before promotion.

Each pair restored the originally installed signed APK, and the active profile’s configuration and cache SHA-256 checks matched before and after. Pixel verification covers the standalone fixtures below. Both emulator paths remain available for controlled comparisons on the experimental branch.

## Independent G57 correctness fixture

The standalone executable runs the same recorder in a separate process. Its core context draws red and green regions, rejects a later farther fragment and synchronously reads back all eight pixels. A second live EGL context attempts the same two entry points used by Android’s presenter and shader worker. The previous process-wide recorder rejected those calls. The thread-local experiment ran both successfully.

The second fixture compares two ordinary draws to one two-instance call for both `glDrawArrays` and `glDrawElements`. A deliberately substituted guest index selects a different `gl_VertexID` color at the same position. The fixture checks both blended passes, a farther primitive rejected by depth and a separate sentinel that writes only when stencil equals two. [ARM64 fixture build](https://github.com/dmikey/cara/actions/runs/45249101997)

```text
renderer: Mali-G57
render: direct_draws=3 worker_draws=3 identical_pixels=8 queued_commands=30 batches=7 synchronous_calls=11
present glBindFramebuffer routed
shader worker glCreateShader routed
native sibling control passed
arrays repeat: 8 blended pixels match, depth reject and stencil=2 verified
elements repeat: 8 blended pixels match, depth reject and stencil=2 verified
```

To reproduce the fixture, dispatch the branch’s Android workflow with `probe_only=true`, extract `tls/cara_ordered_gles_probe`, then push it to `/data/local/tmp`, set mode 700 and run it under ADB shell. For the full-game trial, build with Gradle `-PcaraOrderedGles=ON` and change only `[Debug] ordered_gles_worker` between paired boots of that APK. Keep `packed_layout_uniform_reuse=True`, async indexed shaders, the extended program cache, a common seed and the presentation thread on both arms. The app logs CPU and RPC deltas under `CaraPerf` and restored entries under `CaraShader` with the respective diagnostic counters enabled.

## Residual command traffic after raster-state reuse

The physical Mali-G57 census at `b1f4ebc` enabled `atlas_reuse_raster_state` and `packed_layout_uniform_reuse` on the ordered worker. Dark Moon booted normally at Performance Mode, JIT, GLES, 1×, fixed CLK 100%. The driver accepted all 128 cached binaries. In four 120-refresh Gadd windows ending at 2641, 2761, 2881 and 3001, the per-API histogram summed to the same 4,679,794 asynchronous closures reported by the recorder. There were 170,006 guest draws.

| API or recorder operation | Calls | Calls per guest draw |
| --- | ---: | ---: |
| `glActiveTexture` | 1,568,328 | 9.23 |
| `glBindTexture` | 1,283,912 | 7.55 |
| `glBindBuffer` | 460,364 | 2.71 |
| `glBindSampler` | 298,016 | 1.75 |
| `glBindBufferRange` | 221,759 | 1.30 |
| Owned buffer upload | 170,006 | 1.00 |

Texture unit selection, texture binding and sampler binding accounted for 67.3% of all recorded jobs. The same family accounted for 14.33 calls per draw at the title screen. These are call counts. Each job has a different execution cost, and GPU shader or tiler time cannot be inferred from the counts.

The private packet experiment at `39b02a1` packs an adjacent active-unit, bind and optional sampler into one producer-owned closure. It can also pack a bind immediately followed by a sampler. Each original GLES operation reaches the driver with its original arguments and ordering. Once the batch has transferred ownership or a different command intervenes, the producer starts a new record. This measures the overhead of crossing the queue while keeping driver work equivalent.

For reproduction, use the ordered Android build, `ordered_gles_worker=True`, `trace_ordered_gles_apis=True`, `atlas_reuse_raster_state=True` and `packed_layout_uniform_reuse=True`. Compare `ordered_gles_texture_packets=False` and `True` on the same APK with the same shader-cache seed. At each `ordered_api` marker, subtract `fused` from the sum of the named API counts to recover the number of physical queue records. Compare this against the delta of the adjacent `ordered_gles commands` counters.


### Driver-call control

A second normal boot on the census APK enabled the existing `atlas_retain_vertex_bindings` option. It leaves private vertex input units 7 and 8 bound at draw teardown. Both boots restored the same 128 driver binaries. Four Gadd windows in each boot ended at the same emulated refresh numbers.

| Scene and arm | Recorder jobs / guest draw | Producer graphics CPU ms / refresh | GLES worker CPU ms / refresh | Window p99 range, ms | Worst refresh, ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| Title, ordinary cleanup | 24.43 | 11.44 | 13.23 | 43.89–49.31 | 54.54 |
| Title, retained private inputs | 21.93 | 11.94 | 13.49 | 44.95–49.23 | 57.51 |
| Gadd, ordinary cleanup | 27.53 | 11.28 | 10.80 | 36.99–48.00 | 106.73 |
| Gadd, retained private inputs | 24.21 | 11.44 | 11.42 | 35.91–57.01 | 108.04 |

Retention eliminated 12.1% of Gadd's queue records and their driver calls. Its wall-clock and thread-CPU measurements do not support enabling the flag. Independent samples of GPU-active cycles per input primitive were 216.79 with ordinary cleanup and 218.90 with retention. Both reported 0.539 position-shading tasks per input primitive. Each sample spanned approximately 11 seconds of warm gameplay.


### Worker packet result

The scalar and packet trials used the same signed `39b02a1` APK. The session markers confirmed Performance Mode, Dynarmic, GLES atlas, 1×, fixed CLK 100%, and both boots restored 128 of 128 program binaries. The title-screen checkpoints arrived after 21.35 and 21.18 seconds. The same 204,440 guest title draws emitted precisely 4,994,628 original GL operations in both arms.

| Scene and arm | Recorder jobs / guest draw | Producer graphics CPU ms / refresh | GLES worker CPU ms / refresh | Window p99 range, ms | Worst refresh, ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| Title, scalar | 24.43 | 11.52 | 13.51 | 43.90–49.14 | 58.35 |
| Title, packets | 17.37 | 12.15 | 13.15 | 43.69–48.42 | 58.51 |
| Gadd, scalar | 27.53 | 11.23 | 11.45 | 38.21–108.70 | 113.01 |
| Gadd, packets | 18.42 | 11.73 | 10.69 | 37.74–47.07 | 110.60 |

| Gadd refresh interval | Scalar p99 / worst, ms | Packet p99 / worst, ms |
| --- | ---: | ---: |
| 2522–2641 | 108.70 / 113.01 | 37.74 / 110.60 |
| 2642–2761 | 48.37 / 48.57 | 46.44 / 80.87 |
| 2762–2881 | 38.21 / 47.70 | 47.07 / 48.79 |
| 2882–3001 | 47.35 / 48.50 | 41.69 / 48.04 |

Packing removed 33.1% of Gadd's recorder jobs per guest draw. The worker used 0.76 ms less CPU per refresh and the producer used 0.49 ms more. Independent GPU counter samples measured 218.24→215.94 active cycles and 135.22→135.02 fragment-queue cycles per input primitive. Position-shading tasks per primitive stayed at 0.539. The worst refresh after the entry window rose from 48.57 to 80.87 ms. A single normal boot per arm does not establish a consistent frame-time benefit. The option remains disabled by default. Individual windows include PICA draw counts, command counts, CPU data, RPCs and worst-refresh indices. [Window data](ordered-texture-windows.csv)

The ARM64 EGL fixture on the physical Mali-G57 verified texture and sampler binding after a switch of active unit, preserved `GL_INVALID_ENUM` and the subsequent bind after an invalid unit, and checked a bind across an explicit queue flush. Four commands folded during these checks. Three driver draws produced identical depth-tested RGBA readback on the direct and worker paths. The existing array and indexed repeat tests also matched all eight blended pixels, depth rejection and stencil value two. Both emulator trials restored the original installed APK, configuration and shader cache. The configuration and cache matched their pre-test checksums.

The ordinary lit triangle-list fallback submits `glDrawArrays(GL_TRIANGLES)`. With packed input mode below three and native quaternion geometry disabled, the generated vertex source executes the actual vertex and, on the second and third slots, also evaluates the first vertex of the triangle to choose the quaternion sign. An enabled and supported quaternion slice replaces the full reference evaluation with a narrower function. The unsliced source has five evaluations per triangle. Driver dead-code elimination and Mali's distinct position and varying work affect the instructions ultimately executed. Aggregate position/varying counters cannot attribute time to the reference evaluation within one draw. [Fallback draw](../../emulator/src/video_core/renderer_opengl/gl_rasterizer.cpp#L1768) · [Quaternion sign generation](../../emulator/src/video_core/renderer_opengl/gl_shader_gen.cpp#L2319)
