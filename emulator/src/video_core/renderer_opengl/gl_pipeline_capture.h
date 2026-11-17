// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <memory>
#include <string>
namespace OpenGL::PipelineCapture {
// Only executed GL entrypoints feed this observer. No driver queries are issued.
struct DrawRecord {
    char magic[8];
    std::uint64_t sequence, state_serial, program_generation, link_serial;
    std::uint32_t phase, program, vao, draw_framebuffer, api, mode;
    std::int32_t first, count, base_vertex;
    std::uint32_t index_type, range_start, range_end, flags, reserved;
    std::uint64_t index_offset;
};
static_assert(sizeof(DrawRecord)==104);
struct Journal {char magic[8];std::uint64_t committed;DrawRecord slots[2];};
static_assert(sizeof(Journal)==224);
class Recorder {
public:
    static constexpr std::size_t MaxSource=1024*1024, MaxSourceBytes=32*1024*1024;
    static constexpr std::size_t MaxShaders=512, MaxPrograms=1024, MaxSources=2048, MaxEvents=16384, MaxEventBytes=8*1024*1024;
    explicit Recorder(const std::string& fresh_directory) noexcept;
    ~Recorder();
    void CreateShader(std::uint32_t shader,std::uint32_t type) noexcept;
    void Source(std::uint32_t shader,int count,const char* const* strings,const int* lengths) noexcept;
    void Compile(std::uint32_t shader) noexcept;
    void ShaderStatus(std::uint32_t shader,int value) noexcept;
    void DeleteShader(std::uint32_t shader) noexcept;
    void CreateProgram(std::uint32_t program) noexcept;
    void Attach(std::uint32_t program,std::uint32_t shader,bool attach) noexcept;
    void Link(std::uint32_t program) noexcept;
    void ProgramStatus(std::uint32_t program,int value) noexcept;
    void DeleteProgram(std::uint32_t program) noexcept;
    void Use(std::uint32_t program) noexcept;
    void BindVAO(std::uint32_t vao) noexcept;
    void BindFramebuffer(std::uint32_t target,std::uint32_t framebuffer) noexcept;
    void Draw(unsigned api,unsigned mode,int first,int count,unsigned type,std::uint64_t offset,
              unsigned start=0,unsigned end=0,int base=0) noexcept;
    void DrawReturned() noexcept;
private:
    struct Impl;std::unique_ptr<Impl> impl;
};
bool Requested() noexcept;
}
