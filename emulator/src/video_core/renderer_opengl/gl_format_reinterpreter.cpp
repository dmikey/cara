// Copyright 2020 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/assert.h"
#include <cstdlib>
#include <cstring>
#include <limits>
#include "common/scope_exit.h"
#include "video_core/renderer_opengl/gl_format_reinterpreter.h"
#include "video_core/renderer_opengl/gl_rasterizer_cache.h"
#include "video_core/renderer_opengl/gl_state.h"
#include "video_core/renderer_opengl/gl_vars.h"

namespace OpenGL {

using PixelFormat = SurfaceParams::PixelFormat;

#if defined(CARA_IOS_GLES) || defined(__APPLE__)
#ifdef CARA_IOS_GLES
static constexpr const char* depth_shader_precision=fragment_shader_precision_OES;
#else
// GLSL330 accepts highp qualifiers but has no uimage2D type in the ES preamble.
static constexpr const char* depth_shader_precision="";
#endif
// GLES 3.0 samples the depth component of D24S8 but cannot sample stencil.
// Recover stencil using read-only fixed-function tests, never a feedback loop.
#if defined(__APPLE__) && !defined(CARA_IOS_GLES)
static bool MacGPUReinterpretEnabled() {
    static const bool enabled=[] {
        const char* value=std::getenv("CARA_MAC_GPU_DEPTH_REINTERPRET");
        return value && std::strcmp(value,"1")==0;
    }();
    return enabled;
}
// These desktop-only states can be changed by a raw caller independently of
// the cache. Keep the converter deterministic without changing that caller.
class DesktopDepthRasterState {
public:
    DesktopDepthRasterState() {
        glGetIntegerv(GL_LOGIC_OP_MODE,&logic);
        for(unsigned i=0;i<3;++i){enabled[i]=glIsEnabled(caps[i]);glDisable(caps[i]);}
        glLogicOp(GL_COPY);
    }
    ~DesktopDepthRasterState() {
        glLogicOp(logic);
        for(unsigned i=0;i<3;++i)if(enabled[i])glEnable(caps[i]);else glDisable(caps[i]);
    }
private:
    static constexpr GLenum caps[]={GL_COLOR_LOGIC_OP,GL_CLIP_DISTANCE0,GL_CLIP_DISTANCE1};
    GLint logic=GL_COPY;GLboolean enabled[3]{};
};
static bool CompareNativeDepthUploadWithPBO(std::string& error);
#endif

class GLESDepthStencilToRGBA8 final : public FormatReinterpreterBase {
public:
    GLESDepthStencilToRGBA8() {
        constexpr const char* vs = R"(
const vec2 p[4]=vec2[4](vec2(-1,-1),vec2(1,-1),vec2(-1,1),vec2(1,1));
void main(){gl_Position=vec4(p[gl_VertexID],0,1);}
)";
        const std::string depth_fs = std::string(depth_shader_precision) + R"(
uniform highp sampler2D source;
uniform highp uint maximum_depth;
out highp vec4 color;
void main(){
    highp float z=texelFetch(source,ivec2(gl_FragCoord.xy),0).r;
    highp uint bits;
    if(maximum_depth==65535u)bits=uint(roundEven(z*65535.0));
    else{
        highp uint ieee=floatBitsToUint(z);
        if(ieee==0u)bits=0u;
        else if(ieee>=0x3f800000u)bits=16777215u;
        else{
            highp uint exponent=(ieee>>23u)&255u;
            highp uint mantissa=(ieee&0x7fffffu)|0x800000u;
            bits=(mantissa-1u)>>(126u-exponent);
        }
    }
    color=vec4(0.0,float((bits>>16u)&255u),float((bits>>8u)&255u),float(bits&255u))/255.0;
}
)";
        const std::string stencil_fs = std::string(depth_shader_precision) + R"(
uniform highp float bit_value;
out highp vec4 color;
void main(){color=vec4(bit_value,0,0,0);}
)";
        const std::string upload_depth_fs=std::string(depth_shader_precision)+R"(
uniform highp usampler2D source;
uniform highp ivec2 origin;
uniform highp uint depth_bits;
void main(){
    highp uvec4 bytes=texelFetch(source,ivec2(gl_FragCoord.xy)-origin,0);
    highp uint k=bytes.g|(bytes.b<<8u)|(bytes.a<<16u);
    if(depth_bits==16u){gl_FragDepth=float(k&65535u)/65535.0;return;}
    if(k==0u){gl_FragDepth=0.0;return;}
    // float(k) is exact for every24bit integer. Construct the correctly rounded
    // normalized IEEE value using integer operations, avoiding reciprocal math.
    highp uint p=(floatBitsToUint(float(k))>>23u)-127u;
    gl_FragDepth=uintBitsToFloat(((p+102u)<<23u)+(k<<(23u-p))+1u);
}
)";
        const std::string upload_stencil_fs=std::string(depth_shader_precision)+R"(
