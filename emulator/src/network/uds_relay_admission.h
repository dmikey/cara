// Copyright 2026 Cara contributors. Licensed under GPL-2.0-or-later.
#pragma once
#include <array>
#include "network/wifi_envelope.h"
namespace Network::UDSRelay {
enum class Role { Offline, Hosting, Connecting, Client };
struct Peer {
    Cara::Relay::Mac mac{};
    unsigned node{};
    bool connected{}, has_node_data{};
};
struct Snapshot {
    Role role=Role::Offline;
    Cara::Relay::Mac local{}, host{};
    unsigned channel{}, local_node{}, maximum{}, total{}, peer_count{};
    std::array<Peer,16> peers{};
    bool storage_ready{}, free_node{};
};
struct NodeSet {
    std::array<unsigned,16> slots{};
    unsigned mask{};
};
inline std::optional<NodeSet> ActiveNodes(const Cara::Relay::Bytes& bytes) {
    using namespace Cara::Relay;
    if (bytes.size()!=672 || BE(bytes,6)!=0x888e || BE(bytes,8)!=0x202 || !PayloadValid(Kind::Data,bytes)) return {};
    NodeSet result;
    for(unsigned i=0;i<bytes[26];++i) {
        const auto id=BE(bytes,64+i*40);
        if(!Node(id) || id>bytes[27] || result.slots[id-1]) return {};
        result.slots[id-1]=id;result.mask|=1u<<(id-1);
    }
    if(!(result.mask&1) || !(result.mask&(1u<<(BE(bytes,12)-1)))) return {};
    return result;
}
// A CPU-thread snapshot immediately before callback. The codec is a separate boundary.
inline bool Allows(const Cara::Relay::Packet& packet,const Snapshot& s) {
    using namespace Cara::Relay;
    if(s.peer_count>s.peers.size() || !ShouldDeliver(packet,s.local,s.role==Role::Offline?0:s.channel)) return false;
    if(packet.kind==Kind::Beacon) return true;
    const Peer* peer=nullptr;
    for(unsigned i=0;i<s.peer_count;++i) if(s.peers[i].mac==packet.transmitter) peer=&s.peers[i];
    const bool host=packet.transmitter==s.host && Unicast(s.host);
    const bool room=s.maximum>=2 && s.maximum<=16 && s.total>=1 && s.total<s.maximum && s.free_node && s.storage_ready;
    switch(packet.kind) {
    case Kind::Authentication:
        return LE(packet.payload,2)==1 ? s.role==Role::Hosting && room && !peer && s.peer_count<s.maximum-1 :
               s.role==Role::Connecting && host;
    case Kind::AssociationResponse:return s.role==Role::Connecting && host && packet.destination==s.local;
    case Kind::NodeMap:return (s.role==Role::Connecting || s.role==Role::Client) && host;
    case Kind::Deauthentication:return s.role==Role::Hosting && peer && peer->connected && peer->has_node_data && Node(peer->node);
    case Kind::Data:
        if(BE(packet.payload,6)==0x888e) {
            return BE(packet.payload,8)==0x201 ? s.role==Role::Hosting && room && peer && !peer->connected :
                   (s.role==Role::Connecting || s.role==Role::Client) && host && bool(ActiveNodes(packet.payload));
        }
        if(s.role!=Role::Hosting && s.role!=Role::Client) return false;
        if(peer && peer->connected && peer->node==BE(packet.payload,20)) return true;
        if(s.role!=Role::Client || !host) return false;
        if(BE(packet.payload,20)==1) return true; // Host is absent from the synthetic NodeMap.
        // Explicit authoritative-host forwarding exception; still require an assigned source.
        for(unsigned i=0;i<s.peer_count;++i)
            if(s.peers[i].connected && s.peers[i].node==BE(packet.payload,20)) return true;
        return false;
    default:return false;
    }
}
} // namespace Network::UDSRelay
