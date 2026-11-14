# System application compatibility

Tests on November 13, 2026 used user-provided files in an isolated local simulator
NAND. No console BIOS, account identity, keys or system assets were added to the
repository. The supplied applications and installed content remain private test
data.

## Verified local behavior

`Mii Maker.cci` is a decrypted standalone NCCH, title `0004001000021700`. It
loads, renders setup dialogs and advances via the ordinary HID touchscreen path.
A private compatible checkpoint restored successfully and a scripted touch
advanced the setup message. Creating and saving a complete personal Mii, camera
features, multiplayer and physical-device behavior are not established by this
startup test.

`errfix.3dsx` is an installer for seven data archives: Mii face data, region data,
a bad-word list and four shared-font archives. Its AM import sequence installed
all seven into the isolated NAND. All seven installed content hashes match their
TMD metadata. A restart of Mii Maker found the retained resources. Ticket-key
warnings did not imply encrypted content failure: the installed NCCH archives
are unencrypted. The utility's result 11 is the named `ShutdownRequested` status,
an intentional guest exit, not a renderer failure. The frontend treats it as an
ordinary stopped session.

The [user-supplied community guide](https://gbatemp.net/threads/how-to-get-the-home-menu-on-citra-full-guide-mii-fix.679214/)
identifies the repair utility separately from Home Menu. Neither supplied file
contains Home Menu itself. No system application was downloaded from that guide.

## Separate work

Home Menu needs separately supplied application content and additional APT/NS
launch, handoff and return behavior; several current commands are stubs. Booting
a screen would not establish a working console shell.

The default iOS Mii selector still returns its built-in standard Mii even though
`Frontend::LoadMiis` can read saved Miis. A native saved-Mii picker is a separate
frontend feature. ACT/NNID remains unimplemented. Local wireless uses NWM UDS and
needs its own transport integration and two-instance tests; the deployed relay
does not supply account services or Nintendo authentication.
