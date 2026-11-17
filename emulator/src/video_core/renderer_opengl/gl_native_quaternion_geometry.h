// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>
namespace OpenGL::NativeQuaternionGeometry {
struct Limits {int major=0,minor=0,vertices=0,input=0,output=0,total=0,clips=0,vertex_output=0;};
inline bool Supported(bool desktop,bool separable,const Limits& c) {
 return desktop&&!separable&&(c.major>4||(c.major==4&&c.minor>=1))&&c.vertices>=3&&
        c.input>=24&&c.output>=24&&c.total>=72&&c.clips>=2&&c.vertex_output>=24;
}
inline bool Selected(bool enabled,unsigned indexed_mode){return enabled&&indexed_mode<3;}
inline bool TwoStageCache(bool configured,bool enabled){return configured&&!enabled;}
inline constexpr unsigned SourceOption=0x4e475331;
inline std::string Source() {return R"(layout(triangles) in;
layout(triangle_strip,max_vertices=3) out;
in vec4 vs_primary_color[];
out vec4 primary_color;
in vec2 vs_texcoord0[];
out vec2 texcoord0;
in vec2 vs_texcoord1[];
out vec2 texcoord1;
in vec2 vs_texcoord2[];
out vec2 texcoord2;
in float vs_texcoord0_w[];
out float texcoord0_w;
in vec4 vs_normquat[];
out vec4 normquat;
in vec3 vs_view[];
out vec3 view;
void main() {
    vec4 q0=vs_normquat[0];
    for(int i=0;i<3;++i) {
        primary_color=vs_primary_color[i];
        texcoord0=vs_texcoord0[i];
        texcoord1=vs_texcoord1[i];
        texcoord2=vs_texcoord2[i];
        texcoord0_w=vs_texcoord0_w[i];
        view=vs_view[i];
        normquat=vs_normquat[i];
        if(i>0 && dot(q0,normquat)<0.0) normquat=-normquat;
        gl_Position=gl_in[i].gl_Position;
        gl_ClipDistance[0]=gl_in[i].gl_ClipDistance[0];
        gl_ClipDistance[1]=gl_in[i].gl_ClipDistance[1];
        EmitVertex();
    }
    EndPrimitive();
}
)";}
}
