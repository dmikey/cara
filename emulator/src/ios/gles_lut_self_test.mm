// SPDX-License-Identifier: GPL-2.0-or-later
// ROM-free production GLES LUT oracle. This app has its own bundle/container.
#import <UIKit/UIKit.h>
#import <OpenGLES/EAGL.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <dlfcn.h>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <glad/glad.h>
#include "core/settings.h"
#include "video_core/pica_state.h"
#include "video_core/renderer_opengl/gl_ios_compat.h"
#include "video_core/renderer_opengl/gl_rasterizer.h"
#include "video_core/renderer_opengl/gl_vars.h"

namespace {
using Pixel = std::array<float, 4>;
struct Counts { std::size_t texels{}, components{}, transfers{}, multirow{}, nonzero_x{}, maps{}, negative_controls{}, state_checks{}; } counts;
struct Region { std::uintptr_t first{}, last{}; } source;
bool observing = false;
PFNGLTEXSUBIMAGE2DPROC real_subimage;
PFNGLMAPBUFFERRANGEPROC real_map;
void Require(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
void Clean(const char* where) { const GLenum e=glGetError(); Require(e==GL_NO_ERROR,std::string(where)+" GL error "+std::to_string(e)); }
void* APIENTRY ObserveMap(GLenum target, GLintptr offset, GLsizeiptr size, GLbitfield access) {
    if(observing) ++counts.maps;
    return real_map(target,offset,size,access);
}
void APIENTRY ObserveSubimage(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h,
                                  GLenum format, GLenum type, const void* data) {
    if(observing) {
        GLint pbo{};glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING,&pbo);
        Require(pbo==0,"CPU staging transfer retained an unpack PBO");
        Require(type==GL_FLOAT&&(format==GL_RG||format==GL_RGBA),"Unexpected LUT transfer format");
        const auto first=reinterpret_cast<std::uintptr_t>(data);
        const std::size_t bytes=std::size_t(w)*h*(format==GL_RG?8:16);
        Require(first>=source.first&&first<=source.last&&bytes<=source.last-first,
                "Transfer does not read the registered ordinary owned CPU span");
        ++counts.transfers;if(h>1)++counts.multirow;if(x)++counts.nonzero_x;
    }
    real_subimage(target,level,x,y,w,h,format,type,data);
}
void Own(const void* pointer,std::size_t size) { source={reinterpret_cast<std::uintptr_t>(pointer),reinterpret_cast<std::uintptr_t>(pointer)+size}; }
struct Incoming {
    GLuint unpack{},pack{},array{},sampler{};std::array<GLuint,4> textures{};
    static constexpr std::array<unsigned,4> units{3,4,5,7};
    Incoming() {
        glGenBuffers(1,&unpack);glBindBuffer(GL_PIXEL_UNPACK_BUFFER,unpack);glBufferData(GL_PIXEL_UNPACK_BUFFER,4,nullptr,GL_STREAM_DRAW);
        Require(glMapBufferRange(GL_PIXEL_UNPACK_BUFFER,0,4,GL_MAP_WRITE_BIT)!=nullptr,"Poison PBO mapping failed");
        glGenBuffers(1,&pack);glBindBuffer(GL_PIXEL_PACK_BUFFER,pack);glBufferData(GL_PIXEL_PACK_BUFFER,4,nullptr,GL_STREAM_DRAW);
        glGenBuffers(1,&array);glBindBuffer(GL_ARRAY_BUFFER,array);glBufferData(GL_ARRAY_BUFFER,64,nullptr,GL_STREAM_DRAW);
        glGenSamplers(1,&sampler);glSamplerParameteri(sampler,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        glGenTextures(textures.size(),textures.data());
        for(unsigned i=0;i<units.size();++i){glActiveTexture(GL_TEXTURE0+units[i]);glBindTexture(GL_TEXTURE_2D,textures[i]);glBindSampler(units[i],sampler);}
        glPixelStorei(GL_UNPACK_ALIGNMENT,8);glPixelStorei(GL_UNPACK_ROW_LENGTH,37);
        glPixelStorei(GL_UNPACK_SKIP_ROWS,2);glPixelStorei(GL_UNPACK_SKIP_PIXELS,5);
        glPixelStorei(GL_UNPACK_IMAGE_HEIGHT,19);glPixelStorei(GL_UNPACK_SKIP_IMAGES,3);
        glActiveTexture(GL_TEXTURE7);Clean("poison incoming state");
    }
    ~Incoming(){glBindBuffer(GL_PIXEL_UNPACK_BUFFER,unpack);glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER,0);glBindBuffer(GL_PIXEL_PACK_BUFFER,0);
        glDeleteBuffers(1,&unpack);glDeleteBuffers(1,&pack);glDeleteBuffers(1,&array);
        glDeleteTextures(textures.size(),textures.data());glDeleteSamplers(1,&sampler);}
};
std::vector<GLint> State() {
    std::vector<GLint> out;for(GLenum e:{GL_ACTIVE_TEXTURE,GL_PIXEL_UNPACK_BUFFER_BINDING,GL_PIXEL_PACK_BUFFER_BINDING,
        GL_ARRAY_BUFFER_BINDING,GL_UNPACK_ALIGNMENT,GL_UNPACK_ROW_LENGTH,GL_UNPACK_SKIP_ROWS,GL_UNPACK_SKIP_PIXELS,
        GL_UNPACK_IMAGE_HEIGHT,GL_UNPACK_SKIP_IMAGES}) {GLint v{};glGetIntegerv(e,&v);out.push_back(v);}
    const GLint active=out[0];
    for(unsigned unit:Incoming::units){glActiveTexture(GL_TEXTURE0+unit);GLint v{};glGetIntegerv(GL_TEXTURE_BINDING_2D,&v);out.push_back(v);glGetIntegerv(GL_SAMPLER_BINDING,&v);out.push_back(v);}
    glActiveTexture(active);GLint mapped{};glGetBufferParameteriv(GL_PIXEL_UNPACK_BUFFER,GL_BUFFER_MAPPED,&mapped);out.push_back(mapped);return out;
}
void Transfer(const std::function<void()>& operation) {
    Incoming poison;const auto before=State();const auto maps=counts.maps;observing=true;
    try {operation();observing=false;} catch(...){observing=false;throw;}
    Require(counts.maps==maps,"Default LUT staging called GL MapBufferRange");
    Require(State()==before,"LUT upload did not restore actual poisoned texture/unpack/PBO state");
    ++counts.state_checks;Clean("production upload/state restore");
}
struct Reader {
    GLuint program{},texture{},fbo{},vao{};GLint first{},component{};
    Reader() {
        auto compile=[](GLenum type,const char* text){GLuint s=glCreateShader(type);glShaderSource(s,1,&text,nullptr);glCompileShader(s);GLint ok{};glGetShaderiv(s,GL_COMPILE_STATUS,&ok);if(!ok){char log[2048]{};glGetShaderInfoLog(s,sizeof(log),nullptr,log);throw std::runtime_error(log);}return s;};
        const GLuint vs=compile(GL_VERTEX_SHADER,R"(#version 300 es
precision highp float;void main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2.0-1.0,0.0,1.0);})");
        const GLuint fs=compile(GL_FRAGMENT_SHADER,R"(#version 300 es
precision highp float;precision highp int;uniform highp sampler2D lut;uniform int first;uniform int component;out vec4 color;
void main(){int n=first+int(gl_FragCoord.x);vec4 v=texelFetch(lut,ivec2(n%1024,n/1024),0);
uint b=floatBitsToUint(v[component]);uvec4 bytes=(uvec4(b)>>uvec4(0,8,16,24))&uvec4(255);color=vec4(bytes)/255.0;})");
        program=glCreateProgram();glAttachShader(program,vs);glAttachShader(program,fs);glLinkProgram(program);GLint ok{};glGetProgramiv(program,GL_LINK_STATUS,&ok);Require(ok,"Bit encoder link failed");glDeleteShader(vs);glDeleteShader(fs);
        first=glGetUniformLocation(program,"first");component=glGetUniformLocation(program,"component");
        glGenTextures(1,&texture);glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,texture);glBindBuffer(GL_PIXEL_UNPACK_BUFFER,0);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,256,1,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
        glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
        Require(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"RGBA8 encoder framebuffer incomplete");glGenVertexArrays(1,&vao);Clean("reader setup");
    }
    ~Reader(){glDeleteProgram(program);glDeleteTextures(1,&texture);glDeleteFramebuffers(1,&fbo);glDeleteVertexArrays(1,&vao);}
    bool Matches(GLuint lut,GLint offset,const std::vector<Pixel>& expected,bool account=true) {
        Require(offset>=0,"Missing LUT residency offset");
        glBindFramebuffer(GL_FRAMEBUFFER,fbo);glBindVertexArray(vao);glUseProgram(program);glViewport(0,0,256,1);
        for(GLenum cap:{GL_BLEND,GL_CULL_FACE,GL_DEPTH_TEST,GL_STENCIL_TEST,GL_SCISSOR_TEST,GL_DITHER})glDisable(cap);
        glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,lut);glBindSampler(0,0);glUniform1i(glGetUniformLocation(program,"lut"),0);
        glBindBuffer(GL_PIXEL_PACK_BUFFER,0);glPixelStorei(GL_PACK_ALIGNMENT,1);glPixelStorei(GL_PACK_ROW_LENGTH,0);glPixelStorei(GL_PACK_SKIP_ROWS,0);glPixelStorei(GL_PACK_SKIP_PIXELS,0);glReadBuffer(GL_COLOR_ATTACHMENT0);
        bool same=true;
        for(std::size_t begin=0;begin<expected.size();begin+=256){const auto n=std::min<std::size_t>(256,expected.size()-begin);glUniform1i(first,offset+begin);
            for(unsigned c=0;c<4;++c){glUniform1i(component,c);glDrawArrays(GL_TRIANGLES,0,3);std::array<std::array<unsigned char,4>,256> bytes{};glReadPixels(0,0,n,1,GL_RGBA,GL_UNSIGNED_BYTE,bytes.data());
                for(unsigned i=0;i<n;++i){const auto& b=bytes[i];const std::uint32_t bits=std::uint32_t(b[0])|(std::uint32_t(b[1])<<8)|(std::uint32_t(b[2])<<16)|(std::uint32_t(b[3])<<24);same &= bits==std::bit_cast<std::uint32_t>(expected[begin+i][c]);if(account)++counts.components;}}
        }
        if(account)counts.texels+=expected.size();Clean("float-bit RGBA8 readback");return same;
    }
    void Check(const char* stage,GLuint texture,GLint offset,const std::vector<Pixel>& expected){Require(Matches(texture,offset,expected),std::string(stage)+": GPU float bits differ");}
};
std::vector<Pixel> Solid(unsigned n,Pixel pixel){return std::vector<Pixel>(n,pixel);}
void DirectSpans(Reader& reader) {
    for(GLenum format:{GL_RG,GL_RGBA}) {
        const unsigned channels=format==GL_RG?2:4,stride=channels*4;GLuint texture{};glGenTextures(1,&texture);glActiveTexture(GL_TEXTURE4);glBindTexture(GL_TEXTURE_2D,texture);
        {Incoming poison;glActiveTexture(GL_TEXTURE4);glBindTexture(GL_TEXTURE_2D,texture);GLint before{};glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING,&before);OpenGL::AllocateIOSLUT(format==GL_RG?GL_RG32F:GL_RGBA32F,4096*stride);GLint after{};glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING,&after);Require(before==after,"Allocation did not restore PBO");Clean("poisoned allocation");}
        OpenGL::OGLStreamBuffer stream(GL_ARRAY_BUFFER,4096*stride);
        auto [unused,pad,discard]=stream.MapStaging(1022*stride,16);stream.FinishStaging(1022*stride);
        auto [data,offset,invalid]=stream.MapStaging(2053*stride,16);Require(offset==1022*stride&&!invalid,"Direct span setup offset");Own(data,2053*stride);
        std::vector<Pixel> expected(2053);for(unsigned i=0;i<expected.size();++i){expected[i]={float(int(i%3)-1),float(int((i+1)%3)-1),format==GL_RG?0.0f:float(int((i+2)%3)-1),1};for(unsigned c=0;c<channels;++c)std::memcpy(data+i*stride+c*4,&expected[i][c],4);}
        Transfer([&]{OpenGL::UploadIOSLUT(texture,4,format,offset,data,2053*stride);});stream.FinishStaging(2053*stride);
        reader.Check("row crossing/nonzero offset",texture,1022,expected);
        Require(!reader.Matches(texture,1023,expected,false),"Wrong-offset negative control was not detected");++counts.negative_controls;
        expected[0][0]=2;std::memcpy(data,&expected[0][0],4);
        Require(!reader.Matches(texture,1022,expected,false),"Skipped-upload negative control was not detected");++counts.negative_controls;
        Own(data,2053*stride);Transfer([&]{OpenGL::UploadIOSLUT(texture,4,format,offset,data,2053*stride);});reader.Check("changed first texel",texture,1022,expected);
        Pixel partial{-1,1,0,1};Own(partial.data(),channels*4);Transfer([&]{OpenGL::UploadIOSLUT(texture,4,format,(1022+1025)*stride,reinterpret_cast<const u8*>(partial.data()),stride);});expected[1025]=partial;reader.Check("partial preserves neighbors",texture,1022,expected);
        glDeleteTextures(1,&texture);
    }
}
} // namespace

