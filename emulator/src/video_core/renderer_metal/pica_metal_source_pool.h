// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace Cara {
// Per-renderer immutable UTF-8 source ownership. Set nodes keep even short-string
// views stable. Capacity rejection changes neither ownership nor existing views.
class PicaMetalSourcePool {
public:
    struct Admission {
        std::array<std::string_view,4> sources;
        std::size_t added_bytes{}, hits{};
    };
    explicit PicaMetalSourcePool(std::size_t budget) : budget_(budget) {}
    std::size_t Bytes() const { return bytes_; }
    std::size_t Size() const { return sources_.size(); }
    std::optional<Admission> Intern(const std::array<std::string_view,4>& input) {
        std::array<std::string_view,4> fresh{};
        std::size_t count=0, cost=0;
        for (const auto source : input) {
            if (sources_.find(source)!=sources_.end()) continue;
            bool duplicate=false;
            for (std::size_t i=0;i<count;++i) duplicate |= fresh[i]==source;
            if (duplicate) continue;
            if (source.size()>budget_-bytes_-cost) return std::nullopt;
            cost+=source.size(); fresh[count++]=source;
        }
        // Allocate all new strings before touching the live pool. A failed
        // allocation therefore cannot partially publish an admission either.
        std::set<std::string,std::less<>> staged;
        for (std::size_t i=0;i<count;++i) staged.emplace(fresh[i]);
        sources_.merge(staged); // Same allocator; transfers nodes without copying text.
        bytes_+=cost;
        Admission answer{{},cost,input.size()-count};
        for (std::size_t i=0;i<input.size();++i) answer.sources[i]=*sources_.find(input[i]);
        return answer;
    }
private:
    const std::size_t budget_;
    std::size_t bytes_=0;
    std::set<std::string,std::less<>> sources_;
};
} // namespace Cara