uniform highp usampler2D source;
uniform highp ivec2 origin;
uniform highp uint bit_mask;
void main(){
    if((texelFetch(source,ivec2(gl_FragCoord.xy)-origin,0).r&bit_mask)==0u)discard;
}
)";
        const auto old=OpenGLState::GetCurState();
        GLint active=0;glGetIntegerv(GL_ACTIVE_TEXTURE,&active);
        depth_program.Create(vs,depth_fs.c_str());
        stencil_program.Create(vs,stencil_fs.c_str());
        upload_depth_program.Create(vs,upload_depth_fs.c_str());
        upload_stencil_program.Create(vs,upload_stencil_fs.c_str());
        vao.Create();sample.Create();scratch.Create();work_fb.Create();copy_fb.Create();
        upload_input.Create();upload_fb.Create();
        OpenGLState::BindShaderProgram(depth_program.handle);
        glUniform1i(glGetUniformLocation(depth_program.handle,"source"),0);
        maximum_depth_location=glGetUniformLocation(depth_program.handle,"maximum_depth");
        bit_location=glGetUniformLocation(stencil_program.handle,"bit_value");
        glSamplerParameteri(sample.handle,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        glSamplerParameteri(sample.handle,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glSamplerParameteri(sample.handle,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
        glSamplerParameteri(sample.handle,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        glSamplerParameteri(sample.handle,GL_TEXTURE_COMPARE_MODE,GL_NONE);
        // Initialization changes only the current program. Reapplying raster
        // state here would overwrite raw caller state during lazy CPU creation.
        OpenGLState::BindShaderProgram(old.draw.shader_program);glActiveTexture(active);
    }
    void Reinterpret(GLuint, const Common::Rectangle<u32>&, GLuint, GLuint,
                     const Common::Rectangle<u32>&, GLuint) override {
        // This implementation requires explicit source extent. The cache calls
        // ReinterpretChecked and handles failure without validating the alias.
    }
    bool ReinterpretChecked(GLuint src, const Common::Rectangle<u32>& sr, GLuint,
                            GLuint dst, const Common::Rectangle<u32>& dr, GLuint,
                            u32 width,u32 height) override {
        return EncodeDepthChecked(src,PixelFormat::D24S8,sr,dst,dr,width,height);
    }
    bool EncodeDepthChecked(GLuint src,PixelFormat format,const Common::Rectangle<u32>& sr,
                            GLuint dst,const Common::Rectangle<u32>& dr,u32 width,u32 height) override {
        if(format!=PixelFormat::D16&&format!=PixelFormat::D24&&format!=PixelFormat::D24S8)return false;
        if (!src || !dst || src==dst || !width || !height || sr.right>width || sr.top>height ||
            sr.right<=sr.left || sr.top<=sr.bottom || dr.right<=dr.left || dr.top<=dr.bottom)
            return false;
        GLint limit=0;glGetIntegerv(GL_MAX_TEXTURE_SIZE,&limit);
        if(width>u32(limit)||height>u32(limit))return false;
        GLint linked=0;glGetProgramiv(depth_program.handle,GL_LINK_STATUS,&linked);
        if(!linked)return false;
        glGetProgramiv(stencil_program.handle,GL_LINK_STATUS,&linked);if(!linked)return false;
        const auto previous=OpenGLState::GetCurState();
#if defined(__APPLE__) && !defined(CARA_IOS_GLES)
        DesktopDepthRasterState desktop_raster;
#endif
        GLint active=0,unpack=0,read_framebuffer=0,draw_framebuffer=0;
        glGetIntegerv(GL_ACTIVE_TEXTURE,&active);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&read_framebuffer);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&draw_framebuffer);
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING,&unpack);
        // The state cache normally uses symmetric stencil state. Preserve raw
        // front/back state too, including callers using separate-face APIs.
        constexpr GLenum stencil_names[2][7]={
            {GL_STENCIL_FUNC,GL_STENCIL_REF,GL_STENCIL_VALUE_MASK,GL_STENCIL_WRITEMASK,
             GL_STENCIL_FAIL,GL_STENCIL_PASS_DEPTH_FAIL,GL_STENCIL_PASS_DEPTH_PASS},
            {GL_STENCIL_BACK_FUNC,GL_STENCIL_BACK_REF,GL_STENCIL_BACK_VALUE_MASK,GL_STENCIL_BACK_WRITEMASK,
             GL_STENCIL_BACK_FAIL,GL_STENCIL_BACK_PASS_DEPTH_FAIL,GL_STENCIL_BACK_PASS_DEPTH_PASS}};
        GLint stencil_values[2][7];
        for(unsigned face=0;face<2;++face)for(unsigned i=0;i<7;++i)
            glGetIntegerv(stencil_names[face][i],&stencil_values[face][i]);
        constexpr GLenum capabilities[]={GL_DITHER,GL_RASTERIZER_DISCARD,
            GL_SAMPLE_ALPHA_TO_COVERAGE,GL_SAMPLE_COVERAGE,GL_POLYGON_OFFSET_FILL};
        GLboolean enabled[5];
        for(unsigned i=0;i<5;++i){enabled[i]=glIsEnabled(capabilities[i]);glDisable(capabilities[i]);}
        SCOPE_EXIT({
            // Own framebuffers are detached before returning, so they retain no
            // references to source/destination resources after this operation.
            OpenGLState::BindDrawFramebuffer(work_fb.handle);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_DEPTH_STENCIL_ATTACHMENT,GL_TEXTURE_2D,0,0);
            OpenGLState::BindDrawFramebuffer(copy_fb.handle);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,0,0);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER,unpack);
            previous.Apply();
            // Some startup paths bind framebuffers directly, outside the cache.
            // Restore the actual entry bindings and reconcile the cache with them.
            OpenGLState::BindReadFramebuffer(read_framebuffer);
            OpenGLState::BindDrawFramebuffer(draw_framebuffer);
            for(unsigned face=0;face<2;++face){
                const auto* values=stencil_values[face];const GLenum side=face?GL_BACK:GL_FRONT;
                glStencilFuncSeparate(side,values[0],values[1],values[2]);
                glStencilMaskSeparate(side,values[3]);
                glStencilOpSeparate(side,values[4],values[5],values[6]);
            }
            OpenGLState::InvalidateRasterState();
            for(unsigned i=0;i<5;++i)if(enabled[i])glEnable(capabilities[i]);else glDisable(capabilities[i]);
            glActiveTexture(active);
        });
        OpenGLState state;
        state.cull.enabled=false;state.depth.write_mask=GL_FALSE;
        state.stencil.write_mask=0;
        state.texture_units[0].texture_2d=scratch.handle;
        state.texture_units[0].sampler=sample.handle;
        state.draw.vertex_array=vao.handle;
        state.draw.draw_framebuffer=work_fb.handle;
        state.draw.read_framebuffer=work_fb.handle;
        state.draw.shader_program=depth_program.handle;
        state.viewport={GLint(sr.left),GLint(sr.bottom),GLsizei(sr.GetWidth()),GLsizei(sr.GetHeight())};
        // Raw callers can have changed masks/ops without updating the cache.
        // Force the safe zero-write/KEEP state before ever attaching source DS.
        OpenGLState::InvalidateRasterState();
        state.Apply();
        if(width!=scratch_width||height!=scratch_height){
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER,0);
            glActiveTexture(GL_TEXTURE0);
            glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,width,height,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
            scratch_width=width;scratch_height=height;
        }
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_DEPTH_STENCIL_ATTACHMENT,GL_TEXTURE_2D,0,0);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,scratch.handle,0);
        if(glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)return false;
        // Check the final destination before any destination pixels can change.
        OpenGLState::BindDrawFramebuffer(copy_fb.handle);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,dst,0);
        if(glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)return false;
        state.texture_units[0].texture_2d=src;
        state.Apply();
        glUniform1ui(maximum_depth_location,format==PixelFormat::D16?65535u:16777215u);
        glDrawArrays(GL_TRIANGLE_STRIP,0,4);
        // Stencil pass samples no texture; the source is attached only now.
        if(format==PixelFormat::D24S8){
        state.texture_units[0].texture_2d=0;
        state.draw.shader_program=stencil_program.handle;
        state.stencil.test_enabled=true;state.stencil.test_func=GL_EQUAL;
        state.blend.enabled=true;
        state.blend.src_rgb_func=state.blend.dst_rgb_func=GL_ONE;
        state.blend.src_a_func=state.blend.dst_a_func=GL_ONE;
        state.color_mask.green_enabled=state.color_mask.blue_enabled=state.color_mask.alpha_enabled=GL_FALSE;
        state.Apply();
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_DEPTH_STENCIL_ATTACHMENT,GL_TEXTURE_2D,src,0);
        if(glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)return false;
        for(unsigned bit=0;bit<8;++bit){
            state.stencil.test_ref=state.stencil.test_mask=1u<<bit;state.Apply();
            glUniform1f(bit_location,float(1u<<bit)/255.0f);
            glDrawArrays(GL_TRIANGLE_STRIP,0,4);
        }
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_DEPTH_STENCIL_ATTACHMENT,GL_TEXTURE_2D,0,0);
        }
        OpenGLState::BindReadFramebuffer(work_fb.handle);
        OpenGLState::BindDrawFramebuffer(copy_fb.handle);
        glBlitFramebuffer(sr.left,sr.bottom,sr.right,sr.top,dr.left,dr.bottom,dr.right,dr.top,
                          GL_COLOR_BUFFER_BIT,GL_NEAREST);
        return glGetError()==GL_NO_ERROR;
    }
    bool UploadDepthChecked(GLuint target,PixelFormat format,const Common::Rectangle<u32>& rect,
                            const u8* canonical,size_t canonical_bytes,u32 stride_pixels,
                            u32 width,u32 height) override {
        if(format!=PixelFormat::D16&&format!=PixelFormat::D24&&format!=PixelFormat::D24S8)return false;
        if(!target||!canonical||!width||!height||rect.right>width||rect.top>height||
           rect.right<=rect.left||rect.top<=rect.bottom||stride_pixels<rect.GetWidth()||
           stride_pixels>u32(std::numeric_limits<GLint>::max()))return false;
        const u64 required=(u64(rect.GetHeight()-1)*stride_pixels+rect.GetWidth())*4;
        if(required>canonical_bytes)return false;
        GLint limit=0;glGetIntegerv(GL_MAX_TEXTURE_SIZE,&limit);
        if(width>u32(limit)||height>u32(limit))return false;
        GLint linked=0;glGetProgramiv(upload_depth_program.handle,GL_LINK_STATUS,&linked);if(!linked)return false;
        glGetProgramiv(upload_stencil_program.handle,GL_LINK_STATUS,&linked);if(!linked)return false;
        const auto previous=OpenGLState::GetCurState();
#if defined(__APPLE__) && !defined(CARA_IOS_GLES)
        DesktopDepthRasterState desktop_raster;
#endif
        GLint active=0,unpack=0,read_fb=0,draw_fb=0;
        glGetIntegerv(GL_ACTIVE_TEXTURE,&active);glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING,&unpack);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&read_fb);glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&draw_fb);
        constexpr GLenum pixel_names[]={GL_UNPACK_ALIGNMENT,GL_UNPACK_ROW_LENGTH,GL_UNPACK_SKIP_ROWS,GL_UNPACK_SKIP_PIXELS};
        GLint pixels[4];for(unsigned i=0;i<4;++i)glGetIntegerv(pixel_names[i],&pixels[i]);
        constexpr GLenum stencil_names[2][7]={
            {GL_STENCIL_FUNC,GL_STENCIL_REF,GL_STENCIL_VALUE_MASK,GL_STENCIL_WRITEMASK,
             GL_STENCIL_FAIL,GL_STENCIL_PASS_DEPTH_FAIL,GL_STENCIL_PASS_DEPTH_PASS},
            {GL_STENCIL_BACK_FUNC,GL_STENCIL_BACK_REF,GL_STENCIL_BACK_VALUE_MASK,GL_STENCIL_BACK_WRITEMASK,
             GL_STENCIL_BACK_FAIL,GL_STENCIL_BACK_PASS_DEPTH_FAIL,GL_STENCIL_BACK_PASS_DEPTH_PASS}};
        GLint stencil_values[2][7];for(unsigned face=0;face<2;++face)for(unsigned i=0;i<7;++i)
            glGetIntegerv(stencil_names[face][i],&stencil_values[face][i]);
        constexpr GLenum capabilities[]={GL_DITHER,GL_RASTERIZER_DISCARD,GL_SAMPLE_ALPHA_TO_COVERAGE,
                                         GL_SAMPLE_COVERAGE,GL_POLYGON_OFFSET_FILL};
        GLboolean enabled[5];for(unsigned i=0;i<5;++i){enabled[i]=glIsEnabled(capabilities[i]);glDisable(capabilities[i]);}
        SCOPE_EXIT({
            OpenGLState::BindDrawFramebuffer(upload_fb.handle);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_DEPTH_STENCIL_ATTACHMENT,GL_TEXTURE_2D,0,0);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER,unpack);
            for(unsigned i=0;i<4;++i)glPixelStorei(pixel_names[i],pixels[i]);
            previous.Apply();OpenGLState::BindReadFramebuffer(read_fb);OpenGLState::BindDrawFramebuffer(draw_fb);
            for(unsigned face=0;face<2;++face){const auto* values=stencil_values[face];const GLenum side=face?GL_BACK:GL_FRONT;
                glStencilFuncSeparate(side,values[0],values[1],values[2]);glStencilMaskSeparate(side,values[3]);
                glStencilOpSeparate(side,values[4],values[5],values[6]);}
            OpenGLState::InvalidateRasterState();
            for(unsigned i=0;i<5;++i)if(enabled[i])glEnable(capabilities[i]);else glDisable(capabilities[i]);
            glActiveTexture(active);
        });
        OpenGLState state;state.cull.enabled=false;state.color_mask={GL_FALSE,GL_FALSE,GL_FALSE,GL_FALSE};
        state.depth.test_enabled=true;state.depth.test_func=GL_ALWAYS;state.depth.write_mask=GL_TRUE;
        state.stencil.write_mask=0;
        state.draw.draw_framebuffer=upload_fb.handle;state.draw.vertex_array=vao.handle;
        state.draw.shader_program=upload_depth_program.handle;
        state.texture_units[0].texture_2d=upload_input.handle;state.texture_units[0].sampler=sample.handle;
        state.viewport={GLint(rect.left),GLint(rect.bottom),GLsizei(rect.GetWidth()),GLsizei(rect.GetHeight())};
        OpenGLState::InvalidateRasterState();state.Apply();
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER,0);
        glPixelStorei(GL_UNPACK_ALIGNMENT,4);glPixelStorei(GL_UNPACK_SKIP_ROWS,0);glPixelStorei(GL_UNPACK_SKIP_PIXELS,0);
        glPixelStorei(GL_UNPACK_ROW_LENGTH,stride_pixels);
        OpenGLState::BindTexture2D(0,upload_input.handle);glActiveTexture(GL_TEXTURE0);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8UI,rect.GetWidth(),rect.GetHeight(),0,
                     GL_RGBA_INTEGER,GL_UNSIGNED_BYTE,canonical);
        // Full target extent avoids undefined pixels outside the intersection
        // of differently-sized depth/stencil and color framebuffer attachments.
        if(width!=scratch_width||height!=scratch_height){
            OpenGLState::BindTexture2D(0,scratch.handle);glActiveTexture(GL_TEXTURE0);
            glPixelStorei(GL_UNPACK_ROW_LENGTH,0);
            glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,width,height,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
            scratch_width=width;scratch_height=height;
        }
        OpenGLState::BindTexture2D(0,upload_input.handle);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,scratch.handle,0);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_DEPTH_STENCIL_ATTACHMENT,GL_TEXTURE_2D,0,0);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,format==PixelFormat::D24S8?GL_DEPTH_STENCIL_ATTACHMENT:GL_DEPTH_ATTACHMENT,
                               GL_TEXTURE_2D,target,0);
        if(glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)return false;
        glUniform1i(glGetUniformLocation(upload_depth_program.handle,"source"),0);
        glUniform2i(glGetUniformLocation(upload_depth_program.handle,"origin"),rect.left,rect.bottom);
        glUniform1ui(glGetUniformLocation(upload_depth_program.handle,"depth_bits"),format==PixelFormat::D16?16u:24u);
        glDrawArrays(GL_TRIANGLE_STRIP,0,4);
        if(format==PixelFormat::D24S8){
            state.depth.test_enabled=false;state.depth.write_mask=GL_FALSE;
            state.stencil.write_mask=0xff;
            state.scissor={true,GLint(rect.left),GLint(rect.bottom),GLsizei(rect.GetWidth()),GLsizei(rect.GetHeight())};
            state.Apply();const GLint zero=0;glClearBufferiv(GL_STENCIL,0,&zero);
            state.draw.shader_program=upload_stencil_program.handle;
            state.stencil.test_enabled=true;state.stencil.test_func=GL_ALWAYS;
            state.stencil.action_depth_pass=GL_REPLACE;state.Apply();
            glUniform1i(glGetUniformLocation(upload_stencil_program.handle,"source"),0);
            glUniform2i(glGetUniformLocation(upload_stencil_program.handle,"origin"),rect.left,rect.bottom);
            const GLint bit_mask=glGetUniformLocation(upload_stencil_program.handle,"bit_mask");
            for(unsigned bit=0;bit<8;++bit){
                state.stencil.test_ref=state.stencil.write_mask=1u<<bit;state.Apply();
                glUniform1ui(bit_mask,1u<<bit);glDrawArrays(GL_TRIANGLE_STRIP,0,4);
            }
        }
        return glGetError()==GL_NO_ERROR;
    }
