# Cara

<img src="docs/artwork/cara-citrus-readme.png" alt="Cara app icon" width="104" align="right">

**Fast, accurate, portable Nintendo 3DS emulation.**

Cara is an open-source emulator core and app built on Citra MMJ’s mobile
foundation. We tune Cara for Apple hardware first and develop it for macOS,
iPhone, iPad, Android, and Linux. FastInterp lets iPhone and iPad run games
without JIT, while Cara’s graphics work focuses on smooth play and accurate
game behavior.

[Download Cara](https://dmikey.github.io/cara/#get-cara) ·
[GitHub releases](https://github.com/dmikey/cara/releases) ·
[Documentation](#documentation) ·
[Report an issue](https://github.com/dmikey/cara/issues)

## Features

- **A library you can make your own.** On iPhone and iPad, import games from
  Files, organize favorites, and choose game artwork.
- **Touch or controller play.** Customize touch skins on iPhone and iPad, or
  use a supported controller. Controller and keyboard options vary by platform.
- **Save your place.** Resume from per-game save states. On iPhone and iPad,
  choose an iCloud Drive folder to sync your in-game saves and compatible
  save states. Keep regular in-game saves when updating Cara.
- **Play straight from supported archives.** The Apple apps open compatible
  single-game ZIP and 7z archives and Azahar’s `.z3ds` and `.zcci` cartridge
  images. See the [compressed cartridge guide](docs/native-mac-dev.md#zip-cartridge-images).
- **Choose how each game plays.** Accuracy uses the original 3DS CPU clock and
  normal game speed. Performance can adjust the emulated CPU clock and combine
  screen updates when the host falls behind, while the game’s rendering still runs.
- **Play together.** Online Rooms on iPhone and iPad lets players with matching
  copies of a game connect and use supported in-game local wireless multiplayer.

## Platforms

| Platform | What to expect |
| --- | --- |
| **iPhone and iPad** | Touch-first app with FastInterp and OpenGL ES. iPhone support starts with iPhone 13 and iPhone SE (3rd generation). |
| **Mac** | Native app for Apple Silicon, starting with the M1. Uses OpenGL with FastInterp and optional Dynarmic JIT in JIT-enabled builds. |
| **Android** | Experimental ARM64 builds with OpenGL ES and Dynarmic JIT. Our handheld tuning includes the Unisoc T820-based RG Slide. |
| **Linux** | x86-64 developer build with FastInterp and desktop OpenGL. |

Game compatibility, performance, and available features vary by platform.
Native Metal is in development for Apple devices through separate experimental
builds. Reusing Cara’s core in Delta is a development goal.

## Launch Cara from Daijishō on Android

1. Open Cara and use **Add Folder to Library** to give it access to your
   game folder. Switch back to Daijishō with Cara still open in the background.
2. In Daijishō, go to **Settings → Library → Add a player** and name it
   **3ds - Cara**. Set **Player am start arguments** to:

   ```text
   -n org.cara.emu/org.cara.emu.ui.EmulationActivity
   -a android.intent.action.VIEW
   -d {file.uri}
   -e GamePath {file.uri}
   --grant-read-uri-permission
   ```

3. Set **Player accepted filename regex** to
   `(?i)^.*\.(3ds|cci|cxi|3dsx)$`. Leave **Kill package processes before am
   start** off and save.
4. Edit the **Nintendo 3DS** platform, select **3ds - Cara** as its
   **Default Player**, then add and sync your game folder if needed.

For a debug APK, change only the package before the slash to
`org.cara.emu.debug`. For **Cara Mali Preview**, use `org.cara.emu.malipreview`.
If a game file cannot be read, reopen Cara and check that the same game
opens from its library before trying Daijishō again.

[Daijishō player setup](https://github.com/TapiocaFox/Daijishou/wiki/How-to-Use-Daijish%C5%8D#players)

## Documentation

[Build Cara](docs/build.md) ·
[Save states](docs/save-states.md) ·
[Mac settings](docs/mac-settings.md) ·
[3DS system activity](docs/3ds-system-activity.md) ·
[Build status](https://github.com/dmikey/cara/actions/workflows/build.yml)

[Architecture](docs/technology.md) ·
[FastInterp](docs/fastinterp.md) ·
[Metal development](docs/native-metal-build.md) ·
[Android tuning](docs/rg-slide-rendering.md) ·
[Performance and testing](docs/validation.md)

When reporting a problem, include your device, OS version, Cara build,
game title and region, and the steps to reproduce it.

## Credits and license

Cara builds on [Citra](https://github.com/citra-emu),
[weihuoya’s Citra MMJ](https://github.com/weihuoya/citra), and
[Azahar](https://github.com/azahar-emu/azahar), including
[warmenhoven’s FastInterp](https://github.com/azahar-emu/azahar/pull/2376).
Save-state support builds on work by Hamish Milne and other Citra contributors.
See the [research credits](docs/research-references.md) for additional references.

Cara’s app icon was inspired by Human Trash from the RetroHandhelds.gg Discord.

Cara is licensed under **GPL-2.0-or-later**. Upstream copyright notices and
third-party licenses are preserved.
[License](LICENSE) · [Notices](NOTICE) · [Source provenance](emulator/VENDORED_SOURCE.json)

Cara is an independent preservation project. Use game dumps from your own cartridges.
