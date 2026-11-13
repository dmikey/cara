# Native game library

The Collection screen uses embedded ROM icons and titles when readable, with a
monogram fallback. It never downloads artwork or invents genre, release date, or
play-time metadata. Favorites, last launch, theme, accent, title visibility, and
menu-sound preferences persist locally.

Single tap opens Game details. Long press opens the native context menu. Removing
a game hides its library reference; adding the same file explicitly restores it.
Deleting a game uses coordinated file deletion only after a native confirmation.
Both operations preserve in-game saves and save states. A running game must be
stopped before its file can be deleted.

Game settings store per-file overrides separately from global preferences. They
bind to the existing frontend play policy and audio-stretching flag. Accuracy
locks the guest clock and game speed at 100%; Performance supports adaptive or
fixed guest clocks. Clearing game settings restores global defaults.

The state list uses CaraStateCatalog with the current build fingerprint, CPU
backend, system mode, and game code hash. A game needs one successful launch to
record its identity and revision. A changed ROM must be launched again before
states are offered. A cold state request waits for the game to load, then passes
through the core's full state validation. Sync runs through the existing launch
workflow first. The UI never treats an incompatible state as ready.

The full Settings screen has an iPad sidebar and an iPhone category chooser.
Library appearance is independent of the OLED game skin and orange app icon.
Only functional appearance controls are shown. The eight bundled original
interaction sounds preload through AudioServices, obey the system sound/silent
policy and Menu sounds preference, and have a 75 ms minimum playback interval.
They never activate or change the game's audio session.

## Validation

- `tools/build_ios_shell_preview.py` builds the ROM-free UIKit fixture host and
  includes the bundled interaction sounds.
- `python3 tools/test_ios_library_ui.py --device SIMULATOR_UDID --output /private/tmp/cara-ui-test`
  runs the fixture suite on a booted simulator. `tools/ios_library_ui_smoke.m` exercises single-tap details, real empty states,
  favorite filtering, native long-press actions, confirmation cancellation,
  adaptive settings, theme selection, and the menu-sound preference. Launching
  with `CARA_MENU_SOUND_SELF_TEST=1` requires all eight CAF files to preload
  successfully through AudioServices.
- `library_store_self_test.mm` covers fixture-only hide/re-add/deletion and verifies
  that save and state files remain after deleting the fixture ROM.
- Simulator builds compile and link the frontend with the actual engine. UIKit
  fixture screenshots and tests demonstrate UI behavior; they do not establish
  physical-device audio behavior or an end-to-end commercial-game state restore.
