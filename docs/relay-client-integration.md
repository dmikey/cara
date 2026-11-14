# UDS relay client audit — 2026-11-13

Source audit and an unwired standalone native prototype. The relay can forward opaque packets; emulator/game multiplayer is not implemented or proved. No credentials, console identity or guest state are part of this prototype.

## Smallest integration seam

NWM currently reaches Network::RoomMember at four families in emulator/src/core/hle/service/nwm/nwm_uds.cpp: SendPacket (78), host MAC choice (875), constructor MAC/callback subscription (1502–1513), destructor unsubscribe (1520). Keep UDS state and guest-generated frames intact. A narrow IWifiTransport should provide connected state, the local virtual MAC, bounded TrySend, and an RAII receive subscription. A default RoomMember wrapper preserves desktop ENet behavior. A passive relay session must exist before system.Load so NWM can subscribe; connecting remains an explicit user action.

Relay identity is a session routing tag, not console identity or MAC. Use one virtual MAC consistently in the shared page, host network info and outbound packet header. Track source-tag→MAC consistency for a bounded session; reject collisions/identity changes. Do not invent account IDs, friend codes, keys or Nintendo authentication. The adapter is for UDS, not CECD StreetPass or Nintendo online services.

## Proposed packet format and unwired prototype

Files: tools/network-relay/client-prototype/wifi_envelope.h, wifi_envelope_test.cpp, wifi_envelope_fuzz.cpp, fuzz_driver.cpp. C++17, self-contained; nothing is wired into NWM.

Wire v1: version u8=1, kind u8=0..5, channel u8, reserved u8=0, transmitter MAC6, destination MAC6, payload length u16 BE, payload. Header18B; maximum opaque packet4088B and payload4070B. All lengths are exact, fields use explicit bytes, and allocation is bounded. Relay receive Packet adds a separate source-tag u64 BE before this envelope. A future stream parser must validate that tag/header before decoding.

NodeMap v1 is count u16 BE plus MAC6/node-ID u16 BE entries, at most16. Explicit checked conversion helpers bridge to the current native internal NodeMap representation only at the new relay edge. Legacy ENet serialization and native NodeMap packets are unchanged. Current native NodeMap uses size_t plus native-endian u16 (nwm_uds.cpp99–143), so blindly forwarding it would break cross-ABI compatibility and permit out-of-bounds memcpy.

Routing rejects nonlocal unicast, own-source MAC, and channel mismatch; broadcast is allowed. Channel0 explicitly means scan mode, where only beacons cross channels. Selection of scan/current-channel state must happen at the UDS bridge. The ENet server currently filters MAC destinations (network/room.cpp HandleWifiPacket); the Go relay intentionally fans out, so this local filter is required, not an optional optimization.

Type validation covers Beacon fixed header+bounded TLVs; Authentication6B and supported fields; AssociationResponse minimum6B, success status, bounded association ID and trailing TLVs; current Cara zero-payload Deauthentication; NodeMap count/IDs/unique MACs; LLC header; SecureData exact protocol lengths, size<=0x5c6 guest data, node IDs and unsupported management rejection; EAPoL Start56B; EAPoL Logoff672B, node count<=maximum<=16 and bounded node IDs. It accepts generated zero unused client-MAC fields and cleared zero node slots after disconnect. These are bounds for the implemented emulator forms, not a full 802.11 validator.

## Remaining blockers before callback wiring

The codec is not a sufficient authorization/state boundary. Do not deliver arbitrary decoded frames yet.

- EAPoLStart can be structurally valid but arrive after pending auth peers consume host capacity; HandleEAPoLPacket still ASSERTs full host at nwm_uds.cpp217 before indexing node storage. Recheck host state/capacity/node-map authorization on the CPU thread immediately before delivery. Invalid remote input must return/drop, not assert.
- Beacon IPC output depends on the guest-requested output buffer. nwm_uds.cpp615 only asserts current position, then writes header+remote payload. A bounded queue/envelope cannot prove all entries fit that particular buffer. The consumer must stop before a complete entry exceeds remaining output bytes and report only copied entries.
- SecureData source node IDs must bind to the sending peer's assigned UDS node, not merely fall within1..16. Relay source-tag→MAC consistency does not stop a peer claiming another node ID. Authoritative-host forwarding needs an explicit exception because current host rerouting rewrites the outer transmitter MAC.
- NodeMap/EAPoL host identity must match the UDS session's authoritative host; a room password alone does not grant every peer authority to replace node maps or assigned IDs.
- SecureData incoming queues currently grow without an explicit per-channel byte/packet cap. A bounded URLSession queue does not bound the later UDS queue if delivery outruns the guest reader. Admission/drop policy needs an explicit bound and tests.
- Existing parser memcpy sites (uds_connection.cpp25/77; uds_data.cpp282/312 onward) assume valid sizes. The new edge must always validate before callback, and stateful guards above must survive teardown/load changes. Legacy ENet hardening can be reviewed separately; do not silently change its wire protocol.

## iOS adapter contract

Use Foundation NSURLSessionWebSocketTask (binary messages), ephemeral configuration, normal system hostname trust, TLS minimum1.3, and reject all redirects before sending a Join. No accept-all trust delegate or ATS exception. Set maximumMessageSize4101. The Go stream adapter may split five-byte relay header/body over WebSocket messages; parse a bounded stream, not one-frame-per-message. Accept Welcome only with exactly8B ID, and forbid outbound packets before successful admission.

Delegate callbacks parse/validate/queue immutable values only. HLE::g_hle_lock is recursive but explicitly does not protect guest memory from non-CPU threads (core/hle/lock.h). Drain a bounded queue on the emulator worker immediately before system.RunLoop (ios/main.mm1942 in reviewed canonical), with HLE synchronization and current session validation. Do not call NWM/kernel/memory from URLSession callbacks. Suggested hard caps:64 packets/256KiB in each direction; overflow disconnects and clears that generation. Serialize asynchronous sends; no network waits on emulation thread and no added timer flushes.

Background, ROM switch, explicit disconnect, state load and shutdown invalidate a generation and clear queues/subscriptions before service-manager destruction (system.Shutdown at2359). Late callbacks must never hold/call raw NWM pointers. Avoid main-thread join waiting for callbacks that require that same main thread. Socket/task/password/relay ID are host-session objects and must not be serialized into guest states. Disconnect on load rather than impersonating a previous remote session. Credentials stay in memory unless user explicitly requests secure storage; no log/URL/command-line password.

Primary API references checked:
- https://developer.apple.com/documentation/foundation/urlsessionwebsockettask
- https://developer.apple.com/documentation/foundation/urlsessionconfiguration/tlsminimumsupportedprotocolversion
The installed SDK also exposes maximumMessageSize and TLSMinimumSupportedProtocolVersion. Remote WSS alone requires no Bonjour/local-network discovery feature.

## Native evidence and next gate

See tools/network-relay/client-prototype/README.md for commands/results and limitations. The prototype exercises all six kinds (eight forms including both EAPoL types), malformed/truncated/oversized packets, routing, fixed-width NodeMap/native translation, deterministic mutation and sanitizer checks. It does not execute actual UDS callbacks, NSURLSession, a ROM, or an iOS app.

Next safe implementation gate: review these codec restrictions against actual generated packets, add stateful UDS admission/consumer bounds, then a narrow IWifiTransport wrapper plus lifecycle-safe worker delivery. Before any game claim, run a scripted two-instance UDS host/join/data/disconnect test through the actual WSS service, followed by two distinct real instances performing a named in-game action. No hardware timing change is required by this design.
