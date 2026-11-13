# Library artwork

Cara downloads Nintendo 3DS box covers and gameplay screenshots from
[Libretro's thumbnail collection](https://thumbnails.libretro.com/Nintendo%20-%20Nintendo%203DS/).
Covers appear in the collection; screenshots supply the recent-game hero and
Game details banner. For the usual 400×480 stacked 3DS capture, Cara extracts
the upper screen for the banner. Custom hero images keep their full composition.
Screenshot quality depends on the available source image.
The embedded game icon remains the fallback when an image is unavailable.

Artwork lookup sends the selected artwork filename to Libretro. ROM contents,
saves, and local file paths are not uploaded. Matching prefers the exact ROM
filename, then normalized names and the ROM's region. Ambiguous matches are
ignored. This collection covers retail titles; unmatched and digital titles may
need custom images.

Long-press a game and open **Artwork**, or open **Game details → Artwork**:

- **Choose cover from Files…** replaces the collection artwork.
- **Choose hero banner from Files…** replaces both banner views.
- **Use automatic artwork** removes both custom overrides and restores downloaded
  artwork or the embedded icon.

Choose a square/portrait image for the cover and a wide landscape image for the
hero. Imported images are copied into Cara, so the original file can be moved.
Supported images must be no larger than 12 MB and 100 megapixels. Images are
orientation-corrected and downsampled to a maximum edge of 1,024 pixels for covers
or 1,920 pixels for heroes.

Downloads and image processing run on a serial library queue, outside the
emulator and UI threads. Decoded artwork has a 32 MiB memory-cache budget.
Downloaded images have a 256 MiB disk budget; older downloads can be evicted.
Custom images are stored separately and are not evicted by the download cache.
Libretro indexes refresh after seven days; existing indexes and downloaded
images remain usable offline. Failed image lookups are not repeatedly requested
within one library session. Reopen Cara to retry a failed download.

No third-party game artwork is bundled in the repository. Artwork remains the
property of its respective owners. Libretro's collection is documented at
[libretro-thumbnails](https://github.com/libretro-thumbnails/libretro-thumbnails).

For ROM-free checks, build the shell preview with
`python3 tools/build_ios_shell_preview.py --output /path/to/output`, then launch
it in an iPad simulator with `CARA_UI_SCENE=artwork-checks`. Set
`CARA_ARTWORK_LIVE_CHECK=1` to also verify the live provider. The test writes
`tmp/artwork-checks.txt` in its app container after successful completion.
