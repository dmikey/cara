// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace Cara::GuestGPUTrace {
// Diagnostic allowlist only. Never reads or patches guest memory or changes decode boundaries.
class CallbackMarkers {
public:
    struct Marker { std::uint32_t id=0, phase=0, pc=0, lr=0; int next=-1; };
    static constexpr unsigned Capacity=256, Buckets=1024;
    CallbackMarkers() { heads.fill(-1); }
    bool Load(const char* path) {
        *this=CallbackMarkers{};
        if (!path || !*path) return true;
        std::ifstream in(path);
        if (!in) return Fail("cannot open callback marker map");
        std::string line;
        while (std::getline(in,line)) {
            if (line.empty() || line[0]=='#') continue;
            std::istringstream row(line);
            std::array<std::string,4> fields;
            std::string extra;
            if (!(row>>fields[0]>>fields[1]>>fields[2]>>fields[3]) || (row>>extra))
                return Fail("invalid callback marker row");
            std::array<std::uint32_t,4> values{};
            for (unsigned i=0;i<4;++i) if (!Number(fields[i],values[i]))
                return Fail("invalid callback marker number");
            if (values[1]>2 || !values[2] || (values[2]&1) || (values[3]&1) || count==Capacity)
                return Fail("invalid callback marker bounds");
            for (unsigned i=0;i<count;++i)
                if (markers[i].pc==values[2] && markers[i].lr==values[3])
                    return Fail("ambiguous callback marker");
            const auto bucket=Hash(values[2]);
            markers[count]={values[0],values[1],values[2],values[3],heads[bucket]};
            heads[bucket]=static_cast<int>(count++);
        }
        if (!in.eof() || !count) return Fail("empty or unreadable callback marker map");
        return true;
    }
    const Marker* Find(std::uint32_t pc,std::uint32_t lr) const {
        const Marker* wildcard=nullptr;
        for (int i=heads[Hash(pc)];i!=-1;i=markers[i].next) {
            const auto& marker=markers[i];
            if (marker.pc!=pc) continue;
            if (marker.lr && marker.lr==(lr&~1u)) return &marker;
            if (!marker.lr) wildcard=&marker;
        }
        return wildcard;
    }
    unsigned Count() const { return count; }
    const std::string& Error() const { return error; }
private:
    static unsigned Hash(std::uint32_t pc) { return ((pc>>1)^(pc>>11))&(Buckets-1); }
    static bool Number(std::string_view s,std::uint32_t& out) {
        int base=10;
        if (s.size()>2 && s.substr(0,2)=="0x") { s.remove_prefix(2);base=16; }
        if (s.empty()) return false;
        auto result=std::from_chars(s.data(),s.data()+s.size(),out,base);
        return result.ec==std::errc{} && result.ptr==s.data()+s.size();
    }
    bool Fail(const char* message) { count=0;heads.fill(-1);error=message;return false; }
    std::array<Marker,Capacity> markers{};
    std::array<int,Buckets> heads{};
    unsigned count=0;
    std::string error;
};
}
