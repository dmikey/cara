// SPDX-License-Identifier: GPL-2.0-or-later
// Original, ROM-free PICA programs. Tests the real decompiler and interpreter.
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <nihstro/inline_assembly.h>
#include "video_core/renderer_opengl/gl_shader_decompiler.h"
#include "video_core/shader/shader_interpreter.h"
#ifdef __APPLE__
#define GL_SILENCE_DEPRECATION
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#endif

using S = nihstro::SourceRegister;
using D = nihstro::DestRegister;
using O = nihstro::OpCode;
using A = nihstro::InlineAsm;
using I = nihstro::Instruction;
using Setup = Pica::Shader::ShaderSetup;
static void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
static A Start() { return {O::Id::MOV, D::MakeTemporary(0), S::MakeInput(0)}; }
static A Add(unsigned input) {
    return {O::Id::ADD, D::MakeTemporary(0), S::MakeTemporary(0), S::MakeInput(input)};
}
static A Output() { return {O::Id::MOV, D::MakeOutput(0), S::MakeTemporary(0)}; }
struct Fixture {
    std::string name;
    Setup setup{};
    std::array<float, 5> input{1, 10, 100, 1000, 3};
    float expected{};
    bool dispatch = true;
};
static Fixture Make(std::string name, std::initializer_list<A> program, float expected) {
    Fixture f; f.name = std::move(name); f.expected = expected;
    const auto binary = A::CompileToRawBinary(program);
    std::transform(binary.program.begin(), binary.program.end(), f.setup.program_code.begin(),
                   [](auto word) { return word.hex; });
    std::transform(binary.swizzle_table.begin(), binary.swizzle_table.end(), f.setup.swizzle_data.begin(),
                   [](auto word) { return word.hex; });
    return f;
}
static void Flow(Fixture& f, unsigned pc, O::Id opcode, unsigned target, unsigned count = 0,
                 unsigned uniform = 0) {
    I i{}; i.opcode = opcode; i.flow_control.dest_offset = target;
    i.flow_control.num_instructions = count;
    i.flow_control.bool_uniform_id = uniform;
    if (opcode == O::Id::JMPC) {
        i.flow_control.op = I::FlowControlType::JustX; i.flow_control.refx = true;
    }
    f.setup.program_code[pc] = i.hex;
}
static void CompareLess(Fixture& f, unsigned pc, S left, S right) {
    I i{}; i.opcode = O::Id::CMP; i.common.src1 = left; i.common.src2 = right;
    i.common.operand_desc_id = 0;
    i.common.compare_op.x = I::Common::CompareOpType::LessThan;
    i.common.compare_op.y = I::Common::CompareOpType::LessThan;
    f.setup.program_code[pc] = i.hex;
}
static std::vector<Fixture> Fixtures() {
    std::vector<Fixture> out;
    auto straight = Make("straight", {Start(), Add(1), Output(), {O::Id::END}}, 11);
    straight.dispatch = false; out.push_back(straight);
    for (bool invert : {false, true}) for (bool taken : {false, true}) {
        auto f = Make("jmpu-" + std::to_string(invert) + "-" + std::to_string(taken),
                      {Start(), {O::Id::NOP}, Add(1), Add(2), Output(), {O::Id::END}}, taken ? 101 : 111);
        Flow(f, 1, O::Id::JMPU, 3, invert); f.setup.uniforms.b[0] = taken != invert;
        out.push_back(f);
    }
    for (bool taken : {false, true}) {
        auto f = Make("jmpc-" + std::to_string(taken),
                      {Start(), {O::Id::NOP}, {O::Id::NOP}, Add(1), Add(2), Output(), {O::Id::END}}, taken ? 101 : 111);
        CompareLess(f, 1, S::MakeInput(0), S::MakeInput(4));
        f.input[4] = taken ? 3 : 0; Flow(f, 2, O::Id::JMPC, 4); out.push_back(f);
    }
    auto backwards = Make("backward-counted-jump",
        {Start(), Add(1), {O::Id::NOP}, {O::Id::NOP}, Output(), {O::Id::END}}, 3);
    backwards.input[0] = 0; backwards.input[1] = 1;
    CompareLess(backwards, 2, S::MakeTemporary(0), S::MakeInput(4));
    Flow(backwards, 3, O::Id::JMPC, 1); out.push_back(backwards);
    for (bool first : {false, true}) for (bool second : {false, true}) {
        auto f = Make("fallthrough-" + std::to_string(first) + "-" + std::to_string(second),
            {Start(), {O::Id::NOP}, {O::Id::NOP}, Add(1), Add(2), Add(3), Output(), {O::Id::END}},
            first ? 1101 : second ? 1001 : 1111);
        Flow(f, 1, O::Id::JMPU, 4); Flow(f, 2, O::Id::JMPU, 5, 0, 1);
        f.setup.uniforms.b[0] = first; f.setup.uniforms.b[1] = second; out.push_back(f);
    }
    for (bool taken : {false, true}) {
        auto f = Make("call-jump-" + std::to_string(taken),
            {Start(), {O::Id::NOP}, Add(3), Output(), {O::Id::END}, Add(1), {O::Id::NOP}, Add(2), {O::Id::NOP}},
            taken ? 1011 : 1111);
        Flow(f, 1, O::Id::CALL, 5, 4); Flow(f, 6, O::Id::JMPU, 8);
        f.setup.uniforms.b[0] = taken; out.push_back(f);
        auto ended = Make("call-early-end-" + std::to_string(taken),
            {Start(), {O::Id::NOP}, Add(3), Output(), {O::Id::END}, Add(1), {O::Id::NOP}, Output(), {O::Id::END}},
            taken ? 0 : 11);
        Flow(ended, 1, O::Id::CALL, 5, 4); Flow(ended, 6, O::Id::JMPU, 8);
        ended.setup.uniforms.b[0] = taken; out.push_back(ended);
        auto loop = Make("loop-jump-" + std::to_string(taken),
            {Start(), {O::Id::NOP}, Add(1), {O::Id::NOP}, Add(2), {O::Id::NOP}, Add(3), Output(), {O::Id::END}},
            taken ? 1031 : 1331);
        Flow(loop, 1, O::Id::LOOP, 5); Flow(loop, 3, O::Id::JMPU, 5);
        loop.setup.uniforms.i[0] = {2, 7, 2, 0}; loop.setup.uniforms.b[0] = taken; out.push_back(loop);
        // An untaken outer jump creates a label inside IF, forcing CompileRange's
        // compile_end redispatch; the inner jump separately exercises subroutine break.
        auto branch = Make("if-interior-label-" + std::to_string(taken),
            {Start(), {O::Id::NOP}, {O::Id::NOP}, Add(1), {O::Id::NOP}, Add(2), Add(3), {O::Id::NOP}, Output(), {O::Id::END}},
            taken ? 11 : 1001);
        Flow(branch, 1, O::Id::JMPU, 4, 0, 1); Flow(branch, 2, O::Id::IFU, 6, 2);
        Flow(branch, 4, O::Id::JMPU, 6, 0, 2);
        branch.setup.uniforms.b[0] = taken; branch.setup.uniforms.b[2] = true;
        out.push_back(branch);
    }
    return out;
}
static std::string Generate(const Fixture& f) {
    auto result = OpenGL::ShaderDecompiler::DecompileProgram(f.setup.program_code, f.setup.swizzle_data, 0,
        [](u32 reg) { return "input_values[" + std::to_string(reg) + "]"; },
        [](u32 reg) { return "output" + std::to_string(reg); }, 0);
    Require(result.has_value(), f.name + ": decompilation failed");
    Require(result->code.find("switch (jmp_to)") == std::string::npos, f.name + ": Apple path emitted switch");
    if (f.dispatch) {
        Require(result->code.find("bool cara_case_active = false;") != std::string::npos,
                f.name + ": fixture did not exercise lowered dispatch");
        Require(result->code.find("} while (false);") != std::string::npos,
                f.name + ": missing switch-break scope");
    }
    if (f.name.find("if-interior") == 0)
        Require(result->code.find("{ jmp_to = 8u; break; }") != std::string::npos,
                f.name + ": did not cover interior-label redispatch");
    return result->code;
}
static void CheckInterpreter(Fixture f) {
    Pica::Shader::UnitState state;
    state.registers = {}; state.conditional_code[0] = state.conditional_code[1] = false;
    std::fill(std::begin(state.address_registers), std::end(state.address_registers), 0);
    for (unsigned n = 0; n < f.input.size(); ++n)
        for (unsigned c = 0; c < 4; ++c)
            state.registers.input[n][c] = Pica::float24::FromFloat32(f.input[n]);
    Pica::Shader::InterpreterEngine engine; engine.SetupBatch(f.setup, 0); engine.Run(f.setup, state);
    for (unsigned c = 0; c < 4; ++c)
        Require(state.registers.output[0][c].ToFloat32() == f.expected, f.name + ": independent expected interpreter result");
}
#ifdef __APPLE__
class GLContext {
public:
    GLContext() {
        CGLPixelFormatAttribute attributes[] = {kCGLPFAOpenGLProfile,
            static_cast<CGLPixelFormatAttribute>(kCGLOGLPVersion_3_2_Core),
            static_cast<CGLPixelFormatAttribute>(0)};
        CGLPixelFormatObj format{}; GLint count{};
        Require(CGLChoosePixelFormat(attributes, &format, &count) == kCGLNoError && format,
                "No CGL pixel format: GPU test is unavailable, not a pass");
        const auto error = CGLCreateContext(format, nullptr, &context); CGLDestroyPixelFormat(format);
        Require(error == kCGLNoError && context, "Cannot create isolated CGL context");
        Require(CGLSetCurrentContext(context) == kCGLNoError, "Cannot activate CGL context");
        glGenFramebuffers(1, &framebuffer); glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glGenRenderbuffers(1, &color); glBindRenderbuffer(GL_RENDERBUFFER, color);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 4, 4);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color);
        Require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE,
                "Cannot create isolated complete framebuffer");
        glViewport(0, 0, 4, 4);
        std::cout << "GL renderer: " << glGetString(GL_RENDERER) << "; " << glGetString(GL_VERSION) << '\n';
    }
    ~GLContext() {
        glDeleteFramebuffers(1, &framebuffer); glDeleteRenderbuffers(1, &color);
        CGLSetCurrentContext(nullptr); CGLDestroyContext(context);
    }
