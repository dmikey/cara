# 3DS activity in Cara

## Steps and Play Coins

On iPad and iPhone, stop the game and open **Settings → Library & storage → 3DS activity**. **Import today’s device steps** reads hourly counts from Core Motion on devices with a step counter. Import is opt-in each time. Repeated imports use the highest count seen for each local hour. **Record 100 simulated steps** also works on devices without a pedometer.

At the next game launch the PTM service loads the hourly ledger from the chosen profile. Games can read the total and hourly counts. Cara updates the shared 3DS `gamecoin.dat` before the cartridge runs. Every 100 steps recorded for a date earn one coin, up to ten per day and a wallet balance of 300. Previously credited days and coins spent by games are respected. An existing profile retains its wallet. The reference hardware normally counts steps in sleep mode. Cara imports steps explicitly, so it needs no background motion process. [Nintendo's Play Coin rules](https://en-americas-support.nintendo.com/app/answers/detail/a_id/230/p/50)

## StreetPass

First enable StreetPass **inside a game** and let it prepare a public outgoing message. Stop the game on both devices. In **3DS activity**, enable **Nearby StreetPass** on both and leave their libraries open in the foreground. Allow local-network access when prompted. Cara automatically exchanges registered title IDs, then sends eligible messages for titles registered on both devices over an encrypted MultipeerConnectivity session. Discovery uses a random peer name per session. Incoming messages use the receiving profile’s title key to verify the game payload. The recipient game reads them from its standard CECD inbox on its next launch.

Alternatively use **Export StreetPass messages…** to save a `.carapass` file in Files, then use **Import StreetPass messages…** on the receiving device. The file contains public game-authored messages and their game-level sender metadata. It excludes the mailbox authentication keys. Only share it with intended recipients. Importing an unchanged message again leaves the inbox count unchanged, including after the game consumes it. A new message ID is eligible on the next exchange. Friends-only messages, grouped batches, the original two-way handshake, send-count quotas and timed repeat encounters need further implementation. Nearby discovery stops on leaving the idle library or suspending Cara. Wireless interoperability with Nintendo consoles and unattended encounters while iOS sleeps are unverified. The UDS game multiplayer relay remains a separate transport. [CECD mailbox formats](https://www.3dbrew.org/wiki/StreetPass), [Apple MultipeerConnectivity](https://developer.apple.com/documentation/multipeerconnectivity)

## Desktop

Close any game using the profile. The native Mac and Linux executables support these commands. Supply the Cara profile directory that contains `nand`.

```sh
cara --activity "/path/to/profile" steps 100
cara --activity "/path/to/profile" export "/path/to/encounter.carapass"
cara --activity "/path/to/second-profile" import "/path/to/encounter.carapass"
```

On macOS, `cara --activity "/path/to/profile" nearby 60` starts the Apple nearby transport for 60 seconds, using the same protocol as iOS. The native Metal executable accepts the same arguments.

## Verification

- An ARM guest on Mac FastInterp/Metal opened `ptm:u` through `srv:`, read 337 total steps and read 237 for the requested hour. It opened `cecd:u` and read an inbox index containing one received message. All IPC calls succeeded. The native wallet advanced from 42 to 43 after 100 eligible steps on the current day.
- Two independent Mac processes with separate NAND profiles exchanged one message over actual encrypted MultipeerConnectivity using an isolated Bonjour service name. The receiving profile’s index, body, authentication tag and unread flags were checked separately with the desktop exchange fixture.
- Linux CI exercises two synthetic profiles, tampering, duplicate delivery, consumption and simulated steps. The pedometer regression covers partial hundreds, spending, rollover, daily and wallet limits.
- These checks use synthetic mailbox data. Retail StreetPass content, two physical devices, motion permission transitions, and sensor accuracy need device validation.

## Other game-visible service gaps

This audit inspected the named native HLE service tables and handlers. A registered command can still return placeholder data. The priorities below reflect the player action each gap prevents.

| Area | Current behavior | Next validation |
| --- | --- | --- |
| System time, pedometer and HOME Menu | PTM total and hourly history, system time since 2000, and the shared Play Coin wallet work without booting HOME Menu. Alarms, complete step-history export and activity/play-history logging remain incomplete. | Use a time-sensitive cartridge and compare activity log output and rollover across a DST transition. The Azahar reference has a configurable constant per-hour count and returns zero for total steps. [Azahar PTM](https://github.com/azahar-emu/azahar/blob/6f293a24daa292bd02b4ffee1a2adb5435e1ae00/src/core/hle/service/ptm/ptm.cpp) |
| CECD, NDM and friends | Cara can deliver eligible CECD records while games are stopped and signal the new-message event on boot. CECD daemon scan commands and parts of NDM are stubs. FRD lacks complete login, friend lists and presence, so friends-only messages are excluded. | Get two independently generated retail outboxes, then check consumption, reciprocal batches, friend filtering and repeat timing against real hardware. Upstream CECD also stubs Start/Stop. [Azahar CECD](https://github.com/azahar-emu/azahar/blob/6f293a24daa292bd02b4ffee1a2adb5435e1ae00/src/core/hle/service/cecd/cecd.cpp) |
| NFC/amiibo | The core accepts tag data and implements detection/reading. Several application-data read/write commands lack handlers. | Exercise a title that writes and rereads its own app-data block using an appropriate user-owned test tag/file. |
| APT applets, title management and system data | The frontend provides default keyboard and Mii selector applets. The APT/AM tables have missing operations for title launches, imports and system transitions. | Test game-requested applet transitions separately from system-title boot. Any needed commercial system titles, Miis or shared fonts must come from a user-supplied installation. |
| SpotPass, online accounts, store and notifications | Many BOSS commands return stub results; ACT account commands, NIM operations and NEWS storage are incomplete. | Trace individual game requests and distinguish a usable offline response from a service that requires an external endpoint. |
| Camera, IR and MCU | Camera capture has a frontend path, while exposure and white-balance controls are incomplete. Several IR and MCU commands have no handlers. | Validate sensor input, permission denial and reconnect/absence with suitable devices and games. |
