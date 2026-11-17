// SPDX-License-Identifier: GPL-2.0-or-later
// ROM-free actual native GL/cache oracle. Never invoked by normal emulation.
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
#include "common/frame_timing.h"
#include "common/scope_exit.h"
#include "core/hw/gpu.h"
#include "core/memory.h"
#include "core/settings.h"
#include "video_core/renderer_opengl/gl_rasterizer.h"
#include "video_core/renderer_opengl/gl_rasterizer_cache.h"
#include "video_core/renderer_opengl/gl_state.h"
#include "video_core/renderer_opengl/gl_vars.h"
#include "video_core/video_core.h"

namespace OpenGL {
bool RasterizerCacheOpenGL::RunNativeShaderCopySelfTest(std::string& error) {
    return RunNativeRequiredCopySelfTest(error);
}
bool RasterizerCacheOpenGL::RunNativeRequiredCopySelfTest(std::string& error) {
    const auto require=[](bool ok,const std::string& why){if(!ok)throw std::runtime_error(why);};
    const auto flag=[](const char* key){const char* v=std::getenv(key);return v&&std::strcmp(v,"1")==0;};
    const bool shader_enabled=flag("CARA_NATIVE_SHADER_SURFACE_COPY");
    const bool copytex_enabled=flag("CARA_NATIVE_COPYTEX_SURFACE_COPY");
    if(shader_enabled && copytex_enabled){error="native copy backends are mutually exclusive";return false;}
    const bool enabled=shader_enabled || copytex_enabled;
    const bool audit=flag("CARA_NATIVE_GL_ERROR_AUDIT") && flag("CARA_NATIVE_DIAGNOSTICS");
    const bool runtime_queries = flag("CARA_NATIVE_COPY_FORCE_QUERIES") ||
        !std::getenv("CARA_NATIVE_COPY_RUNTIME_QUERIES") ||
        std::strcmp(std::getenv("CARA_NATIVE_COPY_RUNTIME_QUERIES"),"0")!=0;
    if(!audit && !runtime_queries){error="audit0 required-copy oracle needs runtime queries or FORCE_QUERIES for negative case";return false;}
    const auto backend_calls=[&] {return copytex_enabled ? NativeCopyTexSurfaceCopyCalls() : NativeShaderSurfaceCopyCalls();};
    const auto unselected_calls=[&] {return copytex_enabled ? NativeShaderSurfaceCopyCalls() : NativeCopyTexSurfaceCopyCalls();};
    const u64 initial_unselected=unselected_calls();
    if(GLES){error="native remap oracle requires actual desktop GL";return false;}
    constexpr u32 scratch_size=1024*1024;
    constexpr PAddr scratch=Memory::VRAM_PADDR_END-scratch_size;
    const SurfaceInterval area(scratch,scratch+scratch_size);
    auto occupied=surface_cache.equal_range(area);auto dirty=dirty_regions.equal_range(area);
    if(occupied.first!=occupied.second||dirty.first!=dirty.second){error="oracle scratch occupied";return false;}
    u8* ram=VideoCore::Memory()->GetPhysicalPointer(scratch);
    if(!ram){error="oracle VRAM unavailable";return false;}
    const std::vector<u8> backup(ram,ram+scratch_size);
    const auto old_state=OpenGLState::GetCurState();
    const auto old_counters=Common::FrameTiming::counters;
    const bool old_skip=Settings::values.skip_cpu_write;
    const bool old_skip_copy=Settings::values.skip_texture_copy;
    const bool old_remap=gpu_surface_remap;
    GLint old_pack{},old_alignment{},old_row{},old_skip_rows{},old_skip_pixels{},old_active{};
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING,&old_pack);glGetIntegerv(GL_PACK_ALIGNMENT,&old_alignment);
    glGetIntegerv(GL_PACK_ROW_LENGTH,&old_row);glGetIntegerv(GL_PACK_SKIP_ROWS,&old_skip_rows);
    glGetIntegerv(GL_PACK_SKIP_PIXELS,&old_skip_pixels);glGetIntegerv(GL_ACTIVE_TEXTURE,&old_active);
    const bool old_dither=glIsEnabled(GL_DITHER),old_discard=glIsEnabled(GL_RASTERIZER_DISCARD);
    OGLFramebuffer framebuffer;OGLVertexArray vao;OGLProgram producer;OGLBuffer pack;OGLSampler hostile_sampler;
    std::vector<Surface> owned;
    const auto normal_pack=[] {glBindBuffer(GL_PIXEL_PACK_BUFFER,0);glPixelStorei(GL_PACK_ALIGNMENT,1);
        glPixelStorei(GL_PACK_ROW_LENGTH,0);glPixelStorei(GL_PACK_SKIP_ROWS,0);glPixelStorei(GL_PACK_SKIP_PIXELS,0);};
    const auto cleanup=[&] {dirty_regions.erase(area);for(auto& s:owned){OpenGLState::ResetTexture(s->texture.handle);UnregisterSurface(s);}owned.clear();};
    SCOPE_EXIT({cleanup();std::memcpy(ram,backup.data(),backup.size());gpu_surface_remap=old_remap;
        Settings::values.skip_cpu_write=old_skip;Settings::values.skip_texture_copy=old_skip_copy;Common::FrameTiming::counters=old_counters;
        glBindBuffer(GL_PIXEL_PACK_BUFFER,old_pack);glPixelStorei(GL_PACK_ALIGNMENT,old_alignment);
        glPixelStorei(GL_PACK_ROW_LENGTH,old_row);glPixelStorei(GL_PACK_SKIP_ROWS,old_skip_rows);
        glPixelStorei(GL_PACK_SKIP_PIXELS,old_skip_pixels);
        if(old_dither)glEnable(GL_DITHER);else glDisable(GL_DITHER);
        if(old_discard)glEnable(GL_RASTERIZER_DISCARD);else glDisable(GL_RASTERIZER_DISCARD);
        OpenGLState::InvalidateRasterState();old_state.Apply();glActiveTexture(old_active);});
    try {
        Settings::values.skip_cpu_write=false;Settings::values.skip_texture_copy=false;gpu_surface_remap=true;
        Common::FrameTiming::counters={};Common::FrameTiming::counters.enabled=true;
        normal_pack();require(glGetError()==GL_NO_ERROR,"initial native GL error");
        framebuffer.Create();vao.Create();pack.Create();hostile_sampler.Create();
        glBindBuffer(GL_PIXEL_PACK_BUFFER,pack.handle);const std::vector<u8> pbo_canary(4096,0xB4);glBufferData(GL_PIXEL_PACK_BUFFER,4096,pbo_canary.data(),GL_STREAM_READ);normal_pack();
        producer.Create(R"(
void main(){const vec2 p[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));gl_Position=vec4(p[gl_VertexID],0,1);}
)",R"(
uniform int seed;
out vec4 color;
void main(){ivec2 p=ivec2(gl_FragCoord.xy);ivec4 c=ivec4(p.x+3*p.y+seed,5*p.x+p.y+17+seed,7*p.x+11*p.y+91+seed,13*p.x+19*p.y+151+seed)&ivec4(255);color=vec4(c)/255.0;}
)");
        require(producer.handle!=0,"producer shader compile");
        const GLint seed_location=glGetUniformLocation(producer.handle,"seed");
        const auto params=[](PAddr addr,u32 width,u32 height,bool tiled){SurfaceParams p;p.addr=addr;p.width=p.stride=width;p.height=height;p.is_tiled=tiled;p.res_scale=1;p.pixel_format=SurfaceParams::PixelFormat::RGBA8;p.UpdateParams();return p;};
        const auto create=[&](const SurfaceParams& p){auto s=CreateSurface(p);RegisterSurface(s);owned.push_back(s);return s;};
        // Independent CPU expectation: shader has no guest-layout knowledge.
        const auto bytes=[](const SurfaceParams& p,unsigned seed){std::vector<u8> out(p.size);
            for(u32 y=0;y<p.height;++y)for(u32 x=0;x<p.width;++x){
                const u32 guest_y=p.is_tiled?p.height-1-y:y;
                u32 pixel=guest_y*p.stride+x;
                if(p.is_tiled){u32 morton=0;for(unsigned bit=0;bit<3;++bit){morton|=((x>>bit)&1)<<(2*bit);morton|=((guest_y>>bit)&1)<<(2*bit+1);}pixel=(guest_y/8)*p.stride*8+(x/8)*64+morton;}
                const std::array<u8,4> c={u8(x+3*y+seed),u8(5*x+y+17+seed),u8(7*x+11*y+91+seed),u8(13*x+19*y+151+seed)};
                for(unsigned k=0;k<4;++k)out[pixel*4+k]=c[p.is_tiled?3-k:k];
            }return out;};
        const auto draw=[&](const Surface& s,unsigned seed){OpenGLState state;state.draw.draw_framebuffer=framebuffer.handle;state.draw.read_framebuffer=framebuffer.handle;state.draw.shader_program=producer.handle;state.draw.vertex_array=vao.handle;state.viewport={0,0,GLsizei(s->width),GLsizei(s->height)};state.Apply();glDisable(GL_DITHER);glDisable(GL_RASTERIZER_DISCARD);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,s->texture.handle,0);require(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"producer FBO");glUniform1i(seed_location,seed);glDrawArrays(GL_TRIANGLES,0,3);require(glGetError()==GL_NO_ERROR,"GPU producer draw");InvalidateRegion(s->addr,s->size,s);};
        const auto poison=[&]{std::fill_n(ram,scratch_size,0x6B);for(auto& s:owned)s->gl_buffer.assign(s->size,0x2C);};
        const auto verify=[&](const Surface& s,const std::vector<u8>& expected,const char* name){normal_pack();s->DownloadGLTexture(s->GetRect());s->FlushGLBuffer(s->addr,s->end);const auto at=std::mismatch(expected.begin(),expected.end(),ram+(s->addr-scratch));require(at.first==expected.end(),std::string(name)+" mismatch byte="+std::to_string(at.first-expected.begin()));require(glGetError()==GL_NO_ERROR,std::string(name)+" GL error");};
        const auto hostile_state=[&] {
            OpenGLState hostile;
            hostile.draw.draw_framebuffer=framebuffer.handle;
            hostile.draw.read_framebuffer=framebuffer.handle;
            hostile.draw.vertex_array=vao.handle;
            hostile.draw.shader_program=producer.handle;
            hostile.scissor={true,1,2,3,4};
            hostile.color_mask={false,true,false,true};
            hostile.texture_units[0].sampler=hostile_sampler.handle;
            hostile.texture_units[0].texture_2d=owned.empty()?0:owned.front()->texture.handle;
            hostile.viewport={3,5,7,9};hostile.Apply();
            glEnable(GL_DITHER);glEnable(GL_RASTERIZER_DISCARD);glActiveTexture(GL_TEXTURE3);
            glBindBuffer(GL_PIXEL_PACK_BUFFER,pack.handle);
            glPixelStorei(GL_PACK_ALIGNMENT,8);glPixelStorei(GL_PACK_ROW_LENGTH,23);
            glPixelStorei(GL_PACK_SKIP_ROWS,2);glPixelStorei(GL_PACK_SKIP_PIXELS,3);
            GLint sampler{};glActiveTexture(GL_TEXTURE0);glGetIntegerv(GL_SAMPLER_BINDING,&sampler);
            glActiveTexture(GL_TEXTURE3);
            require(sampler==GLint(hostile_sampler.handle)&&glGetError()==GL_NO_ERROR,
                    "invalid hostile state baseline");
        };
        const auto state_preserved=[&] {
            const auto check=[&](GLenum name,GLint expected,const char* label) {
                GLint actual{};glGetIntegerv(name,&actual);
                require(actual==expected,std::string("copy changed ")+label);
            };
            check(GL_ACTIVE_TEXTURE,GL_TEXTURE3,"active texture");
            glActiveTexture(GL_TEXTURE0);check(GL_SAMPLER_BINDING,hostile_sampler.handle,"sampler0");
            check(GL_TEXTURE_BINDING_2D,owned.empty()?0:owned.front()->texture.handle,"texture0");
            glActiveTexture(GL_TEXTURE3);
            check(GL_PIXEL_PACK_BUFFER_BINDING,pack.handle,"PBO");
            check(GL_PACK_ALIGNMENT,8,"pack alignment");check(GL_PACK_ROW_LENGTH,23,"pack row");
            check(GL_PACK_SKIP_ROWS,2,"pack skip rows");check(GL_PACK_SKIP_PIXELS,3,"pack skip pixels");
            check(GL_DRAW_FRAMEBUFFER_BINDING,framebuffer.handle,"draw FBO");
            check(GL_READ_FRAMEBUFFER_BINDING,framebuffer.handle,"read FBO");
            check(GL_READ_BUFFER,GL_COLOR_ATTACHMENT0,"read buffer");
            check(GL_CURRENT_PROGRAM,producer.handle,"program");
            check(GL_VERTEX_ARRAY_BINDING,vao.handle,"VAO");
            require(glIsEnabled(GL_DITHER)&&glIsEnabled(GL_RASTERIZER_DISCARD)&&
                    glIsEnabled(GL_SCISSOR_TEST),"copy lost raw/tracked enable state");
            GLint viewport[4]{},scissor[4]{};glGetIntegerv(GL_VIEWPORT,viewport);
            glGetIntegerv(GL_SCISSOR_BOX,scissor);
            require(viewport[0]==3&&viewport[1]==5&&viewport[2]==7&&viewport[3]==9,"copy viewport");
            require(scissor[0]==1&&scissor[1]==2&&scissor[2]==3&&scissor[3]==4,"copy scissor");
            GLboolean mask[4]{};glGetBooleanv(GL_COLOR_WRITEMASK,mask);
            require(!mask[0]&&mask[1]&&!mask[2]&&mask[3],"copy color mask");
            std::vector<u8> after(4096);glGetBufferSubData(GL_PIXEL_PACK_BUFFER,0,4096,after.data());
            require(after==pbo_canary,"copy wrote PBO");
            require(glGetError()==GL_NO_ERROR,"copy/state GL error");
        };
        u64 cases=0,integrations=0,backend_successes=0;
        // CanSubRect/CopySurface only permits matching tiling. Cross-layout copies
        // belong to the independent alias-remap oracle, never this precondition.
        for(bool tiled:{false,true})for(unsigned seed:{0u,23u,71u,255u})for(unsigned part=0;part<3;++part) {
            cleanup();normal_pack();
            const auto sp=params(scratch,256,912,tiled);
            const auto dp=params(scratch+400*256*4,256,512,tiled);
            auto source=create(sp),destination=create(dp);
            draw(destination,seed+97);draw(source,seed);
            const auto original=bytes(sp,seed);
            auto expected=bytes(dp,seed+97);
            const u32 unit=tiled?256u:4u;
            const u32 row=256*4*(tiled?8:1);
            const SurfaceInterval interval=part==0?dp.GetInterval():part==1?
                SurfaceInterval(dp.addr+row*2,dp.addr+row*4):
                SurfaceInterval(dp.addr+row*3+unit*3,dp.addr+row*3+unit*5);
            const auto sub=dp.FromInterval(interval);
            require(sub.GetInterval()==interval&&source->CanSubRect(sub)&&
                    source->IsRegionValid(interval),"invalid actual CopySurface fixture precondition");
            const u32 begin=boost::icl::first(interval),end=boost::icl::last_next(interval);
            std::copy(original.begin()+(begin-sp.addr),original.begin()+(end-sp.addr),
                      expected.begin()+(begin-dp.addr));
            if(!audit && !tiled&&seed==0&&part==0) {
                const u64 error_backend=backend_calls();
                glEnable(static_cast<GLenum>(0xFFFFFFFFu));
                bool rejected=false;
                try { CopySurface(source,destination,interval); }
                catch(const std::runtime_error&) { rejected=true; }
                require(rejected&&backend_calls()==error_backend,
                        "preexisting GL error did not reject required copy");
                verify(destination,bytes(dp,seed+97),"preexisting error left destination unchanged");
            }
            poison();hostile_state();
            const u64 before_backend=backend_calls();
            const auto before=Common::FrameTiming::counters;
            CopySurface(source,destination,interval);
            require(glGetError()==GL_NO_ERROR,"direct required-copy API error boundary");
            const u64 chosen=backend_calls()-before_backend;
            require(chosen==(enabled?1u:0u),"wrong direct CopySurface backend count");
            backend_successes+=chosen;
            require(Common::FrameTiming::counters.surface_remap_calls==before.surface_remap_calls,
                    "required copy mislabeled alias remap");
            require(Common::FrameTiming::counters.surface_download_calls==before.surface_download_calls,
                    "required copy unexpectedly downloaded");
            state_preserved();
            require(std::all_of(ram,ram+scratch_size,[](u8 b){return b==0x6B;}),"copy exported RAM");
            verify(destination,expected,"required CopySurface partial/sentinel");
            require(std::all_of(ram,ram+(dp.addr-scratch),[](u8 b){return b==0x6B;})&&
                    std::all_of(ram+(dp.end-scratch),ram+scratch_size,[](u8 b){return b==0x6B;}),
                    "required copy changed outside-view canary");
            ++cases;
            // Fresh real GPU producer -> actual ValidateSurface -> CopySurface.
            draw(source,seed+41);poison();normal_pack();
            const auto current=bytes(sp,seed+41);
            const std::vector<u8> full_expected(current.begin()+(dp.addr-sp.addr),
                                               current.begin()+(dp.end-sp.addr));
            const auto integration_before=Common::FrameTiming::counters;
            const u64 integration_backend=backend_calls();
            ValidateSurface(destination,dp.addr,dp.size);
            require(glGetError()==GL_NO_ERROR,"validation required-copy API error boundary");
            const u64 integrated=backend_calls()-integration_backend;
            require(integrated==(enabled?1u:0u),"wrong ValidateSurface required-copy backend");
            require(Common::FrameTiming::counters.surface_remap_calls==integration_before.surface_remap_calls&&
                    Common::FrameTiming::counters.surface_download_calls==integration_before.surface_download_calls,
                    "validation integration used alias remap/download");
            backend_successes+=integrated;
            verify(destination,full_expected,"ValidateSurface required-copy chain");
            ++integrations;
        }
        // Optional original-data extension: actual registered GPU owners, normal
        // CPU-write/cache paths and current GL bindings. Never normal emulation.
        if (flag("CARA_NATIVE_NEAREST_BORROW_COHERENCE_SELF_TEST")) {
            using R=NearestBorrow::Rect;
            u64 borrow_cases=0,retained_outside=0,evicted_outside=0,inside_rejections=0;
            const R crop{0,0,240,400},write{0,0,240,400};
            for (unsigned seed : {17u,201u}) {
                for (unsigned mode=0;mode<3;++mode) {
                    cleanup();normal_pack();
                    const auto sp=params(scratch,256,912,true);
                    const auto dp=params(scratch,256,512,true);
                    auto source=create(sp),destination=create(dp);
                    draw(source,seed+mode*29);
                    auto expected=bytes(sp,seed+mode*29);
                    Pica::Texture::TextureInfo info{dp.addr,dp.width,dp.height,
                        std::ptrdiff_t(dp.width*32),Pica::TexturingRegs::TextureFormat::RGBA8};
                    const auto destination_invalid=destination->invalid_regions;
                    require(!destination->IsRegionValid(dp.GetInterval()),"borrow destination unexpectedly valid before describe");
                    auto lease=DescribeNearestBorrow(source,info,0,crop,write);
                    require(lease && lease->read_rect==R{0,400,240,800},"registered GPU owner read lease rejected");
                    const auto bind_source=[&] {
                        auto state=OpenGLState::GetCurState();
                        state.draw.draw_framebuffer=framebuffer.handle;
                        state.draw.read_framebuffer=framebuffer.handle;
                        state.texture_units[0].texture_2d=source->texture.handle;
                        state.texture_units[1].texture_2d=0;state.texture_units[2].texture_2d=0;
                        state.Apply();
                        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,
                                              GL_TEXTURE_2D,source->texture.handle,0);
                        require(glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,
                                "borrow fixture source FBO incomplete");
                    };
                    const auto live_revalidate=[&] {
                        const auto actual=OpenGLState::GetCurState();GLint attachment=0;
                        glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,
                            GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME,&attachment);
                        const std::array<GLuint,2> aliases{actual.texture_units[1].texture_2d,
                                                         actual.texture_units[2].texture_2d};
                        return RevalidateNearestBorrow(*lease,source,GLuint(attachment),
                            actual.texture_units[0].texture_2d,aliases,true);
                    };
                    bind_source();require(live_revalidate(),"actual current GPU bindings rejected initial lease");
                    require(destination->invalid_regions==destination_invalid,
                            "describe/revalidate published logical destination validity");
                    // Actual GL aliases and actual changed bindings, not fake handles.
                    auto alias_state=OpenGLState::GetCurState();
                    alias_state.texture_units[1].texture_2d=source->texture.handle;alias_state.Apply();
                    require(!live_revalidate(),"other sampled GPU owner alias accepted");bind_source();
                    alias_state=OpenGLState::GetCurState();
                    alias_state.texture_units[0].texture_2d=destination->texture.handle;alias_state.Apply();
                    require(!live_revalidate(),"changed actual unit0 handle accepted");bind_source();
                    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,
                                          destination->texture.handle,0);
                    require(!live_revalidate(),"changed actual color attachment accepted");bind_source();
                    require(live_revalidate(),"restored actual bindings did not recover lease");

                    // mode0: bulk16 outside R, then bulk16 inside R; modes1/2:
                    // small4 outside/inside under the process's ACTUAL residency policy.
                    const unsigned writes=mode==0?2:1;
                    bool source_evicted=false;
                    for (unsigned step=0;step<writes;++step) {
                        const bool inside=mode==2 || (mode==0&&step==1);
                        const u32 count=mode==0?16:4;
                        const PAddr address=inside?boost::icl::first(lease->read_interval)+256:sp.addr+256;
                        const auto read_begin=boost::icl::first(lease->read_interval);
                        const auto read_end=boost::icl::last_next(lease->read_interval);
                        const bool within_read=address>=read_begin && address+count<=read_end;
                        const bool outside_read=address+count<=read_begin || address>=read_end;
                        require(inside ? within_read : outside_read,
                                "CPU-write/read fixture domains wrong");
                        // Exactly the normal cache-side FlushAndInvalidate order,
                        // before modifying the coherent physical scratch RAM.
                        FlushRegion(address,count);
                        InvalidateRegion(address,count,nullptr,SurfaceInvalidationOrigin::CPU);
                        for (u32 i=0;i<count;++i) {
                            const u8 value=u8(0xD3u+seed+mode*7+step*11+i*13);
                            ram[address-scratch+i]=value;expected[address-sp.addr+i]=value;
                        }
                        bind_source();
                        if (inside) {
                            ++inside_rejections;
                            require(!live_revalidate() && !DescribeNearestBorrow(source,info,0,crop,write),
                                    "CPU write inside actual read interval accepted");
                        } else if (source->registered) {
                            ++retained_outside;
                            require(source->IsRegionValid(lease->read_interval),
                                    "retained outside-write owner lost untouched read validity");
                            // Guest CPU/cache work may legitimately advance the
                            // global registration generation. A lease is per-draw,
                            // never permission to survive arbitrary CPU execution.
                            auto fresh=DescribeNearestBorrow(source,info,0,crop,write);
                            require(fresh.has_value(),"fresh valid read crop rejected after outside CPU write");
                            if (fresh->registration_generation!=lease->registration_generation)
                                require(!live_revalidate(),"old lease ignored actual registration generation change");
                            lease=std::move(fresh);
                            bind_source();
                            require(live_revalidate(),"fresh post-write lease rejected with actual bindings");
                            // The normal CPU read path must export untouched GPU R.
                            const PAddr begin=boost::icl::first(lease->read_interval);
                            const u32 read_size=boost::icl::length(lease->read_interval);
                            FlushRegion(begin,read_size);
                            require(std::equal(expected.begin()+(begin-sp.addr),
                                               expected.begin()+(begin-sp.addr+read_size),
                                               ram+(begin-scratch)),"untouched GPU read bytes lost after outside CPU write");
                        } else {
                            ++evicted_outside;
                            require(!live_revalidate(),"evicted outside-write owner retained lease");
                        }
                        require(!destination->registered || destination->invalid_regions==destination_invalid,
                                "CPU invalidation/borrow unexpectedly made logical destination valid");
                        source_evicted=source_evicted || !source->registered;
                    }
                    // Execute all normal required validation work; do not fix
                    // validity manually to make the lease pass.
                    auto logical=GetTextureSurface(info,0);
                    require(logical && logical->IsRegionValid(dp.GetInterval()),
                            "baseline logical texture acquisition did not validate CPU patches");
                    if (std::find(owned.begin(),owned.end(),logical)==owned.end())owned.push_back(logical);
                    const std::vector<u8> logical_expected(expected.begin(),expected.begin()+dp.size);
                    verify(logical,logical_expected,"baseline texture after actual CPU partial writes");
                    if (source->registered) {
                        ValidateSurface(source,sp.addr,sp.size);
                        bind_source();
                        auto recovered=DescribeNearestBorrow(source,info,0,crop,write);
                        require(recovered && source->IsRegionValid(recovered->read_interval),
                                "baseline validated source did not recover read lease");
                        lease=std::move(recovered);
                        require(live_revalidate(),"recovered GPU source did not pass live revalidation");
                    } else {
                        require(source_evicted && !DescribeNearestBorrow(source,info,0,crop,write),
                                "baseline acquisition revived stale evicted owner");
                    }
                    // Guest-visible read exports newer GPU bytes and preserves
                    // all CPU patches, including bytes outside the sampled crop.
                    FlushRegion(sp.addr,sp.size);
                    require(std::equal(expected.begin(),expected.end(),ram),
                            "guest-visible full owner bytes lost GPU data or CPU patch");
                    if (source->registered) {
                        bind_source();
                        UnregisterSurface(source); // actual eviction/generation path
                        require(!live_revalidate(),"unregistered strong owner retained lease");
                    }
                    auto replacement=GetSurface(sp,ScaleMatch::Exact,true);
                    require(replacement && replacement!=source && replacement->texture.handle!=source->texture.handle,
                            "baseline owner replacement was not a fresh registered GPU allocation");
                    if (std::find(owned.begin(),owned.end(),replacement)==owned.end())owned.push_back(replacement);
                    auto replacement_state=OpenGLState::GetCurState();
                    replacement_state.texture_units[0].texture_2d=replacement->texture.handle;
                    replacement_state.Apply();
                    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,
                                          replacement->texture.handle,0);
                    GLint replacement_attachment=0;
                    glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,
                        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME,&replacement_attachment);
                    const auto bound_replacement=OpenGLState::GetCurState();
                    const std::array<GLuint,2> replacement_aliases{
                        bound_replacement.texture_units[1].texture_2d,bound_replacement.texture_units[2].texture_2d};
                    require(!RevalidateNearestBorrow(*lease,replacement,GLuint(replacement_attachment),
                                bound_replacement.texture_units[0].texture_2d,replacement_aliases,true),
                            "stale lease accepted actual replacement owner/handle");
                    verify(replacement,expected,"replacement owner preserves required guest bytes");
                    require(glGetError()==GL_NO_ERROR,"borrow coherence case GL error");
                    ++borrow_cases;
                }
            }
            std::cout<<"PASS nearest borrow actual-cache coherence cases="<<borrow_cases
                     <<" retained_small_cpu_writes_policy="<<retain_small_cpu_writes
                     <<" retained_outside="<<retained_outside<<" evicted_outside="<<evicted_outside
                     <<" inside_rejections="<<inside_rejections
                     <<" logical_destination_not_published=1 actual_replacement_stale_reject=1\n";
        }
        require(unselected_calls()==initial_unselected,"unselected native copy backend was used");
        std::cout<<"PASS required CopySurface native backend="<<(copytex_enabled?"copytex":shader_enabled?"shader":"baseline")
                 <<" enabled="<<enabled
                 <<" direct_cases="<<cases<<" validation_cases="<<integrations
                 <<" selected_backend_calls="<<backend_successes
                 <<" negative_error_case_exercised="<<(!audit)
                 <<" alias_remaps=0 acquisition_downloads=0 full_destination_bytes="
                 <<(cases+integrations)*256*512*4<<"\n";
        return true;
    }catch(const std::exception& failure){error=failure.what();return false;}
}
} // namespace OpenGL
