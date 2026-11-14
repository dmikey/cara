// Copyright 2026 Cara contributors. Licensed under GPL-2.0-or-later.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "network/wifi_envelope.h"

namespace Core { class System; }

namespace Network {
// Host-only session. Never serialized. No delegate may call HLE/kernel/guest memory.
class RelayClient {
public:
    using Mac = Cara::Relay::Mac;
    using Packet = Cara::Relay::Packet;
    enum class State { Idle, Connecting, Connected, Disconnected, Failed };
    enum class Failure { None, InvalidConfiguration, TLSOrNetwork, AuthenticationRejected,
                         ProtocolError, QueueLimit, Cancelled };
    struct Config {
        std::string endpoint = "wss://carap2p.therad.ninja/relay";
        std::string room;
        std::string password; // Memory only; never logged, serialized, or put in a URL.
    };
    struct Status {
        State state = State::Idle;
        Failure failure = Failure::None;
        std::uint64_t generation{};
        std::uint64_t received{}, sent{}, rejected{};
        std::size_t rx_queued{}, tx_queued{};
    };
    struct ReceivedPacket {
        std::uint64_t source_id{}; // Relay session tag, never a console/friend/node identity.
        Packet packet;
        bool peer_left{}; // Trusted server membership control, ordered with packet delivery.
    };
    static constexpr std::size_t MaxQueuePackets = 64;
    static constexpr std::size_t MaxQueueBytes = 256 * 1024;
    static constexpr std::size_t MaxDrainPackets = 32;
    static std::shared_ptr<RelayClient> Create(); // Foundation implementation on Apple hosts.
    static const char* StatusText(const Status& status); // Fixed generic text, no remote contents.
    virtual ~RelayClient() = default;
    // Returns whether async connection was started, not whether admission succeeded.
    virtual bool Connect(Config config) = 0;
    virtual void Disconnect() = 0;
    virtual Status GetStatus() const = 0;
    virtual Mac GetLocalMac() const = 0;
    // Nonblocking. Outgoing transmitter is always the session's local virtual MAC.
    virtual bool TrySend(const Packet& packet) = 0;
    // Foreground emulation worker ONLY for UDS delivery; no stored emulator callbacks.
    // Main/UI may query status but must not drain game traffic.
    virtual std::vector<ReceivedPacket> Drain(std::size_t limit = MaxDrainPackets) = 0;
};
// Install once before System::Load, then keep the same host session for that ROM.
void InstallRelaySession(std::shared_ptr<RelayClient> client);
std::shared_ptr<RelayClient> GetRelaySession();
namespace Detail { void NotifyRelayWork(); bool TakeRelayWork(); }
// Foreground emulation worker, immediately before RunLoop; capped to32 packets.
std::size_t DrainRelayPackets(Core::System& system);
} // namespace Network
