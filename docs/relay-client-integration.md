# Relay client and UDS integration

The optional Apple relay transport and NWM service bridge are implemented. They use the deployed TLS WebSocket relay at `wss://carap 2p.therad.ninja/relay`. Native transport and two-process service-HLE witnesses pass. These establish packet delivery and emulator service behavior, **not Pokémon trades/battles, guest-issued UDS IPC, Nintendo online services, or StreetPass exchange**. The companion iOS room controls and lifecycle hooks are reviewed separately.

## Frontend contract and lifecycle

`network/relay_client.h` is standard C++. `RelayClient::Create()` returns a shared host session. `Connect(Config{endpoint,room,password})` starts asynchronous admission; its return value does not mean connected. `GetStatus()` and fixed `StatusText()` expose bounded generic state without remote text or credentials. `Disconnect()` cancels tasks, invalidates the generation and clears queues. No callback owns a raw service/kernel pointer.

Install the shared session with `Network::InstallRelaySession` before `System::Load`. Keep it stable for that ROM. Invoke `Network::DrainRelayPackets(system)` immediately before normal foreground `RunLoop`. The empty/disconnected path reads an atomic wake flag and returns without registry locks, status queries or vector allocation. Work drains at most 32 FIFO events; remaining work rearms the flag. URLSession delegates only parse/validate/queue host values. HLE/kernel/memory operations occur on the emulation worker with the recursive HLE lock.

Background, ROM switch, state load and shutdown must disconnect before destroying services. First synchronization clears restored associations and cached peers/beacons even if the new constructor observed the same disconnected generation; it signals actual lost-association events and reasserts the current virtual MAC after Kernel shared-page restore. Sockets, room passwords, source IDs, reconnect tokens and host generations are never serialized. Foreground menus while online must keep ordinary paced guest execution running or explicitly disconnect; do not pump HLE separately while guest time is paused.

The Foundation session is ephemeral, uses system hostname trust with TLS minimum 1.3, rejects redirects, accepts only binary messages and limits them to 4,101B. URL user/password/query/fragment components are forbidden even when empty. Scheme comparison is case-insensitive. Room names are 1..64 UTF-8 bytes and passwords 1..512 bytes; server rooms still require configured high-entropy passwords. Credentials never enter URLs, logs or command arguments. There is no certificate bypass, ATS exception, automatic game-launch connection or persistence of credentials.

## Wire and membership

The shared `network/wifi_envelope.h` codec has version 1, kind 0..5, radio channel, zero reserved byte, transmitter/destination MAC 6 each, payload length u 16 BE and exact payload. Its 18B header fits within the relay's 4,088B opaque limit. NodeMap uses fixed u 16 BE counts and IDs with at most 16 entries; checked bridge helpers translate to/from the existing internal native NodeMap representation. Existing ENet serialization remains unchanged.

The client requires the relay's opt-in membership capability before becoming Connected. Server-assigned PeerJoined/PeerLeft IDs are authenticated by the same TLS room connection. Source→MAC bindings exist only for current admitted IDs. An active duplicate MAC or a peer changing its MAC fails closed. A server PeerLeft retires that binding and queues a control event on the same FIFO as packets, before any replacement can reach NWM. Unknown, never-bound departures do not fabricate a MAC.

A random 128-bit memory-only instance token lets the server retire an old connection before admitting its same-room replacement, even when socket cancellation races a new handshake. Tokens are not console/account identities and are never broadcast or logged. The server's global connection/handshake cap remains intact; full capacity may require an explicit retry. The opt-in protocol preserves the legacy Welcome/Packet stream for clients that do not request it.

## Admission and bounds

The pure state policy in `network/uds_relay_admission.h` runs again against the current NWM snapshot before callback. It checks destination/channel/self filtering, chosen-host authority, assigned source nodes, host capacity/storage and legal connection phase. Authentication/association/EAPoL frames cannot make an arbitrary room peer replace the chosen host or its NodeMap. SecureData's source node must belong to its transmitter; authoritative-host forwarding is an explicit exception limited to known assigned nodes. Host node 1 is absent from the existing synthetic NodeMap and is handled explicitly.

