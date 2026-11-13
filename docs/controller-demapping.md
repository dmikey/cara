# Controller Demapping

A native action-first touch controller, available from **Game details → Game settings → Controller Demapping**. This is an optional per-game layout; classic skins remain the default and fallback.

Choose Fire, Jump, Interact, Aim, Move, Look, Run, Reload, Crouch, Map, or a custom name. Bind the action to a 3DS button, Circle Pad, C-stick, or digital D-pad. Starter actions are deliberately unbound. Bindings describe inputs, not a promise about a particular game's controls.

Arrange controls by dragging; drag the selected lower-right corner to resize. Green guides indicate the safe canvas and approximate thumb reach. Controls have a minimum 44-point runtime touch target. Portrait and landscape are independent. iPad and landscape editors use a side inspector.

Buttons support hold or toggle. Analog controls support fixed sticks, floating sticks, directional pads, and drag zones. Floating and drag controls anchor at the first contact, maintain an analog direction from displacement, and center on lift. Travel controls the point distance to full tilt independently of zone size.

**Invisible Move zone · Circle Pad** and **Invisible Look zone · C-stick** add broad left/right floating zones with explicit analog bindings. Invisible boundaries appear only while arranging. Test mode and gameplay draw nothing for them, even while touched. Visible action buttons take priority over broad zones; each control has one touch owner. A finger rejected because a control is already owned stays rejected until lift. Lift/cancel, pause, background, stop, orientation changes, and layout replacement release inputs. Toggles also clear on these transitions.

**Test touches** uses a separate instance of the same input hub, without taking ownership of physical controllers. It displays the resulting buttons and analog values; it does not run a ROM. Saved layouts feed the gameplay input hub, merged with classic touchscreen input and hardware. Paused/stopped gameplay ignores new action touches. The lower screen remains touchable outside action zones; an action zone intentionally placed over it owns that region. Menu stays available above the overlay.

**Save** applies both variants only to this game. **Cancel** discards the draft. **Layout → Reset this orientation** requires confirmation and replaces only that draft variant with unbound starter actions. **Restore classic controls** disables action layouts while retaining the draft controls; tap Save to apply. Re-enable the switch to return to the saved action controls.

## Data and source

`actionLayout` lives beside existing per-game library metadata. JSON version 1 contains `enabled`, `portrait`, and `landscape`; controls contain stable IDs, labels, kind, binding, toggle, and normalized safe-canvas frames. Optional `hidden` defaults to false and `travel` defaults to 64 points. Imports are limited to 256 KiB and 24 controls per variant; validation rejects unknown versions, malformed fields, invalid bindings, duplicate IDs, and out-of-bounds frames. Failed imports leave the saved layout intact. Export writes a copy of the current draft through the native Files picker.

All implementation source is bundled in `emulator/src/ios/action_layout.{h,mm}`, with routing in `input_hub.{h,mm}` and `main.mm`. UIKit, Foundation, and existing skin helpers are used without a new dependency. Existing GPL licensing and upstream attribution are retained.

## Validation

`tools/build_ios_shell_preview.py --output <directory>` builds a ROM-free simulator host. Launch with `CARA_UI_SCENE=actions`. The host runs `tools/ios_action_layout_checks.h`, which checks schema/JSON validation, contact ownership, hold/toggle semantics, fixed/floating/D-pad/drag inputs, hidden-zone travel, lifecycle releases, and output through the actual input hub.

`tools/ios_library_ui_smoke.m` includes `testActionLayoutEditor` for adding invisible zones, independent orientation variants, saved per-game metadata, a fresh editor instance, and reset cancellation. `input_hub_self_test.mm` also checks keyboard and touch source ownership and lifecycle merging. These are simulator/native synthetic proofs, not physical-device or game-compatibility claims.