private:
    OGLProgram depth_program,stencil_program,upload_depth_program,upload_stencil_program;
    OGLVertexArray vao;OGLSampler sample;OGLTexture scratch;
    OGLFramebuffer work_fb,copy_fb,upload_fb;OGLTexture upload_input;
    GLint bit_location=-1,maximum_depth_location=-1;u32 scratch_width=0,scratch_height=0;
};
bool RunDepthReinterpretSelfTest(std::string& error) {
#if defined(__APPLE__) && !defined(CARA_IOS_GLES)
    if(!MacGPUReinterpretEnabled()){error="Mac depth oracle requires CARA_MAC_GPU_DEPTH_REINTERPRET=1";return false;}
#endif
    if(glGetError()!=GL_NO_ERROR){error="depth oracle preexisting GL error";return false;}
    const auto previous=OpenGLState::GetCurState();
#if defined(__APPLE__) && !defined(CARA_IOS_GLES)
    DesktopDepthRasterState desktop_raster;
#endif
    GLint active=0,pack=0,unpack=0,entry_read_framebuffer=0,entry_draw_framebuffer=0;
    glGetIntegerv(GL_ACTIVE_TEXTURE,&active);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING,&pack);
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING,&unpack);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&entry_read_framebuffer);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&entry_draw_framebuffer);
    constexpr GLenum pixel_names[]={GL_PACK_ALIGNMENT,GL_PACK_ROW_LENGTH,GL_PACK_SKIP_ROWS,
        GL_PACK_SKIP_PIXELS,GL_UNPACK_ALIGNMENT,GL_UNPACK_ROW_LENGTH,GL_UNPACK_SKIP_ROWS,GL_UNPACK_SKIP_PIXELS};
    GLint pixel_values[8];
    for(unsigned i=0;i<8;++i){glGetIntegerv(pixel_names[i],&pixel_values[i]);glPixelStorei(pixel_names[i],i==0||i==4?4:0);}
    glBindBuffer(GL_PIXEL_PACK_BUFFER,0);glBindBuffer(GL_PIXEL_UNPACK_BUFFER,0);
    SCOPE_EXIT({
        for(unsigned i=0;i<8;++i)glPixelStorei(pixel_names[i],pixel_values[i]);
        glBindBuffer(GL_PIXEL_PACK_BUFFER,pack);glBindBuffer(GL_PIXEL_UNPACK_BUFFER,unpack);
        previous.Apply();
        OpenGLState::BindReadFramebuffer(entry_read_framebuffer);
        OpenGLState::BindDrawFramebuffer(entry_draw_framebuffer);
        glActiveTexture(active);
    });
    FormatReinterpreterOpenGL converter;
    OGLTexture source,destination;OGLFramebuffer readback;
    source.Create();destination.Create();readback.Create();
    const auto fail=[&](const std::string& why){error="depth byte oracle: "+why;return false;};
    // Failure-only evidence: read exact shader float bit patterns through an
    // integer target, and compare the uploaded source with a separately
    // fragment-written source. This is never used by normal reinterpretation.
    const auto depth_diagnostic=[&](u32 sx,u32 sy,u32 wanted,u32 stencil,PixelFormat format) {
        constexpr const char* vs=R"(
const vec2 p[4]=vec2[4](vec2(-1,-1),vec2(1,-1),vec2(-1,1),vec2(1,1));
void main(){gl_Position=vec4(p[gl_VertexID],0,1);}
)";
        const std::string fs=std::string(depth_shader_precision)+R"(
uniform highp sampler2D source;
uniform highp ivec2 coordinate;
uniform highp uint maximum_depth;
layout(location=0) out highp uvec4 value;
void main(){highp float z=texelFetch(source,coordinate,0).r;
value=uvec4(floatBitsToUint(z),floatBitsToUint(z*float(maximum_depth)),
uint(roundEven(z*float(maximum_depth))),maximum_depth);}
)";
        const std::string write_fs=std::string(depth_shader_precision)+R"(
