// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <charconv>
#include <fstream>
#include <optional>
#include <string_view>
#include <unordered_set>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/file_util.h"
#include "common/logging/log.h"
#include "video_core/shader/shader.h"

namespace Pica::ShaderHints {

// A hint influences compiler scheduling only. Backends still derive their exact
// keys, sources and layout from the live draw. The file contains no guest code.
class Catalog {
public:
    bool Load(u64 title) {
        if (loaded)
            return enabled;
        loaded = true;
        if (!title)
            return false;
        const auto path = FileUtil::GetUserPath(FileUtil::UserPath::ShaderDir) +
                          fmt::format("{:016x}.pica-hints", title);
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        const auto size = input.tellg();
        if (size < 0 || size > 9 * 1024)
            return false;
        input.seekg(0);
        std::string line;
        if (!std::getline(input, line) || line != "CARA_PICA_HINTS_1")
            return false;
        while (std::getline(input, line)) {
            u64 hash = 0;
            const auto* end = line.data() + line.size();
            const auto value = std::from_chars(line.data(), end, hash, 16);
            if (line.size() != 16 || value.ec != std::errc{} || value.ptr != end ||
                fingerprints.size() == 512) {
                fingerprints.clear();
                return false;
            }
            fingerprints.insert(hash);
        }
        enabled = input.eof();
        if (enabled)
            LOG_INFO(Render_OpenGL, "PICA shader hints title={:016x} rom_entries={}",
                     title, fingerprints.size());
        return enabled;
    }

    static std::optional<u64> Fingerprint(const Shader::ShaderSetup& setup, unsigned entry) {
        constexpr unsigned WordCount = 8;
        if (entry > setup.program_code.size() - WordCount)
            return std::nullopt;
        u64 hash = 0xcbf29ce484222325;
        for (unsigned index = 0; index < WordCount; ++index) {
            const u32 word = setup.program_code[entry + index];
            for (unsigned shift = 0; shift < 32; shift += 8) {
                hash ^= (word >> shift) & 0xff;
                hash *= 0x100000001b3;
            }
        }
        return hash;
    }

    bool Match(const Shader::ShaderSetup& setup, unsigned entry) const {
        const auto hash = Fingerprint(setup, entry);
        return hash && fingerprints.contains(*hash);
    }

    bool Admit(u32 refresh, bool rom_match, unsigned live_budget = 1) {
        if (refresh != last_refresh) {
            last_refresh = refresh;
            live_requests = 0;
            rom_requests = 0;
        }
        auto& requests = rom_match ? rom_requests : live_requests;
        const unsigned budget = rom_match ? 4 : live_budget;
        if (requests == budget)
            return false;
        ++requests;
        return true;
    }

    // Renderers retain their own contexts and read the catalog once per load.
    bool Loaded() const { return loaded; }
    bool Enabled() const { return enabled; }

private:
    std::unordered_set<u64> fingerprints;
    bool loaded = false;
    bool enabled = false;
    u32 last_refresh = 0;
    unsigned live_requests = 0;
    unsigned rom_requests = 0;
};

} // namespace Pica::ShaderHints
