// Standalone diagnostic prototype. No emulator dependencies or memory access.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace CaraRecordSpan {
enum class Reason : std::uint8_t {
    None, NoScope, ContextOverflow, ScopeMismatch, InvalidRange,
    CounterOverflow, PacketMismatch, HeaderMismatch, ParameterMismatch, UnknownAddressMapping
};
struct ScopeToken { std::uint64_t capture=0, id=0; std::size_t depth=0; };
struct PacketToken {
    std::uint64_t capture=0, scope=0, id=0, header_address=0, header_epoch=0;
};
struct Origin {
    Reason reason=Reason::NoScope;
    bool valid=false;
    std::uint64_t capture=0, scope=0, packet=0;
    std::uint64_t header_address=0, header_epoch=0;
    std::uint64_t parameter_address=0, parameter_epoch=0;
    std::uint32_t slot=0, port=0;
};

// Fixed storage, owner-thread only. Invoke solely in an enabled diagnostic.
// Addresses are supplied by the caller from its actual CURRENT parser buffer;
// never infer them from a stale guest command-list addr field.
template<std::size_t Capacity=8> class Tracker {
    static_assert(Capacity>0);
    struct Span { std::uint64_t base=0, words=0, epoch=0; };
    struct Packet {
        PacketToken token{};
        std::uint64_t first_index=0;
        std::uint32_t extra=0, port=0, last_slot=0;
        bool grouped=false, observed=false;
    };
    struct Frame { ScopeToken token{}; Span span{}; Packet packet{}; };
public:
    // Reset invalidates all outstanding scope/packet tokens. Poison cannot be
    // cleared by entering a new list or packet; only a new capture can clear it.
    bool Reset() {
        depth_=0; frames_={};
        if (capture_==Max) { failure_=Reason::CounterOverflow; return false; }
        ++capture_; serial_=0; failure_=Reason::None; return true;
    }
    Tracker() { Reset(); }
    Reason Failure() const { return failure_; }
    std::size_t Depth() const { return depth_; }
    bool Invalidate(Reason reason) {
        return Fail(reason==Reason::None ? Reason::UnknownAddressMapping : reason);
    }

    ScopeToken Begin(std::uint64_t base, std::uint64_t words) {
        if (failure_!=Reason::None) return {};
        if (depth_==Capacity) { Fail(Reason::ContextOverflow); return {}; }
        if (!Range(base,words)) { Fail(Reason::InvalidRange); return {}; }
        const auto id=Next(), epoch=Next();
        if (failure_!=Reason::None) return {};
        Frame f; f.token={capture_,id,depth_+1}; f.span={base,words,epoch};
        frames_[depth_++]=f; return f.token;
    }
    bool End(ScopeToken token) {
        if (failure_!=Reason::None) return false;
        if (!depth_ || token.capture!=capture_ || token.depth!=depth_ ||
            token.id!=frames_[depth_-1].token.id) return Fail(Reason::ScopeMismatch);
        frames_[--depth_]={}; return true;
    }
    // Every explicit redirect must call this, EVEN when base/length repeat.
    // Retain the packet's OLD header while parameters move into the NEW span.
    bool Redirect(std::uint64_t base, std::uint64_t words) {
        if (!Ready()) return false;
        if (!Range(base,words)) return Fail(Reason::InvalidRange);
        const auto epoch=Next();
        if (failure_!=Reason::None) return false;
        frames_[depth_-1].span={base,words,epoch}; return true;
    }
    // PICA first parameter precedes its header. The header's extras may later
    // originate in a redirected list. This does not inspect any guest bytes.
    PacketToken Header(std::uint64_t header_index, std::uint64_t first_index,
                       std::uint32_t extra, std::uint32_t port, bool grouped) {
        if (!Ready()) return {};
        auto& f=frames_[depth_-1]; std::uint64_t header_address=0, unused=0;
        if (first_index==Max || first_index+1!=header_index ||
            !Address(f.span,header_index,header_address) ||
            !Address(f.span,first_index,unused) || port>0xffffu || extra>0x7ffu ||
            (grouped && extra>0xffffu-port)) {
            Fail(Reason::HeaderMismatch); return {};
        }
        const auto id=Next();
        if (failure_!=Reason::None) return {};
        Packet p; p.token={capture_,f.token.id,id,header_address,f.span.epoch};
        p.first_index=first_index; p.extra=extra; p.port=port; p.grouped=grouped;
        f.packet=p; return p.token;
    }
    // May observe just selected parameters (e.g. draw triggers). Skipped slots
    // are intentional; observed slots must increase and match header metadata.
    Origin Parameter(PacketToken token, std::uint32_t slot,
                     std::uint64_t parameter_index, std::uint32_t actual_port) {
        if (!Ready()) return Unknown();
        auto& f=frames_[depth_-1]; auto& p=f.packet;
        if (!Equal(token,p.token) || !token.id) {
            Fail(Reason::PacketMismatch); return Unknown();
        }
        const auto expected_port=p.port+(p.grouped?slot:0u);
        std::uint64_t address=0;
        if (slot>p.extra || (p.observed && slot<=p.last_slot) ||
            actual_port!=expected_port || !Address(f.span,parameter_index,address) ||
            (slot==0 && (f.span.epoch!=token.header_epoch || parameter_index!=p.first_index))) {
            Fail(Reason::ParameterMismatch); return Unknown();
        }
        p.observed=true; p.last_slot=slot;
        Origin o; o.reason=Reason::None; o.valid=true;
        o.capture=capture_; o.scope=f.token.id; o.packet=token.id;
        o.header_address=token.header_address; o.header_epoch=token.header_epoch;
        o.parameter_address=address; o.parameter_epoch=f.span.epoch;
        o.slot=slot; o.port=actual_port; return o;
    }
private:
    static constexpr auto Max=std::numeric_limits<std::uint64_t>::max();
    static constexpr std::uint64_t AddressLimit=std::uint64_t{1}<<32;
    static bool Range(std::uint64_t base,std::uint64_t words) {
        // Division precedes multiplication; exclusive end may equal 2^32.
        return !(base&3u) && base<AddressLimit && words &&
               words<=(AddressLimit-base)/4u;
    }
    static bool Address(const Span& s,std::uint64_t index,std::uint64_t& out) {
        if (!Range(s.base,s.words) || index>=s.words) return false;
        out=s.base+index*4u; return true; // bounded by Range and index
    }
    static bool Equal(PacketToken a,PacketToken b) {
        return a.capture==b.capture && a.scope==b.scope && a.id==b.id &&
               a.header_address==b.header_address && a.header_epoch==b.header_epoch;
    }
    bool Fail(Reason reason) { if (failure_==Reason::None) failure_=reason; return false; }
    bool Ready() {
        if (failure_!=Reason::None) return false;
        if (!depth_) return Fail(Reason::NoScope);
        return true;
    }
    std::uint64_t Next() {
        if (serial_==Max) { Fail(Reason::CounterOverflow); return 0; }
        return ++serial_;
    }
    Origin Unknown() const { Origin o; o.reason=failure_; o.capture=capture_; return o; }
    std::array<Frame,Capacity> frames_{};
    std::size_t depth_=0;
    std::uint64_t capture_=0,serial_=0;
    Reason failure_=Reason::None;
};
} // namespace CaraRecordSpan
