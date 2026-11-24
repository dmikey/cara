# Display settings across Cara

Use the iPad and iPhone controls as the reference for separating internal
rendering from LCD presentation. Both controls appear under Display. Game
texture filtering changes the guest materials and is independent of either
display scaling control.

| Control | iPad and iPhone | Android | macOS |
| --- | --- | --- | --- |
| Internal resolution | 2× Native Resolution, off by default | Native resolution selects 1× through 4×, with 1× by default | The public desktop preferences currently use 1× internal resolution |
| Display scaling | Native (Fit), Lossless 2×, Lossless 4× | Same choices in the library and during play | Same choices in desktop preferences |
| Lossless output sizing | Keeps the skin’s LCD rectangles | Keeps the selected screen layout and touch rectangles | Centers the LCDs at whole multiples that fit the desktop window |

At 2× internal resolution, the upper LCD renders at 800×480. The lower
LCD renders at 640×480. The option increases the work done on each guest
frame. Lossless presentation scales the completed screens and does not
increase the guest render resolution.

On GLES, when both destination LCD rectangles already use whole multiples
of the current render size, Android samples them directly. Otherwise it
first expands both LCDs with nearest-neighbor filtering in a reusable GPU
renderbuffer, then fits the two regions independently, using nearest
filtering at integral output sizes and linear filtering for fractional fit.
The scale decision uses the same helper as iOS. The intermediate is owned
by the render context before Android's optional presentation mailbox.
