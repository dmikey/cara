// SPDX-License-Identifier: GPL-2.0-or-later
// Actual generated atlas shaders, independent original-PICA plane and pixel answers.
#ifndef CARA_MAC_ATLAS_PIPELINE
#error This oracle requires the native Mac atlas prototype
#endif
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include <iostream>
#include <nihstro/inline_assembly.h>
#include "common/scope_exit.h"
#include "core/settings.h"
#include "video_core/regs.h"
#include "video_core/renderer_opengl/gl_resource_manager.h"
#include "video_core/renderer_opengl/gl_shader_gen.h"
#include "video_core/renderer_opengl/gl_shader_manager.h"
#include "video_core/renderer_opengl/gl_state.h"

namespace {
using namespace OpenGL;
using Vec = std::array<float,4>;
constexpr unsigned Side=64;
void Check(bool value,const std::string& why) {if(!value) throw std::runtime_error(why);}
float Plane(const Vec& p,const Vec& c) {return p[0]*c[0]+p[1]*c[1]+p[2]*c[2]+p[3]*c[3];}
std::string ProgramLog(GLuint program) {
    GLint size{};glGetProgramiv(program,GL_INFO_LOG_LENGTH,&size);
    std::string text(size>0?size:1,'\0');glGetProgramInfoLog(program,text.size(),nullptr,text.data());return text;
}
struct Program {
    GLuint handle{};
    Program(const std::string& source,bool producer) {
        OGLShader vs,fs;vs.Create(source.c_str(),GL_VERTEX_SHADER);
        fs.Create("out vec4 color; void main(){color=vec4(1.0);}",GL_FRAGMENT_SHADER);
        Check(vs.handle && fs.handle,"clip shader compilation failed");
        handle=glCreateProgram();glAttachShader(handle,vs.handle);glAttachShader(handle,fs.handle);
        const char* planes[]={"gl_Position","gl_ClipDistance[0]","gl_ClipDistance[1]"};
        const char* fields[]={"cara_feedback0","cara_feedback1","cara_feedback2","cara_feedback3","cara_feedback4","cara_feedback5"};
        glTransformFeedbackVaryings(handle,producer?6:3,producer?fields:planes,GL_INTERLEAVED_ATTRIBS);
        glLinkProgram(handle);GLint linked{};glGetProgramiv(handle,GL_LINK_STATUS,&linked);
        if(!linked) {const auto log=ProgramLog(handle);glDeleteProgram(handle);handle=0;throw std::runtime_error("clip TF link failed: "+log);}
        const auto block=glGetUniformBlockIndex(handle,"shader_data");
        if(block!=GL_INVALID_INDEX)glUniformBlockBinding(handle,block,1);
    }
    ~Program(){if(handle)glDeleteProgram(handle);}
    Program(const Program&)=delete;
};
void Sampler(GLuint program,const char* name,unsigned unit) {
    const auto location=glGetUniformLocation(program,name);if(location>=0)glUniform1i(location,unit);
}
}
bool RunMacAtlasClipSelfTest(std::string& error) {
    using namespace OpenGL;
    try {
        Check(!Settings::values.disable_clip_coef,"clip oracle requires clip coefficients enabled");
        using S=nihstro::SourceRegister;using D=nihstro::DestRegister;using O=nihstro::OpCode;
        using Semantic=Pica::RasterizerRegs::VSOutputAttributes;
        const auto binary=nihstro::InlineAsm::CompileToRawBinary({
            {O::Id::MOV,D::MakeOutput(0),S::MakeInput(0)},{O::Id::END}});
        Pica::Shader::ShaderSetup setup;
        std::transform(binary.program.begin(),binary.program.end(),setup.program_code.begin(),[](auto instruction){return instruction.hex;});
        setup.MarkProgramCodeDirty();
        std::transform(binary.swizzle_table.begin(),binary.swizzle_table.end(),setup.swizzle_data.begin(),[](auto s){return s.hex;});
        setup.MarkSwizzleDataDirty();
        Pica::Regs regs{};regs.vs.output_mask.Assign(1);regs.vs.max_input_attribute_index.Assign(0);
        regs.rasterizer.vs_output_total.Assign(1);
        auto& mapping=regs.rasterizer.vs_output_attributes[0];
        mapping.map_x.Assign(Semantic::POSITION_X);mapping.map_y.Assign(Semantic::POSITION_Y);
        mapping.map_z.Assign(Semantic::POSITION_Z);mapping.map_w.Assign(Semantic::POSITION_W);
        const PicaVSConfig config(regs,setup);const PicaFixedGSConfig fixed(regs);
        const auto previous=OpenGLState::GetCurState();
        OGLFramebuffer fbo;OGLTexture color,inputs,indices,outputs;OGLVertexArray vao;
        OGLBuffer raster,feedback;fbo.Create();color.Create();inputs.Create();indices.Create();outputs.Create();vao.Create();raster.Create();feedback.Create();
        SCOPE_EXIT({glDisable(GL_RASTERIZER_DISCARD);glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER,0,0);glBindBuffer(GL_PIXEL_UNPACK_BUFFER,0);previous.Apply();});
        OpenGLState::BindTexture2D(0,color.handle);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,Side,Side,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
        OpenGLState::BindDrawFramebuffer(fbo.handle);OpenGLState::BindReadFramebuffer(fbo.handle);
        glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,color.handle,0);
        Check(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"clip framebuffer incomplete");
        std::uint64_t vertices_checked{},pixels_checked{},feedback_draws{};
        const std::array<Vec,2> coefficients{{{1,.5f,.25f,-.125f},{-.5f,1,-.25f,-.125f}}};
        const std::array<std::array<Vec,3>,2> positions{{
            {{{-1,-1,-.5f,1},{3,-1,-.5f,1},{-1,3,-.5f,1}}},
            {{{-515.f/512,-.25f,-.5f,2},{.25f,-.75f,-.25f,1},{.5f,.25f,.125f,1}}}}};
        for(unsigned mode:{0u,6u,5u}) {
            Program draw(GenerateVertexShader(setup,config,false,&fixed,1,mode),false);
            Program producer(GenerateVertexShader(setup,config,false,&fixed,1,4),true);
            for(unsigned geometry=0;geometry<positions.size();++geometry) {
                // Upload once; the two following draws change ONLY clip_coef.
                std::vector<Vec> data(128);for(unsigned v=0;v<3;++v)data[v]=positions[geometry][v];
                glActiveTexture(GL_TEXTURE7);glBindTexture(GL_TEXTURE_2D,inputs.handle);glBindSampler(7,0);
                glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
                glTexImage2D(GL_TEXTURE_2D,0,mode==0?GL_RGBA32F:GL_RGBA32UI,128,1,0,mode==0?GL_RGBA:GL_RGBA_INTEGER,mode==0?GL_FLOAT:GL_UNSIGNED_INT,data.data());
                std::array<u16,256> ids{};for(unsigned v=0;v<3;++v){if(mode==6)ids[v*2]=ids[v*2+1]=v;else ids[v]=v;}
                glActiveTexture(GL_TEXTURE8);glBindTexture(GL_TEXTURE_2D,indices.handle);glBindSampler(8,0);
                glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
                glTexImage2D(GL_TEXTURE_2D,0,mode==6?GL_RG16UI:GL_R16UI,128,1,0,mode==6?GL_RG_INTEGER:GL_RED_INTEGER,GL_UNSIGNED_SHORT,ids.data());
                glActiveTexture(GL_TEXTURE9);glBindTexture(GL_TEXTURE_2D,outputs.handle);glBindSampler(9,0);
                glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
                glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA32F,128,1,0,GL_RGBA,GL_FLOAT,nullptr);
                const auto bind_program=[&](GLuint program) {
                    auto state=OpenGLState::GetCurState();state.draw.shader_program=program;state.draw.vertex_array=vao.handle;
                    state.draw.draw_framebuffer=state.draw.read_framebuffer=fbo.handle;state.viewport={0,0,Side,Side};
                    state.clip_distance[0]=state.clip_distance[1]=true;state.cull.enabled=false;state.depth.test_enabled=false;state.depth.write_mask=false;
                    state.stencil.test_enabled=false;state.blend.enabled=false;state.scissor.enabled=false;state.Apply();
                    Check(glIsEnabled(GL_CLIP_DISTANCE0) && glIsEnabled(GL_CLIP_DISTANCE1),"both actual clip planes must be enabled");
                    Sampler(program,"cara_vertex_inputs",7);Sampler(program,"cara_vertex_indices",8);Sampler(program,"cara_vertex_outputs",9);
                    std::array<std::array<GLint,4>,16> layout{};layout[0]={0,16,3,4};
                    const auto location=glGetUniformLocation(program,"cara_vertex_layout[0]");if(location>=0)glUniform4iv(location,16,layout[0].data());
                };
                for(const auto& coefficient:coefficients) {
                    UniformData uniforms{};uniforms.cara_viewport={0,0,32,32};
                    std::copy(coefficient.begin(),coefficient.end(),uniforms.clip_coef.begin());
                    glBindBuffer(GL_UNIFORM_BUFFER,raster.handle);glBufferData(GL_UNIFORM_BUFFER,sizeof(uniforms),&uniforms,GL_STREAM_DRAW);glBindBufferBase(GL_UNIFORM_BUFFER,1,raster.handle);
                    if(mode==5) {
                        bind_program(producer.handle);glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER,feedback.handle);
                        glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER,128*sizeof(Vec),nullptr,GL_STREAM_COPY);glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER,0,feedback.handle);
                        glEnable(GL_RASTERIZER_DISCARD);glBeginTransformFeedback(GL_POINTS);glDrawArrays(GL_POINTS,0,3);glEndTransformFeedback();glDisable(GL_RASTERIZER_DISCARD);
                        const auto* raw=static_cast<const float*>(glMapBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER,0,72*sizeof(float),GL_MAP_READ_BIT));Check(raw,"clip producer map failed");
                        bool matches=true;for(unsigned v=0;v<3;++v)matches&=raw[v*24+19]==-positions[geometry][v][2] && raw[v*24+23]==Plane(positions[geometry][v],coefficient);
                        Check(glUnmapBuffer(GL_TRANSFORM_FEEDBACK_BUFFER)==GL_TRUE,"clip producer corrupted");Check(matches,"clip producer spare-W distances differ from original PICA planes");
                        glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER,0,0);glBindBuffer(GL_PIXEL_UNPACK_BUFFER,feedback.handle);
                        glActiveTexture(GL_TEXTURE9);glBindTexture(GL_TEXTURE_2D,outputs.handle);glTexSubImage2D(GL_TEXTURE_2D,0,0,0,128,1,GL_RGBA,GL_FLOAT,nullptr);glBindBuffer(GL_PIXEL_UNPACK_BUFFER,0);++feedback_draws;
                    }
                    bind_program(draw.handle);glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER,feedback.handle);
                    glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER,18*sizeof(float),nullptr,GL_STREAM_READ);glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER,0,feedback.handle);
                    glEnable(GL_RASTERIZER_DISCARD);glBeginTransformFeedback(GL_TRIANGLES);glDrawArrays(GL_TRIANGLES,0,3);glEndTransformFeedback();glDisable(GL_RASTERIZER_DISCARD);
                    const auto* raw=static_cast<const float*>(glMapBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER,0,18*sizeof(float),GL_MAP_READ_BIT));Check(raw,"clip TF map failed");
                    bool matches=true;for(unsigned v=0;v<3;++v)matches&=raw[v*6+4]==-positions[geometry][v][2] && raw[v*6+5]==Plane(positions[geometry][v],coefficient);
                    Check(glUnmapBuffer(GL_TRANSFORM_FEEDBACK_BUFFER)==GL_TRUE,"clip TF corrupted");Check(matches,"clip TF distances differ from original PICA planes mode="+std::to_string(mode));vertices_checked+=3;
                    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER,0,0);
                    if(geometry==0) {
                        glClearColor(0,0,0,0);glClear(GL_COLOR_BUFFER_BIT);glDrawArrays(GL_TRIANGLES,0,3);
                        std::vector<u8> pixels(Side*Side*4);glBindBuffer(GL_PIXEL_PACK_BUFFER,0);glReadPixels(0,0,Side,Side,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
                        for(unsigned y=0;y<Side;++y)for(unsigned x=0;x<Side;++x) {
                            const Vec p={2.f*(x+.5f)/Side-1,2.f*(y+.5f)/Side-1,-.5f,1};const u8 expected=Plane(p,coefficient)>=0?255:0;
                            for(unsigned c=0;c<4;++c)Check(pixels[(y*Side+x)*4+c]==expected,"clip coverage mismatch mode="+std::to_string(mode)+" pixel="+std::to_string(x)+","+std::to_string(y));++pixels_checked;
                        }
                    }
                    Check(glGetError()==GL_NO_ERROR,"clip fixture GL error mode="+std::to_string(mode));
                }
            }
        }
        std::cout<<"CLIP_PLANES vertices="<<vertices_checked<<" pixels="<<pixels_checked<<" feedback_draws="<<feedback_draws<<" both planes/custom coefficient-only changes/original position before remap+snap passed\n";
        return true;
    } catch(const std::exception& failure) {error=failure.what();return false;}
}
