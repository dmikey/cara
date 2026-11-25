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