private: CGLContextObj context{}; GLuint framebuffer{}, color{};
};
static GLuint Compile(GLenum type, const std::string& source) {
    const auto shader = glCreateShader(type); const char* data = source.c_str();
    glShaderSource(shader, 1, &data, nullptr); glCompileShader(shader);
    GLint ok{}; glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[8192]{}; glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        throw std::runtime_error(std::string("shader compile failed: ") + log); }
    return shader;
}
static void CheckGPU(const Fixture& f, const std::string& generated) {
    std::string source = "#version 330 core\n" + OpenGL::ShaderDecompiler::GetCommonDeclarations();
    source += "uniform pica_uniforms uniforms;\nuniform vec4 input_values[16];\nout vec4 result;\n";
    for (unsigned n = 0; n < 16; ++n)
        source += "vec4 output" + std::to_string(n) + ";\n";
    source += generated + "\nvoid main() {\n";
    for (unsigned n = 0; n < 16; ++n)
        source += "output" + std::to_string(n) + " = vec4(0.0);\n";
    source += "exec_shader(); result = output0; gl_Position = vec4(0,0,0,1); }\n";
    const GLuint vs = Compile(GL_VERTEX_SHADER, source);
    const GLuint fs = Compile(GL_FRAGMENT_SHADER, "#version 330 core\nout vec4 color; void main(){color=vec4(1);}");
    const GLuint program = glCreateProgram(); glAttachShader(program, vs); glAttachShader(program, fs);
    const char* varying = "result"; glTransformFeedbackVaryings(program, 1, &varying, GL_INTERLEAVED_ATTRIBS);
    glLinkProgram(program); GLint linked{}; glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) { char log[8192]{}; glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        throw std::runtime_error(f.name + ": link failed: " + log); }
    GLuint vao{}, buffer{}; glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    glGenBuffers(1, &buffer); glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, buffer);
    std::array<float, 4> result{-12345, -12345, -12345, -12345};
    glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, sizeof(result), result.data(), GL_STREAM_READ);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, buffer); glUseProgram(program);
    // Actual uniforms keep branch choices and loop bounds dynamic at link time.
    // Constant-folded GLSL would not exercise the driver's control-flow execution.
    for (unsigned n = 0; n < 16; ++n) {
        const float value = n < f.input.size() ? f.input[n] : 0.f;
        const auto input_name = "input_values[" + std::to_string(n) + "]";
        glUniform4f(glGetUniformLocation(program, input_name.c_str()), value, value, value, value);
        const auto bool_name = "uniforms.b[" + std::to_string(n) + "]";
        glUniform1i(glGetUniformLocation(program, bool_name.c_str()), f.setup.uniforms.b[n]);
    }
    for (unsigned n = 0; n < 4; ++n) {
        const auto name = "uniforms.i[" + std::to_string(n) + "]";
        const auto& value = f.setup.uniforms.i[n];
        glUniform4ui(glGetUniformLocation(program, name.c_str()), value.x, value.y, value.z, value.w);
    }
    glEnable(GL_RASTERIZER_DISCARD); glBeginTransformFeedback(GL_POINTS); glDrawArrays(GL_POINTS, 0, 1);
    glEndTransformFeedback(); glDisable(GL_RASTERIZER_DISCARD);
    glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, sizeof(result), result.data());
    const auto error = glGetError();
    Require(error == GL_NO_ERROR, f.name + ": GL error during execution/readback " + std::to_string(error));
    for (float value : result) Require(value == f.expected, f.name + ": GPU output differs from fixed answer/interpreter");
    glDeleteBuffers(1, &buffer); glDeleteVertexArrays(1, &vao); glUseProgram(0);
    glDeleteProgram(program); glDeleteShader(vs); glDeleteShader(fs);
}
#endif
int main(int argc, char** argv) {
    try {
        const bool gpu = argc == 2 && std::string(argv[1]) == "--gpu";
        Require(argc == 1 || gpu, "Usage: cara-shader-flow-self-test [--gpu]");
#ifdef __APPLE__
        std::unique_ptr<GLContext> context; if (gpu) context = std::make_unique<GLContext>();
#else
        Require(!gpu, "GPU execution test currently requires macOS CGL");
#endif
        const auto fixtures = Fixtures();
        for (const auto& f : fixtures) {
            CheckInterpreter(f); const auto generated = Generate(f);
#ifdef __APPLE__
            if (gpu) CheckGPU(f, generated);
#endif
            std::cout << "PASS " << f.name << '\n';
        }
        std::cout << "PASS " << fixtures.size() << " generated PICA control-flow fixtures; fixed answers + interpreter + Apple source";
        std::cout << (gpu ? " + native GL transform feedback\n" : "; GPU execution not requested\n");
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
