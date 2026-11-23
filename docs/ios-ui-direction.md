# iPad library presentation

The November 20 presentation pass follows the supplied XBMC and PPSSPP
references: library first, persistent navigation, a dense cover grid, and a
restrained teal selection treatment. It adapts those principles to UIKit rather
than reproducing either product's artwork or interface assets.

At widths of 900 points and above, Collection, Favorites, Recently played,
Add games and Settings live in a scrollable sidebar. Smaller windows use the
existing segmented navigation and toolbar. Settings and import actions appear
once in either mode. No cover art is fetched from outside services; embedded
icons and user-imported artwork continue through the existing cache.

New users start in dark appearance. Existing explicit theme/accent choices are
preserved. Shared background, accent and navigation styling covers the library,
settings, save manager, input mappings, multiplayer and control layout editor.
Rounded display type and continuous card corners establish a common style.
Gameplay LCD colors and guest rendering remain unchanged.

Cover tiles reserve height for scaled title/subtitle fonts. Accessibility text
sizes reduce grid density and use compact category navigation instead of the
fixed-width settings sidebar. The optional recent-game hero yields space in
short windows and at accessibility sizes. Primary actions use SF Symbols rather
than literal play/plus glyphs. The editor is labeled “Control layout.”

## Runtime acceptance still required

Xcode 27.2 and its iOS SDKs are now installed. The UIKit Simulator fixture
compiles and the demapping schema/input checks run through Mac Catalyst.
No iOS Simulator runtime is installed, so screenshot comparison and iPad
gesture/device acceptance remain unverified.
The UIKit suite includes landscape library navigation and offline About readers.
Run it through `tools/test_ios_library_ui.py` on an iPad simulator, then inspect:

- Portrait, landscape, narrow Split View, and rotation while settings are open.
- Dark/light appearance, every accent, larger accessibility text and VoiceOver.
- Empty collection, search with no results, favorites and missing artwork.
- Keyboard/controller focus, import, save-state actions and cancel/back paths.
- Offline credits/license scrolling and return to settings.
- On-device gameplay controls, pause/resume and background/foreground behavior.

Keep screenshot attachments from the UIKit suite with the release validation.
