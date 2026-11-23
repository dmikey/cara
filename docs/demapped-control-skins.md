# Enhanced Delta control skins

Cara's demapped controls replace the standard skin artwork and standard button
hit targets while preserving both LCDs, the touchscreen and a reachable Menu.
They are translucent glyphs with independent portrait/landscape frames. Drag a
glyph to move it; drag its selection corner to resize it. The editor offers
system glyph selection, opacity, single-input binding and a multi-select button
combo picker. Combos are simultaneous held inputs, not timed input macros.

Game settings → Control layout opens an isolated draft. Arrange and Test touches
share the runtime control/input-hub path. Save validates and registers an enhanced
`.deltaskin` under Application Support/CaraControlSkins and stores the selection
in that game's library metadata. Cancel discards the draft. Registered presets
load as independent drafts; applying one in another game does not overwrite the
original. Importing from Files registers the skin even when no game is loaded;
imported identifiers are replaced so an existing authored preset cannot be
silently overwritten. A physical-controller skin import registers an enhanced
touch layout without replacing that physical-controller skin. Use Layout → Name
layout to name a reusable preset. Restore classic controls disables the
replacement layout.

The export is a ZIP `.deltaskin` with standard `info.json`, dual-screen Delta
representations, standard fallback input items and a transparent PNG backing.
The `caraActionLayout` extension carries schema version 2:

- `portrait` and `landscape`: normalized frames and stable contact owner IDs.
- `binding`: legacy single button/stick binding; `bindings`: optional simultaneous
  button array (zero through eight distinct, known 3DS button names).
- `glyph`: approved SF Symbol name, rendered by iOS; no Apple glyph image is
  embedded in the exported package.
- `opacity`, `hidden`, `toggle`, and analog `travel` behavior.
- `representations`: screen placement corresponding to the normalized canvas.

Version 1 JSON remains importable. Cara extensions such as glyph rendering,
toggles and floating zones require Cara; support in other Delta-skin readers
is not asserted. The presence of Delta fallback metadata does not certify a
third-party emulator's behavior.

`tools/generate_control_presets.py` reproducibly creates four bundled registered
starting skins: standard 3DS, Luigi Dark Moon, Zelda and Xenoblade. They carry
explicit 3DS button labels and are deliberately not described as verified
semantic/action mappings. Test each title before publishing a game-tuned preset.
The registry records that gameplay validation is outstanding; no game artwork or
screenshots are distributed. Only Luigi currently has a pinned USA title ID.

## Validation

`tests/skin_archive_roundtrip_test.cpp` exercises the actual archive reader and
writer, corruption/path rejection and every generated preset. Run it with
AddressSanitizer/UndefinedBehaviorSanitizer using the local C++ toolchain.
`tests/test_control_presets.py` independently checks registry identity, screen
bounds, archive integrity, decoded transparent pixels and generation stability.

`tools/test_demapped_controls.py` builds and executes the actual UIKit action
layout, skin loader and input hub on Mac Catalyst, without downloading a
Simulator runtime. Its checks cover combo validation, press/release,
latched-combo reset, decoded transparent export pixels, archive/schema round
trip, UUID registration/copy/path safety, legacy v1 data, editor type changes,
independent orientation edits, multitouch and lifecycle release. These checks
passed on the current Apple Silicon Mac with Xcode 27.2.

The iOS Simulator UIKit preview also compiles. Simulator runtimes are not
installed, so the editor's actual iPad gestures, visual layout and physical
hardware acceptance remain unverified. Catalyst data/input checks are not a
substitute for those device checks.