uniform highp float wanted;
out highp vec4 color;
void main(){gl_FragDepth=wanted;color=vec4(0);}
)";
        OGLProgram read_program,write_program;OGLVertexArray vao;OGLSampler sampler;
        OGLFramebuffer fb,writer_fb;OGLTexture integer_target,written_depth,writer_color;
        read_program.Create(vs,fs.c_str());write_program.Create(vs,write_fs.c_str());
        vao.Create();sampler.Create();fb.Create();writer_fb.Create();
        integer_target.Create();written_depth.Create();writer_color.Create();
        glSamplerParameteri(sampler.handle,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        glSamplerParameteri(sampler.handle,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glSamplerParameteri(sampler.handle,GL_TEXTURE_COMPARE_MODE,GL_NONE);
        glBindBuffer(GL_PIXEL_PACK_BUFFER,0);glBindBuffer(GL_PIXEL_UNPACK_BUFFER,0);
        OpenGLState::BindTexture2D(0,integer_target.handle);glActiveTexture(GL_TEXTURE0);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA32UI,1,1,0,GL_RGBA_INTEGER,GL_UNSIGNED_INT,nullptr);
        OpenGLState::BindDrawFramebuffer(fb.handle);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,integer_target.handle,0);
        const u32 maximum=format==PixelFormat::D16?65535:16777215;
        const auto sample=[&](GLuint texture,u32 x,u32 y){
            OpenGLState state;state.cull.enabled=false;state.depth.write_mask=GL_FALSE;
            state.draw.draw_framebuffer=state.draw.read_framebuffer=fb.handle;
            state.draw.shader_program=read_program.handle;state.draw.vertex_array=vao.handle;
            state.texture_units[0].texture_2d=texture;state.texture_units[0].sampler=sampler.handle;
            state.viewport={0,0,1,1};OpenGLState::InvalidateRasterState();state.Apply();
            glUniform1i(glGetUniformLocation(read_program.handle,"source"),0);
            glUniform2i(glGetUniformLocation(read_program.handle,"coordinate"),x,y);
            glUniform1ui(glGetUniformLocation(read_program.handle,"maximum_depth"),maximum);
            glDrawArrays(GL_TRIANGLE_STRIP,0,4);
            std::array<u32,4> result{};
            glReadPixels(0,0,1,1,GL_RGBA_INTEGER,GL_UNSIGNED_INT,result.data());return result;
        };
        const auto uploaded=sample(source.handle,sx,sy);
        OpenGLState::BindTexture2D(0,written_depth.handle);glActiveTexture(GL_TEXTURE0);
        const GLenum internal=format==PixelFormat::D16?GL_DEPTH_COMPONENT16:
            format==PixelFormat::D24?GL_DEPTH_COMPONENT24:GL_DEPTH24_STENCIL8;
        glTexImage2D(GL_TEXTURE_2D,0,internal,1,1,0,format==PixelFormat::D24S8?GL_DEPTH_STENCIL:GL_DEPTH_COMPONENT,
                     format==PixelFormat::D16?GL_UNSIGNED_SHORT:format==PixelFormat::D24?GL_UNSIGNED_INT:GL_UNSIGNED_INT_24_8,nullptr);
        OpenGLState::BindTexture2D(0,writer_color.handle);glActiveTexture(GL_TEXTURE0);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
        OpenGLState writer;writer.cull.enabled=false;
        writer.draw.draw_framebuffer=writer_fb.handle;writer.draw.vertex_array=vao.handle;
        writer.draw.shader_program=write_program.handle;writer.viewport={0,0,1,1};
        writer.depth.test_enabled=true;writer.depth.test_func=GL_ALWAYS;writer.depth.write_mask=GL_TRUE;
        writer.stencil.test_enabled=format==PixelFormat::D24S8;writer.stencil.test_ref=stencil;
        writer.stencil.action_depth_pass=GL_REPLACE;OpenGLState::InvalidateRasterState();writer.Apply();
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,writer_color.handle,0);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,format==PixelFormat::D24S8?GL_DEPTH_STENCIL_ATTACHMENT:GL_DEPTH_ATTACHMENT,
                               GL_TEXTURE_2D,written_depth.handle,0);
        const float normalized=float(double(wanted)/maximum);u32 normalized_bits=0;
        std::memcpy(&normalized_bits,&normalized,4);
        glUniform1f(glGetUniformLocation(write_program.handle,"wanted"),normalized);
        glDrawArrays(GL_TRIANGLE_STRIP,0,4);
        const auto fragment=sample(written_depth.handle,0,0);
        const auto values=[](const auto& value){return std::to_string(value[0])+","+std::to_string(value[1])+","+
            std::to_string(value[2])+","+std::to_string(value[3]);};
        std::string edge_probes;
        if(format==PixelFormat::D24S8){
            // Observe potential conversion/sampling collisions at both ends of
            // the critical upper-half range. Never infer injectivity from just
            // the first failing texel or replace the strict oracle with these.
            for(u32 k:{0x7ffffeu,0x7fffffu,0x800000u,0xfffffdu,0xfffffeu,0xffffffu}){
                const u32 packed=(k<<8)|stencil;
                OpenGLState::BindTexture2D(0,written_depth.handle);glActiveTexture(GL_TEXTURE0);
                glTexSubImage2D(GL_TEXTURE_2D,0,0,0,1,1,GL_DEPTH_STENCIL,GL_UNSIGNED_INT_24_8,&packed);
                const auto upload_probe=sample(written_depth.handle,0,0);
                OpenGLState::InvalidateRasterState();writer.Apply();
                const float exact=float(double(k)/16777215.0);u32 exact_bits=0;
                std::memcpy(&exact_bits,&exact,4);
                glUniform1f(glGetUniformLocation(write_program.handle,"wanted"),exact);
                glDrawArrays(GL_TRIANGLE_STRIP,0,4);
                const auto fragment_probe=sample(written_depth.handle,0,0);
                edge_probes+=" edge_k="+std::to_string(k)+" CPU_floatbits="+std::to_string(exact_bits)+
                    " upload=["+values(upload_probe)+"] fragment=["+values(fragment_probe)+"]";
            }
        }
        return " CPU_normalized_floatbits="+std::to_string(normalized_bits)+
            " uploaded[z_floatbits,product_floatbits,rounded,max]="+values(uploaded)+
            " fragment_written[z_floatbits,product_floatbits,rounded,max]="+values(fragment)+edge_probes+
            " diagnostic_GL="+std::to_string(glGetError());
    };
    const auto snapshot=[] {
        std::vector<std::pair<std::string,GLint>> result;
        constexpr GLenum names[]={GL_CURRENT_PROGRAM,GL_VERTEX_ARRAY_BINDING,GL_ACTIVE_TEXTURE,
            GL_DRAW_FRAMEBUFFER_BINDING,GL_READ_FRAMEBUFFER_BINDING,GL_DEPTH_WRITEMASK,
            GL_STENCIL_FUNC,GL_STENCIL_REF,GL_STENCIL_VALUE_MASK,GL_STENCIL_WRITEMASK,
            GL_STENCIL_FAIL,GL_STENCIL_PASS_DEPTH_FAIL,GL_STENCIL_PASS_DEPTH_PASS,
            GL_STENCIL_BACK_FUNC,GL_STENCIL_BACK_REF,GL_STENCIL_BACK_VALUE_MASK,GL_STENCIL_BACK_WRITEMASK,
            GL_STENCIL_BACK_FAIL,GL_STENCIL_BACK_PASS_DEPTH_FAIL,GL_STENCIL_BACK_PASS_DEPTH_PASS,
            GL_BLEND_SRC_RGB,GL_BLEND_DST_RGB,GL_BLEND_SRC_ALPHA,GL_BLEND_DST_ALPHA,
            GL_BLEND_EQUATION_RGB,GL_BLEND_EQUATION_ALPHA,GL_PIXEL_UNPACK_BUFFER_BINDING,
            GL_UNPACK_ALIGNMENT,GL_UNPACK_ROW_LENGTH,GL_UNPACK_SKIP_ROWS,GL_UNPACK_SKIP_PIXELS};
        constexpr const char* labels[]={"CURRENT_PROGRAM","VERTEX_ARRAY_BINDING","ACTIVE_TEXTURE",
            "DRAW_FRAMEBUFFER_BINDING","READ_FRAMEBUFFER_BINDING","DEPTH_WRITEMASK",
            "STENCIL_FUNC","STENCIL_REF","STENCIL_VALUE_MASK","STENCIL_WRITEMASK",
            "STENCIL_FAIL","STENCIL_PASS_DEPTH_FAIL","STENCIL_PASS_DEPTH_PASS",
            "STENCIL_BACK_FUNC","STENCIL_BACK_REF","STENCIL_BACK_VALUE_MASK","STENCIL_BACK_WRITEMASK",
            "STENCIL_BACK_FAIL","STENCIL_BACK_PASS_DEPTH_FAIL","STENCIL_BACK_PASS_DEPTH_PASS",
            "BLEND_SRC_RGB","BLEND_DST_RGB","BLEND_SRC_ALPHA","BLEND_DST_ALPHA",
            "BLEND_EQUATION_RGB","BLEND_EQUATION_ALPHA","PIXEL_UNPACK_BUFFER_BINDING",
            "UNPACK_ALIGNMENT","UNPACK_ROW_LENGTH","UNPACK_SKIP_ROWS","UNPACK_SKIP_PIXELS"};
        for(unsigned i=0;i<std::size(names);++i){GLint value;glGetIntegerv(names[i],&value);result.emplace_back(labels[i],value);}
        for(auto name:{GL_VIEWPORT,GL_SCISSOR_BOX,GL_COLOR_WRITEMASK}){
            GLint values[4];glGetIntegerv(name,values);
            for(unsigned i=0;i<4;++i)result.emplace_back(std::string(name==GL_VIEWPORT?"VIEWPORT":name==GL_SCISSOR_BOX?"SCISSOR_BOX":"COLOR_WRITEMASK")+"["+std::to_string(i)+"]",values[i]);}
        constexpr GLenum caps[]={GL_BLEND,GL_DEPTH_TEST,GL_STENCIL_TEST,GL_SCISSOR_TEST,GL_CULL_FACE,
            GL_DITHER,GL_RASTERIZER_DISCARD,GL_SAMPLE_ALPHA_TO_COVERAGE,GL_SAMPLE_COVERAGE,GL_POLYGON_OFFSET_FILL};
        constexpr const char* cap_labels[]={"BLEND","DEPTH_TEST","STENCIL_TEST","SCISSOR_TEST","CULL_FACE",
            "DITHER","RASTERIZER_DISCARD","SAMPLE_ALPHA_TO_COVERAGE","SAMPLE_COVERAGE","POLYGON_OFFSET_FILL"};
        for(unsigned i=0;i<std::size(caps);++i)result.emplace_back(cap_labels[i],glIsEnabled(caps[i]));
#if defined(__APPLE__) && !defined(CARA_IOS_GLES)
        GLint logic=0;glGetIntegerv(GL_LOGIC_OP_MODE,&logic);result.emplace_back("LOGIC_OP_MODE",logic);
        result.emplace_back("COLOR_LOGIC_OP",glIsEnabled(GL_COLOR_LOGIC_OP));
        result.emplace_back("CLIP_DISTANCE0",glIsEnabled(GL_CLIP_DISTANCE0));
        result.emplace_back("CLIP_DISTANCE1",glIsEnabled(GL_CLIP_DISTANCE1));
#endif
        return result;
    };
    const auto verify=[&](u32 width,u32 height,u32 destination_width,u32 destination_height,
                          const std::vector<u32>& packed,const Common::Rectangle<u32>& sr,
                          const Common::Rectangle<u32>& dr,bool upload_source,
                          PixelFormat format=PixelFormat::D24S8) {
        if(upload_source){
            OpenGLState::BindTexture2D(0,source.handle);glActiveTexture(GL_TEXTURE0);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
            {
                glTexImage2D(GL_TEXTURE_2D,0,format==PixelFormat::D24S8?GL_DEPTH24_STENCIL8:
                    format==PixelFormat::D24?GL_DEPTH_COMPONENT24:GL_DEPTH_COMPONENT16,
                    width,height,0,format==PixelFormat::D24S8?GL_DEPTH_STENCIL:GL_DEPTH_COMPONENT,
                    format==PixelFormat::D24S8?GL_UNSIGNED_INT_24_8:
                    format==PixelFormat::D24?GL_UNSIGNED_INT:GL_UNSIGNED_SHORT,nullptr);
                std::vector<u8> canonical(packed.size()*4);
                for(size_t i=0;i<packed.size();++i){const u32 value=packed[i],z=value>>8;
                    canonical[i*4]=format==PixelFormat::D24S8?u8(value):0;
                    canonical[i*4+1]=u8(z);canonical[i*4+2]=u8(z>>8);canonical[i*4+3]=u8(z>>16);}
                const auto copy=canonical;const auto before_upload=snapshot();
                if(!converter.UploadDepthChecked(source.handle,format,{0,height,width,0},canonical.data(),
                    canonical.size(),width,width,height))return fail("exact uploader failed format="+std::to_string(unsigned(format)));
                if(canonical!=copy)return fail("uploader modified CPU bytes");
                const auto after_upload=snapshot();
                if(after_upload!=before_upload){
                    std::string detail="uploader GL state changed";
                    for(unsigned i=0;i<before_upload.size();++i)if(before_upload[i]!=after_upload[i])
                        detail+=" "+before_upload[i].first+" before="+std::to_string(before_upload[i].second)+" after="+std::to_string(after_upload[i].second);
                    return fail(detail);
                }
            }
        }
        std::vector<u8> initial(size_t(destination_width)*destination_height*4,0x5b),actual(initial.size());
        OpenGLState::BindTexture2D(0,destination.handle);glActiveTexture(GL_TEXTURE0);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,destination_width,destination_height,0,GL_RGBA,
                     GL_UNSIGNED_BYTE,initial.data());
        auto stress=OpenGLState::GetCurState();
        stress.viewport={1,2,3,4};stress.scissor={true,2,3,4,5};
        stress.depth.test_enabled=true;stress.depth.write_mask=GL_TRUE;
        stress.stencil.test_enabled=true;stress.stencil.test_func=GL_NOTEQUAL;
        stress.stencil.test_ref=23;stress.stencil.write_mask=0;
        stress.stencil.action_stencil_fail=stress.stencil.action_depth_fail=stress.stencil.action_depth_pass=GL_KEEP;
        stress.blend.enabled=true;stress.blend.src_rgb_func=GL_SRC_ALPHA;
        stress.color_mask.red_enabled=GL_FALSE;stress.Apply();
        // Exact cache-poison case: cache already says safe mask0/KEEP, while
        // raw FRONT would overwrite source stencil if the encoder reused it.
        glStencilMaskSeparate(GL_FRONT,0xff);
        glStencilOpSeparate(GL_FRONT,GL_REPLACE,GL_REPLACE,GL_REPLACE);
        glStencilFuncSeparate(GL_BACK,GL_LESS,61,0x37);
        glStencilMaskSeparate(GL_BACK,0x39);glStencilOpSeparate(GL_BACK,GL_INVERT,GL_ZERO,GL_REPLACE);
        // Real startup context can have a raw read binding different from the
        // cached value. The checked API must restore both actual binding/cache.
        glBindFramebuffer(GL_READ_FRAMEBUFFER,readback.handle);
