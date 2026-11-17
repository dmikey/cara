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
bool RasterizerCacheOpenGL::RunNativeRGBA8RemapSelfTest(std::string& error) {
    const auto require=[](bool ok,const std::string& why){if(!ok)throw std::runtime_error(why);};
    const auto flag=[](const char* key){const char* v=std::getenv(key);return v&&std::strcmp(v,"1")==0;};
    const bool enabled=flag("CARA_NATIVE_RGBA8_SURFACE_REMAP")&&flag("CARA_GPU_SURFACE_REMAP");
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
        const auto verify=[&](const Surface& s,const std::vector<u8>& expected,const char* name){normal_pack();s->DownloadGLTexture(s->GetRect());s->FlushGLBuffer(s->addr,s->end);const auto at=std::mismatch(expected.begin(),expected.end(),ram+s->addr-scratch);require(at.first==expected.end(),std::string(name)+" mismatch byte="+std::to_string(at.first-expected.begin()));require(glGetError()==GL_NO_ERROR,std::string(name)+" GL error");};
        // Cross layouts followed by same layouts catches persistent reversal uniforms.
        if(enabled)for(auto layouts: {std::array<bool,2>{true,false},{false,true},{true,true},{false,false}}){
            cleanup();normal_pack();auto sp=params(scratch,256,416,layouts[0]);auto dp=params(scratch,128,416,layouts[1]);
            auto source=create(sp),destination=create(dp);draw(source,23);auto expected=bytes(sp,23);expected.resize(dp.size);poison();
            OpenGLState hostile;hostile.draw.draw_framebuffer=framebuffer.handle;hostile.draw.read_framebuffer=framebuffer.handle;hostile.draw.vertex_array=vao.handle;hostile.draw.shader_program=producer.handle;hostile.scissor.enabled=true;hostile.scissor.x=1;hostile.scissor.y=2;hostile.scissor.width=3;hostile.scissor.height=4;hostile.color_mask={false,true,false,true};hostile.texture_units[0].sampler=hostile_sampler.handle;hostile.viewport={3,5,7,9};hostile.Apply();
            glEnable(GL_DITHER);glEnable(GL_RASTERIZER_DISCARD);glActiveTexture(GL_TEXTURE3);
            glBindBuffer(GL_PIXEL_PACK_BUFFER,pack.handle);glPixelStorei(GL_PACK_ALIGNMENT,8);glPixelStorei(GL_PACK_ROW_LENGTH,23);glPixelStorei(GL_PACK_SKIP_ROWS,2);glPixelStorei(GL_PACK_SKIP_PIXELS,3);
            GLint baseline_sampler{};glActiveTexture(GL_TEXTURE0);glGetIntegerv(GL_SAMPLER_BINDING,&baseline_sampler);glActiveTexture(GL_TEXTURE3);require(baseline_sampler==GLint(hostile_sampler.handle)&&glGetError()==GL_NO_ERROR,"invalid hostile sampler baseline");
            const auto before=Common::FrameTiming::counters;
            require(TryRemapSurface(destination,dp),"eligible actual native cache remap rejected");
            GLint value{};glActiveTexture(GL_TEXTURE0);glGetIntegerv(GL_SAMPLER_BINDING,&value);glActiveTexture(GL_TEXTURE3);require(value==GLint(hostile_sampler.handle),"remap changed sampler0 expected="+std::to_string(hostile_sampler.handle)+" actual="+std::to_string(value)+" glerror="+std::to_string(glGetError()));glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING,&value);require(value==GLint(pack.handle),"remap changed PBO");glGetIntegerv(GL_PACK_ROW_LENGTH,&value);require(value==23,"remap changed pack row");glGetIntegerv(GL_PACK_ALIGNMENT,&value);require(value==8,"remap changed pack alignment");glGetIntegerv(GL_PACK_SKIP_ROWS,&value);require(value==2,"remap changed pack skip rows");glGetIntegerv(GL_PACK_SKIP_PIXELS,&value);require(value==3,"remap changed pack skip pixels");glGetIntegerv(GL_ACTIVE_TEXTURE,&value);require(value==GL_TEXTURE3,"remap changed active texture");require(glIsEnabled(GL_DITHER)&&glIsEnabled(GL_RASTERIZER_DISCARD),"remap lost raw enable state");std::vector<u8> pbo_after(4096);glGetBufferSubData(GL_PIXEL_PACK_BUFFER,0,4096,pbo_after.data());require(pbo_after==pbo_canary,"remap wrote PBO");
            GLint viewport[4]{};glGetIntegerv(GL_VIEWPORT,viewport);require(viewport[0]==3&&viewport[1]==5&&viewport[2]==7&&viewport[3]==9,"remap lost viewport");GLboolean mask[4]{};glGetBooleanv(GL_COLOR_WRITEMASK,mask);require(!mask[0]&&mask[1]&&!mask[2]&&mask[3],"remap lost color mask");
            require(Common::FrameTiming::counters.surface_remap_calls==before.surface_remap_calls+1&&Common::FrameTiming::counters.surface_download_calls==before.surface_download_calls,"remap count/download proof");require(std::all_of(ram,ram+scratch_size,[](u8 b){return b==0x6B;}),"remap exported poisoned RAM");
            verify(destination,expected,"layout");
            // Actual validation after a fresh GPU producer must not see old pixels.
            draw(source,71);expected=bytes(sp,71);expected.resize(dp.size);poison();const auto rewrite_before=Common::FrameTiming::counters;ValidateSurface(destination,dp.addr,dp.size);require(Common::FrameTiming::counters.surface_download_calls==rewrite_before.surface_download_calls,"GPU rewrite validation downloaded");verify(destination,expected,"fresh GPU rewrite");
            // CPU owns precisely these bytes; remaining dirty GPU bytes must survive fallback.
            InvalidateRegion(scratch+64,16,nullptr);std::fill_n(ram+64,16,0xA7);std::fill_n(expected.data()+64,16,0xA7);ValidateSurface(destination,dp.addr,dp.size);verify(destination,expected,"CPU invalidation");
            std::cout<<"PASS native layout "<<layouts[0]<<"->"<<layouts[1]<<" GPU rewrite + CPU invalidation\n";
        }
        // Exact traced AccelerateTextureCopy, with untouched output gaps.
        cleanup();normal_pack();auto owner_params=params(scratch,256,416,true);auto input_params=params(scratch+0xb3000,16,416,false);
        auto owner=create(owner_params),input=create(input_params);draw(owner,31);draw(input,99);
        // Match observed mixed ownership: first16KiB CPU-owned; remaining destination GPU-owned.
        InvalidateRegion(scratch,16384,nullptr);
        auto expected=bytes(owner_params,31);expected.resize(128*416*4);std::fill_n(expected.data(),16384,0x6B);const auto input_expected=bytes(input_params,99);
        for(unsigned row=0;row<416;++row)std::copy_n(input_expected.data()+row*64,32,expected.data()+row*512);
        poison();normal_pack();GPU::Regs::DisplayTransferConfig config{};config.input_address=input_params.addr/8;config.output_address=scratch/8;config.flags=12;config.texture_copy.size=13312;config.texture_copy.input_size=2u|(2u<<16);config.texture_copy.output_size=2u|(30u<<16);
        const auto before=Common::FrameTiming::counters;
        require(static_cast<RasterizerOpenGL*>(VideoCore::Rasterizer())->AccelerateTextureCopy(config),"actual traced texture copy rejected");
        const auto after=Common::FrameTiming::counters;
        std::cout<<"COPY_COUNTS enabled="<<enabled<<" counters="<<after.enabled<<" remaps="<<after.surface_remap_calls-before.surface_remap_calls<<" downloads="<<after.surface_download_calls-before.surface_download_calls<<" bytes="<<after.surface_download_bytes-before.surface_download_bytes<<std::endl;
        if(enabled){require(after.surface_remap_calls>before.surface_remap_calls,"copy acquisition did not remap");require(after.surface_download_calls==before.surface_download_calls,"copy acquisition downloaded");require(std::all_of(ram,ram+scratch_size,[](u8 b){return b==0x6B;}),"ON copy exported RAM");}
        else require(after.surface_download_calls>before.surface_download_calls,"OFF fallback had no counted downloads");
        auto dp=params(scratch,128,416,false);auto destination=GetSurface(dp,ScaleMatch::Exact,true);require(bool(destination),"copy destination absent");owned.push_back(destination);
        std::fill_n(ram,scratch_size,0x6B);verify(destination,expected,"actual copy+480byte gaps");require(std::all_of(ram+dp.size,ram+scratch_size,[](u8 b){return b==0x6B;}),"copy verification changed canary");
        std::cout<<"PASS actual native copy enabled="<<enabled<<" remaps="<<after.surface_remap_calls-before.surface_remap_calls<<" downloads="<<after.surface_download_calls-before.surface_download_calls<<" download_bytes="<<after.surface_download_bytes-before.surface_download_bytes<<" preserved_gap_bytes="<<416*480<<"\n";
        return true;
    }catch(const std::exception& failure){error=failure.what();return false;}
}
} // namespace OpenGL
