// Copyright 2026 Cara contributors. GPLv2 or later; see license.txt.
// Native-atlas observers are unavailable elsewhere: no clocks, files or GL work.
#include "gl_host_latency.h"
#include "gl_pipeline_capture.h"
namespace OpenGL::HostLatency {
struct Recorder::Impl {};
bool Requested() noexcept { return false; }
std::uint64_t ReadHostTick() noexcept { return 0; }
Recorder::Recorder(const std::string&) noexcept {}
Recorder::~Recorder() = default;
void Recorder::BindOwnerThread() noexcept {}
Token Recorder::Begin(Api, std::uint32_t, std::uint32_t, std::uint32_t, std::int32_t) noexcept { return {}; }
void Recorder::End(Token, std::uint64_t, std::int32_t, bool) noexcept {}
bool Recorder::CanCopyStatus(const Token&) noexcept { return false; }
void Recorder::CreateShader(std::uint32_t, std::uint32_t) noexcept {}
void Recorder::Source(std::uint32_t) noexcept {}
void Recorder::DeleteShader(std::uint32_t) noexcept {}
void Recorder::CreateProgram(std::uint32_t) noexcept {}
void Recorder::Link(std::uint32_t) noexcept {}
void Recorder::Use(std::uint32_t) noexcept {}
void Recorder::DeleteProgram(std::uint32_t) noexcept {}
void Recorder::BindVAO(std::uint32_t) noexcept {}
void Recorder::BindFramebuffer(std::uint32_t, std::uint32_t) noexcept {}
}
namespace OpenGL::PipelineCapture {
struct Recorder::Impl {};
bool Requested() noexcept { return false; }
Recorder::Recorder(const std::string&) noexcept {}
Recorder::~Recorder() = default;
void Recorder::CreateShader(std::uint32_t, std::uint32_t) noexcept {}
void Recorder::Source(std::uint32_t, int, const char* const*, const int*) noexcept {}
void Recorder::Compile(std::uint32_t) noexcept {}
void Recorder::ShaderStatus(std::uint32_t, int) noexcept {}
void Recorder::DeleteShader(std::uint32_t) noexcept {}
void Recorder::CreateProgram(std::uint32_t) noexcept {}
void Recorder::Attach(std::uint32_t, std::uint32_t, bool) noexcept {}
void Recorder::Link(std::uint32_t) noexcept {}
void Recorder::ProgramStatus(std::uint32_t, int) noexcept {}
void Recorder::DeleteProgram(std::uint32_t) noexcept {}
void Recorder::Use(std::uint32_t) noexcept {}
void Recorder::BindVAO(std::uint32_t) noexcept {}
void Recorder::BindFramebuffer(std::uint32_t, std::uint32_t) noexcept {}
void Recorder::Draw(unsigned, unsigned, int, int, unsigned, std::uint64_t, unsigned, unsigned, int) noexcept {}
void Recorder::DrawReturned() noexcept {}
}
