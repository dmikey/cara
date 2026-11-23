// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#if defined(__ANDROID__) && defined(CARA_GL_CONTEXT_DISPATCH)
#include <cstdint>
#include <vector>
#include "video_core/renderer_opengl/gl_command_queue_api.h"

namespace OpenGL::CommandQueue::Backlog {
enum class Source : std::uint8_t { Other, LitPica, UnlitPica, SurfaceRemap, Display, DisplayTransfer, TextureCopy, MemoryFill };
inline constexpr const char* SourceNames[] = {"other", "lit_pica", "unlit_pica", "remap", "display", "display_transfer", "texture_copy", "memory_fill"};
inline thread_local Source source = Source::Other;

inline constexpr std::uint16_t NoEntry = 0xffff;
inline thread_local std::uint16_t vertex_entry = NoEntry;

struct Scope {
    explicit Scope(Source next, std::uint16_t entry = NoEntry)
        : previous(source), previous_entry(vertex_entry) {
        source = next;
        vertex_entry = entry;
    }
    ~Scope() { source = previous; vertex_entry = previous_entry; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
private:
    Source previous;
    std::uint16_t previous_entry;
};

struct Tag {
    Source source{};
    RpcApi api{};
    std::uint16_t entry{NoEntry};
};
struct Row {
    Tag request, command;
    std::uint64_t calls{}, ns{};
};
bool Enabled();
std::vector<Row> Take();
}
#endif