#if defined(__APPLE__) && !defined(CARA_IOS_GLES)
        glEnable(GL_COLOR_LOGIC_OP);glLogicOp(GL_XOR);
        glEnable(GL_CLIP_DISTANCE0);glEnable(GL_CLIP_DISTANCE1);
#endif
        const auto before=snapshot();
        if(!converter.EncodeDepthChecked(source.handle,format,sr,destination.handle,dr,width,height))
            return fail("checked conversion format="+std::to_string(unsigned(format))+" GL="+std::to_string(glGetError()));
        const auto after=snapshot();
        if(after!=before){
            std::string detail="GL state changed";
            for(unsigned i=0;i<before.size();++i)if(before[i]!=after[i])
                detail+=" "+before[i].first+" before="+std::to_string(before[i].second)+" after="+std::to_string(after[i].second);
            return fail(detail);
        }
        OpenGLState::BindReadFramebuffer(readback.handle);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,destination.handle,0);
        if(glCheckFramebufferStatus(GL_READ_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)return fail("read framebuffer");
        glReadPixels(0,0,destination_width,destination_height,GL_RGBA,GL_UNSIGNED_BYTE,actual.data());
        if(glGetError()!=GL_NO_ERROR)return fail("readback GL error");
        for(u32 y=0;y<destination_height;++y)for(u32 x=0;x<destination_width;++x){
            const auto offset=(size_t(y)*destination_width+x)*4;
            if(x>=dr.left&&x<dr.right&&y>=dr.bottom&&y<dr.top){
                const u32 sx=sr.left+(u64(2*(x-dr.left)+1)*sr.GetWidth())/(2*dr.GetWidth());
                const u32 sy=sr.bottom+(u64(2*(y-dr.bottom)+1)*sr.GetHeight())/(2*dr.GetHeight());
                const u32 value=packed[size_t(sy)*width+sx],z=value>>8;
                const u8 expected[]={format==PixelFormat::D24S8?u8(value):u8(0),u8(z>>16),u8(z>>8),u8(z)};
                if(std::memcmp(actual.data()+offset,expected,4))
                    return fail("bytes format="+std::to_string(unsigned(format))+" x="+std::to_string(x)+" y="+std::to_string(y)+
                        " packed="+std::to_string(value)+" observed="+
                        std::to_string(actual[offset])+","+std::to_string(actual[offset+1])+","+
                        std::to_string(actual[offset+2])+","+std::to_string(actual[offset+3])+
                        depth_diagnostic(sx,sy,z,value&255,format));
            }else if(std::memcmp(actual.data()+offset,initial.data()+offset,4))return fail("outside pixels changed");
        }
        return true;
    };
    std::vector<u32> small(64*64);
    constexpr u32 edges[]={0,1,2,255,256,257,65535,65536,8388606,8388607,8388608,
                            8388609,16777213,16777214,16777215};
    u32 random=0x4915;
    for(unsigned i=0;i<small.size();++i){random=random*1664525u+1013904223u;
        const u32 z=i/256<sizeof(edges)/sizeof(edges[0])?edges[i/256]:random&0xffffff;
        small[i]=(z<<8)|(i&255);
    }
    if(!verify(64,64,40,40,small,{3,21,19,5},{7,29,23,13},true))return false;
    if(converter.EncodeDepthChecked(source.handle,PixelFormat::D24S8,{0,64,64,0},source.handle,{0,64,64,0},64,64))
        return fail("same-handle alias accepted");
    // Reuse the source without uploading it: depth/stencil KEEP and zero write
    // masks must leave every source pixel intact after the offset operation.
    if(!verify(64,64,72,72,small,{0,64,64,0},{4,68,68,4},false))return false;
    if(!verify(64,64,40,40,small,{3,21,19,5},{2,34,34,2},false))return false;
    for(auto format:{PixelFormat::D24S8,PixelFormat::D24}){
        if(!verify(64,64,64,64,small,{0,64,64,0},{0,64,64,0},true,format))return false;
        auto changed=small;const Common::Rectangle<u32> update{5,29,25,9};
        for(u32 y=update.bottom;y<update.top;++y)for(u32 x=update.left;x<update.right;++x){
            const u32 i=y*64+x,z=0xffffffu-((i*7919u)&0xffffffu);changed[i]=(z<<8)|((i*13u)&255u);}
        std::vector<u8> canonical(changed.size()*4);
        for(size_t i=0;i<changed.size();++i){const u32 value=changed[i],z=value>>8;
            canonical[i*4]=format==PixelFormat::D24S8?u8(value):0;
            canonical[i*4+1]=u8(z);canonical[i*4+2]=u8(z>>8);canonical[i*4+3]=u8(z>>16);}
        const auto cpu_copy=canonical;const size_t offset=(update.bottom*64+update.left)*4;
        const auto before=snapshot();
        if(converter.UploadDepthChecked(source.handle,format,update,canonical.data()+offset,1,64,64,64))
            return fail("short upload source accepted");
        if(snapshot()!=before)return fail("rejected upload changed state");
        if(!converter.UploadDepthChecked(source.handle,format,update,canonical.data()+offset,
            canonical.size()-offset,64,64,64))return fail("partial upload failed");
        if(canonical!=cpu_copy)return fail("partial uploader modified CPU bytes");
        if(snapshot()!=before)return fail("partial uploader changed state");
        // Full readback proves every pixel outside update, including stencil,
        // survived the scissored upload and its stencil clear/write passes.
        if(!verify(64,64,64,64,changed,{0,64,64,0},{0,64,64,0},false,format))return false;
    }
    // Exhaust every D24 code once in bounded 1024x1024 batches. Stencil cycles
    // through all 256 values independently of the high depth bits.
    std::vector<u32> packed(1024*1024);
    for(auto format:{PixelFormat::D24S8,PixelFormat::D24})for(u32 batch=0;batch<16;++batch){
        for(u32 i=0;i<packed.size();++i){const u32 z=batch*u32(packed.size())+i;
            packed[i]=(z<<8)|((i*73+batch*19)&255);}
        if(!verify(1024,1024,1024,1024,packed,{0,1024,1024,0},{0,1024,1024,0},true,format))return false;
    }
    std::vector<u32> d16(256*256);
    for(u32 i=0;i<d16.size();++i)d16[i]=i<<8;
    if(!verify(256,256,260,260,d16,{0,256,256,0},{2,258,258,2},true,PixelFormat::D16))return false;
