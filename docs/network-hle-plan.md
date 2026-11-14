# BIOS-free networking and StreetPass plan

Audit date: 2026-11-13. This work uses high-level emulation and an original Cara relay protocol. No console BIOS, firmware, account credentials, keys, identity certificates, or proprietary service blobs are required by the new relay.

## Three separate capabilities

| Capability | Existing source | Missing work / release proof |
| --- | --- | --- |
| StreetPass | `cecd/cecd.cpp` stores mailbox files and handles message operations. `Start`, `Stop`, and `GetCecdState` are stubs; the state is always idle. | Opt-in mailbox matching/exchange, duplicate and capacity handling, mailbox metadata/event transitions, save persistence. Validate two different player saves exchanging game-generated data. |
| Local wireless (including compatible Pokémon local modes) | `nwm/nwm_uds.cpp` emulates beacons, association and data frames through `Network::RoomMember`. `network/network.cpp` initializes an ENet room client/server. | iOS currently lacks room initialization/join UI/lifecycle. Add an adapter, TLS trust, validated packet serialization, bounded receive queue, core-thread delivery and teardown. Validate two actual emulator instances performing the named game action. |
| Internet access / game online services | `soc_u.cpp` wraps real host IPv4 TCP/UDP sockets and DNS. AC connection state is partially stubbed. HTTP has optional host web support. | Opt-in network settings plus truthful AC state, nonblocking/cancellable socket operations and robust guest buffer validation. Game online services separately require a live compatible service, accounts/authentication and protocol implementation. A socket connection alone is insufficient. |

[Nintendo's official discontinuation notice](https://en-americas-support.nintendo.com/app/answers/detail/a_id/63227) states that general Nintendo 3DS online play ended on April 8, 2024 and explains that StreetPass uses local communication. Do not promise official Pokémon matchmaking, GTS, trading, battling, Bank, or other title-specific services from this work. Service availability and replacement-server compatibility need independent, current verification.

## Source findings

- CECD filesystem/message operations are already HLE. Their existence does not establish a working StreetPass exchange. The upstream [Azahar CECD source](https://github.com/azahar-emu/azahar/blob/master/src/core/hle/service/cecd/cecd.cpp) is a primary reference for further open-source comparison; no upstream feature-parity claim is made here.
- NWM `SendPacket` only sends when the ENet room member is joined/moderator; constructor callbacks likewise use the global room member. iOS `main.mm` does not initialize or join it. Existing desktop room behavior cannot be assumed on iOS.
- AC `ConnectAsync` reports success and toggles internal state; `GetWifiStatus` always reports no connection. This needs a consistent adapter-backed state rather than unconditional success.
- SOC currently makes native host calls and has no network opt-in gate. Several blocking operations can block the emulation thread (already noted in source). `Bind` passes `max(sizeof(sockaddr), guest_len)` for a stack address. Bind/connect/sendto also copy a full 28-byte guest union without validating buffer length although IPv4's defined fields need only 8 bytes. These are follow-up fixes; this prototype does not change core behavior.
- HTTP host requests are guarded by `ENABLE_WEB_SERVICE`, disabled in `tools/build.py`. `OpenDefaultClientCertContext` relies on `ClCertA`; `DecryptClCertA` reads a NAND title and AES key. Do not enable, supply, synthesize or depend on this path. Generic host HTTPS with the host trust store and application-controlled services must remain distinct from Nintendo identity authentication.
- `ssl_c.cpp` has unimplemented default-client-certificate and connection commands. FRD contains stubbed friend/presence operations; ACT mostly registers commands without an account implementation. No complete NEX/Nintendo account service is present.
- iOS `Info.plist` currently lacks a local-network privacy usage string. Before any LAN adapter, add a truthful purpose string and validate the OS permission flow. [Apple's local-network privacy guidance](https://developer.apple.com/documentation/technotes/tn3179-understanding-local-network-privacy) is the primary reference. Remote TLS relay access and LAN discovery should be separate controls.

## Implemented first increment

`tools/network-relay/` contains an independent, standard-library Go TCP/TLS prototype. It authenticates configured private rooms, forwards unchanged bounded opaque packets only inside a room, assigns routing-only source tags, bounds every peer queue, disconnects slow peers, caps total concurrent connections and tears sessions down on disconnect. The CLI refuses a non-loopback listener without TLS 1.3 credentials. Its protocol is documented beside the code.

This is **not ENet-compatible** and has no emulator adapter yet. It does not modify current game network behavior or claim title compatibility. The original transport tests were local. The private WebSocket transport is deployed at `wss://carap2p.therad.ninja/relay` and passed an external trusted-hostname TLS1.3 packet-forward/isolation test; see `tools/network-relay/DEPLOYMENT.md` for the scoped receipt and limits. No emulator adapter or title compatibility is implied.

## Implementation sequence and proof gates

1. Keep the Go transport independently testable. Verify join rejection, room isolation, binary payload integrity, disconnect cleanup, malformed frames, connection caps, slow-peer behavior and shutdown under the race detector. Measure local packet forwarding; retain transport limitations in the result.
2. Introduce a transport interface at the existing UDS packet boundary. Preserve desktop ENet compatibility as a separate backend. Add an iOS opt-in relay connection UI with explicit endpoint and user-entered room credentials. Initialize/tear down on the correct lifecycle; never auto-connect at game launch. Deliver parsed receive packets through a bounded queue on the emulation thread.
3. Validate two isolated emulator saves in a specifically named Pokémon title and local mode. Record successful discovery, join, actual trade/battle payload progression, disconnect/rejoin and save integrity. Local multiplayer over a relay still uses the game's local mode; it does not restore its Internet mode.
4. Implement a separate CECD mailbox exchange behind an explicit StreetPass opt-in. Exchange only game-generated outgoing messages between consented peers. Validate title/region matching, maximum sizes/counts, deduplication, transactional writes, result codes and CECD notification events. Never manufacture game rewards or import console system archives as a substitute for protocol handling.
5. Independently harden/gate SOC and expose generic host Internet access. Test with a controlled local HTTP/TCP/UDP endpoint and homebrew before any commercial title. Implement host-trusted HTTPS independently from Nintendo client certificates. Compatible replacement online-service integration is a later, separately scoped project.
