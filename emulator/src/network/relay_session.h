// Copyright 2026 Cara contributors. Licensed under GPL-2.0-or-later.
#pragma once
#include <algorithm>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include "network/relay_client.h"

namespace Network::Detail {
// All methods require the owning transport's mutex. No HLE references or callbacks.
struct RelaySession {
    using Bytes = Cara::Relay::Bytes;
    RelayClient::Status status;
    RelayClient::Mac local{};
    std::uint64_t peer_id{};
    bool welcomed{};
    std::set<std::uint64_t> admitted;
    Bytes partial;
    struct Queued {RelayClient::ReceivedPacket value;std::size_t bytes;};
    std::deque<Queued> incoming;
    std::deque<Bytes> outgoing;
    std::size_t rx_bytes{}, tx_bytes{};
    std::map<std::uint64_t, RelayClient::Mac> sources;
    explicit RelaySession(RelayClient::Mac mac) : local(mac) {}
    void Clear() {
        peer_id=0;welcomed=false;admitted.clear();partial.clear(); incoming.clear(); outgoing.clear(); sources.clear();
        rx_bytes=tx_bytes=0; status.rx_queued=status.tx_queued=0;
    }
    void Fail(RelayClient::Failure failure) {
        Clear(); status.state=RelayClient::State::Failed; status.failure=failure; ++status.rejected;
    }
    void Begin() {
        Clear(); ++status.generation; status.state=RelayClient::State::Connecting;
        status.failure=RelayClient::Failure::None;
    }
    void Disconnect() {
        Clear(); ++status.generation; status.state=RelayClient::State::Disconnected;
        status.failure=RelayClient::Failure::Cancelled;
    }
    static Bytes Frame(std::uint8_t type, const Bytes& payload) {
        Bytes b{type,0,0,std::uint8_t(payload.size()>>8),std::uint8_t(payload.size())};
        b.insert(b.end(),payload.begin(),payload.end()); return b;
    }
    static std::uint64_t ID(const std::uint8_t* p) {
        std::uint64_t id=0;for(unsigned i=0;i<8;++i) id=(id<<8)|p[i];return id;
    }
    bool Handle(std::uint8_t type, const Bytes& body) {
        if(type==5) {Fail(RelayClient::Failure::AuthenticationRejected);return false;}
        if(type==2 && status.state==RelayClient::State::Connecting && !welcomed && body.size()==8) {
            peer_id=ID(body.data());
            if(peer_id==0) return false;
            welcomed=true;return true;
        }
        if(type==8 && status.state==RelayClient::State::Connecting && welcomed && body==Bytes{1}) {
            status.state=RelayClient::State::Connected;return true;
        }
        if((type==6 || type==7) && status.state==RelayClient::State::Connected && body.size()==8) {
            const auto id=ID(body.data());if(!id || id==peer_id) return false;
            if(type==7) return admitted.size()<32 && admitted.insert(id).second;
            if(admitted.erase(id)!=1) return false;
            auto source=sources.find(id);
            if(source!=sources.end()) {
                if(incoming.size()>=RelayClient::MaxQueuePackets || 8>RelayClient::MaxQueueBytes-rx_bytes) {Fail(RelayClient::Failure::QueueLimit);return false;}
                RelayClient::Packet departed;departed.transmitter=source->second;
                incoming.push_back({{id,std::move(departed),true},8});rx_bytes+=8;status.rx_queued=incoming.size();sources.erase(source);
            }
            return true;
        }
        if(type!=3 || status.state!=RelayClient::State::Connected || body.size()<8+Cara::Relay::HeaderSize) return false;
        const auto id=ID(body.data());if(!id || id==peer_id || admitted.count(id)!=1) return false;
        auto decoded=Cara::Relay::Decode(Bytes(body.begin()+8,body.end()));
        if(!decoded || decoded->transmitter==local) return false;
        auto source=sources.find(id);
        if(source!=sources.end() && source->second!=decoded->transmitter) return false;
        if(source==sources.end()) {
            if(sources.size()>=32 || std::any_of(sources.begin(),sources.end(),[&](const auto& pair){return pair.second==decoded->transmitter;})) return false;
            sources.emplace(id,decoded->transmitter);
        }
        if(incoming.size()>=RelayClient::MaxQueuePackets || body.size()>RelayClient::MaxQueueBytes-rx_bytes) {
            Fail(RelayClient::Failure::QueueLimit);return false;
        }
        rx_bytes+=body.size();incoming.push_back({{id,std::move(*decoded),false},body.size()});
        ++status.received;status.rx_queued=incoming.size();return true;
    }
    bool Feed(const Bytes& chunk) {
        if(status.state!=RelayClient::State::Connecting && status.state!=RelayClient::State::Connected) return false;
        if(chunk.empty() || chunk.size()>4101 || partial.size()+chunk.size()>8202) {Fail(RelayClient::Failure::ProtocolError);return false;}
        partial.insert(partial.end(),chunk.begin(),chunk.end());
        while(partial.size()>=5) {
            const std::uint32_t length=(std::uint32_t(partial[1])<<24)|(std::uint32_t(partial[2])<<16)|(std::uint32_t(partial[3])<<8)|partial[4];
            if(length>4096) {Fail(RelayClient::Failure::ProtocolError);return false;}
            if(partial.size()<5+length) return true;
            const auto type=partial[0];Bytes body(partial.begin()+5,partial.begin()+5+length);
            partial.erase(partial.begin(),partial.begin()+5+length);
            if(!Handle(type,body)) {
                if(status.state!=RelayClient::State::Failed) Fail(RelayClient::Failure::ProtocolError);
                return false;
            }
        }
        return true;
    }
    bool Send(const RelayClient::Packet& packet) {
        if(status.state!=RelayClient::State::Connected) return false;
        auto local_packet=packet;local_packet.transmitter=local;
        auto encoded=Cara::Relay::Encode(local_packet);if(!encoded) {++status.rejected;return false;}
        auto frame=Frame(3,*encoded);
        if(outgoing.size()>=RelayClient::MaxQueuePackets || frame.size()>RelayClient::MaxQueueBytes-tx_bytes) {
            Fail(RelayClient::Failure::QueueLimit);return false;
        }
        tx_bytes+=frame.size();outgoing.push_back(std::move(frame));status.tx_queued=outgoing.size();return true;
    }
    bool SendForGeneration(const RelayClient::Packet& packet, std::uint64_t expected_generation) {
        if (status.generation != expected_generation) return false;
        return Send(packet);
    }
    std::vector<RelayClient::ReceivedPacket> Drain(std::size_t count = RelayClient::MaxDrainPackets) {
        std::vector<RelayClient::ReceivedPacket> result;
        count=std::min({count,RelayClient::MaxDrainPackets,incoming.size()});result.reserve(count);
        while(count--) {
            rx_bytes-=incoming.front().bytes;
            result.push_back(std::move(incoming.front().value));incoming.pop_front();
        }
        status.rx_queued=incoming.size();return result;
    }
};
} // namespace Network::Detail