#if defined(__APPLE__) && !defined(CARA_IOS_GLES)
    if(!CompareNativeDepthUploadWithPBO(error))return false;
#endif
    return true;
}
#else
bool RunDepthReinterpretSelfTest(std::string&) { return true; }
#endif

// Virtual Console
class RGBA4toRGB5A1 final : public FormatReinterpreterBase {
public:
    RGBA4toRGB5A1() {
        constexpr std::string_view vs_source = R"(
out vec2 tex_coord;

const vec2 vertices[4] =
    vec2[4](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0), vec2(1.0, 1.0));

void main() {
    gl_Position = vec4(vertices[gl_VertexID], 0.0, 1.0);
    tex_coord = (vertices[gl_VertexID] / 2.0 + 0.5);
}
)";

        std::string fs_source = GLES ? fragment_shader_precision_OES : "";
        fs_source += R"(
in vec2 tex_coord;

out vec4 frag_color;

uniform sampler2D source;
uniform ivec2 src_size;
uniform ivec2 src_offset;

void main() {
    ivec2 coord = ivec2(tex_coord * vec2(src_size)) + src_offset;

    ivec4 rgba4 = ivec4(texelFetch(source, coord, 0) * 15.0f);
    ivec3 rgb5 = ((rgba4.rgb << ivec3(1, 2, 3)) | (rgba4.gba >> ivec3(3, 2, 1))) & 31;
    frag_color = vec4(vec3(rgb5) / 31.0, rgba4.a & 1);
}
)";

        program.Create(vs_source.data(), fs_source.data());
        auto old_program = OpenGLState::BindShaderProgram(program.handle);
        glUniform1i(glGetUniformLocation(program.handle, "source"), 0);
        src_size_loc = glGetUniformLocation(program.handle, "src_size");
        src_offset_loc = glGetUniformLocation(program.handle, "src_offset");
        OpenGLState::BindShaderProgram(old_program);
        vao.Create();
    }

    void Reinterpret(GLuint src_tex, const Common::Rectangle<u32>& src_rect, GLuint read_fb_handle,
                     GLuint dst_tex, const Common::Rectangle<u32>& dst_rect,
                     GLuint draw_fb_handle) override {
        OpenGLState prev_state = OpenGLState::GetCurState();
        SCOPE_EXIT({ prev_state.Apply(); });

        OpenGLState state;
        state.texture_units[0].texture_2d = src_tex;
        state.draw.draw_framebuffer = draw_fb_handle;
        state.draw.shader_program = program.handle;
        state.draw.vertex_array = vao.handle;
        state.viewport = {static_cast<GLint>(dst_rect.left), static_cast<GLint>(dst_rect.bottom),
                          static_cast<GLsizei>(dst_rect.GetWidth()),
                          static_cast<GLsizei>(dst_rect.GetHeight())};
        state.Apply();

        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst_tex,
                               0);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0,
                               0);

        glUniform2i(src_size_loc, src_rect.GetWidth(), src_rect.GetHeight());
        glUniform2i(src_offset_loc, src_rect.left, src_rect.bottom);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

private:
    OGLProgram program;
    GLint src_size_loc{-1}, src_offset_loc{-1};
    OGLVertexArray vao;
};

// Batman
class ShaderRGBA8toD24S8 final : public FormatReinterpreterBase {
public:
    ShaderRGBA8toD24S8() {
        constexpr std::string_view vs_source = R"(
out vec2 tex_coord;

const vec2 vertices[4] =
vec2[4](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0), vec2(1.0, 1.0));

void main() {
    gl_Position = vec4(vertices[gl_VertexID], 0.0, 1.0);
    tex_coord = (vertices[gl_VertexID] / 2.0 + 0.5);
}
)";

        std::string fs_source = GLES ? fragment_shader_precision_OES : "";
        fs_source += R"(
in vec2 tex_coord;

uniform sampler2D source;
uniform ivec2 src_size;
uniform ivec2 src_offset;

void main() {
    ivec2 coord = ivec2(tex_coord * vec2(src_size)) + src_offset;
    vec4 rgba = texelFetch(source, coord, 0);
    gl_FragDepth = 0.0f;
}
)";

        program.Create(vs_source.data(), fs_source.data());
        auto old_program = OpenGLState::BindShaderProgram(program.handle);
        glUniform1i(glGetUniformLocation(program.handle, "source"), 0);
        src_size_loc = glGetUniformLocation(program.handle, "src_size");
        src_offset_loc = glGetUniformLocation(program.handle, "src_offset");
        OpenGLState::BindShaderProgram(old_program);
        vao.Create();
    }

    void Reinterpret(GLuint src_tex, const Common::Rectangle<u32>& src_rect, GLuint read_fb_handle,
                     GLuint dst_tex, const Common::Rectangle<u32>& dst_rect,
                     GLuint draw_fb_handle) override {
        OpenGLState prev_state = OpenGLState::GetCurState();
        SCOPE_EXIT({ prev_state.Apply(); });

        OpenGLState state;
        state.texture_units[0].texture_2d = src_tex;
        state.draw.draw_framebuffer = draw_fb_handle;
        state.draw.shader_program = program.handle;
        state.draw.vertex_array = vao.handle;
        state.viewport = {static_cast<GLint>(dst_rect.left), static_cast<GLint>(dst_rect.bottom),
                          static_cast<GLsizei>(dst_rect.GetWidth()),
                          static_cast<GLsizei>(dst_rect.GetHeight())};
        state.Apply();

        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D,
                               dst_tex, 0);

        glUniform2i(src_size_loc, src_rect.GetWidth(), src_rect.GetHeight());
        glUniform2i(src_offset_loc, src_rect.left, src_rect.bottom);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

private:
    OGLProgram program;
    GLint src_size_loc{-1}, src_offset_loc{-1};
    OGLVertexArray vao;
};

// Dead or Alive
class ShaderD16toRG8 final : public FormatReinterpreterBase {
public:
    ShaderD16toRG8() {
        constexpr std::string_view vs_source = R"(
out vec2 tex_coord;

const vec2 vertices[4] =
vec2[4](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0), vec2(1.0, 1.0));

void main() {
    gl_Position = vec4(vertices[gl_VertexID], 0.0, 1.0);
    tex_coord = (vertices[gl_VertexID] / 2.0 + 0.5);
}
)";

        std::string fs_source = GLES ? fragment_shader_precision_OES : "";
        fs_source += R"(
in vec2 tex_coord;
out vec4 frag_color;

uniform usampler2D depth;
uniform ivec2 src_size;
uniform ivec2 src_offset;

void main() {
    ivec2 coord = ivec2(tex_coord * vec2(src_size)) + src_offset;
    uint depth_val = texelFetch(depth, coord, 0).x;
    uvec2 components = (uvec2(depth_val) >> uvec2(8u, 0u)) & 255u;
    frag_color = vec4(vec2(components) / 255.0f, 0.0f, 0.0f);
}
)";

        program.Create(vs_source.data(), fs_source.data());
        auto old_program = OpenGLState::BindShaderProgram(program.handle);
        glUniform1i(glGetUniformLocation(program.handle, "source"), 0);
        src_size_loc = glGetUniformLocation(program.handle, "src_size");
        src_offset_loc = glGetUniformLocation(program.handle, "src_offset");
        OpenGLState::BindShaderProgram(old_program);
        vao.Create();
    }

    void Reinterpret(GLuint src_tex, const Common::Rectangle<u32>& src_rect, GLuint read_fb_handle,
                     GLuint dst_tex, const Common::Rectangle<u32>& dst_rect,
                     GLuint draw_fb_handle) override {
        OpenGLState prev_state = OpenGLState::GetCurState();
        SCOPE_EXIT({ prev_state.Apply(); });

        OpenGLState state;
        state.texture_units[0].texture_2d = src_tex;
        state.draw.draw_framebuffer = draw_fb_handle;
        state.draw.shader_program = program.handle;
        state.draw.vertex_array = vao.handle;
        state.viewport = {static_cast<GLint>(dst_rect.left), static_cast<GLint>(dst_rect.bottom),
                          static_cast<GLsizei>(dst_rect.GetWidth()),
                          static_cast<GLsizei>(dst_rect.GetHeight())};
        state.Apply();

        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst_tex,
                               0);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0,
                               0);

        glUniform2i(src_size_loc, src_rect.GetWidth(), src_rect.GetHeight());
        glUniform2i(src_offset_loc, src_rect.left, src_rect.bottom);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

