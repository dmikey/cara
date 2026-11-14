# Cara packet relay prototype

A standalone Go room relay for Cara-controlled local-wireless adapters. It uses binary packet frames after a JSON join, bounded queues, a total connection cap, and TLS 1.3 for remote listeners. No BIOS, firmware, console keys, Nintendo client certificates, account tokens are used. The WebSocket deployment transport uses the pinned `github.com/coder/websocket` dependency; its license is retained in `THIRD_PARTY_NOTICES.txt`.

**Status: private relay transport deployed and remotely verified at `wss://carap2p.therad.ninja/relay`; deployment/rollback evidence is in `DEPLOYMENT.md`.** Cara's current ENet `RoomMember` cannot connect to this protocol. The iOS adapter and StreetPass mailbox exchange are not implemented. Running this process does not enable Pokémon Internet play, trading, battling, or physical-console wireless interoperability.

## Local development

Use a currently supported Go release. The module language minimum is Go 1.23; production should use a currently supported toolchain. This deployment is built and tested with Go 1.27.4. The older Go 1.20.3 results below are historical TCP-only validation.

```sh
cd tools/network-relay
go test -race ./...
go test -run '^$' -bench BenchmarkLoopbackForward -benchmem
export CARA_RELAY_ROOMS='{"friends":"replace-with-a-long-random-room-password"}'
go run ./cmd/cara-relay
```

The default listener is `127.0.0.1:24873`. To run on a remote interface, provide your own TLS server certificate and private key using `-tls-cert` and `-tls-key`. Clients must validate the server's chain and hostname. Plaintext is allowed only on literal loopback addresses for local development. Room passwords travel inside TLS for remote use. Do not use Nintendo certificates or console identities.

`-max-peers` defaults to 32 total connections, including incomplete handshakes; `-queue-depth` defaults to 64 frames per peer. Both have a maximum of 1024. Joins time out after 10 seconds. Writes time out after 5 seconds. A full peer queue disconnects that peer. Authenticated peers may remain connected while idle. There is no persistent room state, packet storage, external service discovery, anonymous public room creation, or automatic network access from games.

## Protocol v1

Each frame is `type:u8 | length:u32 big endian | payload:length bytes`. The length cannot exceed 4096. Types:

| Type | Direction | Payload |
| --- | --- | --- |
| 1 Join | Client → server, first frame only | JSON `{"version":1,"room":"friends","password":"…"}` |
| 2 Welcome | Server → client | Relay-assigned peer ID, unsigned 64-bit big endian |
| 3 Packet | Client → server | 1–4088 opaque bytes |
| 3 Packet | Server → clients in same room | 8-byte source peer ID followed by unchanged opaque bytes |
| 4 Leave | Client → server | Ignored; closes this session |
| 5 Failure | Server → rejected client | Generic join rejection, then connection closes |

Packets fan out to other peers in the sender's room and never echo to the sender. Room passwords are compared via constant-time SHA-256 digest comparison. Source IDs are relay session routing tags, **not** console IDs, friend codes, MAC addresses, or authenticated Nintendo identities. Invalid protocol order, empty/oversized packets, malformed joins, and unknown frame types close the connection. Unexpected disconnects remove the peer.

The first adapter must define a stable serialization for `Network::WifiPacket` (type, channel, transmitter/destination MAC, payload), validate lengths/enums before touching the core, and route unicast/broadcast correctly. The relay intentionally does not parse or invent guest game messages. A later UDS adapter may replace all-peer fanout with destination-aware routing. StreetPass needs a distinct mailbox protocol and CECD events; routing arbitrary UDS frames cannot substitute for it.

## Limits of this prototype

TCP/TLS keeps the first protocol small and testable and provides ordered delivery, but packet loss can cause head-of-line blocking. Benchmark the actual two-device game path before selecting TCP, QUIC, or UDP for release. This relay has no reconnect/resume, protocol negotiation beyond v1, metrics, per-room bandwidth quotas, room-owner revocation API, or production abuse controls. This deployment serves configured private rooms; it does not provide public hosting/account management.

## Local validation record — 2026-11-13

On the development Mac (`darwin/arm64`, Go 1.20.3), `go vet ./...` and `go test -race ./...` passed. The earlier suite passed 10 consecutive race runs; the final suite passed five consecutive race runs and additionally checks malformed packets after an authenticated join and verifies that a deliberately blocked connection close cannot hold the room lock or prevent another room from forwarding. The tests use loopback TCP and in-memory pipes; they cover private room join/rejection, exact binary forwarding/source tagging, room isolation/no self-echo, malformed/truncated/oversized framing, connection caps, bounded queue disconnection, leave and shutdown. A two-second, one-worker parser fuzz run completed 63,965 executions without a failure. A CLI check rejected `0.0.0.0` without TLS before opening a listener.

The TLS loopback integration test uses the same `TLSListener` helper as the CLI. It generates an ephemeral CA and server certificate entirely in memory, connects two clients that verify the CA and hostname, confirms TLS 1.3 negotiation, joins a room and forwards exact binary payload/source tags. It also checks rejection of an untrusted CA, a wrong hostname and TLS 1.2. No permanent certificate or external network is used. `go vet ./...` and five consecutive race-test runs passed after these additions.

Three loopback benchmark runs forwarding a 1200-byte packet from one client through the relay to a second client measured **28.9–33.5 µs per packet (35.8–41.5 MB/s), 7 allocations per operation**. This sequential benchmark includes client framing and both local TCP legs, excludes TLS and WAN latency, and does not measure game compatibility or physical-device performance. Remote hosting and actual iOS game behavior remain untested.

## WebSocket deployment transport

`cmd/cara-relay-websocket` serves the same protocol-v1 byte stream at `/relay` over binary WebSocket messages. A message may contain part or all of a protocol frame; message boundaries do not replace the five-byte protocol framing. Each WebSocket message is limited to 4,101 bytes. Text and compression are disabled. The authenticated packet size and source tags are unchanged. This is a new transport for a future adapter, not an ENet bridge.

The HTTP listener accepts only literal loopback addresses. Publish it through a trusted HTTPS proxy with the exact `-public-host`, preserving Host and WebSocket upgrade headers. Same-host browser Origin checks remain enabled; native clients can omit Origin. Room secrets are read from a regular JSON file at most 64 KiB, mode0600 or0400, with the same room/password map used by the TCP server. Never put credentials in URLs, logs or command arguments. `/healthz` reports only service readiness, protocol and transport, never rooms, passwords or peer identities.

The deployment compose file confines the read-only, non-root service to loopback24874 on the existing host network; it exposes no raw TCP relay port. Its memory, CPU, process, descriptor and Docker log limits are explicit. Only its own container uses the new restart policy and health check. Nginx configuration is scoped to `carap2p.therad.ninja`, with bounded admission/connections, no traffic access log, and a dedicated Let's Encrypt certificate. Its renewal hook only acts for that certificate lineage. Existing DNS and other services remain separate.

Public clients must validate the system trust chain and hostname and require TLS1.3. The edge proxy negotiates public TLS; do not infer that a shared Cloudflare zone rejects TLS1.2 merely because this service and its verification client use TLS1.3. No global zone TLS policy is changed by this package.

`cmd/cara-relay-check` performs a bounded remote transport check using a private credentials file: three authenticated peers, exact maximum-size binary forwarding/source tag, no self echo, room isolation, wrong-password rejection, hostname/CA rejection and HTTPS health. It requires verified TLS1.3. It does not create rooms, invoke game services or prove emulator integration.

See `DEPLOYMENT.md` for the deployment receipt, scoped operations and rollback.
