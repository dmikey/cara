// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>
#include "common/scope_exit.h"
#include "core/memory.h"
#include "core/settings.h"
#include "video_core/renderer_opengl/gl_rasterizer_cache.h"
#include "video_core/renderer_opengl/gl_state.h"
#include "video_core/video_core.h"

namespace OpenGL {

// Run immediately after core initialization, before guest execution. This uses
// original generated data and the production GPU surface/interval conversion path.
bool RasterizerCacheOpenGL::RunSmallWriteSelfTest(std::string& error) {
    if (const GLenum initial_error = glGetError(); initial_error != GL_NO_ERROR) {
        error = "surface coherence entered with GL error=" + std::to_string(initial_error);
        return false;
    }
    constexpr u32 scratch_size = 512 * 1024;
    constexpr PAddr scratch = Memory::VRAM_PADDR_END - scratch_size;
    const SurfaceInterval scratch_interval(scratch, scratch + scratch_size);
    const auto occupied = surface_cache.equal_range(scratch_interval);
    const auto dirty = dirty_regions.equal_range(scratch_interval);
    if (occupied.first != occupied.second || dirty.first != dirty.second) {
        error = "surface coherence fixture requires unused scratch VRAM";
        return false;
    }
    u8* memory = VideoCore::Memory()->GetPhysicalPointer(scratch);
    if (!memory) {
        error = "surface coherence scratch VRAM unavailable";
        return false;
    }
    const std::vector<u8> backup(memory, memory + scratch_size);
    const auto old_state = OpenGLState::GetCurState();
    const bool old_trace = trace_flushes;
    const bool old_retain = retain_small_cpu_writes;
    const bool old_skip_write = Settings::values.skip_cpu_write;
    Settings::values.skip_cpu_write = false;
    trace_flushes = false;
    Surface test_surface;
    Surface remap_destination;
    const bool old_remap = gpu_surface_remap;
    SCOPE_EXIT({
        if (remap_destination) {
            OpenGLState::ResetTexture(remap_destination->texture.handle);
            UnregisterSurface(remap_destination);
        }
        gpu_surface_remap = old_remap;
        if (test_surface) {
            dirty_regions.erase(test_surface->GetInterval());
            OpenGLState::ResetTexture(test_surface->texture.handle);
            UnregisterSurface(test_surface);
        }
        std::memcpy(memory, backup.data(), backup.size());
        trace_flushes = old_trace;
        retain_small_cpu_writes = old_retain;
        Settings::values.skip_cpu_write = old_skip_write;
        OpenGLState::InvalidateRasterState();
        old_state.Apply();
    });

    // Exercise the actual upload -> normalized GPU storage -> portable RGBA
    // readback -> packed bytes path for every possible 16-bit color, not merely
    // the packing helpers. Repeat through both guest layouts.
    for (bool tiled : {false, true}) {
        for (const auto format : {SurfaceParams::PixelFormat::RGB565,
                                  SurfaceParams::PixelFormat::RGB5A1,
                                  SurfaceParams::PixelFormat::RGBA4}) {
            SurfaceParams params;
            params.addr = scratch;
            params.width = params.stride = params.height = 256;
            params.pixel_format = format;
            params.is_tiled = tiled;
            params.res_scale = 1;
            params.UpdateParams();
            for (u32 pixel = 0; pixel < 65536; ++pixel) {
                const u16 packed = u16(pixel);
                std::memcpy(memory + pixel * 2, &packed, 2);
            }
            const std::vector<u8> expected(memory, memory + params.size);
            test_surface = GetSurface(params, ScaleMatch::Exact, true);
            const GLenum upload_error = glGetError();
            if (!test_surface || upload_error != GL_NO_ERROR) {
                error = "exhaustive packed color upload format=" + std::to_string(unsigned(format)) +
                        " GL error=" + std::to_string(upload_error);
                return false;
            }
            std::fill(test_surface->gl_buffer.begin(), test_surface->gl_buffer.end(), 0x2C);
            std::fill_n(memory, scratch_size, 0x6B);
            test_surface->DownloadGLTexture(test_surface->GetRect());
            const GLenum read_error = glGetError();
            test_surface->FlushGLBuffer(params.addr, params.end);
            const auto mismatch = std::mismatch(expected.begin(), expected.end(), memory);
            if (mismatch.first != expected.end() || read_error != GL_NO_ERROR) {
                error = "exhaustive packed color readback format=" + std::to_string(unsigned(format)) +
                        " tiled=" + std::to_string(tiled) + " GL error=" + std::to_string(read_error) +
                        " offset=" + std::to_string(mismatch.first - expected.begin());
                return false;
            }
            OpenGLState::ResetTexture(test_surface->texture.handle);
            UnregisterSurface(test_surface);
            test_surface.reset();
        }
    }

    // Match the observed framebuffer alias, plus both GL/guest orientations,
    // partial destination rectangles, and a CPU-write invalidation fallback.
    gpu_surface_remap = true;
    for (bool source_tiled : {false, true}) {
        for (bool destination_tiled : {false, true}) {
            for (bool partial : {false, true}) {
                SurfaceParams source_params;
                source_params.addr = scratch;
                source_params.width = source_params.stride = 256;
                source_params.height = 416;
                source_params.pixel_format = SurfaceParams::PixelFormat::RGBA8;
                source_params.is_tiled = source_tiled;
                source_params.res_scale = 1;
                source_params.UpdateParams();
                SurfaceParams destination_params = source_params;
                destination_params.addr += 16384;
                destination_params.width = destination_params.stride = 128;
                destination_params.height = 384;
                destination_params.is_tiled = destination_tiled;
                destination_params.UpdateParams();
                std::vector<u8> original(source_params.size);
                u32 random = 0x91735ACD;
                for (auto& byte : original) {
                    random ^= random << 13; random ^= random >> 17; random ^= random << 5;
                    byte = u8(random);
                }
                std::memcpy(memory, original.data(), original.size());
                test_surface = GetSurface(source_params, ScaleMatch::Exact, true);
                std::fill_n(memory + 16384, destination_params.size, 0xD9);
                remap_destination = CreateSurface(destination_params);
                RegisterSurface(remap_destination);
                remap_destination->LoadGLBuffer(destination_params.addr, destination_params.end);
                remap_destination->UploadGLTexture(remap_destination->GetRect());
                InvalidateRegion(source_params.addr, source_params.size, test_surface);
                std::fill_n(memory, scratch_size, 0x6B);
                std::fill(test_surface->gl_buffer.begin(), test_surface->gl_buffer.end(), 0x2C);
                std::fill(remap_destination->gl_buffer.begin(), remap_destination->gl_buffer.end(), 0x2C);
                const auto copy_params = partial
                    ? remap_destination->FromInterval(SurfaceInterval(destination_params.addr + 4096,
                                                                       destination_params.addr + 8192))
                    : destination_params;
                if (!TryRemapSurface(remap_destination, copy_params)) {
                    error = "RGBA8 GPU alias remap rejected eligible fixture";
                    return false;
                }
                remap_destination->DownloadGLTexture(remap_destination->GetRect());
                remap_destination->FlushGLBuffer(destination_params.addr, destination_params.end);
                std::vector<u8> expected(destination_params.size, 0xD9);
                std::copy_n(original.data() + copy_params.addr - source_params.addr, copy_params.size,
                            expected.data() + copy_params.addr - destination_params.addr);
                const auto mismatch = std::mismatch(expected.begin(), expected.end(), memory + 16384);
                const bool neighbors = std::all_of(memory, memory + 16384, [](u8 b) { return b == 0x6B; }) &&
                    std::all_of(memory + 16384 + destination_params.size, memory + scratch_size,
                                [](u8 b) { return b == 0x6B; });
                const GLenum remap_error = glGetError();
                if (mismatch.first != expected.end() || !neighbors || remap_error != GL_NO_ERROR) {
                    error = "RGBA8 alias source_tiled=" + std::to_string(source_tiled) +
                            " destination_tiled=" + std::to_string(destination_tiled) +
                            " partial=" + std::to_string(partial) + " offset=" +
                            std::to_string(mismatch.first - expected.begin()) +
                            " neighbors=" + std::to_string(neighbors) + " GL error=" + std::to_string(remap_error);
                    return false;
                }
                const PAddr write_address = copy_params.addr + 4;
                InvalidateRegion(write_address, 16, nullptr);
                std::fill_n(memory + write_address - scratch, 16, 0xA7);
                if (TryRemapSurface(remap_destination, copy_params)) {
                    error = "RGBA8 alias accepted CPU-invalidated source bytes";
                    return false;
                }
                // The normal RAM fallback must retain those CPU bytes and reload
                // the still-current GPU neighbors, including the untouched area.
                ValidateSurface(remap_destination, destination_params.addr, destination_params.size);
                remap_destination->DownloadGLTexture(remap_destination->GetRect());
                remap_destination->FlushGLBuffer(destination_params.addr, destination_params.end);
                std::copy_n(original.data() + 16384, destination_params.size, expected.data());
                std::fill_n(expected.data() + write_address - destination_params.addr, 16, 0xA7);
                if (!std::equal(expected.begin(), expected.end(), memory + 16384) || glGetError() != GL_NO_ERROR) {
                    error = "RGBA8 alias CPU-write fallback coherence mismatch";
                    return false;
                }
                if (!remap_destination->IsRegionValid(destination_params.GetInterval())) {
                    error = "RGBA8 alias fallback did not validate destination";
                    return false;
                }
                // A later GPU producer must invalidate the cached destination,
                // then the real ValidateSurface entry point must remap fresh data
                // without exporting any of the GPU-owned bytes into poisoned RAM.
                for (auto& byte : original) byte ^= 0xD3;
                std::memcpy(memory, original.data(), original.size());
                test_surface->LoadGLBuffer(source_params.addr, source_params.end);
                test_surface->UploadGLTexture(test_surface->GetRect());
                InvalidateRegion(source_params.addr, source_params.size, test_surface);
                if (remap_destination->IsRegionValid(destination_params.GetInterval())) {
                    error = "RGBA8 alias GPU write left destination valid";
                    return false;
                }
                std::fill_n(memory, scratch_size, 0x6B);
                std::fill(test_surface->gl_buffer.begin(), test_surface->gl_buffer.end(), 0x2C);
                ValidateSurface(remap_destination, destination_params.addr, destination_params.size);
                if (!std::all_of(memory, memory + scratch_size, [](u8 b) { return b == 0x6B; })) {
                    error = "RGBA8 alias GPU validation unexpectedly used RAM readback";
                    return false;
                }
                remap_destination->DownloadGLTexture(remap_destination->GetRect());
                remap_destination->FlushGLBuffer(destination_params.addr, destination_params.end);
                if (!std::equal(original.begin() + 16384,
                                original.begin() + 16384 + destination_params.size,
                                memory + 16384) || glGetError() != GL_NO_ERROR) {
                    error = "RGBA8 alias reused stale bytes after GPU owner write";
                    return false;
                }
                dirty_regions.erase(source_params.GetInterval());
                OpenGLState::ResetTexture(remap_destination->texture.handle);
                UnregisterSurface(remap_destination);
                remap_destination.reset();
                OpenGLState::ResetTexture(test_surface->texture.handle);
                UnregisterSurface(test_surface);
                test_surface.reset();
            }
        }
    }
    gpu_surface_remap = old_remap;

    for (bool retain : {false, true}) {
      retain_small_cpu_writes = retain;
      for (bool tiled : {false, true}) {
        for (const auto format : {SurfaceParams::PixelFormat::RGBA8,
                                  SurfaceParams::PixelFormat::RGB8,
                                  SurfaceParams::PixelFormat::RGB565,
                                  SurfaceParams::PixelFormat::RGB5A1,
                                  SurfaceParams::PixelFormat::RGBA4}) {
            SurfaceParams params;
            params.addr = scratch;
            params.width = params.stride = params.height = 16;
            params.pixel_format = format;
            params.is_tiled = tiled;
            params.res_scale = 1;
            params.UpdateParams();
            const u32 bpp = params.GetFormatBpp() / 8;
            const u32 tile_bytes = bpp * 64;
            std::vector<u8> expected(params.size);
            for (bool validate_texture : {false, true}) {
                std::fill_n(memory, scratch_size, 0xD9);
                for (size_t i = 0; i < expected.size(); ++i)
                    expected[i] = u8((i * 37 + (i / 17) * 11 + unsigned(format)) & 255);
                std::memcpy(memory, expected.data(), expected.size());
                test_surface = GetSurface(params, ScaleMatch::Exact, true);
                if (!test_surface) {
                    error = "surface coherence texture allocation failed";
                    return false;
                }
                const auto check_gl = [&](const char* operation) {
                    const GLenum gl_error = glGetError();
                    if (gl_error == GL_NO_ERROR) return true;
                    error = std::string("surface coherence ") + operation +
                            " format=" + std::to_string(unsigned(format)) +
                            " retain=" + std::to_string(retain) +
                            " tiled=" + std::to_string(tiled) +
                            " validation=" + std::to_string(validate_texture) +
                            " GL error=" + std::to_string(gl_error);
                    return false;
                };
                if (!check_gl("initial upload")) return false;
                // Poison both CPU copies. A failed GPU readback must not pass by
                // copying the original upload's still-valid staging bytes.
                std::fill(test_surface->gl_buffer.begin(), test_surface->gl_buffer.end(), 0x2C);
                std::fill_n(memory, expected.size(), 0x6B);
                InvalidateRegion(params.addr, params.size, test_surface);

                // Pixel interiors, pixel edges, tile edges, row edges and final byte.
                const std::array<std::pair<u32, u32>, 8> writes{{
                    {1, 1}, {bpp - 1, 2}, {tile_bytes - 2, 4}, {tile_bytes - 3, 8},
                    {tile_bytes + 1, 1}, {bpp * 16 - 1, 2}, {params.size - 8, 8},
                    {params.size - 1, 1},
                }};
                for (size_t w = 0; w < writes.size(); ++w) {
                    const auto [offset, size] = writes[w];
                    InvalidateRegion(params.addr + offset, size, nullptr);
                    if (!check_gl("CPU write invalidation")) return false;
                    for (u32 i = 0; i < size; ++i)
                        memory[offset + i] = expected[offset + i] = u8(0xA0 + w * 7 + i);
                }
                if (validate_texture) {
                    // A subsequent texture use must merge CPU bytes with the dirty
                    // GPU neighbors, including partial tiles, before uploading.
                    auto current = GetSurface(params, ScaleMatch::Exact, true);
                    if (!current || !check_gl("texture validation")) return false;
                    if (current != test_surface) {
                        OpenGLState::ResetTexture(test_surface->texture.handle);
                        test_surface = current;
                    }
                    std::fill(test_surface->gl_buffer.begin(), test_surface->gl_buffer.end(), 0x2C);
                    test_surface->DownloadGLTexture(test_surface->GetRect());
                    if (!check_gl("validated texture readback")) return false;
                    test_surface->FlushGLBuffer(params.addr, params.end);
                } else {
                    // CPU read path must flush only still-GPU-owned bytes.
                    FlushRegion(params.addr, params.size);
                    if (!check_gl("CPU read flush")) return false;
                }
                const auto mismatch = std::mismatch(expected.begin(), expected.end(), memory);
                const bool neighbors = std::all_of(memory + expected.size(), memory + scratch_size,
                                                    [](u8 value) { return value == 0xD9; });
                if (mismatch.first != expected.end() || !neighbors) {
                    error = "surface byte coherence format=" + std::to_string(unsigned(format)) +
                            " retain=" + std::to_string(retain) +
                            " tiled=" + std::to_string(tiled) +
                            " validation=" + std::to_string(validate_texture) +
                            " offset=" + std::to_string(mismatch.first - expected.begin()) +
                            " neighbors=" + std::to_string(neighbors);
                    return false;
                }
                dirty_regions.erase(params.GetInterval());
                OpenGLState::ResetTexture(test_surface->texture.handle);
                UnregisterSurface(test_surface);
                test_surface.reset();
            }
        }
    }
    }
    return true;
}
} // namespace OpenGL
