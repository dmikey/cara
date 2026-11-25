// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_opengl/gl_shader_unused_quaternion.h"
#include <cassert>
#include <iostream>
#include <string>
using namespace OpenGL::UnusedQuaternion;
int main() {
    static_assert(ResolveRequest(true,true) && !ResolveRequest(false,true) && !ResolveRequest(true,false));
    constexpr auto language = "OpenGL ES GLSL ES 3.20";
    constexpr auto gc = Resolve(true, true, "GR0006", "GR0006", "Qualcomm", "Adreno (TM) 618", GCloudDriver, language);
    constexpr auto rg = Resolve(true, true, "RG Slide", "ums9620_2h10_native", "ARM", "Mali-G57", SlideDriver, language);
    static_assert(gc.target && gc.enabled && gc.device == Device::GCloud);
    static_assert(rg.target && rg.enabled && rg.device == Device::RGSlide);
    static_assert(RequestedByDefault("GR0006") && RequestedByDefault("RG Slide") && !RequestedByDefault("Other"));
    static_assert(!Resolve(false,true,"GR0006","GR0006","Qualcomm","Adreno (TM) 618",GCloudDriver,language).enabled);
    static_assert(!Resolve(false,true,"RG Slide","ums9620_2h10_native","ARM","Mali-G57",SlideDriver,language).enabled);
    static_assert(!Resolve(true,false,"GR0006","GR0006","Qualcomm","Adreno (TM) 618",GCloudDriver,language).target);
    static_assert(!Resolve(true,true,"GR0006","GR0006","Qualcomm","Adreno (TM) 618","different",language).target);
    static_assert(!Resolve(true,true,"RG Slide","ums9620_2h10","ARM","Mali-G57",SlideDriver,language).target);
    static_assert(!Resolve(true,true,"RG Slide","ums9620_2h10_native","ARM","Mali-G52",SlideDriver,language).target);
    static_assert(!Resolve(true,true,"RG Slide","ums9620_2h10_native","ARM","Mali-G57",SlideDriver,"unknown").target);
    static_assert(CacheTarget(gc) != CacheTarget(rg));
    constexpr auto rg_off=Resolve(false,true,"RG Slide","ums9620_2h10_native","ARM","Mali-G57",SlideDriver,language);
    constexpr auto gc_off=Resolve(false,true,"GR0006","GR0006","Qualcomm","Adreno (TM) 618",GCloudDriver,language);
    static_assert(CacheIdentity(rg_off).empty() && CacheSuffix(rg_off).empty());
    static_assert(!CacheIdentity(rg).empty() && !CacheSuffix(rg).empty());
    static_assert(CacheIdentity(gc)=="\nadreno618-unused-quaternion-v1=1");
    static_assert(CacheIdentity(gc_off)=="\nadreno618-unused-quaternion-v1=0");
    static_assert(CacheSuffix(gc)==".adreno618-uq-v1-on" && CacheSuffix(gc_off)==".adreno618-uq-v1-off");
    static_assert(CacheIdentity({}).empty() && CacheSuffix({}).empty());
    assert(NoRead("in vec4 normquat;\nvoid main() {}"));
    assert(!NoRead("in vec4 normquat;\nvoid main() { frag=normquat; }"));
    assert(!NoRead("#define X(a,b) a ## b\nin vec4 normquat;"));
    // Entirely authored source fixture; no retail shader/source/ROM data.
    std::string prefix="vec4 vs_in_reg0;\nvec4 reg_tmp0;\n";
    for(int i=0;i<16;++i) prefix+="vec4 vs_out_attr"+std::to_string(i)+";\n";
    prefix+="void cara_load_vertex(int vertex_id) {\nvs_in_reg0 = cara_decode_vertex(input_vertex_id, 0);\n}\nvoid cara_run_vertex(int vertex_id) {\n";
    prefix+="conditional_code = bvec2(false);\naddress_registers = ivec3(0);\ncara_load_vertex(vertex_id);\nreg_tmp0 = vec4(0.0, 0.0, 0.0, 1.0);\n";
    for(int i=0;i<16;++i) prefix+="vs_out_attr"+std::to_string(i)+" = vec4(0.0, 0.0, 0.0, 1.0);\n";
    prefix+="exec_shader();\n}\n";
    const std::string selected=prefix+"void main() {\ncara_run_vertex(gl_VertexID);\ngl_Position = vs_out_attr0;\n}\n";
    const std::string original=prefix+"void main() {\nvec4 first_quat=vec4(0.0);\nif (gl_VertexID % 3 != 0) { cara_run_vertex(gl_VertexID - gl_VertexID % 3); first_quat = vs_out_attr1; }\ncara_run_vertex(gl_VertexID);\nnormquat = vs_out_attr1;\nif (gl_VertexID % 3 != 0 && dot(first_quat, normquat) < 0.0) normquat = -normquat;\ngl_Position = vs_out_attr0;\n}\n";
    assert(VertexDelta(original,selected));
    assert(!VertexDelta(original,selected+"// different\n"));
    auto altered=selected;altered.replace(altered.find("gl_Position = vs_out_attr0"),25,"gl_Position = vs_out_attr1");assert(!VertexDelta(original,altered));
    altered=selected;altered.replace(altered.find("conditional_code = bvec2(false);"),31,"conditional_code = bvec2(true);");assert(!VertexDelta(original,altered));
    std::cout<<"Android unused quaternion target/default/cache/source contract PASS\n";
}
