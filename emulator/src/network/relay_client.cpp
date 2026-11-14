// Copyright 2026 Cara contributors. Licensed under GPL-2.0-or-later.
#include "network/relay_client.h"
#include <mutex>
#include <atomic>
namespace Network {
namespace {
std::mutex session_mutex;
std::atomic<bool> relay_work{false};
std::shared_ptr<RelayClient> relay_session;
}
void InstallRelaySession(std::shared_ptr<RelayClient> client) {
    std::lock_guard lock(session_mutex);relay_session=std::move(client);
    relay_work.store(true,std::memory_order_release);
}
std::shared_ptr<RelayClient> GetRelaySession() {
    std::lock_guard lock(session_mutex);return relay_session;
}
namespace Detail {
void NotifyRelayWork() {relay_work.store(true,std::memory_order_release);}
bool TakeRelayWork() {return relay_work.load(std::memory_order_acquire) && relay_work.exchange(false,std::memory_order_acq_rel); }
}
const char* RelayClient::StatusText(const Status& status) {
    switch(status.state) {
    case State::Idle:return "Not connected";
    case State::Connecting:return "Connecting";
    case State::Connected:return "Connected to private room";
    case State::Disconnected:return "Disconnected";
    case State::Failed:break;
    }
    switch(status.failure) {
    case Failure::InvalidConfiguration:return "Check the secure endpoint and room details";
    case Failure::AuthenticationRejected:return "Room admission was rejected";
    case Failure::ProtocolError:return "Unexpected relay traffic; connection closed";
    case Failure::QueueLimit:return "Traffic limit reached; connection closed";
    default:return "Connection failed";
    }
}
#ifndef __APPLE__
std::shared_ptr<RelayClient> RelayClient::Create() {return {};}
#endif
} // namespace Network