Connected peer departure uses the host's deauth cleanup; pending-auth departure simply releases its pending entry. A client disconnects only when its selected host leaves. Host deauth broadcasts the existing authoritative NodeMap, allowing a later-joined client to prune a departed peer it knew only through that map. Removed assignments also purge their queued data before node reuse. Active host NodeInfo entries stay compact while retaining node IDs; received Logoff records rebuild status arrays by actual node ID, including sparse/reused order `[1,3,2]`.

RX and TX each have 64-packet/256KiB caps; stream reassembly is bounded to 8,202B. Sends are serialized with only one Foundation send in flight. Slow/overflowing transport sessions disconnect and clear their generation. UDS separately caps each data channel to 64 packets and all channel backlog to 256KiB. Beacons remain capped at 15; mapped beacon replies count only complete entries that fit the actual guest output, including short-header rejection. No guest clock, instruction charge, timer visibility, event deadline or idle-loop policy changes.

## Reproducible evidence

`tools/network-relay/client-prototype/check.sh` compiles the same shared codec, stream/session parser and admission policy under ASan+UBSan with no extra libraries or network. Results:72,604 codec assertions and 100,000 deterministic mutations;250,000 standalone mutations/resizes;2,257 stream/membership/queue/generation assertions;290 state/host/node assertions. These are deterministic mutation checks, not coverage-guided fuzzing. The Apple toolchains lacked a libFuzzer runtime; its portable entry point is retained.

Apple CMake provides these explicit standalone targets:

- `cara-relay-client-configuration-self-test`: malformed/empty URL components, uppercase scheme, byte limits and cancellation generation.
- `cara-relay-client-live-test`: private stdin credentials; independent sender/receiver, exact maximum envelope and reply, source tag, no echo, isolated room and bad-password rejection.
- `cara-relay-client-reconnect-test`: two processes;40 same-MAC reconnect epochs, unique source IDs and trusted leave before every rebind.
- `cara-nwm-relay-hle-live-test` (headless software presentation only): actual Core/Kernel/NWM service-HLE witness. Generate its original guest with `tools/make_uds_test_elf.py`. Run `BINARY ELF EMPTY_OUTPUT_DIRECTORY --offline` for pending/full host, sparse node reuse, host authority, NodeMap-only departure, channel/global queues, real mapped beacon IPC buffer bounds, unchanged ticks and restored-association checks. Live mode instead reads private JSON config on stdin and uses two processes (`role` host/client).

The live NWM witness loads an original generated busy-counter ELF, initializes real UDS shared memory/events, discovers an actual beacon, and invokes actual Initialize/BeginHosting/Connect/Bind/SendTo/PullPacket helpers. It verifies 512-byte payloads both ways, assigned node 2, source spoof rejection, departure/status events and disconnected restore handling. It enters the generated guest before Connect so the requesting thread is Running. Host pacing retains 100% guest ticks through ordinary RunLoop. The test-only friend driver invokes services; the generated guest itself does not issue UDS IPC. No firmware or proprietary assets are needed. The standalone fixture has no production UI entry point.

The first private harness attempt incorrectly invoked Connect on a Ready thread and hit the scheduler assertion. The corrected busy-counter witness and running-thread check pass. Native syntax/compile and these receipts do not certify arbitrary game behavior. Next release proof is two real isolated game instances performing a named local-wireless action and preserving their saves; StreetPass and Internet-service integration remain separate work.

Primary API references: [Apple URLSessionWebSocketTask](https://developer.apple.com/documentation/foundation/urlsessionwebsockettask), [TLS minimum](https://developer.apple.com/documentation/foundation/urlsessionconfiguration/tlsminimumsupportedprotocolversion). Local SDK declarations were also checked.
