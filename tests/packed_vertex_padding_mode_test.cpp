// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_opengl/gl_packed_vertex_padding.h"
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
bool PaddingModeOtherTranslationUnit();
int main(int argc,char** argv) {
    const bool expected=argc==2 && std::string(argv[1])=="1";
    if(PaddingModeOtherTranslationUnit()!=expected ||
       OpenGL::PackedVertexPaddingOnlyEnabled()!=expected)
        throw std::runtime_error("initial shared flag mismatch");
    setenv("CARA_PACKED_PADDING_ONLY_ZERO",expected?"0":"1",1);
    if(PaddingModeOtherTranslationUnit()!=expected ||
       OpenGL::PackedVertexPaddingOnlyEnabled()!=expected)
        throw std::runtime_error("cross-translation-unit cached flag mismatch");
    std::cout<<std::boolalpha<<"{\"packed_padding_only_zero\":"
             <<OpenGL::PackedVertexPaddingOnlyEnabled()<<"}\n";
}
