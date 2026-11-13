// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <array>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include "common/scope_exit.h"
#include "common/frame_timing.h"
#include "core/memory.h"
#include "core/settings.h"
#include "video_core/renderer_opengl/gl_rasterizer_cache.h"
#include "video_core/renderer_opengl/gl_state.h"
#include "video_core/video_core.h"

namespace OpenGL {

// Run immediately after core initialization, before guest execution. This uses
// original generated data and the production GPU surface/interval conversion path.
// Original GPU surface/cache proof using the production lookup and mutation paths.
bool RasterizerCacheOpenGL::RunTextureLookupSelfTest(std::string& error) {
    constexpr u32 bytes=64*1024;
    constexpr PAddr scratch=Memory::VRAM_PADDR_END-bytes;
    const SurfaceInterval interval(scratch,scratch+bytes);
    const auto occupied=surface_cache.equal_range(interval);
    const auto dirty=dirty_regions.equal_range(interval);
    if (occupied.first!=occupied.second || dirty.first!=dirty.second) {
        error="texture lookup oracle requires unused scratch VRAM"; return false;
    }
    const GLenum entry_error=glGetError();
    if (entry_error!=GL_NO_ERROR) { error="texture lookup oracle preexisting GL="+std::to_string(entry_error); return false; }
    u8* memory=VideoCore::Memory()->GetPhysicalPointer(scratch);
    if (!memory) { error="texture lookup oracle scratch unavailable"; return false; }
    const std::vector<u8> backup(memory,memory+bytes);
    const auto saved_state=OpenGLState::GetCurState();
    const auto saved_counters=Common::FrameTiming::counters;
    const bool saved_reuse=texture_lookup_reuse,saved_verify=texture_lookup_verify;
    const bool saved_trace=trace_flushes,saved_retain=retain_small_cpu_writes;
    const bool saved_skip=Settings::values.skip_cpu_write;
    trace_flushes=false; retain_small_cpu_writes=true; Settings::values.skip_cpu_write=false;
    Common::FrameTiming::counters.enabled=true;
    texture_lookup_reuse=texture_lookup_verify=true;
    texture_lookup_entries={};texture_lookup_next=0;
    SCOPE_EXIT({
        std::vector<Surface> remove;
        const auto range=surface_cache.equal_range(interval);
        for (auto it=range.first;it!=range.second;++it)
            for (const auto& surface:it->second)
                if (std::find(remove.begin(),remove.end(),surface)==remove.end()) remove.push_back(surface);
        dirty_regions.erase(interval);
        for (const auto& surface:remove) {
            OpenGLState::ResetTexture(surface->texture.handle);
            UnregisterSurface(surface);
            remove_surfaces.erase(surface);
        }
        texture_lookup_entries={};texture_lookup_next=0;
        texture_lookup_reuse=saved_reuse;texture_lookup_verify=saved_verify;
        trace_flushes=saved_trace;retain_small_cpu_writes=saved_retain;Settings::values.skip_cpu_write=saved_skip;
        std::memcpy(memory,backup.data(),backup.size());
        OpenGLState::InvalidateRasterState();saved_state.Apply();
        Common::FrameTiming::counters=saved_counters;
    });
    const auto fail=[&](const char* stage) {
        error=std::string("texture lookup oracle: ")+stage+" GL="+std::to_string(glGetError()); return false;
    };
    const auto info_at=[&](PAddr address) {
        Pica::Texture::TextureInfo info{};info.physical_address=address;
        info.width=info.height=8;info.format=Pica::TexturingRegs::TextureFormat::RGBA8;
        info.SetDefaultStride();return info;
    };
    auto info=info_at(scratch);
    SurfaceParams params;params.addr=scratch;params.width=params.height=params.stride=8;
    params.is_tiled=true;params.res_scale=1;params.pixel_format=SurfaceParams::PixelFormat::RGBA8;params.UpdateParams();
    for(u32 i=0;i<params.size;++i) memory[i]=u8(i*17+3);
    std::vector<u8> expected(memory,memory+params.size);
    auto base=GetTextureSurface(info,0);
    if(!base) return fail("initial texture creation");
    // Creation deliberately cannot admit an old selection proof. A fresh exact
    // query records the unique winner, then the following query must hit.
    if(GetTextureSurface(info,0)!=base) return fail("initial identity");
    const auto before_warm=Common::FrameTiming::counters.texture_lookup_hits;
    if(GetTextureSurface(info,0)!=base || Common::FrameTiming::counters.texture_lookup_hits!=before_warm+1)
        return fail("warm hit not exercised");
    const auto check_bytes=[&](const Surface& surface,const std::vector<u8>& wanted) {
        surface->DownloadGLTexture(surface->GetRect());
        surface->FlushGLBuffer(surface->addr,surface->end);
        return glGetError()==GL_NO_ERROR && std::equal(wanted.begin(),wanted.end(),memory);
    };
    if(!check_bytes(base,expected)) return fail("original generated RGBA bytes");
    // No-load callers must retain usage updates but never validate dirty bytes.
    base->last_used_frame=~0u;
    const auto no_load_hits=Common::FrameTiming::counters.texture_lookup_hits;
    if(GetSurface(params,ScaleMatch::Ignore,false)!=base ||
       Common::FrameTiming::counters.texture_lookup_hits!=no_load_hits+1 ||
       base->last_used_frame!=VideoCore::GetCurrentFrame()) return fail("valid no-load hit/usage");
    InvalidateRegion(scratch,16,nullptr);
    for(u32 i=0;i<16;++i) expected[i]=memory[i]=u8(0xD3+i);
    const auto pending=base->invalid_regions;
    const auto dirty_hits=Common::FrameTiming::counters.texture_lookup_hits;
    const auto dirty_stores=Common::FrameTiming::counters.texture_lookup_stores;
    texture_lookup_reuse=false;auto no_load_reference=GetSurface(params,ScaleMatch::Ignore,false);texture_lookup_reuse=true;
    base->last_used_frame=~0u;
    if(GetSurface(params,ScaleMatch::Ignore,false)!=no_load_reference || no_load_reference!=base ||
       base->invalid_regions!=pending || pending.empty() ||
       Common::FrameTiming::counters.texture_lookup_hits!=dirty_hits ||
       Common::FrameTiming::counters.texture_lookup_stores!=dirty_stores ||
       base->last_used_frame!=VideoCore::GetCurrentFrame()) return fail("invalid no-load identity/no-validation/no-store");
    if(GetTextureSurface(info,0)!=base || !check_bytes(base,expected))
        return fail("ordinary texture revalidation after no-load query");

    for(u32 size:{4u,16u}) {
        const auto hits=Common::FrameTiming::counters.texture_lookup_hits;
        InvalidateRegion(scratch,size,nullptr);
        for(u32 i=0;i<size;++i) expected[i]=memory[i]=u8(0x91+i+size);
        if(GetTextureSurface(info,0)!=base || Common::FrameTiming::counters.texture_lookup_hits!=hits ||
           !check_bytes(base,expected)) return fail("CPU write invalidation/readback");
        const auto warm=Common::FrameTiming::counters.texture_lookup_hits;
        if(GetTextureSurface(info,0)!=base || Common::FrameTiming::counters.texture_lookup_hits!=warm+1)
            return fail("CPU write revalidated hit");
    }
    // Exercise a real GPU owner write. This mirrors framebuffer acquisition's
    // watcher invalidation and Draw's region-owner invalidation after rendering.
    {
        const auto previous=OpenGLState::GetCurState();
        OpenGLState state=previous;
        OGLFramebuffer clear_framebuffer;clear_framebuffer.Create();
        OpenGLState::BindDrawFramebuffer(clear_framebuffer.handle);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,base->texture.handle,0);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_DEPTH_STENCIL_ATTACHMENT,GL_TEXTURE_2D,0,0);
        state.draw.draw_framebuffer=clear_framebuffer.handle;
        state.scissor.enabled=false;state.color_mask={GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE};state.SubApply();
        base->InvalidateAllWatcher();
        const GLfloat color[]={0.25f,0.5f,0.75f,1.0f};glClearBufferfv(GL_COLOR,0,color);
        InvalidateRegion(scratch,params.size,base);
        previous.SubApply();
    }
    const auto gpu_hits=Common::FrameTiming::counters.texture_lookup_hits;
    if(GetTextureSurface(info,0)!=base || Common::FrameTiming::counters.texture_lookup_hits!=gpu_hits)
        return fail("GPU owner write invalidation");
    base->DownloadGLTexture(base->GetRect());const auto gpu_bytes=base->gl_buffer;
    texture_lookup_reuse=false;auto reference=GetTextureSurface(info,0);
    reference->DownloadGLTexture(reference->GetRect());texture_lookup_reuse=true;
    if(reference!=base || reference->gl_buffer!=gpu_bytes) return fail("GPU owner bytes/identity");
    // Competing equal-scale exact matches must not be admitted. Validate an
    // earlier invalid alias without invalidating the later winner's watcher.
    auto duplicate=CreateSurface(params);RegisterSurface(duplicate);
    ValidateSurface(duplicate,scratch,params.size);
    const Surface earlier=std::less<Surface>{}(base,duplicate)?base:duplicate;
    const Surface later=earlier==base?duplicate:base;
    earlier->invalid_regions.insert(earlier->GetInterval());earlier->InvalidateAllWatcher();
    ValidateSurface(later,scratch,params.size);
    const auto ambiguous_hits=Common::FrameTiming::counters.texture_lookup_hits;
    if(GetSurface(params,ScaleMatch::Exact,true)!=later) return fail("valid later exact alias");
    ValidateSurface(earlier,scratch,params.size);
    texture_lookup_reuse=false;reference=GetSurface(params,ScaleMatch::Exact,true);texture_lookup_reuse=true;
    if(reference!=earlier || GetSurface(params,ScaleMatch::Exact,true)!=reference ||
       Common::FrameTiming::counters.texture_lookup_hits!=ambiguous_hits)
        return fail("equal-scale validity-change selection");
    // A newly registered higher-scale candidate must replace the old winner.
    SurfaceParams high_params=params;high_params.res_scale=2;
    auto high=CreateSurface(high_params);RegisterSurface(high);
    if(GetTextureSurface(info,0)!=high || GetTextureSurface(info,0)!=high)
        return fail("higher-scale registration");
    const auto unlink_hits=Common::FrameTiming::counters.texture_lookup_hits;
    high->UnlinkAllWatcher();
    if(GetTextureSurface(info,0)!=high || Common::FrameTiming::counters.texture_lookup_hits!=unlink_hits)
        return fail("unlinked deferred-removal watcher");
    OpenGLState::ResetTexture(high->texture.handle);
    UnregisterSurface(high);
    texture_lookup_reuse=false;reference=GetTextureSurface(info,0);texture_lookup_reuse=true;
    if(GetTextureSurface(info,0)!=reference || reference==high)
        return fail("unregister/reacquire identity");
    // Pre-create nine disjoint textures before populating the bounded eight
    // entries, so registration epochs do not explain the eviction miss.
    std::array<Pica::Texture::TextureInfo,9> infos{};
    texture_lookup_reuse=false;
    for(size_t i=0;i<infos.size();++i) {
        infos[i]=info_at(scratch+2048*u32(i+1));
        std::fill_n(memory+2048*(i+1),256,u8(i*19));
        if(!GetTextureSurface(infos[i],0)) return fail("eviction texture creation");
    }
    texture_lookup_reuse=true;texture_lookup_entries={};texture_lookup_next=0;
    for(const auto& item:infos) if(!GetTextureSurface(item,0)) return fail("eviction admission");
    const auto eviction_hits=Common::FrameTiming::counters.texture_lookup_hits;
    if(!GetTextureSurface(infos.back(),0) || Common::FrameTiming::counters.texture_lookup_hits!=eviction_hits+1)
        return fail("last bounded entry hit");
    if(!GetTextureSurface(infos.front(),0) || Common::FrameTiming::counters.texture_lookup_hits!=eviction_hits+1)
        return fail("oldest bounded entry eviction");
    if(Common::FrameTiming::counters.texture_lookup_verified_hits<=saved_counters.texture_lookup_verified_hits ||
       Common::FrameTiming::counters.texture_lookup_verify_mismatches!=saved_counters.texture_lookup_verify_mismatches ||
       glGetError()!=GL_NO_ERROR) return fail("shadow identity verification");
    return true;
}

