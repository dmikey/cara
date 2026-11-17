// SPDX-License-Identifier: GPL-2.0-or-later
// Explicit, original-synthetic-data oracle. Never called by normal emulation.
#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>
#include "common/scope_exit.h"
#include "core/memory.h"
#include "video_core/command_processor.h"
#include "video_core/lut_effective_change.h"
#include "video_core/regs.h"
#include "video_core/renderer_opengl/gl_rasterizer.h"
#include "video_core/renderer_opengl/gl_state.h"
#include "video_core/renderer_opengl/gl_vars.h"
#include "video_core/video_core.h"

namespace OpenGL {
bool RasterizerOpenGL::RunLUTEffectiveChangeSelfTest(std::string& error) {
#ifndef CARA_IOS_GLES
    error = "LUT effective-change oracle requires the production 2D LUT atlas build";
    return false;
#else
    const auto require=[](bool ok,const std::string& why){if(!ok)throw std::runtime_error(why);};
    const auto flag=[](const char* key){const char* v=std::getenv(key);return v&&std::strcmp(v,"1")==0;};
    if (GLES || !flag("CARA_LUT_EFFECTIVE_CHANGE") || !flag("CARA_LUT_EFFECTIVE_CHANGE_PROFILE")) {
        error="oracle requires desktop GL with 2D LUT atlas and both LUT effective-change/profile switches=1";
        return false;
    }
    // Require fresh renderer state, before guest execution; do not borrow a live
    // guest surface range. The parent trigger must run directly after System.Load.
    constexpr u32 scratch_size=64*1024;
    constexpr PAddr scratch=Memory::VRAM_PADDR_END-scratch_size;
    auto* ram=VideoCore::Memory()->GetPhysicalPointer(scratch);
    if(!ram){error="oracle scratch VRAM unavailable";return false;}
    const std::vector<u8> backup(ram,ram+scratch_size);
    auto& p=Pica::g_state;
    const auto saved_regs=p.regs;
    const auto saved_lighting=p.lighting;
    const auto saved_fog=p.fog;
    const auto saved_proc=p.proctex;
    const auto saved_cmd=p.cmd_list;
    const auto saved_blocks=uniform_block_data;
    const auto saved_light_shadow=lighting_lut_data;
    const auto saved_fog_shadow=fog_lut_data;
    const auto saved_noise_shadow=proctex_noise_lut_data;
    const auto saved_map_shadow=proctex_color_map_data;
    const auto saved_alpha_shadow=proctex_alpha_map_data;
    const auto saved_color_shadow=proctex_lut_data;
    const auto saved_diff_shadow=proctex_diff_lut_data;
    const bool saved_shader_dirty=shader_dirty;
    GLint old_read_fbo{},old_pack{},old_alignment{},old_row{},old_skip_rows{},old_skip_pixels{},old_active{};
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&old_read_fbo);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING,&old_pack);glGetIntegerv(GL_PACK_ALIGNMENT,&old_alignment);
    glGetIntegerv(GL_PACK_ROW_LENGTH,&old_row);glGetIntegerv(GL_PACK_SKIP_ROWS,&old_skip_rows);
    glGetIntegerv(GL_PACK_SKIP_PIXELS,&old_skip_pixels);glGetIntegerv(GL_ACTIVE_TEXTURE,&old_active);
    OGLFramebuffer fbo;
    SCOPE_EXIT({
        p.regs=saved_regs;p.lighting=saved_lighting;p.fog=saved_fog;p.proctex=saved_proc;p.cmd_list=saved_cmd;
        uniform_block_data=saved_blocks;lighting_lut_data=saved_light_shadow;fog_lut_data=saved_fog_shadow;
        proctex_noise_lut_data=saved_noise_shadow;proctex_color_map_data=saved_map_shadow;
        proctex_alpha_map_data=saved_alpha_shadow;proctex_lut_data=saved_color_shadow;
        proctex_diff_lut_data=saved_diff_shadow;shader_dirty=saved_shader_dirty;
        std::memcpy(ram,backup.data(),backup.size());
        glBindFramebuffer(GL_READ_FRAMEBUFFER,old_read_fbo);glBindBuffer(GL_PIXEL_PACK_BUFFER,old_pack);
        glPixelStorei(GL_PACK_ALIGNMENT,old_alignment);glPixelStorei(GL_PACK_ROW_LENGTH,old_row);
        glPixelStorei(GL_PACK_SKIP_ROWS,old_skip_rows);glPixelStorei(GL_PACK_SKIP_PIXELS,old_skip_pixels);
        glActiveTexture(old_active);
    });
    try {
        require(glGetError()==GL_NO_ERROR,"incoming GL error");
        const auto zero=[](const auto& table){return std::all_of(table.begin(),table.end(),[](const auto& e){return e.raw==0;});};
        for(unsigned i=0;i<24;++i) {
            require(zero(p.lighting.luts[i]),"requires initial zero lighting state");
            require(uniform_block_data.lighting_lut_dirty[i] &&
                uniform_block_data.light_data.lighting_lut_offset[i/4][i%4]<0,"requires fresh lighting sentinel/dirty");
        }
        require(zero(p.fog.lut)&&zero(p.proctex.noise_table)&&zero(p.proctex.color_map_table)&&
            zero(p.proctex.alpha_map_table)&&zero(p.proctex.color_table)&&zero(p.proctex.color_diff_table),"requires initial zero fog/proctex state");
        require(uniform_block_data.lighting_lut_dirty_any&&uniform_block_data.fog_lut_dirty&&
            uniform_block_data.proctex_noise_lut_dirty&&uniform_block_data.proctex_color_map_dirty&&
            uniform_block_data.proctex_alpha_map_dirty&&uniform_block_data.proctex_lut_dirty&&
            uniform_block_data.proctex_diff_lut_dirty,"requires all initial dirty flags");
        require(uniform_block_data.data.fog_lut_offset<0&&uniform_block_data.data.proctex_noise_lut_offset<0&&
            uniform_block_data.data.proctex_color_map_offset<0&&uniform_block_data.data.proctex_alpha_map_offset<0&&
            uniform_block_data.data.proctex_lut_offset<0&&uniform_block_data.data.proctex_diff_lut_offset<0,"requires initial negative offsets");
        fbo.Create();
        glBindBuffer(GL_PIXEL_PACK_BUFFER,0);glPixelStorei(GL_PACK_ALIGNMENT,1);
        glPixelStorei(GL_PACK_ROW_LENGTH,0);glPixelStorei(GL_PACK_SKIP_ROWS,0);glPixelStorei(GL_PACK_SKIP_PIXELS,0);
        std::uint64_t checked=0,logical_writes=0;
        // Each pair is a real raw PICA value/header, not a mock register callback.
        const auto submit=[&](std::initializer_list<std::pair<u32,u32>> writes,unsigned last_mask=15) {
            std::vector<u32> words;words.reserve(writes.size()*2);
            for(const auto& [id,value]:writes){const unsigned mask=words.size()+2==writes.size()*2?last_mask:15u;words.push_back(value);words.push_back(id|(mask<<16));}
            require(words.size()*4<=scratch_size,"scratch command overflow");
            std::memcpy(ram,words.data(),words.size()*4);
            Pica::CommandProcessor::ProcessCommandList(scratch,static_cast<u32>(words.size()*4));
        };
        const auto check_counter=[&](Cara::LutEffectiveChange::Kind kind,const auto& before,bool changed,bool valid=true) {
            const auto now=Cara::LutEffectiveChange::ReadCounters();const auto k=static_cast<std::size_t>(kind);
            require(now[k].writes==before[k].writes+1&&now[k].changed==before[k].changed+unsigned(changed)&&
                now[k].notifications==before[k].notifications+unsigned(changed||!valid)&&
                now[k].invalid_targets==before[k].invalid_targets+unsigned(!valid),"actual command-port counters");
            ++logical_writes;
        };
        const auto light=[&](unsigned type,unsigned index,unsigned port,u32 value,unsigned mask=15) {
            const bool changed=p.lighting.luts[type][index].raw!=value;
            const auto before=Cara::LutEffectiveChange::ReadCounters();
            submit({{PICA_REG_INDEX(lighting.lut_config),index|(type<<8)},
                    {PICA_REG_INDEX(lighting.lut_data[0])+port,value}},mask);
            require(p.lighting.luts[type][index].raw==value&&p.regs.lighting.lut_config.index==((index+1)&255),"lighting actual target/index");
            check_counter(Cara::LutEffectiveChange::Kind::Lighting,before,changed);
        };
        const auto fog=[&](unsigned index,unsigned port,u32 value) {
            const bool changed=p.fog.lut[index%128].raw!=value;
            const auto before=Cara::LutEffectiveChange::ReadCounters();
            submit({{PICA_REG_INDEX(texturing.fog_lut_offset),index},
                    {PICA_REG_INDEX(texturing.fog_lut_data[0])+port,value}});
            require(p.fog.lut[index%128].raw==value&&p.regs.texturing.fog_lut_offset==((index+1)&65535),"fog actual target/index");
            check_counter(Cara::LutEffectiveChange::Kind::Fog,before,changed);
        };
        const auto proc_raw=[&](unsigned table,unsigned index)->u32& {
            switch(table){case 0:return p.proctex.noise_table[index%128].raw;
            case 2:return p.proctex.color_map_table[index%128].raw;
            case 3:return p.proctex.alpha_map_table[index%128].raw;
            case 4:return p.proctex.color_table[index%256].raw;
            default:return p.proctex.color_diff_table[index%256].raw;}
        };
        const auto proc=[&](unsigned table,unsigned index,unsigned port,u32 value,bool valid=true) {
            const bool changed=valid&&proc_raw(table,index)!=value;
            const auto before=Cara::LutEffectiveChange::ReadCounters();
            submit({{PICA_REG_INDEX(texturing.proctex_lut_config),index|(table<<8)},
                    {PICA_REG_INDEX(texturing.proctex_lut_data[0])+port,value}});
            require(p.regs.texturing.proctex_lut_config.index==((index+1)&255),"proctex actual index");
            if(valid)require(proc_raw(table,index)==value,"proctex actual target");
            check_counter(Cara::LutEffectiveChange::Kind::ProcTex,before,changed,valid);
        };
        const auto clean=[&] {return !uniform_block_data.lighting_lut_dirty_any&&!uniform_block_data.fog_lut_dirty&&
            !uniform_block_data.proctex_noise_lut_dirty&&!uniform_block_data.proctex_color_map_dirty&&
            !uniform_block_data.proctex_alpha_map_dirty&&!uniform_block_data.proctex_lut_dirty&&!uniform_block_data.proctex_diff_lut_dirty;};
        // First equal-zero writes use all eight actual ports and wrap all index
        // fields. They must not destroy the constructor's first-upload dirty bits.
        for(unsigned table=0;table<24;++table)for(unsigned port=0;port<8;++port)light(table,(255+port)&255,port,0);
        for(unsigned port=0;port<8;++port)fog((65535+port)&65535,port,0);
        for(unsigned table:{0u,2u,3u,4u,5u})for(unsigned port=0;port<8;++port)proc(table,(255+port)&255,port,0);
        require(!clean(),"initial zero writes cleared constructor dirty");
        SyncAndUploadLUTsLF();SyncAndUploadLUTs();require(clean(),"initial uploads did not clear dirty");
        // Direct readback of the actual production textures at actual uniform
        // offsets. Query width rather than assuming the atlas implementation's 1024.
        const auto verify=[&](GLuint texture,GLint offset,unsigned count,unsigned components,const auto& expected) {
            require(offset>=0,"negative uploaded offset");
            GLint active{},binding{},width{},height{},read_fbo{};
            glGetIntegerv(GL_ACTIVE_TEXTURE,&active);glActiveTexture(GL_TEXTURE0);
            glGetIntegerv(GL_TEXTURE_BINDING_2D,&binding);glBindTexture(GL_TEXTURE_2D,texture);
            glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_WIDTH,&width);
            glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_HEIGHT,&height);
            glBindTexture(GL_TEXTURE_2D,binding);glActiveTexture(active);
            require(width>0&&height>0&&std::uint64_t(offset)+count<=std::uint64_t(width)*height,"atlas bounds");
            glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&read_fbo);glBindFramebuffer(GL_READ_FRAMEBUFFER,fbo.handle);
            glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
            require(glCheckFramebufferStatus(GL_READ_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"LUT atlas not float-readable FBO");
            glReadBuffer(GL_COLOR_ATTACHMENT0);
            for(unsigned i=0;i<count;) {
                const unsigned at=unsigned(offset)+i,x=at%unsigned(width),y=at/unsigned(width);
                const unsigned n=std::min(count-i,unsigned(width)-x);
                std::vector<float> actual(n*4);
                glReadPixels(x,y,n,1,GL_RGBA,GL_FLOAT,actual.data());
                require(glGetError()==GL_NO_ERROR,"LUT float GPU readback error");
                for(unsigned j=0;j<n;++j){const auto want=expected(i+j);
                    for(unsigned c=0;c<components;++c){require(std::bit_cast<u32>(actual[j*4+c])==std::bit_cast<u32>(want[c]),"actual LUT GPU texel mismatch");++checked;}}
                i+=n;
            }
            glBindFramebuffer(GL_READ_FRAMEBUFFER,read_fbo);
        };
        const auto offsets=[&] {
            std::vector<std::pair<std::size_t,std::size_t>> lf,pt;
            for(unsigned t=0;t<24;++t){const auto o=uniform_block_data.light_data.lighting_lut_offset[t/4][t%4];require(o>=0,"lighting negative offset");lf.emplace_back(std::size_t(o)*8,std::size_t(o)*8+2048);}
            const auto f=uniform_block_data.data.fog_lut_offset;require(f>=0,"fog negative offset");lf.emplace_back(std::size_t(f)*8,std::size_t(f)*8+1024);
            const auto& d=uniform_block_data.data;
            for(auto o:{d.proctex_noise_lut_offset,d.proctex_color_map_offset,d.proctex_alpha_map_offset}){require(o>=0,"proctex RG negative offset");pt.emplace_back(std::size_t(o)*8,std::size_t(o)*8+1024);}
            for(auto o:{d.proctex_lut_offset,d.proctex_diff_lut_offset}){require(o>=0,"proctex RGBA negative offset");pt.emplace_back(std::size_t(o)*16,std::size_t(o)*16+4096);}
            const auto disjoint=[&](auto ranges,std::size_t capacity){std::sort(ranges.begin(),ranges.end());for(unsigned i=0;i<ranges.size();++i){require(ranges[i].second<=capacity,"LUT range outside ring");if(i)require(ranges[i-1].second<=ranges[i].first,"different LUT ranges overlap");}};
            disjoint(lf,texture_lf_buffer.GetSize());disjoint(pt,texture_buffer.GetSize());
        };
        const auto verify_all=[&] {
            offsets();
            for(unsigned t=0;t<24;++t)verify(texture_buffer_lut_lf.handle,uniform_block_data.light_data.lighting_lut_offset[t/4][t%4],256,2,[&](unsigned i){const auto& e=p.lighting.luts[t][i];return std::array<float,4>{e.ToFloat(),e.DiffToFloat(),0,1};});
            verify(texture_buffer_lut_lf.handle,uniform_block_data.data.fog_lut_offset,128,2,[&](unsigned i){const auto& e=p.fog.lut[i];return std::array<float,4>{e.ToFloat(),e.DiffToFloat(),0,1};});
            const auto& d=uniform_block_data.data;
            const auto values=[&](const auto& table,GLint at){verify(texture_buffer_lut_rg.handle,at,128,2,[&](unsigned i){const auto& e=table[i];return std::array<float,4>{e.ToFloat(),e.DiffToFloat(),0,1};});};
            values(p.proctex.noise_table,d.proctex_noise_lut_offset);values(p.proctex.color_map_table,d.proctex_color_map_offset);values(p.proctex.alpha_map_table,d.proctex_alpha_map_offset);
            const auto colors=[&](const auto& table,GLint at){verify(texture_buffer_lut_rgba.handle,at,256,4,[&](unsigned i){const auto& e=table[i];return std::array<float,4>{static_cast<u8>(e.r)/255.f,static_cast<u8>(e.g)/255.f,static_cast<u8>(e.b)/255.f,static_cast<u8>(e.a)/255.f};});};
            colors(p.proctex.color_table,d.proctex_lut_offset);colors(p.proctex.color_diff_table,d.proctex_diff_lut_offset);
        };
        verify_all(); // Actual first all-zero GPU data, not an unused shader path.
        light(0,0,0,0x00812567);light(0,1,0,0x00812567); // Same port, different target.
        light(0,2,3,0x00000456,0); // Zero backing mask still has raw stream effect.
        fog(127,7,0x00812345);
        for(unsigned table:{0u,2u,3u,4u,5u})proc(table,255,table&7,0x81234567);
        SyncAndUploadLUTsLF();SyncAndUploadLUTs();verify_all();
        require(clean(),"nonzero upload left dirty");
        light(0,0,7,0x00812567);fog(127,0,0x00812345);
        for(unsigned table:{0u,2u,3u,4u,5u})proc(table,255,(table+1)&7,0x81234567);
        require(clean(),"equal actual target replay dirtied tables");
        SyncAndUploadLUTsLF();SyncAndUploadLUTs();verify_all();
        proc(1,255,0,0x76543210,false);require(clean(),"invalid selector changed LUT dirty state");
        // Change one table until the real LF ring wraps. Wrap must relocate and
        // reupload every referenced table, including unchanged all-zero tables.
        bool wrapped=false;GLint previous=uniform_block_data.light_data.lighting_lut_offset[0][0];
        const auto limit=std::size_t(texture_lf_buffer.GetSize())/2048+64;
        require(limit<16384,"unexpected ring size; refuse unbounded fixture");
        for(std::size_t n=0;n<limit;++n){light(0,0,unsigned(n&7),n&1?0x101:0x202);SyncAndUploadLUTsLF();
            const auto current=uniform_block_data.light_data.lighting_lut_offset[0][0];
            if(current<previous){wrapped=true;break;}previous=current;}
        require(wrapped,"LF ring wrap not observed");verify_all();
        require(clean()&&glGetError()==GL_NO_ERROR,"final LUT/GL state error");
        std::cout<<"PASS actual PICA LUT ports; first-zero/nonzero/replay/LF-ring-wrap GPU components="<<checked
                 <<" logical_port_writes="<<logical_writes<<"; no lighting-render/save-load claim\n";
        return true;
    } catch(const std::exception& e){error=e.what();return false;}
#endif
}
} // namespace OpenGL