private:
    OGLProgram program;
    GLint src_size_loc{-1}, src_offset_loc{-1};
    OGLVertexArray vao;
};

class PixelBufferD24S8toABGR final : public FormatReinterpreterBase {
public:
    PixelBufferD24S8toABGR() {
        attributeless_vao.Create();
        d24s8_abgr_buffer.Create();
        d24s8_abgr_buffer_size = 0;

        constexpr std::string_view vs_source = R"(
const vec2 vertices[4] = vec2[4](vec2(-1.0, -1.0), vec2(1.0, -1.0),
                                 vec2(-1.0,  1.0), vec2(1.0,  1.0));
void main() {
    gl_Position = vec4(vertices[gl_VertexID], 0.0, 1.0);
}
)";

        std::string fs_source = GLES ? fragment_shader_precision_OES : "";
        fs_source += R"(
uniform samplerBuffer tbo;
uniform vec2 tbo_size;
uniform vec4 viewport;

out vec4 color;

void main() {
    vec2 tbo_coord = (gl_FragCoord.xy - viewport.xy) * tbo_size / viewport.zw;
    int tbo_offset = int(tbo_coord.y) * int(tbo_size.x) + int(tbo_coord.x);
    color = texelFetch(tbo, tbo_offset).rabg;
}
)";

        d24s8_abgr_shader.Create(vs_source.data(), fs_source.c_str());
        auto old_program = OpenGLState::BindShaderProgram(d24s8_abgr_shader.handle);
        glUniform1i(glGetUniformLocation(d24s8_abgr_shader.handle, "tbo"), 0);
        d24s8_abgr_tbo_size_u_id = glGetUniformLocation(d24s8_abgr_shader.handle, "tbo_size");
        d24s8_abgr_viewport_u_id = glGetUniformLocation(d24s8_abgr_shader.handle, "viewport");
        OpenGLState::BindShaderProgram(old_program);
    }

    ~PixelBufferD24S8toABGR() {}

    void Reinterpret(GLuint src_tex, const Common::Rectangle<u32>& src_rect, GLuint read_fb_handle,
                     GLuint dst_tex, const Common::Rectangle<u32>& dst_rect,
                     GLuint draw_fb_handle) override {
        OpenGLState prev_state = OpenGLState::GetCurState();
        SCOPE_EXIT({ prev_state.Apply(); });

        OpenGLState state;
        state.draw.read_framebuffer = read_fb_handle;
        state.draw.draw_framebuffer = draw_fb_handle;
        state.Apply();

        glBindBuffer(GL_PIXEL_PACK_BUFFER, d24s8_abgr_buffer.handle);

        GLsizeiptr target_pbo_size =
            static_cast<GLsizeiptr>(src_rect.GetWidth()) * src_rect.GetHeight() * 4;
        if (target_pbo_size > d24s8_abgr_buffer_size) {
            d24s8_abgr_buffer_size = target_pbo_size * 2;
            glBufferData(GL_PIXEL_PACK_BUFFER, d24s8_abgr_buffer_size, nullptr, GL_STREAM_COPY);
        }

        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D,
                               src_tex, 0);
        glReadPixels(static_cast<GLint>(src_rect.left), static_cast<GLint>(src_rect.bottom),
                     static_cast<GLsizei>(src_rect.GetWidth()),
                     static_cast<GLsizei>(src_rect.GetHeight()), GL_DEPTH_STENCIL,
                     GL_UNSIGNED_INT_24_8, 0);

        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

        // PBO now contains src_tex in RABG format
        state.draw.shader_program = d24s8_abgr_shader.handle;
        state.draw.vertex_array = attributeless_vao.handle;
        state.viewport.x = static_cast<GLint>(dst_rect.left);
        state.viewport.y = static_cast<GLint>(dst_rect.bottom);
        state.viewport.width = static_cast<GLsizei>(dst_rect.GetWidth());
        state.viewport.height = static_cast<GLsizei>(dst_rect.GetHeight());
        state.Apply();

        OGLTexture tbo;
        tbo.Create();
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_BUFFER, tbo.handle);
        glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA8, d24s8_abgr_buffer.handle);

        glUniform2f(d24s8_abgr_tbo_size_u_id, static_cast<GLfloat>(src_rect.GetWidth()),
                    static_cast<GLfloat>(src_rect.GetHeight()));
        glUniform4f(d24s8_abgr_viewport_u_id, static_cast<GLfloat>(state.viewport.x),
                    static_cast<GLfloat>(state.viewport.y),
                    static_cast<GLfloat>(state.viewport.width),
                    static_cast<GLfloat>(state.viewport.height));

        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst_tex,
                               0);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0,
                               0);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        glBindTexture(GL_TEXTURE_BUFFER, 0);
    }

private:
    OGLVertexArray attributeless_vao;
    OGLBuffer d24s8_abgr_buffer;
    GLsizeiptr d24s8_abgr_buffer_size;
    OGLProgram d24s8_abgr_shader;
    GLint d24s8_abgr_tbo_size_u_id;
    GLint d24s8_abgr_viewport_u_id;
};

#if defined(__APPLE__) && !defined(CARA_IOS_GLES)
// Compare the actual desktop upload/legacy-PBO route with the candidate. The
// exhaustive oracle above uses fragment-written depth to guarantee exact seeds;
// this independent check also covers the driver's native integer upload path.
static bool CompareNativeDepthUploadWithPBO(std::string& error) {
    const auto previous=OpenGLState::GetCurState();
    DesktopDepthRasterState raster;
    SCOPE_EXIT({previous.Apply();});
    OGLTexture source,old_output,new_output;
    OGLFramebuffer read_fb,write_fb,readback;
    source.Create();old_output.Create();new_output.Create();
    read_fb.Create();write_fb.Create();readback.Create();
    PixelBufferD24S8toABGR legacy;
    GLESDepthStencilToRGBA8 candidate;
    constexpr u32 width=256,height=256;
    std::vector<u32> packed(width*height);
    constexpr u32 edges[]={0,1,2,255,256,65535,65536,0x7ffffe,0x7fffff,
                           0x800000,0x800001,0xfffffd,0xfffffe,0xffffff};
    u32 random=0x71539u;
    for(u32 i=0;i<packed.size();++i) {
        random=random*1664525u+1013904223u;
        const u32 z=i/256<std::size(edges)?edges[i/256]:(random&0xffffffu);
        packed[i]=(z<<8)|(i&255);
    }
    OpenGLState::BindTexture2D(0,source.handle);glActiveTexture(GL_TEXTURE0);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER,0);
    glTexImage2D(GL_TEXTURE_2D,0,GL_DEPTH24_STENCIL8,width,height,0,
                 GL_DEPTH_STENCIL,GL_UNSIGNED_INT_24_8,packed.data());
    if(glGetError()!=GL_NO_ERROR){error="native depth seed upload failed";return false;}
    const auto read=[&](GLuint texture,std::vector<u8>& bytes) {
        OpenGLState::BindReadFramebuffer(readback.handle);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
        glReadBuffer(GL_COLOR_ATTACHMENT0);glBindBuffer(GL_PIXEL_PACK_BUFFER,0);
        if(glCheckFramebufferStatus(GL_READ_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)return false;
        std::fill(bytes.begin(),bytes.end(),0xa7);
        glReadPixels(0,0,width,height,GL_RGBA,GL_UNSIGNED_BYTE,bytes.data());
        return glGetError()==GL_NO_ERROR;
    };
    const auto verify=[&](const Common::Rectangle<u32>& sr,const Common::Rectangle<u32>& dr) {
        std::vector<u8> initial(width*height*4,0x5b),a(initial.size()),b(initial.size());
        for(GLuint texture:{old_output.handle,new_output.handle}) {
            OpenGLState::BindTexture2D(0,texture);glActiveTexture(GL_TEXTURE0);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER,0);
            glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,width,height,0,GL_RGBA,GL_UNSIGNED_BYTE,initial.data());
        }
        legacy.Reinterpret(source.handle,sr,read_fb.handle,old_output.handle,dr,write_fb.handle);
        if(glGetError()!=GL_NO_ERROR || !read(old_output.handle,a)) {
            error="legacy PBO native-upload oracle failed";return false;
        }
        if(!candidate.ReinterpretChecked(source.handle,sr,read_fb.handle,new_output.handle,dr,
                                         write_fb.handle,width,height) || !read(new_output.handle,b)) {
            error="candidate native-upload oracle failed";return false;
        }
        if(a!=b) {
            const auto index=std::mismatch(a.begin(),a.end(),b.begin()).first-a.begin();
            error="native-upload PBO/GPU byte mismatch at="+std::to_string(index)+
                  " PBO="+std::to_string(a[index])+" GPU="+std::to_string(b[index]);return false;
        }
        // A second conversion without re-upload catches writes to source depth/stencil.
        return true;
    };
    return verify({0,height,width,0},{0,height,width,0}) &&
           verify({7,71,39,7},{11,109,75,45}) &&
           verify({0,height,width,0},{0,height,width,0});
}
#endif

// Danball Senki W
class ShaderD24S8toRGBA8 final : public FormatReinterpreterBase {
public:
    ShaderD24S8toRGBA8() {
        constexpr std::string_view vs_source = R"(
out vec2 tex_coord;

const vec2 vertices[4] =
    vec2[4](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0), vec2(1.0, 1.0));

void main() {
    gl_Position = vec4(vertices[gl_VertexID], 0.0, 1.0);
    tex_coord = vertices[gl_VertexID] / 2.0 + 0.5;
}
)";

        std::string fs_source = GLES ? fragment_shader_precision_OES : "";
        fs_source += R"(
