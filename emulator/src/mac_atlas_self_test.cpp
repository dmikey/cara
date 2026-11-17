// SPDX-License-Identifier: GPL-2.0-or-later
// ROM-free oracle for the atlas compiler path using a real desktop GL4.1 API.
#ifndef CARA_MAC_ATLAS_PIPELINE
#error This oracle requires the isolated Mac atlas build option
#endif
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <dlfcn.h>
#define GL_SILENCE_DEPRECATION
#include <OpenGL/OpenGL.h>
#include <glad/glad.h>
#include "cara/config.h"
#include "common/file_util.h"
#include "common/scope_exit.h"
#include "core/frontend/emu_window.h"
#include "core/memory.h"
#include "core/settings.h"
#include "ios/gpu_state_self_test.h"
#include "ios/gpu_vertex_self_test.h"
#include "video_core/renderer_opengl/gl_rasterizer.h"
#include "video_core/renderer_opengl/gl_shader_manager.h"
#include "video_core/renderer_opengl/gl_state.h"
#include "video_core/renderer_opengl/gl_vars.h"
#include "video_core/video_core.h"
bool RunMacAtlasClipSelfTest(std::string& error);
namespace {
class Window final : public Frontend::EmuWindow {
public:
    Window() { UpdateFramebufferLayout(400,480); }
    void PollEvents() override {}
    void MakeCurrent() override {}
    void DoneCurrent() override {}
    void SwapBuffers() override {}
};
void Require(bool ok,const std::string& why) { if(!ok) throw std::runtime_error(why); }
}
int main(int argc,char** argv) {
    const bool native_copy=argc==2 && std::string(argv[1])=="--native-copy";
    const bool native_remap=argc==2 && std::string(argv[1])=="--native-remap";
    const bool vertex_only=argc==2 && std::string(argv[1])=="--vertex-only";
    const bool clip_only=argc==2 && std::string(argv[1])=="--clip-only";
    const bool shared_coherence=argc==2 && std::string(argv[1])=="--shared-ios-coherence";
    if(argc!=1 && !vertex_only && !shared_coherence && !clip_only && !native_remap && !native_copy) {
        std::cerr<<"usage: atlas oracle [--vertex-only | --clip-only | --shared-ios-coherence | --native-remap | --native-copy]\n";return 2;
    }
    CGLPixelFormatAttribute attributes[]={kCGLPFAAccelerated,kCGLPFAOpenGLProfile,
        static_cast<CGLPixelFormatAttribute>(kCGLOGLPVersion_GL4_Core),
        static_cast<CGLPixelFormatAttribute>(0)};
    CGLPixelFormatObj format{};CGLContextObj context{};GLint count{};
    if(CGLChoosePixelFormat(attributes,&format,&count)!=kCGLNoError || !format) {
        std::cerr<<"FAIL native GL4.1 pixel format unavailable\n";return 2;
    }
    const auto error=CGLCreateContext(format,nullptr,&context);CGLDestroyPixelFormat(format);
    if(error!=kCGLNoError || !context || CGLSetCurrentContext(context)!=kCGLNoError) {
        std::cerr<<"FAIL native GL4.1 context unavailable\n";return 2;
    }
    SCOPE_EXIT({CGLSetCurrentContext(nullptr);CGLDestroyContext(context);});
    static void* library=dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL",RTLD_LAZY|RTLD_LOCAL);
    if(!library || !gladLoadGLLoader([](const char* name)->void*{return dlsym(library,name);})) {
        std::cerr<<"FAIL native GL loader\n";return 2;
    }
    try {
        GLint major{},minor{};
        glGetIntegerv(GL_MAJOR_VERSION,&major);glGetIntegerv(GL_MINOR_VERSION,&minor);
        Require(major>4 || (major==4 && minor>=1),"driver did not provide GL4.1");
        Require(glTexImage2D && glTexSubImage2D && glMapBufferRange && glUnmapBuffer &&
            glTransformFeedbackVaryings && glBeginTransformFeedback && glEndTransformFeedback &&
            glGetQueryObjectuiv && glBindSampler && glClearDepthf && glDepthRangef,
            "required core4.1/ES2-compatibility entry point missing");
        GLint texture_size{},vertex_units{},combined_units{};
        glGetIntegerv(GL_MAX_TEXTURE_SIZE,&texture_size);
        glGetIntegerv(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS,&vertex_units);
        glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS,&combined_units);
        Require(texture_size>=1024 && vertex_units>=3 && combined_units>=10,"insufficient atlas resources");
        std::cout<<"MODE native_gl41_atlas_prototype api_gles=0 GL="<<glGetString(GL_VERSION)
            <<" renderer="<<glGetString(GL_RENDERER)<<" ES3_extension="<<GLAD_GL_ARB_ES3_compatibility
            <<" max_texture="<<texture_size<<" vertex_texture_units="<<vertex_units<<'\n';
        char temporary[]="/tmp/cara-mac-atlas-oracle-XXXXXX";
        Require(mkdtemp(temporary)!=nullptr,"private oracle directory unavailable");
        FileUtil::SetUserPath(std::string(temporary)+"/");
        FileUtil::CreateFullPath(FileUtil::GetUserPath(FileUtil::UserPath::LogDir));
        Config config;Settings::Apply(); // Same guarded preset as the developer game host.
        Require(!Settings::values.use_gles && !Settings::values.use_cpu_jit &&
            !Settings::values.use_shader_jit && Settings::values.use_hw_renderer &&
            Settings::values.use_hw_shader && !Settings::values.use_separable_shader &&
            Settings::values.frame_limit==100,"unexpected native atlas test settings");
        static_assert(sizeof(OpenGL::UniformData)==0x120,"atlas UBO layout mismatch");
        OpenGL::GLES=false;OpenGL::AllowShadow=false;
        OpenGL::OpenGLState initial;OpenGL::OpenGLState::InvalidateRasterState();initial.Apply();
        // Bare CGL has no drawable/default framebuffer. Match the real shell's
        // complete target even for transform-feedback draws with discard set.
        GLuint capture_framebuffer{},capture_color{};
        glGenFramebuffers(1,&capture_framebuffer);glGenTextures(1,&capture_color);
        SCOPE_EXIT({glDeleteFramebuffers(1,&capture_framebuffer);glDeleteTextures(1,&capture_color);});
        const auto old_texture=OpenGL::OpenGLState::BindTexture2D(0,capture_color);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,16,16,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
        OpenGL::OpenGLState::BindTexture2D(0,old_texture);
        OpenGL::OpenGLState::BindDrawFramebuffer(capture_framebuffer);
        OpenGL::OpenGLState::BindReadFramebuffer(capture_framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,capture_color,0);
        Require(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,
            "private oracle framebuffer incomplete");
        Require(glGetError()==GL_NO_ERROR,"private oracle framebuffer setup error");
        std::cout<<"SETUP complete private CGL framebuffer (no window drawable)\n";
        std::string why;
        if (native_remap || native_copy) {
            Memory::MemorySystem memory;Window window;
            Require(VideoCore::Init(window,memory)==VideoCore::ResultStatus::Success,"native remap video init failed");
            SCOPE_EXIT({VideoCore::Shutdown();});
            auto* rasterizer=static_cast<OpenGL::RasterizerOpenGL*>(VideoCore::Rasterizer());
            const bool passed=rasterizer && (native_copy ? rasterizer->RunNativeShaderCopySelfTest(why) :
                rasterizer->RunNativeRGBA8RemapSelfTest(why));
            Require(passed,"actual native remap: "+why);
            std::cout<<"PASS ROM-free actual native cache/remap oracle; no gameplay/timing claim\n";
            return 0;
        }
        if (clip_only) {
            if(!RunMacAtlasClipSelfTest(why)) throw std::runtime_error("actual clip-plane test: "+why);
            return 0;
        }
        if (!vertex_only) {
            if (!RunGPUStateSelfTest(why)) throw std::runtime_error("actual GPU state test: "+why);
            Require(!OpenGL::GLES,"state test changed native API mode");
            std::cout<<"PASS actual GPU state self-test on native GL4.1\n";
        } else std::cout<<"DIAGNOSTIC vertex-only fresh context; state/coherence not exercised\n";
        why.clear();if (!RunGPUVertexSelfTest(why)) throw std::runtime_error("actual GPU vertex test: "+why);
        Require(!OpenGL::GLES,"vertex test changed native API mode");
        std::cout<<"PASS actual GPU vertex self-test on native GL4.1 atlas path\n";
        why.clear();if(!RunMacAtlasClipSelfTest(why)) throw std::runtime_error("actual clip-plane test: "+why);
        if (!shared_coherence) {
            if (!vertex_only) std::cout<<"PASS atlas state+vertex scope; native framebuffer/coherence/depth required separately\n";
            return 0;
        }
        std::cout<<"DIAGNOSTIC shared iOS coherence contract; native GLES-remap eligibility unsupported\n";
        Memory::MemorySystem memory;Window window;
        Require(VideoCore::Init(window,memory)==VideoCore::ResultStatus::Success,"native atlas video init failed");
        SCOPE_EXIT({VideoCore::Shutdown();});
        Require(!OpenGL::GLES && !OpenGL::AllowShadow,"unexpected ES/shadow API path");
        auto* rasterizer=static_cast<OpenGL::RasterizerOpenGL*>(VideoCore::Rasterizer());
        why.clear();if (!rasterizer->RunSurfaceCoherenceSelfTest(why)) throw std::runtime_error("actual surface coherence: "+why);
        Require(glGetError()==GL_NO_ERROR,"oracle ended with a GL error");
        std::cout<<"PASS actual surface coherence; ROM-free, not a gameplay speed result\n";
    } catch(const std::exception& failure) {
        std::cerr<<"FAIL "<<failure.what()<<'\n';return 1;
    }
    return 0;
}