namespace OpenGL {
struct LUTResidencyTestAccess {
    static void Run(Reader& reader) {
        RasterizerOpenGL r;
        constexpr std::size_t lf_size=8*256*24+8*128,proc_size=8*128*3+16*256*2;
        auto prime=[](OGLStreamBuffer& stream,std::size_t size){auto [data,offset,invalid]=stream.MapStaging(size,16);stream.FinishStaging(0);return data;};
        u8* lf=prime(r.texture_lf_buffer,lf_size);u8* proc=prime(r.texture_buffer,proc_size);
        const auto lf_upload=[&]{Own(lf,lf_size);Transfer([&]{r.SyncAndUploadLUTsLF();});};
        const auto proc_upload=[&]{Own(proc,proc_size);Transfer([&]{r.SyncAndUploadLUTs();});};
        const auto lighting=[&](unsigned i){return r.uniform_block_data.light_data.lighting_lut_offset[i/4][i%4];};
        for(auto& table:Pica::g_state.lighting.luts)for(auto& v:table)v.raw=0;
        for(auto& v:Pica::g_state.lighting.luts[1])v.raw=4095|(2047u<<12)|(1u<<23);
        for(auto& v:Pica::g_state.fog.lut)v.raw=(2047u<<13)|((unsigned(-2047))&8191u);
        lf_upload();
        for(unsigned i=0;i<24;++i)reader.Check("initial lighting residency",r.texture_buffer_lut_lf.handle,lighting(i),Solid(256,i==1?Pixel{1,-1,0,1}:Pixel{0,0,0,1}));
        reader.Check("fog signed",r.texture_buffer_lut_lf.handle,r.uniform_block_data.data.fog_lut_offset,Solid(128,{1,-1,0,1}));
        auto& guest=Pica::g_state.proctex;for(auto& v:guest.noise_table)v.raw=4095;for(auto& v:guest.color_map_table)v.raw=0;for(auto& v:guest.alpha_map_table)v.raw=0;for(auto& v:guest.color_table)v.raw=0;for(auto& v:guest.color_diff_table)v.raw=0;
        proc_upload();auto& u=r.uniform_block_data;
        reader.Check("RG noise",r.texture_buffer_lut_rg.handle,u.data.proctex_noise_lut_offset,Solid(128,{1,0,0,1}));
        reader.Check("RG zero neighbor",r.texture_buffer_lut_rg.handle,u.data.proctex_color_map_offset,Solid(128,{0,0,0,1}));
        reader.Check("RGBA zero color",r.texture_buffer_lut_rgba.handle,u.data.proctex_lut_offset,Solid(256,{0,0,0,0}));
        reader.Check("RGBA zero difference",r.texture_buffer_lut_rgba.handle,u.data.proctex_diff_lut_offset,Solid(256,{0,0,0,0}));
        const auto old_lighting=lighting(0);Pica::g_state.lighting.luts[0][17].raw=4095;u.lighting_lut_dirty[0]=true;u.lighting_lut_dirty_any=true;lf_upload();Require(lighting(0)>old_lighting,"Partial update did not use nonzero ring offset");auto expected=Solid(256,{0,0,0,1});expected[17]={1,0,0,1};reader.Check("lighting partial",r.texture_buffer_lut_lf.handle,lighting(0),expected);
        auto transfers=counts.transfers;u.lighting_lut_dirty[0]=true;u.lighting_lut_dirty_any=true;lf_upload();Require(counts.transfers==transfers,"Unchanged lighting uploaded bytes");reader.Check("unchanged lighting",r.texture_buffer_lut_lf.handle,lighting(0),expected);
        guest.color_table[23].raw=0xff0000ff;u.proctex_lut_dirty=true;proc_upload();auto colors=Solid(256,{0,0,0,0});colors[23]={1,0,0,1};reader.Check("RGBA partial",r.texture_buffer_lut_rgba.handle,u.data.proctex_lut_offset,colors);
        transfers=counts.transfers;u.proctex_lut_dirty=true;proc_upload();Require(counts.transfers==transfers,"Unchanged proctex uploaded bytes");reader.Check("RGBA unchanged",r.texture_buffer_lut_rgba.handle,u.data.proctex_lut_offset,colors);
        // Preserve the current production signed-difference byte conversion.
        guest.color_diff_table[31].raw=0x00ff0000;u.proctex_diff_lut_dirty=true;proc_upload();auto differences=Solid(256,{0,0,0,0});differences[31]={0,0,1,0};reader.Check("RGBA difference partial",r.texture_buffer_lut_rgba.handle,u.data.proctex_diff_lut_offset,differences);
        // Actual production ring wrap; do not pre-reserve again before Sync, which
        // would consume the invalidation being tested. Poison every GPU texel.
        auto poison=[](GLuint texture,unsigned unit,GLenum format,std::size_t bytes){std::vector<float> values(bytes/4,-123.5f);Own(values.data(),bytes);Transfer([&]{UploadIOSLUT(texture,unit,format,0,reinterpret_cast<const u8*>(values.data()),bytes);});};
        poison(r.texture_buffer_lut_lf.handle,3,GL_RG,r.texture_lf_buffer.GetSize());
        poison(r.texture_buffer_lut_rg.handle,4,GL_RG,r.texture_buffer.GetSize());poison(r.texture_buffer_lut_rgba.handle,5,GL_RGBA,r.texture_buffer.GetSize());
        auto [lf_next,lf_offset,lf_invalid]=r.texture_lf_buffer.MapStaging(r.texture_lf_buffer.GetSize(),16);r.texture_lf_buffer.FinishStaging(r.texture_lf_buffer.GetSize());lf=lf_next;
        auto [proc_next,proc_offset,proc_invalid]=r.texture_buffer.MapStaging(r.texture_buffer.GetSize(),16);r.texture_buffer.FinishStaging(r.texture_buffer.GetSize());proc=proc_next;
        Require(lf_invalid&&proc_invalid,"Oracle failed to force actual ring wrap");
        u.fog_lut_dirty=true;lf_upload();u.proctex_lut_dirty=true;proc_upload();
        for(unsigned i=0;i<24;++i)reader.Check("lighting after wrap",r.texture_buffer_lut_lf.handle,lighting(i),i==0?expected:Solid(256,i==1?Pixel{1,-1,0,1}:Pixel{0,0,0,1}));
        reader.Check("fog after wrap",r.texture_buffer_lut_lf.handle,u.data.fog_lut_offset,Solid(128,{1,-1,0,1}));reader.Check("RG view after wrap",r.texture_buffer_lut_rg.handle,u.data.proctex_noise_lut_offset,Solid(128,{1,0,0,1}));reader.Check("RGBA view after wrap",r.texture_buffer_lut_rgba.handle,u.data.proctex_lut_offset,colors);reader.Check("RGBA difference after wrap",r.texture_buffer_lut_rgba.handle,u.data.proctex_diff_lut_offset,differences);
    }
};
} // namespace OpenGL

