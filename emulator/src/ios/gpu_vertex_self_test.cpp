// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <nihstro/inline_assembly.h>
#include "ios/gpu_vertex_self_test.h"
#include "video_core/regs.h"
#include "video_core/renderer_opengl/gl_resource_manager.h"
#include "video_core/renderer_opengl/gl_shader_gen.h"
#include "video_core/renderer_opengl/gl_shader_manager.h"
#include "video_core/shader/shader_interpreter.h"

bool RunGPUVertexSelfTest(std::string& error) {
    using namespace OpenGL;
    using S = nihstro::SourceRegister;
    using D = nihstro::DestRegister;
    using O = nihstro::OpCode;
    using Semantic = Pica::RasterizerRegs::VSOutputAttributes;
    // One uniform branch skips a counted loop; false falls through the loop and
    // its subsequent label. This exercises the GLES switch replacement too.
    const auto binary = nihstro::InlineAsm::CompileToRawBinary({
        {O::Id::MOV, D::MakeOutput(0), S::MakeInput(0)},
        {O::Id::MOV, D::MakeOutput(1), S::MakeInput(1)},
        {O::Id::MOV, D::MakeTemporary(0), S::MakeInput(2)},
        {O::Id::NOP}, {O::Id::NOP},
        {O::Id::ADD, D::MakeTemporary(0), S::MakeTemporary(0), S::MakeInput(2)},
        {O::Id::NOP},
        {O::Id::MOV, D::MakeOutput(2), S::MakeTemporary(0)},
        {O::Id::END},
    });
    Pica::Shader::ShaderSetup setup{};
    std::transform(binary.program.begin(), binary.program.end(), setup.program_code.begin(),
                   [](auto instruction) { return instruction.hex; });
    std::transform(binary.swizzle_table.begin(), binary.swizzle_table.end(), setup.swizzle_data.begin(),
                   [](auto swizzle) { return swizzle.hex; });
    nihstro::Instruction jump{};
    jump.opcode = O::Id::JMPU;
    jump.flow_control.dest_offset = 7;
    setup.program_code[3] = jump.hex;
    nihstro::Instruction loop{};
    loop.opcode = O::Id::LOOP;
    loop.flow_control.dest_offset = 5;
    setup.program_code[4] = loop.hex;
    setup.uniforms.i[0] = {1, 0, 1, 0};
    Pica::Regs regs{};
    regs.vs.output_mask.Assign(7);
    regs.vs.max_input_attribute_index.Assign(2);
    regs.vs.input_attribute_to_register_map_low = 0x210;
    regs.vs.int_uniforms[0].x.Assign(1);
    regs.vs.int_uniforms[0].z.Assign(1);
    regs.rasterizer.vs_output_total.Assign(3);
    const std::array<std::array<Semantic::Semantic, 4>, 3> semantics{{
        {Semantic::POSITION_X, Semantic::POSITION_Y, Semantic::POSITION_Z, Semantic::POSITION_W},
        {Semantic::QUATERNION_X, Semantic::QUATERNION_Y, Semantic::QUATERNION_Z, Semantic::QUATERNION_W},
        {Semantic::COLOR_R, Semantic::COLOR_G, Semantic::COLOR_B, Semantic::COLOR_A},
    }};
    for (size_t i = 0; i < semantics.size(); ++i) {
        auto& attribute = regs.rasterizer.vs_output_attributes[i];
        attribute.map_x.Assign(semantics[i][0]); attribute.map_y.Assign(semantics[i][1]);
        attribute.map_z.Assign(semantics[i][2]); attribute.map_w.Assign(semantics[i][3]);
    }
    const PicaVSConfig config(regs, setup);
    const PicaFixedGSConfig output(regs);
    OGLShader vertex, fragment;
    vertex.Create(GenerateVertexShader(setup, config, false, &output).c_str(), GL_VERTEX_SHADER);
    fragment.Create("out vec4 color; void main() { color = vec4(1.0); }", GL_FRAGMENT_SHADER);
    if (!vertex.handle || !fragment.handle) { error = "fixture shader compilation failed"; return false; }
    const GLuint program = glCreateProgram();
    glAttachShader(program, vertex.handle); glAttachShader(program, fragment.handle);
    const char* varying[] = {"gl_Position", "normquat", "primary_color"};
    glTransformFeedbackVaryings(program, 3, varying, GL_INTERLEAVED_ATTRIBS);
    glLinkProgram(program);
    GLint linked = 0; glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) { glDeleteProgram(program); error = "fixture shader linkage failed"; return false; }
    std::array<GLvec4, 256> inputs{};
    inputs[0] = {-0.5f, -0.5f, -0.5f, 1}; inputs[16] = {0.5f, -0.5f, -0.5f, 1}; inputs[32] = {0, 0.5f, -0.5f, 1};
    inputs[1] = {1, 0, 0, 0}; inputs[17] = {-1, 0, 0, 0}; inputs[33] = {0, 1, 0, 0};
    inputs[2] = {-0.25f, 0.125f, 0.5f, 1}; inputs[18] = {0.125f, -0.5f, 1.5f, 1}; inputs[34] = {0.25f, 0.5f, -0.125f, 1};
    OGLTexture texture; texture.Create();
    glActiveTexture(GL_TEXTURE7); glBindTexture(GL_TEXTURE_2D, texture.handle);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 256, 1, 0, GL_RGBA, GL_FLOAT, inputs.data());
    OGLBuffer uniform_buffer, feedback_buffer; uniform_buffer.Create(); feedback_buffer.Create();
    OGLVertexArray vao; vao.Create(); glBindVertexArray(vao.handle);
    glUseProgram(program);
    glUniform1i(glGetUniformLocation(program, "citra_vertex_inputs"), 7);
    glUniformBlockBinding(program, glGetUniformBlockIndex(program, "vs_config"), 0);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, uniform_buffer.handle);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, feedback_buffer.handle);
    glEnable(GL_RASTERIZER_DISCARD);
    bool passed = true;
    for (bool branch : {false, true}) {
        setup.uniforms.b[0] = branch;
        VSUniformData uniforms{}; uniforms.uniforms.SetFromRegs(regs.vs, setup);
        glBindBuffer(GL_UNIFORM_BUFFER, uniform_buffer.handle);
        glBufferData(GL_UNIFORM_BUFFER, sizeof(uniforms), &uniforms, GL_STREAM_DRAW);
        glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, feedback_buffer.handle);
        glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, 36 * sizeof(float), nullptr, GL_STREAM_READ);
        glBeginTransformFeedback(GL_TRIANGLES); glDrawArrays(GL_TRIANGLES, 0, 3); glEndTransformFeedback();
        const float* actual = static_cast<const float*>(glMapBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 36 * sizeof(float), GL_MAP_READ_BIT));
        if (!actual) { passed = false; error = "fixture readback failed"; break; }
        for (size_t v = 0; v < 3; ++v) {
            Pica::Shader::UnitState state;
            for (size_t reg = 0; reg < 3; ++reg)
                for (size_t c = 0; c < 4; ++c)
                    state.registers.input[reg][c] = Pica::float24::FromFloat32(inputs[v * 16 + reg][c]);
            Pica::Shader::InterpreterEngine interpreter;
            interpreter.SetupBatch(setup, 0); interpreter.Run(setup, state);
            for (size_t c = 0; c < 4; ++c) {
                const float expected[] = {state.registers.output[0][c].ToFloat32(),
                    state.registers.output[1][c].ToFloat32() * (v == 1 ? -1 : 1),
                    std::min(1.0f, std::abs(state.registers.output[2][c].ToFloat32()))};
                for (size_t field = 0; field < 3; ++field)
                    if (!std::isfinite(actual[v * 12 + field * 4 + c]) ||
                        std::abs(actual[v * 12 + field * 4 + c] - expected[field]) > 1e-5f) {
                        passed = false; error = "GPU/interpreter mismatch in branch/loop/quaternion fixture";
                    }
            }
        }
        if (!glUnmapBuffer(GL_TRANSFORM_FEEDBACK_BUFFER)) { passed = false; error = "fixture buffer corrupted"; }
    }
    glDisable(GL_RASTERIZER_DISCARD);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0); glBindBufferBase(GL_UNIFORM_BUFFER, 0, 0);
    glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, 0); glBindBuffer(GL_UNIFORM_BUFFER, 0);
    glBindVertexArray(0); glUseProgram(0); glDeleteProgram(program);
    glBindTexture(GL_TEXTURE_2D, 0); glActiveTexture(GL_TEXTURE0);
    return passed;
}
