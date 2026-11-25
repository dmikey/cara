# Android external display

The Android frontend offers **Settings → External display**:

- Both screens on handheld (default).
- Top screen on external display; touch screen and controls on handheld.
- Touch screen on external display; top screen and controls on handheld.

The setting is global. Select it before opening a game. Cara uses Android presentation displays (including supported wired video outputs). A connection that only mirrors the handheld and does not expose a presentation display cannot be split by the app.

The external image preserves the 3DS screen's aspect ratio with black borders as needed. If the external panel accepts Android touch input, the bottom-screen mode maps touch through that fitted rectangle. Touch in the borders does not begin a guest touch.

Disconnecting or pausing dismisses the external presentation and releases touch. Reconnecting selects an available presentation display while emulation is active. There is no additional guest frame, guest clock change, skipped draw or second emulation instance.

## Physical verification checklist

On the T820 with external display output, test both external modes during gameplay:

1. Verify each physical display shows the selected 3DS screen and matches the ordinary layout.
2. Test lower-screen taps, including all edges and border rejection, on a touch-capable external panel.
3. Connect and disconnect repeatedly; verify both screens return to handheld and there is no stuck touch.
4. Rotate the handheld, suspend/resume and stop/reopen a game.
5. Compare frame timing with the ordinary display mode at the same guest clock and scene.

A successful build or layout fixture does not establish physical display compatibility. Record device, Android version, output adapter and display model with physical results.

## Upstream comparison

Reviewed Azahar commit `c621eafbf316699c55f4c9c0c689d4e49498d360` on 2026-11-25:

- [SecondaryDisplay.kt](https://github.com/azahar-emu/azahar/blob/c621eafbf316699c55f4c9c0c689d4e49498d360/src/android/app/src/main/java/org/citra/citra_emu/display/SecondaryDisplay.kt)
- [OpenGL renderer](https://github.com/azahar-emu/azahar/blob/c621eafbf316699c55f4c9c0c689d4e49498d360/src/video_core/renderer_opengl/renderer_opengl.cpp)
- [Secondary display crash prevention, PR 2751](https://github.com/azahar-emu/azahar/pull/2751)

Cara follows the same Android architecture: DisplayManager lifecycle notifications, a Presentation containing a SurfaceView, holder callbacks to native surfaces, and single-pointer touch tracking. Review identified missing Java guards; Cara now excludes private, powered-off and invalid displays, catches invalid-display and bad-token failures, checks destroyed activities and avoids repeatedly selecting a rejected display. Touch requests unbuffered dispatch.

Azahar's renderer owns a separate mailbox for the secondary window and composes each layout from completed LCD textures. Cara instead composes one canonical atlas and crops it into each output; its existing command worker and mailbox require a different native implementation. The source attachment must remain owned until both GPU reads finish. Similar Android APIs do not establish identical EGL synchronization or physical correctness.

Azahar also creates a hidden virtual display for its renderer lifecycle. Cara does not need a dummy secondary window: it retains the ordinary handheld path until an actual external surface is ready. Failed attachment restores the ordinary combined output.

Presentation displays do not cover every internal dual-panel handheld. Azahar issues [2420](https://github.com/azahar-emu/azahar/issues/2420) and [2727](https://github.com/azahar-emu/azahar/issues/2727) document devices that require a secondary Activity instead. Cara's present scope is external/presentation displays; internal panels lacking Android's presentation capability remain unsupported. This includes no claim of universal dual-screen handheld support.

This was an independent implementation comparison; upstream source was not copied into Cara.
