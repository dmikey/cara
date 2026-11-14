// SPDX-License-Identifier: GPL-2.0-or-later
// Original ROM-free test. Requires a real native macOS OpenGL context.
#include <cstdlib>
#include <iostream>
#include <string>
#include <dlfcn.h>
#define GL_SILENCE_DEPRECATION
#include <OpenGL/OpenGL.h>
#include <glad/glad.h>
#include "video_core/renderer_opengl/gl_format_reinterpreter.h"
#include "video_core/renderer_opengl/gl_vars.h"
#include "video_core/renderer_opengl/gl_state.h"
int main() {
    CGLPixelFormatAttribute attrs[]={kCGLPFAOpenGLProfile,
        static_cast<CGLPixelFormatAttribute>(kCGLOGLPVersion_3_2_Core),
        static_cast<CGLPixelFormatAttribute>(0)};
    CGLPixelFormatObj format{};CGLContextObj context{};GLint count{};
    if(CGLChoosePixelFormat(attrs,&format,&count)!=kCGLNoError || !format)return 2;
    const auto error=CGLCreateContext(format,nullptr,&context);CGLDestroyPixelFormat(format);
    if(error!=kCGLNoError || !context || CGLSetCurrentContext(context)!=kCGLNoError)return 2;
    void* lib=dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL",RTLD_LAZY|RTLD_LOCAL);
    static void* symbols=lib;
    if(!lib || !gladLoadGLLoader([](const char* name)->void*{return dlsym(symbols,name);}))return 2;
    OpenGL::GLES=false;OpenGL::AllowShadow=false;
    std::cout<<"GL="<<glGetString(GL_VERSION)<<" renderer="<<glGetString(GL_RENDERER)
             <<" stencil_texturing="<<GLAD_GL_ARB_stencil_texturing<<'\n'<<std::flush;
    OpenGL::OpenGLState initial;OpenGL::OpenGLState::InvalidateRasterState();initial.Apply();
    std::string why;const bool passed=OpenGL::RunDepthReinterpretSelfTest(why);
    std::cout<<(passed?"PASS exact D16/all D24 codes/stencil/native-PBO bytes and GL state":"FAIL "+why)<<'\n';
    CGLSetCurrentContext(nullptr);CGLDestroyContext(context);return passed?0:1;
}
