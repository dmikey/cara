// Copyright 2026 Cara contributors. GPLv2 or later; see license.txt.
#include "video_core/renderer_opengl/gl_host_latency.h"
#include "video_core/renderer_opengl/gl_pipeline_capture.h"
#include <cassert>
#include <cstdlib>
#include <filesystem>
int main(int argc, char** argv) {
    assert(argc==2);const std::string path=argv[1];assert(!std::filesystem::exists(path));
    setenv("CARA_GL_HOST_LATENCY","1",1);setenv("CARA_GL_PIPELINE_CAPTURE","1",1);
    using namespace OpenGL;
    assert(!HostLatency::Requested() && !PipelineCapture::Requested());
    assert(HostLatency::ReadHostTick()==0);
    HostLatency::Recorder timer(path);timer.BindOwnerThread();
    auto token=timer.Begin(HostLatency::Api::DrawArrays);assert(!token.active && !timer.CanCopyStatus(token));
    timer.End(token,1);timer.CreateShader(1,1);timer.Source(1);timer.DeleteShader(1);
    timer.CreateProgram(1);timer.Link(1);timer.Use(1);timer.DeleteProgram(1);timer.BindVAO(1);timer.BindFramebuffer(1,1);
    PipelineCapture::Recorder capture(path);capture.CreateShader(1,1);capture.Source(1,0,nullptr,nullptr);
    capture.Compile(1);capture.ShaderStatus(1,1);capture.DeleteShader(1);capture.CreateProgram(1);
    capture.Attach(1,1,true);capture.Link(1);capture.ProgramStatus(1,1);capture.Use(1);capture.DeleteProgram(1);
    capture.BindVAO(1);capture.BindFramebuffer(1,1);capture.Draw(1,1,0,1,0,0,0,0,0);capture.DrawReturned();
    assert(!std::filesystem::exists(path));
}
