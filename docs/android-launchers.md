# Launch Cara games from Daijishō

In Daijishō, open Settings → Library → Add a Player. Name the player
**3ds - Cara**, select it as the default player for the Nintendo 3DS
platform, and keep **Kill package processes before am start** disabled.

Use this am start argument for the release app:

```text
-n org.cara.emu/org.cara.emu.ui.EmulationActivity
-a android.intent.action.VIEW
-d {file.uri}
--grant-read-uri-permission
```

For the debug package, use `org.cara.emu.debug` to the left of the slash.
For the standard preview, use `org.cara.emu.malipreview`. The activity
class on the right is the same for all builds.

Use the Android folder picker in Daijishō to add the ROM directory.
Cara opens the selected file directly using the launcher's read grant.
Use the legacy `-e GamePath {file.path}` only for a path Cara can already
read. Android scoped storage can restrict direct paths on newer devices.

Player accepted filename regex:

```regex
(?i)^(.+?)(?:\.(?:3ds|cci|cxi)(?=\.(?:zip|7z)$))?\.(?:3ds|3dsx|cci|cxi|app|axf|elf|zip|7z|z3ds|zcci|cia)$
```

Daijishō manages library scanning, artwork, favorites, widgets and game
shortcuts. The player arguments also apply when a game is launched from
those entry points. Cara uses its normal renderer, per-game preferences
and saves when launched externally. If a different game is active, Cara
requests confirmation and closes the first session before loading the
next one. No process kill is needed.

[Daijishō player setup and start arguments](https://github.com/TapiocaFox/Daijishou/wiki/Start-Arguments-Cheat-Sheet)
