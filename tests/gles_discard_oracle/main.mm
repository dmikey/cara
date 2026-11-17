// SPDX-License-Identifier: GPL-2.0-or-later
// ROM-free EAGL ES3 API probe; deliberately independent of emulator shader generation.
#import <UIKit/UIKit.h>
#import <OpenGLES/EAGL.h>
#import <OpenGLES/ES3/gl.h>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>
#include <map>

namespace {
constexpr int W=32,H=24,N=W*H;
using Color=std::array<unsigned char,4>;
using Image=std::array<Color,N>;
unsigned checks=0,stages=0;
std::map<std::string,std::pair<unsigned,unsigned>> errors_by_stage;
template<class T> NSString* Bytes(const T& a){return [[NSData dataWithBytes:a.data() length:sizeof(a)] base64EncodedStringWithOptions:0];}
void Require(bool good,const std::string& message){++checks;if(!good)throw std::runtime_error(message);}
void Clean(const char* stage){++stages;const auto error=glGetError();auto& counts=errors_by_stage[stage];++counts.first;counts.second+=error!=GL_NO_ERROR;Require(error==GL_NO_ERROR,std::string(stage)+" GLerror="+std::to_string(error));}
GLuint Compile(GLenum type,const std::string& source){GLuint s=glCreateShader(type);const char* p=source.c_str();glShaderSource(s,1,&p,nullptr);glCompileShader(s);GLint ok{};glGetShaderiv(s,GL_COMPILE_STATUS,&ok);if(!ok){char log[4096]{};glGetShaderInfoLog(s,sizeof log,nullptr,log);throw std::runtime_error(log);}return s;}
std::string VertexSource(bool quad){return quad ?
    "#version 300 es\nvoid main(){vec2 p[6]=vec2[6](vec2(-1,-1),vec2(1,-1),vec2(-1,1),vec2(-1,1),vec2(1,-1),vec2(1,1));gl_Position=vec4(p[gl_VertexID],0,1);}" :
    "#version 300 es\nvoid main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2.0-1.0,0,1);}";
}
GLuint Program(const std::string& body,bool quad=true){
    const auto vs=Compile(GL_VERTEX_SHADER,VertexSource(quad));
    const auto fs=Compile(GL_FRAGMENT_SHADER,"#version 300 es\nprecision highp float;precision highp int;out vec4 color;uniform float test_depth;\nvoid main(){"+body+"}\n");
    GLuint p=glCreateProgram();glAttachShader(p,vs);glAttachShader(p,fs);glLinkProgram(p);GLint ok{};glGetProgramiv(p,GL_LINK_STATUS,&ok);glDeleteShader(vs);glDeleteShader(fs);Require(ok,"program link failed");return p;
}
struct Probe {
    GLuint target{},reader{},color{},reader_color{},ds{},vao{},read_program{};
    std::array<GLuint,3> programs{};
    bool quad;
    std::array<GLint,4> actual_viewport{},actual_scissor{};
    Probe(bool q):quad(q){
        glGenVertexArrays(1,&vao);glBindVertexArray(vao);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER,0);glPixelStorei(GL_UNPACK_ALIGNMENT,1);glPixelStorei(GL_UNPACK_ROW_LENGTH,0);glPixelStorei(GL_UNPACK_SKIP_ROWS,0);glPixelStorei(GL_UNPACK_SKIP_PIXELS,0);
        glGenRenderbuffers(1,&ds);glBindRenderbuffer(GL_RENDERBUFFER,ds);glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH24_STENCIL8,W,H);
        for(auto pair:{std::pair<GLuint*,GLuint*>{&target,&color},{&reader,&reader_color}}){
            glGenTextures(1,pair.second);glBindTexture(GL_TEXTURE_2D,*pair.second);glTexStorage2D(GL_TEXTURE_2D,1,GL_RGBA8,W,H);
            glGenFramebuffers(1,pair.first);glBindFramebuffer(GL_FRAMEBUFFER,*pair.first);
            glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,*pair.second,0);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_STENCIL_ATTACHMENT,GL_RENDERBUFFER,ds);
            Require(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"framebuffer incomplete");
        }
        const std::string writes="color=vec4(0.8,0.6,0.4,1.0);gl_FragDepth=0.25;";
        programs[0]=Program(writes,quad);
        programs[1]=Program("discard;",quad);
        programs[2]=Program("if(!(gl_FragCoord.x>=6.0&&gl_FragCoord.y>=7.0&&gl_FragCoord.x<18.0&&gl_FragCoord.y<16.0))discard;"+writes,quad);
        read_program=Program("color=vec4(1.0);gl_FragDepth=test_depth;");
        Clean("construction");
    }
    ~Probe(){for(auto p:programs)glDeleteProgram(p);glDeleteProgram(read_program);glDeleteTextures(1,&color);glDeleteTextures(1,&reader_color);glDeleteRenderbuffers(1,&ds);glDeleteFramebuffers(1,&target);glDeleteFramebuffers(1,&reader);glDeleteVertexArrays(1,&vao);}
    Image ReadColor(GLuint fbo){
        glBindFramebuffer(GL_READ_FRAMEBUFFER,fbo);glReadBuffer(GL_COLOR_ATTACHMENT0);
        glBindBuffer(GL_PIXEL_PACK_BUFFER,0);glPixelStorei(GL_PACK_ALIGNMENT,1);glPixelStorei(GL_PACK_ROW_LENGTH,0);glPixelStorei(GL_PACK_SKIP_ROWS,0);glPixelStorei(GL_PACK_SKIP_PIXELS,0);
        Image pixels{};glReadPixels(0,0,W,H,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());Clean("RGBA8 readback");return pixels;
    }
    void ReaderSetup(){
        glBindFramebuffer(GL_FRAMEBUFFER,reader);glViewport(0,0,W,H);glBindVertexArray(vao);glUseProgram(read_program);
        for(auto cap:{GL_SCISSOR_TEST,GL_BLEND,GL_CULL_FACE,GL_DITHER,GL_SAMPLE_ALPHA_TO_COVERAGE,GL_SAMPLE_COVERAGE})glDisable(cap);
        glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);glDepthMask(GL_FALSE);glStencilMask(0);glStencilOp(GL_KEEP,GL_KEEP,GL_KEEP);
    }
    Image ReadTest(){const GLfloat zero[4]{};glClearBufferfv(GL_COLOR,0,zero);glDrawArrays(GL_TRIANGLES,0,6);Clean("read-only classification draw");return ReadColor(reader);}
    std::array<unsigned char,N> Stencil(){
        ReaderSetup();glDisable(GL_DEPTH_TEST);glEnable(GL_STENCIL_TEST);
        std::array<unsigned char,N> result{};
        for(unsigned bit=0;bit<8;++bit){glStencilFunc(GL_EQUAL,1<<bit,1<<bit);const auto image=ReadTest();for(unsigned i=0;i<N;++i){Require(image[i][0]==0||image[i][0]==255,"nonbinary stencil classifier");if(image[i][0])result[i]|=1<<bit;}}
        Clean("stencil classification complete");return result;
    }
    std::array<unsigned char,N> Depth(){
        ReaderSetup();glDisable(GL_STENCIL_TEST);glEnable(GL_DEPTH_TEST);glDepthFunc(GL_EQUAL);
        glUniform1f(glGetUniformLocation(read_program,"test_depth"),1.0f);const auto sentinel=ReadTest();
        glUniform1f(glGetUniformLocation(read_program,"test_depth"),0.25f);const auto drawn=ReadTest();
        std::array<unsigned char,N> result{};
        for(unsigned i=0;i<N;++i){Require((sentinel[i][0]==255) != (drawn[i][0]==255),"unknown or ambiguous depth value");result[i]=drawn[i][0]==255;}
        Clean("depth classification complete");return result;
    }
    void Reset(){
        glBindFramebuffer(GL_FRAMEBUFFER,target);glDisable(GL_SCISSOR_TEST);glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);glDepthMask(GL_TRUE);glStencilMask(0xff);
        const GLfloat initial[]{13.f/255,29.f/255,47.f/255,61.f/255};glClearBufferfv(GL_COLOR,0,initial);glClearBufferfi(GL_DEPTH_STENCIL,0,1.0f,0x5a);Clean("sentinel clear");
    }
    void Variant(unsigned v,GLuint program,int clip){
        glBindFramebuffer(GL_FRAMEBUFFER,target);glBindVertexArray(vao);glUseProgram(program);glViewport(0,0,W,H);
        for(auto cap:{GL_BLEND,GL_CULL_FACE,GL_DITHER,GL_SAMPLE_ALPHA_TO_COVERAGE,GL_SAMPLE_COVERAGE})glDisable(cap);
        glEnable(GL_SCISSOR_TEST);glScissor(4,5,16,12);if(clip==1)glScissor(6,7,12,9);if(clip==2)glScissor(0,0,0,0);
        const std::array<GLenum,8> ops{GL_KEEP,GL_ZERO,GL_REPLACE,GL_INCR,GL_DECR,GL_INVERT,GL_INCR_WRAP,GL_DECR_WRAP};
        glEnable(GL_DEPTH_TEST);glDepthFunc(v==3?GL_NEVER:GL_ALWAYS);glDepthMask(v%3==0);
        glEnable(GL_STENCIL_TEST);glStencilFunc(v==2?GL_NEVER:GL_ALWAYS,0x3c,0xff);glStencilMask(v==4?0:v==6?0x55:0xff);glStencilOp(ops[v%8],ops[v%8],ops[v%8]);
        glColorMask(v!=1,v%2==0,v!=1,v%2==0);
        if(v>=8){glColorMask(v==10||v==12,v==10||v==12,v==10||v==12,v==10||v==12);glDepthMask(v==11||v==12);glDepthFunc(GL_ALWAYS);glStencilFunc(GL_ALWAYS,0x3c,0xff);glStencilMask(0xff);glStencilOp(v==9?GL_KEEP:GL_ZERO,v==9?GL_KEEP:GL_ZERO,v==9?GL_KEEP:GL_ZERO);if(v==8)glDisable(GL_STENCIL_TEST);}
        glGetIntegerv(GL_VIEWPORT,actual_viewport.data());glGetIntegerv(GL_SCISSOR_BOX,actual_scissor.data());
        Require(actual_viewport==std::array<GLint,4>{0,0,W,H},"actual viewport differs");
        const std::array<GLint,4> wanted=clip==1?std::array<GLint,4>{6,7,12,9}:clip==2?std::array<GLint,4>{0,0,0,0}:std::array<GLint,4>{4,5,16,12};
        Require(glIsEnabled(GL_SCISSOR_TEST)&&actual_scissor==wanted,"actual scissor differs");
        glDrawArrays(GL_TRIANGLES,0,quad?6:3);Clean("test draw");
    }
};
NSMutableDictionary* Run(){
    NSMutableDictionary* report=[@{@"measurement_completed":@NO,@"rom_inputs":@NO,@"shader_family":@"minimal hand-written GLSL ES 3.00; not emulator-generated",@"direct_depth_stencil_readpixels":@NO,@"readback_method":@"RGBA8 probe FBO shares D24S8 renderbuffer; 8 stencil bit tests KEEP/write_mask0, depth EQUAL tests at1 and0.25/depth_mask0; no DS readpixels"} mutableCopy];
    report[@"GL_VENDOR"]=[NSString stringWithUTF8String:(const char*)glGetString(GL_VENDOR)];report[@"GL_RENDERER"]=[NSString stringWithUTF8String:(const char*)glGetString(GL_RENDERER)];report[@"GL_VERSION"]=[NSString stringWithUTF8String:(const char*)glGetString(GL_VERSION)];
    report[@"oversized_triangle_vertex_source"]=[NSString stringWithUTF8String:VertexSource(false).c_str()];report[@"quad_vertex_source"]=[NSString stringWithUTF8String:VertexSource(true).c_str()];
    NSMutableArray* cases=[NSMutableArray array];unsigned mismatches=0;
    constexpr std::array<unsigned,13> color_expected{108,0,0,0,108,108,108,108,0,0,108,0,108};
    constexpr std::array<unsigned,13> depth_expected{108,0,0,0,0,0,108,0,0,0,0,108,108};
    constexpr std::array<unsigned,13> stencil_expected{0,108,108,108,0,108,108,108,0,0,108,108,108};
    const std::array<unsigned char,13> stencil_value{0x5a,0,0x3c,0x5b,0x5a,0xa5,0x5b,0x59,0x5a,0x5a,0,0,0};
    for(bool quad:{false,true}){Probe p(quad);
    for(unsigned mode=0;mode<6;++mode)for(unsigned variant=0;variant<13;++variant){
        p.Reset();const auto before_color=p.ReadColor(p.target);const auto before_s=p.Stencil();const auto before_d=p.Depth();
        for(unsigned i=0;i<N;++i){Require(before_color[i]==Color{13,29,47,61},"color sentinel mismatch");Require(before_s[i]==0x5a,"stencil sentinel mismatch");Require(before_d[i]==0,"depth sentinel mismatch");}
        p.Variant(variant,p.programs[mode==0||mode==5?0:mode==1?1:2],mode==3||mode==5?1:mode==4?2:0);
        const auto after_color=p.ReadColor(p.target);const auto after_s=p.Stencil();const auto after_d=p.Depth();
        // Re-read classifications in reverse order to witness read passes not
        // changing either attachment, even across the other plane's tests.
        Require(after_d==p.Depth() && after_s==p.Stencil(),"read-only classification changed DS");
        unsigned c=0,d=0,s=0,escape=0,wrong_s=0;
        for(int y=0;y<H;++y)for(int x=0;x<W;++x){const auto i=y*W+x;const bool cc=before_color[i]!=after_color[i],dd=before_d[i]!=after_d[i],ss=before_s[i]!=after_s[i];c+=cc;d+=dd;s+=ss;
            const bool inside=x>=6&&x<18&&y>=7&&y<16;if((cc||dd||ss)&&!inside)++escape;
            if(ss && after_s[i]!=stencil_value[variant])++wrong_s;
        }
        const unsigned factor=mode==0?192:108;
        const unsigned ec=(mode==1||mode==4)?0:color_expected[variant]*factor/108;
        const unsigned ed=(mode==1||mode==4)?0:depth_expected[variant]*factor/108;
        const unsigned es=(mode==1||mode==4)?0:stencil_expected[variant]*factor/108;
        const bool expected=c==ec&&d==ed&&s==es&&!wrong_s;mismatches+=!expected;
        [cases addObject:@{@"mode":@[@"pass",@"unconditional_discard",@"conditional_rectangle",@"conditional_host_include",@"conditional_host_empty",@"pass_host_include"][mode],@"geometry":quad?@"explicit_two_triangle_quad":@"oversized_triangle",@"draw_vertex_count":@(quad?6:3),@"actual_viewport":@[@(p.actual_viewport[0]),@(p.actual_viewport[1]),@(p.actual_viewport[2]),@(p.actual_viewport[3])],@"actual_scissor":@[@(p.actual_scissor[0]),@(p.actual_scissor[1]),@(p.actual_scissor[2]),@(p.actual_scissor[3])],@"direct_color_captured_before_DS_classification":@YES,@"variant":@(variant),@"changed_color":@(c),@"changed_depth":@(d),@"changed_stencil":@(s),@"changed_outside_rectangle":@(escape),@"unexpected_changed_stencil_values":@(wrong_s),@"expected_color":@(ec),@"expected_depth":@(ed),@"expected_stencil":@(es),@"matches_spec_expected":@(expected),@"sentinels_verified":@YES,@"read_passes_preserved_DS":@YES,@"before_color_rgba8_base64":Bytes(before_color),@"after_color_rgba8_base64":Bytes(after_color),@"before_stencil_u8_base64":Bytes(before_s),@"after_stencil_u8_base64":Bytes(after_s),@"before_depth_class_u8_base64":Bytes(before_d),@"after_depth_class_u8_base64":Bytes(after_d)}];
    }
    }
    Clean("completion");NSMutableDictionary* stage_report=[NSMutableDictionary dictionary];for(const auto& [name,count]:errors_by_stage)stage_report[[NSString stringWithUTF8String:name.c_str()]]=@{@"checks":@(count.first),@"errors":@(count.second)};report[@"gl_errors_by_stage"]=stage_report;report[@"cases"]=cases;report[@"spec_expected_mismatch_cases"]=@(mismatches);report[@"measurement_completed"]=@YES;report[@"checks"]=@(checks);report[@"GL_error_stages"]=@(stages);return report;
}
}
@interface DiscardOracleSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic,strong) UIWindow* window;
@end
@implementation DiscardOracleSceneDelegate
- (void)scene:(UIScene*)scene willConnectToSession:(UISceneSession*)session options:(UISceneConnectionOptions*)options {
    self.window=[[UIWindow alloc]initWithWindowScene:(UIWindowScene*)scene];UIViewController* vc=[UIViewController new];vc.view.backgroundColor=UIColor.blackColor;UILabel* label=[[UILabel alloc]initWithFrame:CGRectMake(20,80,700,100)];label.textColor=UIColor.whiteColor;label.numberOfLines=0;label.text=@"Running ROM-free GLES discard probe";[vc.view addSubview:label];self.window.rootViewController=vc;[self.window makeKeyAndVisible];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^{@autoreleasepool{
        NSMutableDictionary* report;EAGLContext* context=[[EAGLContext alloc]initWithAPI:kEAGLRenderingAPIOpenGLES3];
        try{Require(context && [EAGLContext setCurrentContext:context],"EAGL ES3 unavailable");report=Run();}catch(const std::exception&e){report=[@{@"measurement_completed":@NO,@"error":[NSString stringWithUTF8String:e.what()]} mutableCopy];}
        NSString* documents=NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,NSUserDomainMask,YES).firstObject;NSData* data=[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:nil];[data writeToFile:[documents stringByAppendingPathComponent:@"gles-discard-oracle.json"] options:NSDataWritingAtomic error:nil];[EAGLContext setCurrentContext:nil];
        dispatch_async(dispatch_get_main_queue(),^{label.text=[report[@"measurement_completed"] boolValue]?@"Completed GLES discard probe":report[@"error"];});
    }});
}
@end
@interface DiscardOracleDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation DiscardOracleDelegate
- (BOOL)application:(UIApplication*)application didFinishLaunchingWithOptions:(NSDictionary*)options{return YES;}
@end
int main(int argc,char**argv){@autoreleasepool{return UIApplicationMain(argc,argv,nil,NSStringFromClass(DiscardOracleDelegate.class));}}