bool RasterizerCacheOpenGL::RunSmallWriteSelfTest(std::string& error) {
    if (texture_lookup_reuse && !RunTextureLookupSelfTest(error)) return false;
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
    Surface second_remap_source;
    const bool old_remap = gpu_surface_remap;
    SCOPE_EXIT({
        if (remap_destination) {
            OpenGLState::ResetTexture(remap_destination->texture.handle);
            UnregisterSurface(remap_destination);
        }
        gpu_surface_remap = old_remap;
        if (second_remap_source) {
            dirty_regions.erase(second_remap_source->GetInterval());
            OpenGLState::ResetTexture(second_remap_source->texture.handle);
            UnregisterSurface(second_remap_source);
        }
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
    const char* byte_remap_value = std::getenv("CARA_GPU_BYTE_SURFACE_REMAP");
    const bool byte_remap = !byte_remap_value || std::strcmp(byte_remap_value, "1") == 0;
    for (const auto source_format : {SurfaceParams::PixelFormat::RGBA8, SurfaceParams::PixelFormat::RGB8}) {
      for (const auto destination_format : {SurfaceParams::PixelFormat::RGBA8, SurfaceParams::PixelFormat::RGB8}) {
        if (!byte_remap && (source_format != SurfaceParams::PixelFormat::RGBA8 ||
                           destination_format != SurfaceParams::PixelFormat::RGBA8)) continue;
    for (bool source_tiled : {false, true}) {
        for (bool destination_tiled : {false, true}) {
            for (bool partial : {false, true}) {
                SurfaceParams source_params;
                source_params.addr = scratch;
                source_params.width = source_params.stride = 256;
                source_params.height = 416;
                source_params.pixel_format = source_format;
                source_params.is_tiled = source_tiled;
                source_params.res_scale = 1;
                source_params.UpdateParams();
                SurfaceParams destination_params = source_params;
                destination_params.addr += 16384;
                destination_params.width = destination_params.stride = 128;
                destination_params.height = 384;
                destination_params.pixel_format = destination_format;
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
      }
    }
    // Observed Luigi dimensions: no single source covers the destination. The
    // RGB source's byte seam is inside a destination tile row and the second
    // source starts after it. Validation must progress through both producers
    // without ever exporting their bytes to RAM.
    if (byte_remap) {
      for (bool source_tiled : {false, true}) {
        for (bool destination_tiled : {false, true}) {
            SurfaceParams first;
            first.addr = scratch;
            first.width = first.stride = 128;
            first.height = 416;
            first.pixel_format = SurfaceParams::PixelFormat::RGB8;
            first.is_tiled = source_tiled;
            first.res_scale = 1;
            first.UpdateParams();
            SurfaceParams second = first;
            second.addr = first.end;
            second.height = 288;
            second.pixel_format = SurfaceParams::PixelFormat::RGBA8;
            second.UpdateParams();
            SurfaceParams destination = first;
            destination.width = destination.stride = 240;
            destination.height = 320;
            destination.pixel_format = SurfaceParams::PixelFormat::RGBA8;
            destination.is_tiled = destination_tiled;
            destination.UpdateParams();
            if (second.end != destination.end) {
                error = "byte remap fixture source coverage mismatch";
                return false;
            }
            std::vector<u8> expected(destination.size);
            u32 random = 0xC09FE219;
            for (auto& byte : expected) {
                random ^= random << 13; random ^= random >> 17; random ^= random << 5;
                byte = u8(random);
            }
            std::memcpy(memory, expected.data(), expected.size());
            test_surface = GetSurface(first, ScaleMatch::Exact, true);
            second_remap_source = GetSurface(second, ScaleMatch::Exact, true);
            std::fill_n(memory, destination.size, 0xD9);
            remap_destination = CreateSurface(destination);
            RegisterSurface(remap_destination);
            remap_destination->LoadGLBuffer(destination.addr, destination.end);
            remap_destination->UploadGLTexture(remap_destination->GetRect());
            InvalidateRegion(first.addr, first.size, test_surface);
            InvalidateRegion(second.addr, second.size, second_remap_source);
            std::fill_n(memory, scratch_size, 0x6B);
            std::fill(test_surface->gl_buffer.begin(), test_surface->gl_buffer.end(), 0x2C);
            std::fill(second_remap_source->gl_buffer.begin(), second_remap_source->gl_buffer.end(), 0x2C);
            // A byte-fragment request may only produce complete destination
            // pixels/tiles inside it. Report exactly what was rendered.
            const SurfaceInterval requested(scratch + 13, scratch + 513);
            SurfaceInterval copied;
            if (!TryRemapSurface(remap_destination, remap_destination->FromInterval(requested),
                                 &copied, &requested) ||
                boost::icl::first(copied) < boost::icl::first(requested) ||
                boost::icl::last_next(copied) > boost::icl::last_next(requested) ||
                boost::icl::length(copied) == 0) {
                error = "byte remap partial ownership did not make bounded progress";
                return false;
            }
            remap_destination->DownloadGLTexture(remap_destination->GetRect());
            remap_destination->FlushGLBuffer(destination.addr, destination.end);
            std::vector<u8> partial_expected(destination.size, 0xD9);
            const auto offset = boost::icl::first(copied) - scratch;
            std::copy_n(expected.data() + offset, boost::icl::length(copied), partial_expected.data() + offset);
            if (!std::equal(partial_expected.begin(), partial_expected.end(), memory)) {
                error = "byte remap partial ownership overwrote neighboring bytes";
                return false;
            }
            std::fill_n(memory, scratch_size, 0x6B);
            ValidateSurface(remap_destination, destination.addr, destination.size);
            if (!remap_destination->IsRegionValid(destination.GetInterval()) ||
                !std::all_of(memory, memory + scratch_size, [](u8 b) { return b == 0x6B; })) {
                error = "byte remap spanning producers used RAM or left invalid bytes";
                return false;
            }
            remap_destination->DownloadGLTexture(remap_destination->GetRect());
            remap_destination->FlushGLBuffer(destination.addr, destination.end);
            if (!std::equal(expected.begin(), expected.end(), memory) || glGetError() != GL_NO_ERROR) {
                error = "byte remap spanning producers changed RGB/RGBA guest bytes";
                return false;
            }
            dirty_regions.erase(destination.GetInterval());
            OpenGLState::ResetTexture(remap_destination->texture.handle);
            UnregisterSurface(remap_destination);
            remap_destination.reset();
            OpenGLState::ResetTexture(test_surface->texture.handle);
            UnregisterSurface(test_surface);
            test_surface.reset();
            OpenGLState::ResetTexture(second_remap_source->texture.handle);
            UnregisterSurface(second_remap_source);
            second_remap_source.reset();
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
    const char* cleanup_value = std::getenv("CARA_SELECTIVE_SURFACE_CLEANUP");
    if (!cleanup_value || std::strcmp(cleanup_value, "1") == 0) {
        const auto saved_counters = Common::FrameTiming::counters;
        Common::FrameTiming::counters.enabled = true;
        // CleanUp operates on the complete cache. Keep any earlier fixtures or
        // setup surfaces recent, then restore their original age after the test.
        SurfaceSet protected_surfaces;
        for (const auto& group : surface_cache)
            protected_surfaces.insert(group.second.begin(), group.second.end());
        std::vector<std::pair<Surface, u32>> saved_frames;
        for (const auto& surface : protected_surfaces) {
            saved_frames.emplace_back(surface, surface->last_used_frame);
            surface->last_used_frame = 2;
        }
        SCOPE_EXIT({
            for (const auto& entry : saved_frames) entry.first->last_used_frame = entry.second;
            Common::FrameTiming::counters = saved_counters;
        });
        retain_small_cpu_writes = true;
        for (bool tiled : {false, true}) {
            for (bool stale_dirty : {false, true}) {
              for (bool orphan : {false, true}) {
                SurfaceParams stale;
                stale.addr = scratch;
                stale.width = stale.stride = stale.height = 16;
                stale.pixel_format = SurfaceParams::PixelFormat::RGBA8;
                stale.is_tiled = tiled;
                stale.res_scale = 1;
                stale.UpdateParams();
                std::vector<u8> old_bytes(stale.size);
                for (size_t i = 0; i < old_bytes.size(); ++i) old_bytes[i] = u8(i * 73 + 17);
                std::memcpy(memory, old_bytes.data(), old_bytes.size());
                test_surface = GetSurface(stale, ScaleMatch::Exact, true);
                InvalidateRegion(stale.addr, stale.size, test_surface);

                // The live alias splits the stale surface across interval-map
                // segments. It owns either the entire range (clean eviction) or
                // only the middle, leaving two stale-owned ranges to export.
                SurfaceParams live = stale;
                live.width = live.stride = 8;
                live.height = stale_dirty ? 16 : 32;
                live.addr += stale_dirty ? 256 : 0;
                live.UpdateParams();
                std::vector<u8> live_bytes(live.size);
                for (size_t i = 0; i < live_bytes.size(); ++i) live_bytes[i] = u8(i * 113 + 5);
                std::memcpy(memory + live.addr - scratch, live_bytes.data(), live_bytes.size());
                remap_destination = CreateSurface(live);
                RegisterSurface(remap_destination);
                remap_destination->LoadGLBuffer(live.addr, live.end);
                remap_destination->UploadGLTexture(remap_destination->GetRect());
                InvalidateRegion(live.addr, live.size, remap_destination);
                if (orphan) {
                    // Exercise the real deferred replacement path: an old
                    // framebuffer is queued, then a different producer updates
                    // only its own draw interval. Old dirty prefix/suffix bytes
                    // must survive unregister until selective cleanup drains them.
                    remove_surfaces.emplace(test_surface);
                    InvalidateRegion(live.addr, live.size, remap_destination);
                    if (test_surface->registered) {
                        error = "cleanup orphan fixture did not unregister deferred producer";
                        return false;
                    }
                }
                if (stale_dirty) InvalidateRegion(scratch + 3, 1, nullptr);
                std::fill_n(memory, scratch_size, 0x6B);
                if (stale_dirty) memory[3] = 0xA7;
                std::fill(test_surface->gl_buffer.begin(), test_surface->gl_buffer.end(), 0x2C);
                std::fill(remap_destination->gl_buffer.begin(), remap_destination->gl_buffer.end(), 0x2C);
                auto retained_dirty = dirty_regions;
                for (const auto& entry : dirty_regions)
                    if (entry.second == test_surface) retained_dirty.erase(entry.first);
                test_surface->last_used_frame = 0;
                remap_destination->last_used_frame = 2;
                const auto download_calls = Common::FrameTiming::counters.surface_download_calls;
                CleanUp(1);
                std::vector<u8> expected(stale.size, 0x6B);
                if (stale_dirty) {
                    std::copy_n(old_bytes.data(), 256, expected.data());
                    std::copy(old_bytes.begin() + 768, old_bytes.end(), expected.begin() + 768);
                    expected[3] = 0xA7;
                }
                if (test_surface->registered || !remap_destination->registered ||
                    dirty_regions != retained_dirty ||
                    !std::equal(expected.begin(), expected.end(), memory) ||
                    !std::all_of(memory + stale.size, memory + scratch_size, [](u8 b) { return b == 0x6B; }) ||
                    (!stale_dirty && Common::FrameTiming::counters.surface_download_calls != download_calls) ||
                    glGetError() != GL_NO_ERROR) {
                    error = "selective cleanup changed retained GPU ownership or CPU bytes tiled=" +
                            std::to_string(tiled) + " stale_dirty=" + std::to_string(stale_dirty) +
                            " orphan=" + std::to_string(orphan);
                    return false;
                }
                // Evicting the alias must not consume the live producer. A later
                // real CPU request must still recover its original GPU bytes.
                FlushRegion(live.addr, live.size, remap_destination);
                std::copy(live_bytes.begin(), live_bytes.end(), expected.begin() + live.addr - scratch);
                if (!std::equal(expected.begin(), expected.end(), memory) ||
                    glGetError() != GL_NO_ERROR) {
                    error = "selective cleanup lost later live-owner readback";
                    return false;
                }
                dirty_regions.erase(stale.GetInterval());
                OpenGLState::ResetTexture(remap_destination->texture.handle);
                UnregisterSurface(remap_destination);
                remap_destination.reset();
                OpenGLState::ResetTexture(test_surface->texture.handle);
                test_surface.reset();
              }
            }
        }
    }
    return true;
}
} // namespace OpenGL
