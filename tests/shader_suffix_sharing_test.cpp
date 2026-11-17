// Copyright 2026 Cara contributors. GPLv2 or later; see license.txt.
// Authored generated-subset CPU fixtures; no captured game shader or GL context.
#include "gl_shader_suffix_sharing.h"
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
using namespace OpenGL::ShaderSuffixSharing;
static void CheckAt(bool ok,int line) { if(!ok) throw std::runtime_error("source fixture failed at line "+std::to_string(line)); }
#define Check(ok) CheckAt((ok),__LINE__)
static std::string Replace(std::string s,const std::string& from,const std::string& to) { auto p=s.find(from);Check(p!=s.npos);s.replace(p,from.size(),to);return s; }
int main() {
    Check(!Requested(nullptr,"1") && !Requested("0","1") && !Requested("true","1"));
    Check(!Requested("1",nullptr) && !Requested("1","0") && Requested("1","1"));
    const std::string fragment="void main() { }\n";
    Check(Apply(fragment)==fragment);
    const std::string src=R"GLSL(bool sub_1_3();
bool sub_2_3();
bool sub_1_3() {
    // 1: mov
    reg_tmp0 = vec4(1.0);
    // 2: mov
    reg_tmp1 = reg_tmp0;
    return false;
}
bool sub_2_3() {
    // 2: mov
    reg_tmp1 = reg_tmp0;
    return false;
}
)GLSL";
    Check(Apply(src)==Replace(src,"    // 2: mov\n    reg_tmp1 = reg_tmp0;\n    return false;\n}","    return sub_2_3();\n}"));
    int negative_case=0;
    for (const auto& bad: {
        Replace(src,"reg_tmp0 =","vec4 reg_tmp0 ="),
        Replace(src,"reg_tmp0 =","Custom reg_tmp0 ="),
        Replace(src,"reg_tmp0 = vec4(1.0);","if (true) {}"),
        Replace(src,"reg_tmp0 = vec4(1.0);","for (;;) {}"),
        Replace(src,"reg_tmp0 = vec4(1.0);","reg_tmp0 = value ? a : b;"),
        Replace(src,"reg_tmp1 = reg_tmp0;","reg_tmp1 = vec4(0.0);"),
        Replace(src,"return false;","return true;"),
        Replace(src,"reg_tmp0 = vec4(1.0);","sub_1_3();"),
        Replace(src,"reg_tmp0 = vec4(1.0);","sub_1_3 /*comment*/ ();"),
        Replace(src,"vec4(1.0)","vec4[1.0)"),
        Replace(src,"    // 1: mov","#define CARA_GLES\n    // 1: mov"),
        std::string("#define reg_tmp0 bad\n")+src,
        std::string("#define CARA_GLES\n#define CARA_GLES\n")+src,
        src+"/*",src+"bool sub_9_10() {",src+"\"quoted\"",
        Replace(src,"bool sub_2_3();\n", ""),
        Replace(src,"bool sub_2_3() {", "bool sub_4_3() {"),
        src+src
    }) { ++negative_case; std::cerr << "negative_case=" << negative_case << "\n"; Check(Apply(bad)==bad); }
    // Ending-PC mismatch and no matching suffix are legitimate no-op sources.
    auto different=Replace(Replace(src,"sub_2_3();","sub_2_4();"),"sub_2_3() {","sub_2_4() {");
    Check(Apply(different)==different);
    Check(Apply(src+"/*")==src+"/*");
    std::cerr << "SOURCE_FIXTURE_PASS: policy, authored positive and 21 no-op/rollback cases\n";
}
