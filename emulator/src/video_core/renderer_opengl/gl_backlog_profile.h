// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#if defined(__ANDROID__) && defined(CARA_GL_CONTEXT_DISPATCH)
#include <cstdint>
#include <vector>
#include "video_core/renderer_opengl/gl_command_queue_api.h"

namespace OpenGL::CommandQueue::Backlog {
enum class Source : std::uint8_t { Other, LitPica, UnlitPica, SurfaceRemap, Display };
inline constexpr const char* SourceNames[] = {"other", "lit_pica", "unlit_pica", "remap", "display"};
inline thread_local Source source = Source::Other;

struct Scope {
    explicit Scope(Source next) : previous(source) { source = next; }
    ~Scope() { source = previous; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
private:
    Source previous;
};

struct Tag {
    Source source{};
    RpcApi api{};
};
struct Row {
    Tag request, command;
    std::uint64_t calls{}, ns{};
};
bool Enabled();
std::vector<Row> Take();
}
#endif