@interface LUTOracleDelegate : UIResponder <UIApplicationDelegate>
@property(nonatomic,strong) UIWindow* window;
@end
@implementation LUTOracleDelegate
- (BOOL)application:(UIApplication*)application didFinishLaunchingWithOptions:(NSDictionary*)options {
    self.window=[[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];UIViewController* controller=[UIViewController new];controller.view.backgroundColor=UIColor.systemBackgroundColor;UILabel* label=[[UILabel alloc] initWithFrame:CGRectMake(30,80,700,160)];label.numberOfLines=0;label.text=@"Running ROM-free GLES LUT oracle…";[controller.view addSubview:label];self.window.rootViewController=controller;[self.window makeKeyAndVisible];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^{@autoreleasepool{
        NSMutableDictionary* report=[NSMutableDictionary dictionaryWithDictionary:@{@"passed":@NO,@"rom_inputs":@NO,@"performance_claim":@NO,@"upload_mode":@"owned CPU staging; no optional PBO path",@"old_UB_negative_control":@NO,@"source_contract":@"live owned CPU span checked at every transfer; no GL maps during default uploads"}];
        EAGLContext* context=[[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES3];
        try {Require(context&&[EAGLContext setCurrentContext:context],"EAGL ES3 context unavailable");static void* library=dlopen("/System/Library/Frameworks/OpenGLES.framework/OpenGLES",RTLD_NOW|RTLD_LOCAL);Require(gladLoadGLES2Loader([](const char* name){return dlsym(library,name);}),"GLES loader failed");
            report[@"GL_VENDOR"]=[NSString stringWithUTF8String:reinterpret_cast<const char*>(glGetString(GL_VENDOR))];report[@"GL_RENDERER"]=[NSString stringWithUTF8String:reinterpret_cast<const char*>(glGetString(GL_RENDERER))];report[@"GL_VERSION"]=[NSString stringWithUTF8String:reinterpret_cast<const char*>(glGetString(GL_VERSION))];GLint range[2]{},precision{};glGetShaderPrecisionFormat(GL_FRAGMENT_SHADER,GL_HIGH_FLOAT,range,&precision);Require(precision>=23,"Fragment highp lacks exact float32 precision");report[@"fragment_highp_precision"]=@(precision);
            OpenGL::GLES=true;OpenGL::AllowShadow=false;Settings::values.use_gles=true;Settings::values.use_separable_shader=false;Settings::values.use_shader_cache=false;Settings::values.custom_textures=false;
            real_subimage=glad_glTexSubImage2D;real_map=glad_glMapBufferRange;glad_glTexSubImage2D=ObserveSubimage;glad_glMapBufferRange=ObserveMap;
            {Reader reader;DirectSpans(reader);NSMutableArray* modes=[NSMutableArray new];
                for(bool hack:{false,true}){const auto begin=counts.texels,transfers=counts.transfers;Settings::values.stream_buffer_hack=hack;
                    for(unsigned reconstruction=0;reconstruction<2;++reconstruction){Pica::g_state.Reset();OpenGL::OpenGLState initial;OpenGL::OpenGLState::InvalidateRasterState();initial.Apply();OpenGL::LUTResidencyTestAccess::Run(reader);}
                    [modes addObject:@{@"stream_buffer_hack":@(hack),@"reconstructions":@2,@"checked_texels":@(counts.texels-begin),@"transfer_calls":@(counts.transfers-transfers)}];}
                report[@"modes"]=modes;
            }
            Clean("oracle completion");Require(counts.multirow&&counts.nonzero_x&&counts.negative_controls==4,"Required oracle branch coverage missing");report[@"passed"]=@YES;
        } catch(const std::exception& error){report[@"error"]=[NSString stringWithUTF8String:error.what()];}
        if(real_subimage)glad_glTexSubImage2D=real_subimage;if(real_map)glad_glMapBufferRange=real_map;
        report[@"checked_texels"]=@(counts.texels);report[@"checked_components"]=@(counts.components);report[@"transfer_calls"]=@(counts.transfers);report[@"multirow_calls"]=@(counts.multirow);report[@"nonzero_x_calls"]=@(counts.nonzero_x);report[@"GL_maps_during_default_upload"]=@(counts.maps);report[@"negative_controls_detected"]=@(counts.negative_controls);report[@"poisoned_state_checks"]=@(counts.state_checks);
        NSString* documents=NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,NSUserDomainMask,YES).firstObject;NSData* data=[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:nil];[data writeToFile:[documents stringByAppendingPathComponent:@"gles-lut-oracle.json"] options:NSDataWritingAtomic error:nil];[EAGLContext setCurrentContext:nil];
        dispatch_async(dispatch_get_main_queue(),^{label.text=[report[@"passed"] boolValue]?@"PASS — GLES LUT oracle completed": [@"FAIL — " stringByAppendingString:report[@"error"]?:@"unknown error"];});
    }});return YES;
}
@end
int main(int argc,char** argv){@autoreleasepool{return UIApplicationMain(argc,argv,nil,NSStringFromClass(LUTOracleDelegate.class));}}
