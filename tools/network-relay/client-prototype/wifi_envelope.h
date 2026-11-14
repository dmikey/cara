// Copyright 2026 Cara contributors. Licensed under GPL-2.0-or-later.
// PRIVATE PROTOTYPE: no emulator callbacks or legacy ENet format changes.
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>
namespace Cara::Relay {
using Bytes = std::vector<std::uint8_t>;
using Mac = std::array<std::uint8_t, 6>;
enum class Kind : std::uint8_t { Beacon, Data, Authentication, AssociationResponse, Deauthentication, NodeMap };
struct Packet { Kind kind{}; std::uint8_t channel{}; Mac transmitter{}, destination{}; Bytes payload; };
constexpr std::size_t HeaderSize = 18, MaxOpaque = 4088, MaxData = MaxOpaque - HeaderSize;
constexpr Mac Broadcast{255,255,255,255,255,255};
inline std::uint16_t BE(const Bytes& p, std::size_t n) { return (std::uint16_t(p[n]) << 8) | p[n+1]; }
inline std::uint16_t LE(const Bytes& p, std::size_t n) { return p[n] | (std::uint16_t(p[n+1]) << 8); }
inline bool Unicast(const Mac& m) { return !(m[0] & 1) && m != Mac{}; }
inline bool Node(std::uint16_t id) { return id >= 1 && id <= 16; }
inline bool Tags(const Bytes& p, std::size_t start) {
    while (start < p.size()) {
        if (p.size()-start < 2) return false;
        const auto size = p[start+1]; start += 2;
        if (size > p.size()-start) return false;
        start += size;
    }
    return true;
}
// Bounds for currently implemented UDS forms, not a generic 802.11 decoder.
// Stateful capacity/host identity checks must ALSO precede eventual HLE delivery.
inline bool PayloadValid(Kind kind, const Bytes& p) {
    if (p.size() > MaxData) return false;
    switch (kind) {
    case Kind::Beacon: return p.size() >= 12 && Tags(p, 12);
    case Kind::Authentication:
        return p.size() == 6 && LE(p,0) == 0 && (LE(p,2) == 1 || LE(p,2) == 2) && LE(p,4) == 0;
    case Kind::AssociationResponse:
        return p.size() >= 6 && LE(p,2) == 0 && Node(LE(p,4)&0x3fff) && Tags(p,6);
    case Kind::Deauthentication: return p.empty(); // Cara's current synthetic deauth form.
    case Kind::NodeMap: {
        if (p.size() < 2) return false;
        const auto count = BE(p,0);
        if (count > 16 || p.size() != 2 + std::size_t(count)*8) return false;
        std::array<bool,17> ids{};
        std::vector<Mac> addresses;
        for (std::size_t i=0; i<count; ++i) {
            Mac m{}; std::copy_n(p.begin()+2+i*8, 6, m.begin());
            const auto id = BE(p,8+i*8);
            if (!Unicast(m) || !Node(id) || ids[id] || std::find(addresses.begin(),addresses.end(),m)!=addresses.end()) return false;
            ids[id]=true; addresses.push_back(m);
        }
        return true;
    }
    case Kind::Data: {
        if (p.size()<10 || p[0]!=0xaa || p[1]!=0xaa || p[2]!=3 || p[3] || p[4] || p[5]) return false;
        if (BE(p,6)==0x876d) {
            if (p.size()<22) return false;
            const auto length=BE(p,8);
            return length>=14 && length<=14+0x5c6 && std::size_t(length)==p.size()-8 &&
                BE(p,12)==length-4 && p[14]==0 &&
                (Node(BE(p,18)) || BE(p,18)==0xffff) && Node(BE(p,20));
        }
        if (BE(p,6)!=0x888e) return false;
        if (BE(p,8)==0x201) return p.size()==8+0x30 && Node(BE(p,10)) && BE(p,12)==1;
        if (BE(p,8)!=0x202 || p.size()!=8+0x298) return false;
        const auto connected=p[26], maximum=p[27];
        if (!Node(maximum) || connected<1 || connected>maximum || !Node(BE(p,12)) || BE(p,12)>maximum) return false;
        // Current generator leaves client_mac_address zero; receiver never reads it.
        std::array<bool,17> ids{};
        for (std::size_t i=0;i<connected;++i) {
            const auto id=BE(p,8+24+i*40+32);
            // Reset slots can be zero after a disconnect; preserve that current form.
            if (id>maximum || (id!=0 && ids[id])) return false;
            ids[id]=true;
        }
        return true;
    }
    }
    return false;
}
inline bool Valid(const Packet& p) {
    return p.channel>=1 && p.channel<=14 && Unicast(p.transmitter) &&
        (Unicast(p.destination)||p.destination==Broadcast) && PayloadValid(p.kind,p.payload);
}
inline std::optional<Bytes> Encode(const Packet& p) {
    if (!Valid(p)) return {};
    Bytes out{1,static_cast<std::uint8_t>(p.kind),p.channel,0};
    out.insert(out.end(),p.transmitter.begin(),p.transmitter.end());
    out.insert(out.end(),p.destination.begin(),p.destination.end());
    out.push_back(p.payload.size()>>8);out.push_back(p.payload.size()&255);
    out.insert(out.end(),p.payload.begin(),p.payload.end()); return out;
}
inline std::optional<Packet> Decode(const Bytes& b) {
    if (b.size()<HeaderSize || b.size()>MaxOpaque || b[0]!=1 || b[1]>5 || b[3] || BE(b,16)!=b.size()-HeaderSize) return {};
    Packet p; p.kind=static_cast<Kind>(b[1]);p.channel=b[2];
    std::copy_n(b.begin()+4,6,p.transmitter.begin());std::copy_n(b.begin()+10,6,p.destination.begin());
    p.payload.assign(b.begin()+HeaderSize,b.end());
    if (!Valid(p)) return {};
    return p;
}
// channel==0 explicitly means scanning: only Beacon is delivered across channels.
inline bool ShouldDeliver(const Packet& p, const Mac& local, std::uint8_t channel) {
    return Valid(p) && Unicast(local) && p.transmitter!=local &&
        (p.destination==Broadcast || p.destination==local) &&
        (p.channel==channel || (channel==0 && p.kind==Kind::Beacon));
}
// NodeMap v1 is fixed-width BE. Translate ONLY at a future relay/UDS bridge.
// These helpers intentionally leave the existing native ENet bytes unchanged.
inline std::optional<Bytes> NativeNodeMapToWire(const Bytes& native) {
    if (native.size()<sizeof(std::size_t)) return {};
    std::size_t count=0;std::memcpy(&count,native.data(),sizeof(count));
    if (count>16 || native.size()!=sizeof(count)+count*8) return {};
    Bytes wire{0,static_cast<std::uint8_t>(count)};
    for (std::size_t i=0;i<count;++i) {
        auto start=sizeof(count)+i*8;
        wire.insert(wire.end(),native.begin()+start,native.begin()+start+6);
        std::uint16_t id;std::memcpy(&id,native.data()+start+6,2);
        wire.push_back(id>>8);wire.push_back(id&255);
    }
    if (!PayloadValid(Kind::NodeMap,wire)) return {};
    return wire;
}
inline std::optional<Bytes> WireNodeMapToNative(const Bytes& wire) {
    if (!PayloadValid(Kind::NodeMap,wire)) return {};
    std::size_t count=BE(wire,0);Bytes native(sizeof(count)+count*8);
    std::memcpy(native.data(),&count,sizeof(count));
    for (std::size_t i=0;i<count;++i) {
        std::copy_n(wire.begin()+2+i*8,6,native.begin()+sizeof(count)+i*8);
        const auto id=BE(wire,8+i*8);std::memcpy(native.data()+sizeof(count)+i*8+6,&id,2);
    }
    return native;
}
} // namespace Cara::Relay
