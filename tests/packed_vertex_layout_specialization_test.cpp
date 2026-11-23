// SPDX-License-Identifier: GPL-2.0-or-later
// Authored descriptor fixtures; no guest shader, GPU or emulator runtime needed.
#include "video_core/renderer_opengl/gl_packed_vertex.h"
#include <iostream>
#include <stdexcept>

static void Check(bool value) {
    if (!value) throw std::runtime_error("packed descriptor specialization regression");
}
int main() {
    OpenGL::PackedVertexLayout layout{};
    for (unsigned i = 0; i < 16; ++i)
        layout[i] = {int(i * 17), int(i * 3), int(i % 4), int(i % 5)};
    const auto original = layout;
    Check(OpenGL::CanAdmitPackedVertexLayout(0, 0));
    Check(OpenGL::CanAdmitPackedVertexLayout(63, 7));
    Check(!OpenGL::CanAdmitPackedVertexLayout(64, 0));
    Check(!OpenGL::CanAdmitPackedVertexLayout(0, 8));
    Check(!OpenGL::CanAdmitPackedVertexLayout(64, 8));
    for (const char* decoder : {OpenGL::PackedVertexDecoderGLSL,
                                OpenGL::PackedVertexFetchReuseGLSL}) {
        const auto source = OpenGL::SpecializePackedVertexDecoder(decoder, layout);
        Check(!source.empty());
        Check(source.find("ivec4 info = cara_vertex_layout[reg];\n    info = cara_known_vertex_layout[reg];") != source.npos);
        Check(source.find("uniform highp ivec4 cara_vertex_layout[16];") != source.npos);
        Check(source.find("ivec4(255,45,3,0));") != source.npos);
        for (unsigned reg = 0; reg < 16; ++reg) {
            auto changed = layout;
            changed[reg][3] |= (8 << 3) | (65535 << 7);
            Check(OpenGL::SpecializePackedVertexDecoder(decoder, changed) == source);
            for (unsigned component = 0; component < 4; ++component) {
                changed = layout;
                ++changed[reg][component];
                Check(OpenGL::SpecializePackedVertexDecoder(decoder, changed) != source);
            }
        }
        Check(OpenGL::SpecializePackedVertexDecoder(std::string(decoder) + decoder, layout).empty());
    }
    Check(layout == original); // Never modify the live uniform/header owner.
    Check(OpenGL::SpecializePackedVertexDecoder("unknown decoder", layout).empty());
    std::cout << "PASS: both packed decoders; 128 descriptor mutations, 32 live-header changes, exact anchors and fail-closed source forms\n";
}