in vec2 tex_coord;
out vec4 frag_color;

uniform sampler2D depth;
uniform usampler2D stencil;
uniform ivec2 src_size;
uniform ivec2 src_offset;

void main() {
    ivec2 coord = ivec2(tex_coord * vec2(src_size)) + src_offset;

    uint depth_val = uint(texelFetch(depth, coord, 0).x * (exp2(32.0) - 1.0));
    uint stencil_val = texelFetch(stencil, coord, 0).x;
    uvec4 components = uvec4(stencil_val, (uvec3(depth_val) >> uvec3(24u, 16u, 8u)) & 255u);
    frag_color = vec4(components) / 255.0f;
}
)";

        program.Create(vs_source.data(), fs_source.data());
        auto cur_program = OpenGLState::BindShaderProgram(program.handle);
        src_size_loc = glGetUniformLocation(program.handle, "src_size");
        src_offset_loc = glGetUniformLocation(program.handle, "src_offset");
        glUniform1i(glGetUniformLocation(program.handle, "depth"), 0);
        glUniform1i(glGetUniformLocation(program.handle, "stencil"), 1);
        OpenGLState::BindShaderProgram(cur_program);
        vao.Create();

        // OES_texture_view doesn't seem to support D24S8 views, at least on adreno
        // so instead it will do an intermediate copy before running through the shader
        if (GLAD_GL_ARB_texture_view) {
            texture_view_func = glTextureView;
        } else {
            LOG_INFO(Render_OpenGL,
                     "Texture views are unsupported, reinterpretation will do intermediate copy");
            temp_tex.Create();
        }
    }

    void Reinterpret(GLuint src_tex, const Common::Rectangle<u32>& src_rect, GLuint read_fb_handle,
                     GLuint dst_tex, const Common::Rectangle<u32>& dst_rect,
                     GLuint draw_fb_handle) override {
        OpenGLState prev_state = OpenGLState::GetCurState();
        SCOPE_EXIT({ prev_state.Apply(); });

        OpenGLState state;
        state.texture_units[0].texture_2d = src_tex;

        if (texture_view_func) {
            temp_tex.Create();
            glActiveTexture(GL_TEXTURE1);
            texture_view_func(temp_tex.handle, GL_TEXTURE_2D, src_tex, GL_DEPTH24_STENCIL8, 0, 1, 0,
                              1);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        } else if (src_rect.top > temp_rect.top || src_rect.right > temp_rect.right) {
            temp_tex.Release();
            temp_tex.Create();
            OpenGLState::BindTexture2D(1, temp_tex.handle);
            glActiveTexture(GL_TEXTURE1);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH24_STENCIL8, src_rect.right, src_rect.top);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            temp_rect = src_rect;
        }

        state.texture_units[1].texture_2d = temp_tex.handle;
        state.draw.draw_framebuffer = draw_fb_handle;
        state.draw.shader_program = program.handle;
        state.draw.vertex_array = vao.handle;
        state.viewport = {static_cast<GLint>(dst_rect.left), static_cast<GLint>(dst_rect.bottom),
                          static_cast<GLsizei>(dst_rect.GetWidth()),
                          static_cast<GLsizei>(dst_rect.GetHeight())};
        state.Apply();

        glActiveTexture(GL_TEXTURE1);
        if (!texture_view_func) {
            glCopyImageSubData(src_tex, GL_TEXTURE_2D, 0, src_rect.left, src_rect.bottom, 0,
                               temp_tex.handle, GL_TEXTURE_2D, 0, src_rect.left, src_rect.bottom, 0,
                               src_rect.GetWidth(), src_rect.GetHeight(), 1);
        }
        glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_STENCIL_TEXTURE_MODE, GL_STENCIL_INDEX);

        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst_tex,
                               0);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0,
                               0);

        glUniform2i(src_size_loc, src_rect.GetWidth(), src_rect.GetHeight());
        glUniform2i(src_offset_loc, src_rect.left, src_rect.bottom);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        if (texture_view_func) {
            temp_tex.Release();
        }
    }

private:
    decltype(glTextureView) texture_view_func = nullptr;
    OGLProgram program{};
    GLint src_size_loc{-1}, src_offset_loc{-1};
    OGLVertexArray vao{};
    OGLTexture temp_tex{};
    Common::Rectangle<u32> temp_rect{0, 0, 0, 0};
};

class CopyImageSubData final : public FormatReinterpreterBase {
    void Reinterpret(GLuint src_tex, const Common::Rectangle<u32>& src_rect, GLuint read_fb_handle,
                     GLuint dst_tex, const Common::Rectangle<u32>& dst_rect,
                     GLuint draw_fb_handle) override {
        glCopyImageSubData(src_tex, GL_TEXTURE_2D, 0, src_rect.left, src_rect.bottom, 0, dst_tex,
                           GL_TEXTURE_2D, 0, dst_rect.left, dst_rect.bottom, 0, src_rect.GetWidth(),
                           src_rect.GetHeight(), 1);
    }
};

FormatReinterpreterOpenGL::FormatReinterpreterOpenGL() {
#ifdef CARA_IOS_GLES
    const char* depth=std::getenv("CARA_GPU_DEPTH_REINTERPRET");
    if(!depth||std::strcmp(depth,"1")==0)
        reinterpreters.emplace_back(PixelFormat::RGBA8,PixelFormat::D24S8,
                                    std::make_unique<GLESDepthStencilToRGBA8>());
    // Other unsupported reinterpretations keep the existing flush/reload path.
    return;
#endif
    std::string_view vendor{reinterpret_cast<const char*>(glGetString(GL_VENDOR))};
#if defined(__APPLE__) && !defined(CARA_IOS_GLES)
    if (MacGPUReinterpretEnabled()) {
        reinterpreters.emplace_back(PixelFormat::RGBA8,PixelFormat::D24S8,
                                    std::make_unique<GLESDepthStencilToRGBA8>());
        LOG_INFO(Render_OpenGL,"Using opt-in exact GPU D24S8 reinterpretation on desktop GL");
    } else
#endif
    if (vendor.find("NVIDIA") != vendor.npos) {
        reinterpreters.emplace_back(PixelFormat::RGBA8, PixelFormat::D24S8,
                                    std::make_unique<CopyImageSubData>());
        // Nvidia bends the spec and allows direct copies between color and depth formats
        // might as well take advantage of it
        LOG_INFO(Render_OpenGL, "Using glCopyImageSubData for D24S8 to RGBA8 reinterpretation");
    } else if ((GLAD_GL_ARB_stencil_texturing && GLAD_GL_ARB_texture_storage) || GLES) {
        reinterpreters.emplace_back(PixelFormat::RGBA8, PixelFormat::D24S8,
                                    std::make_unique<ShaderD24S8toRGBA8>());
        LOG_INFO(Render_OpenGL, "Using shader for D24S8 to RGBA8 reinterpretation");
    } else {
        reinterpreters.emplace_back(PixelFormat::RGBA8, PixelFormat::D24S8,
                                    std::make_unique<PixelBufferD24S8toABGR>());
        LOG_INFO(Render_OpenGL, "Using pbo for D24S8 to RGBA8 reinterpretation");
    }
    reinterpreters.emplace_back(PixelFormat::D24S8, PixelFormat::RGBA8,
                                std::make_unique<ShaderRGBA8toD24S8>());
    reinterpreters.emplace_back(PixelFormat::RG8, PixelFormat::D16,
                                std::make_unique<ShaderD16toRG8>());
    reinterpreters.emplace_back(PixelFormat::RGB5A1, PixelFormat::RGBA4,
                                std::make_unique<RGBA4toRGB5A1>());
}

FormatReinterpreterOpenGL::~FormatReinterpreterOpenGL() = default;

bool FormatReinterpreterOpenGL::EncodeDepthChecked(GLuint src,PixelFormat format,
    const Common::Rectangle<u32>& sr,GLuint dst,const Common::Rectangle<u32>& dr,u32 width,u32 height) {
#if defined(CARA_IOS_GLES) || defined(__APPLE__)
#if defined(__APPLE__) && !defined(CARA_IOS_GLES)
    if(!MacGPUReinterpretEnabled())return false;
#endif
    if(!depth_encoder)depth_encoder=std::make_unique<GLESDepthStencilToRGBA8>();
    return depth_encoder->EncodeDepthChecked(src,format,sr,dst,dr,width,height);
#else
    return false;
#endif
}

bool FormatReinterpreterOpenGL::UploadDepthChecked(GLuint target,PixelFormat format,
    const Common::Rectangle<u32>& rect,const u8* canonical,size_t bytes,u32 stride,u32 width,u32 height) {
#if defined(CARA_IOS_GLES) || defined(__APPLE__)
#if defined(__APPLE__) && !defined(CARA_IOS_GLES)
    if(!MacGPUReinterpretEnabled())return false;
#endif
    if(!depth_encoder)depth_encoder=std::make_unique<GLESDepthStencilToRGBA8>();
    return depth_encoder->UploadDepthChecked(target,format,rect,canonical,bytes,stride,width,height);
#else
    return false;
#endif
}

const std::vector<FormatReinterpreterOpenGL::Reinterpreter>&
FormatReinterpreterOpenGL::GetReinterpreters() {
    return reinterpreters;
}

} // namespace OpenGL
