# Unwired relay client packet prototype

**Not wired to the emulator. Stateful admission is still required.** These standalone C++17 files propose a versioned, bounded `WifiPacket` envelope, destination/channel filtering, and payload bounds before a future UDS callback. They do not implement a WebSocket client, modify ENet serialization, access guest memory, or establish game multiplayer. All source is original Cara contribution under GPL-2.0-or-later; the relay's third-party ISC notice remains separate.

See [the integration audit](../../../docs/relay-client-integration.md) for the proposed transport seam and remaining safety gates, including host capacity, authoritative peer identity, guest output-buffer space, downstream queue bounds, and CPU-thread delivery/lifecycle.

## Run the checks

From the repository root:

```sh
tools/network-relay/client-prototype/check.sh
```

Requires a C++17 compiler with AddressSanitizer and UndefinedBehaviorSanitizer (the Ubuntu GitHub runner provides these). No extra libraries, Go build, ROM, emulator build, network access, credentials, or proprietary assets are required. An optional build-directory argument retains generated binaries and corpus seeds for inspection; otherwise a temporary directory is removed on exit. Set `CXX` to choose a compiler. The relay CI job runs this same command.

The unit suite checks all six packet kinds (eight forms including two EAPoL forms), truncation/oversize, enum/length/TLV/count corruption, routing filters, and fixed-width NodeMap conversion at an explicit bridge boundary. It performs 100,000 deterministic mutations. The second executable invokes the fuzz entry point with 250,000 deterministic corpus mutations and resizes under both sanitizers. Seeds are generated arithmetic fixtures from source, never captured game or user data.

The repository script passed 72,604 unit assertions and the 250,000-mutation driver on macOS with both sanitizers. This includes maximum-size envelope and all16-node NodeMap boundaries. The Ubuntu CI step is configured; this local result is not an executed Ubuntu CI receipt. These are native parser/routing checks, **not coverage-guided fuzzing**, emulator-callback, iOS, scheduling, or game evidence. The installed Apple toolchains lacked the libFuzzer runtime; no coverage-guided run is claimed. `wifi_envelope_fuzz.cpp` remains a standard `LLVMFuzzerTestOneInput` entry point for toolchains that provide it.

Fixtures are hand-authored against current UDS generator layouts rather than linked to those generators. Compatibility details include the unused zero EAPoLLogoff client-MAC field and cleared zero node slots after disconnect. Bounds cover currently implemented Cara UDS forms; this is not a general 802.11 decoder. Do not connect this parser directly to NWM without implementing and testing the stateful checks in the audit.
