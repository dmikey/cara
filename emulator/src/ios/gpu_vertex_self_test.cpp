// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <map>
#include <cstring>
#include "video_core/renderer_opengl/gl_packed_vertex.h"
#include <vector>
#include <nihstro/inline_assembly.h>
#include "ios/gpu_vertex_self_test.h"
#include "video_core/regs.h"
#include "video_core/renderer_opengl/gl_resource_manager.h"
#include "video_core/renderer_opengl/gl_shader_gen.h"
#include "video_core/renderer_opengl/gl_shader_manager.h"
#include "video_core/renderer_opengl/gl_vertex_input_layout.h"
#include "video_core/renderer_opengl/gl_vertex_source_map.h"
#include "video_core/shader/shader_interpreter.h"

// Isolate driver capture from the PICA translation and packed input texture.
static bool RunFeedbackCaptureFixture(std::string& error) {
    using namespace OpenGL;
    constexpr GLsizei count = 41967;
    OGLShader vertex, fragment;
    vertex.Create("uniform highp int capture_base; out highp vec4 sample_value; void main() { sample_value = vec4(float(gl_VertexID + capture_base), 2.0, 3.0, 4.0); gl_Position = vec4(0.0, 0.0, 0.0, 1.0); }", GL_VERTEX_SHADER);
    fragment.Create("out vec4 color; void main() { color=vec4(1.0); }", GL_FRAGMENT_SHADER);
    const GLuint program = glCreateProgram();
    glAttachShader(program,vertex.handle); glAttachShader(program,fragment.handle);
    const char* varying[] = {"sample_value"};
    glTransformFeedbackVaryings(program,1,varying,GL_INTERLEAVED_ATTRIBS);
    glLinkProgram(program);
    GLint linked=0; glGetProgramiv(program,GL_LINK_STATUS,&linked);
    if (!linked) { glDeleteProgram(program); error="feedback capture linkage failed"; return false; }
    OGLVertexArray vao; vao.Create(); glBindVertexArray(vao.handle);
    OGLBuffer buffer; buffer.Create();
    glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER,buffer.handle);
    glUseProgram(program); glEnable(GL_RASTERIZER_DISCARD);
    bool passed=true;
    // On the test iPad, GLES feedback loses outputs on large draws or nonzero first.
    // Bound each capture and supply its logical vertex base explicitly.
    constexpr GLsizei chunk_size=3072;
    for (GLsizei first=0;first<count && passed;first+=chunk_size) {
        const GLsizei vertices=std::min(chunk_size,count-first);
        glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER,vertices*4*sizeof(float),nullptr,GL_STREAM_READ);
        glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER,0,buffer.handle);
        GLuint query=0; glGenQueries(1,&query);
        glBeginQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN,query);
        glUniform1i(glGetUniformLocation(program,"capture_base"),first);
        glBeginTransformFeedback(GL_TRIANGLES); glDrawArrays(GL_TRIANGLES,0,vertices); glEndTransformFeedback();
        glEndQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN);
        GLuint primitives=0; glGetQueryObjectuiv(query,GL_QUERY_RESULT,&primitives);
        const GLenum draw_error=glGetError();
        const float* data=static_cast<const float*>(glMapBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER,0,vertices*4*sizeof(float),GL_MAP_READ_BIT));
        passed=data && primitives==vertices/3 && draw_error==GL_NO_ERROR;
        if (!passed) error="feedback capture query="+std::to_string(primitives)+" error="+std::to_string(draw_error);
        if (data) {
            for (GLsizei v=0;v<vertices;++v) {
                if (data[v*4]!=float(first+v) || data[v*4+1]!=2 || data[v*4+2]!=3 || data[v*4+3]!=4) {
                    passed=false; error="feedback capture mismatch vertex="+std::to_string(first+v)+" actual="+std::to_string(data[v*4]); break;
                }
            }
            if (!glUnmapBuffer(GL_TRANSFORM_FEEDBACK_BUFFER)) { passed=false; error="feedback capture corrupted"; }
        }
        glDeleteQueries(1,&query);
    }
    glDisable(GL_RASTERIZER_DISCARD); glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER,0,0);
    glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER,0); glBindVertexArray(0); glUseProgram(0);
    glDeleteProgram(program);
    return passed;
}

static bool RunVertexLayoutFixture(u16 input_mask, std::string& error, size_t vertex_count=99, unsigned indexed_inputs=0, size_t source_count=257) {
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
        {O::Id::MOV, D::MakeTemporary(0), S::MakeInput(7)},
        {O::Id::NOP}, {O::Id::NOP},
        {O::Id::ADD, D::MakeTemporary(0), S::MakeTemporary(0), S::MakeInput(7)},
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
    regs.vs.input_attribute_to_register_map_low = 0x710;
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
    std::string vertex_source=GenerateVertexShader(setup, config, false, &output, input_mask, indexed_inputs);
    // Exercise the entire large atlas without relying on large feedback captures.
    for (size_t pos=0;(pos=vertex_source.find("gl_VertexID",pos))!=std::string::npos;) {
        const std::string replacement="(gl_VertexID + capture_base)";
        vertex_source.replace(pos,11,replacement); pos+=replacement.size();
    }
    vertex_source="uniform highp int capture_base;\n"+vertex_source;
    vertex.Create(vertex_source.c_str(), GL_VERTEX_SHADER);
    fragment.Create("out vec4 color; void main() { color = vec4(1.0); }", GL_FRAGMENT_SHADER);
    if (!vertex.handle || !fragment.handle) { error = "fixture shader compilation failed"; return false; }
    const GLuint program = glCreateProgram();
    glAttachShader(program, vertex.handle); glAttachShader(program, fragment.handle);
    const char* varying[] = {"gl_Position", "normquat", "primary_color"};
    glTransformFeedbackVaryings(program, 3, varying, GL_INTERLEAVED_ATTRIBS);
    glLinkProgram(program);
    GLint linked = 0; glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) { glDeleteProgram(program); error = "fixture shader linkage failed"; return false; }
    constexpr size_t feedback_chunk_vertices = 3072;
    std::vector<GLvec4> inputs(vertex_count * 16);
    inputs[0] = {-0.5f, -0.5f, -0.5f, 1}; inputs[16] = {0.5f, -0.5f, -0.5f, 1}; inputs[32] = {0, 0.5f, -0.5f, 1};
    inputs[1] = {1, 0, 0, 0}; inputs[17] = {-1, 0, 0, 0}; inputs[33] = {0, 1, 0, 0};
    inputs[7] = {-0.25f, 0.125f, 0.5f, 1}; inputs[23] = {0.125f, -0.5f, 1.5f, 1}; inputs[39] = {0.25f, 0.5f, -0.125f, 1};
    std::array<std::array<GLvec4,16>,3> seeds;
    for (unsigned v=0;v<3;++v) std::copy_n(inputs.begin()+v*16,16,seeds[v].begin());
    const IOSVertexInputLayout layout(input_mask);
    auto source_map=std::make_unique<IOSVertexSourceMap>(); source_map->BeginDraw();
    const size_t index_rows=(vertex_count+IOS_VERTEX_INPUT_WIDTH-1)/IOS_VERTEX_INPUT_WIDTH;
    std::vector<u16> indices(index_rows*IOS_VERTEX_INPUT_WIDTH);
    std::vector<GLvec4> packed;
    for (size_t v=0;v<vertex_count;++v) {
        const u16 source=indexed_inputs ? u16(source_count==vertex_count ? (v*7)%source_count : ((v/3)*7+(v%3)*13)%source_count) : u16(v);
        std::copy_n(seeds[source%3].begin(),16,inputs.begin()+v*16);
        inputs[v*16][0]+=float(source/3)/1024.0f;
        const auto record=source_map->Resolve(source);
        indices[v]=record.index;
        if (record.inserted) {
            const size_t first=packed.size(); packed.resize(first+layout.stride);
            for (unsigned reg=0;reg<16;++reg)
                if (layout.offsets[reg]>=0) packed[first+layout.offsets[reg]]=inputs[v*16+reg];
        }
    }
    const size_t record_count=source_map->Size();
    const size_t texture_rows=(packed.size()+IOS_VERTEX_INPUT_WIDTH-1)/IOS_VERTEX_INPUT_WIDTH;
    packed.resize(texture_rows*IOS_VERTEX_INPUT_WIDTH);
    OGLTexture texture; texture.Create();
    glActiveTexture(GL_TEXTURE7); glBindTexture(GL_TEXTURE_2D, texture.handle);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, indexed_inputs==2?GL_RGBA32UI:GL_RGBA32F, IOS_VERTEX_INPUT_WIDTH, static_cast<GLsizei>(texture_rows), 0, indexed_inputs==2?GL_RGBA_INTEGER:GL_RGBA, indexed_inputs==2?GL_UNSIGNED_INT:GL_FLOAT, packed.data());
    OGLTexture index_texture;
    if (indexed_inputs) {
        index_texture.Create(); glActiveTexture(GL_TEXTURE8); glBindTexture(GL_TEXTURE_2D,index_texture.handle);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glBindSampler(8,0);
        glTexImage2D(GL_TEXTURE_2D,0,GL_R16UI,IOS_VERTEX_INPUT_WIDTH,static_cast<GLsizei>(index_rows),0,
                     GL_RED_INTEGER,GL_UNSIGNED_SHORT,indices.data());
        glActiveTexture(GL_TEXTURE7);
    }
    OGLBuffer uniform_buffer, feedback_buffer, raster_uniforms; uniform_buffer.Create(); feedback_buffer.Create(); raster_uniforms.Create();
    OGLVertexArray vao; vao.Create(); glBindVertexArray(vao.handle);
    glUseProgram(program);
    glUniform1i(glGetUniformLocation(program, "cara_vertex_inputs"), 7);
    glUniform1i(glGetUniformLocation(program, "cara_vertex_indices"), 8);
    if (indexed_inputs==2) {
        std::array<std::array<GLint,4>,16> raw_layout{};
        for (unsigned reg=0;reg<16;++reg) if (layout.offsets[reg]>=0)
            raw_layout[reg]={layout.offsets[reg]*16,GLint(layout.stride*16),3,4};
        glUniform4iv(glGetUniformLocation(program,"cara_vertex_layout[0]"),16,raw_layout[0].data());
    }

    glUniformBlockBinding(program, glGetUniformBlockIndex(program, "vs_config"), 0);
    const GLvec4 viewport = {13.0f, 7.0f, 200.25f, 120.125f};
    UniformData raster_data{};
    raster_data.citra_viewport = viewport;
    const GLuint raster_block = glGetUniformBlockIndex(program, "shader_data");
    if (raster_block != GL_INVALID_INDEX) {
        glUniformBlockBinding(program, raster_block, 1);
        glBindBuffer(GL_UNIFORM_BUFFER, raster_uniforms.handle);
        glBufferData(GL_UNIFORM_BUFFER, sizeof(raster_data), &raster_data, GL_STREAM_DRAW);
        glBindBufferBase(GL_UNIFORM_BUFFER, 1, raster_uniforms.handle);
    }
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, uniform_buffer.handle);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, feedback_buffer.handle);
    glEnable(GL_RASTERIZER_DISCARD);
    bool passed = true;
    for (bool branch : {false, true}) {
        setup.uniforms.b[0] = branch;
        VSUniformData uniforms{}; uniforms.uniforms.SetFromRegs(regs.vs, setup);
        glBindBuffer(GL_UNIFORM_BUFFER, uniform_buffer.handle);
        glBufferData(GL_UNIFORM_BUFFER, sizeof(uniforms), &uniforms, GL_STREAM_DRAW);
        glBindBufferBase(GL_UNIFORM_BUFFER, 0, uniform_buffer.handle);
        for (size_t first=0;first<vertex_count;first+=feedback_chunk_vertices) {
            const size_t captured_vertices=std::min(feedback_chunk_vertices,vertex_count-first);
            const size_t feedback_floats=captured_vertices*12;
            glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, feedback_buffer.handle);
            glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, feedback_floats * sizeof(float), nullptr, GL_STREAM_READ);
            glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, feedback_buffer.handle);
            glUniform1i(glGetUniformLocation(program,"capture_base"),static_cast<GLint>(first));
            glBeginTransformFeedback(GL_TRIANGLES); glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(captured_vertices)); glEndTransformFeedback();
            const float* actual = static_cast<const float*>(glMapBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER, 0, feedback_floats * sizeof(float), GL_MAP_READ_BIT));
            if (!actual) { passed = false; error = "fixture readback failed"; break; }
            std::array<float,4> first_quaternion{};
            for (size_t local=0;local<captured_vertices;++local) {
                const size_t v=first+local;
                Pica::Shader::UnitState state;
                for (size_t reg = 0; reg < 16; ++reg)
                    for (size_t c = 0; c < 4; ++c)
                        state.registers.input[reg][c] = Pica::float24::FromFloat32((input_mask & (1u << reg)) ? inputs[v * 16 + reg][c] : 0.0f);
                Pica::Shader::InterpreterEngine interpreter;
                interpreter.SetupBatch(setup, 0); interpreter.Run(setup, state);
                float quaternion_dot=0;
                for (size_t c=0;c<4;++c) {
                    if (v%3==0) first_quaternion[c]=state.registers.output[1][c].ToFloat32();
                    quaternion_dot+=first_quaternion[c]*state.registers.output[1][c].ToFloat32();
                }
                for (size_t c = 0; c < 4; ++c) {
                    float position = state.registers.output[0][c].ToFloat32();
                    if (c < 2 && !std::getenv("CARA_UNSNAPPED_VERTICES")) {
                        const float w = state.registers.output[0][3].ToFloat32();
                        const float screen = (position / w + 1.0f) * viewport[c + 2] + viewport[c];
                        position = ((std::round(screen * 16.0f) / 16.0f - viewport[c]) / viewport[c + 2] - 1.0f) * w;
                    }
                    if (c == 2)
                        position = 2.0f * position + state.registers.output[0][3].ToFloat32();
                    const float expected[] = {position,
                        state.registers.output[1][c].ToFloat32() * (quaternion_dot < 0 ? -1 : 1),
                        std::min(1.0f, std::abs(state.registers.output[2][c].ToFloat32()))};
                    for (size_t field = 0; field < 3; ++field)
                        if (!std::isfinite(actual[local * 12 + field * 4 + c]) ||
                            std::abs(actual[local * 12 + field * 4 + c] - expected[field]) > 1e-5f) {
                            if (passed) error = "GPU/interpreter mismatch at vertex="+std::to_string(v)+
                                " field="+std::to_string(field)+" component="+std::to_string(c)+
                                " expected="+std::to_string(expected[field])+" actual="+std::to_string(actual[local*12+field*4+c]);
                            passed = false;
                        }
                }
            }
            if (!glUnmapBuffer(GL_TRANSFORM_FEEDBACK_BUFFER)) { passed = false; error = "fixture buffer corrupted"; }
        }
    }
    glDisable(GL_RASTERIZER_DISCARD);
    // PICA accepts -w <= z <= 0. GLES must reject the entire positive-z triangle.
    GLint old_draw = 0, old_read = 0;
    std::array<GLint, 4> old_viewport{};
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_draw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read);
    glGetIntegerv(GL_VIEWPORT, old_viewport.data());
    OGLFramebuffer clip_fbo; clip_fbo.Create();
    OGLTexture clip_color; clip_color.Create();
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, clip_color.handle);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindFramebuffer(GL_FRAMEBUFFER, clip_fbo.handle);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, clip_color.handle, 0);
    glViewport(0, 0, 1, 1);
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glActiveTexture(GL_TEXTURE7); glBindTexture(GL_TEXTURE_2D, texture.handle);
    glUniform1i(glGetUniformLocation(program,"capture_base"),0);
    for (bool outside : {false, true}) {
        for (size_t v = 0; v < record_count; ++v)
            packed[v * layout.stride + layout.offsets[0]][2] = outside ? 0.125f : -0.5f;
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, IOS_VERTEX_INPUT_WIDTH,
                        static_cast<GLsizei>(texture_rows), indexed_inputs==2?GL_RGBA_INTEGER:GL_RGBA, indexed_inputs==2?GL_UNSIGNED_INT:GL_FLOAT, packed.data());
        glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertex_count));
        std::array<u8, 4> pixel{};
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
        const u8 expected = outside ? 0 : 255;
        if (pixel[0] != expected || pixel[1] != expected || pixel[2] != expected) {
            passed = false;
            error = outside ? "PICA positive-z clipping plane missing" : "PICA in-range triangle incorrectly clipped";
        }
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(old_draw));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(old_read));
    glViewport(old_viewport[0], old_viewport[1], old_viewport[2], old_viewport[3]);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, 0); glActiveTexture(GL_TEXTURE7);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0); glBindBufferBase(GL_UNIFORM_BUFFER, 0, 0);
    glBindBufferBase(GL_UNIFORM_BUFFER, 1, 0);
    glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, 0); glBindBuffer(GL_UNIFORM_BUFFER, 0);
    glBindVertexArray(0); glUseProgram(0); glDeleteProgram(program);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE8); glBindTexture(GL_TEXTURE_2D,0);
    glActiveTexture(GL_TEXTURE0);
    return passed;
}

static bool RunPackedDecodeFixture(std::string& error) {
    using namespace OpenGL;
    constexpr unsigned count=3072, per_attribute=count/16;
    std::array<std::array<GLint,4>,16> layout{};
    std::vector<u8> bytes(16*per_attribute*24+2048,0);
    std::vector<float> expected(count*4);
    const std::array<float,8> floats={-32768.f,32767.f,-1.25f,0.f,-0.f,0.125f,1024.5f,1.0e-20f};
    for (unsigned reg=0;reg<16;++reg) {
        const unsigned format=reg/4, components=reg%4+1, width=format==3?4:format==2?2:1;
        const unsigned offset=reg*per_attribute*24+reg%4;
        layout[reg]={GLint(offset),24,GLint(format),GLint(components)};
        for (unsigned v=0;v<per_attribute;++v) for (unsigned c=0;c<4;++c) {
            float value=c==3?1.f:0.f;
            if (c<components) {
                u8* destination=bytes.data()+offset+v*24+c*width;
                const u32 bits=(v*977+reg*131+c*65521)^0x8000;
                if (format==0) { s8 x=static_cast<s8>(bits); std::memcpy(destination,&x,1); value=x; }
                if (format==1) { u8 x=static_cast<u8>(bits); std::memcpy(destination,&x,1); value=x; }
                if (format==2) { s16 x=static_cast<s16>(bits); std::memcpy(destination,&x,2); value=x; }
                if (format==3) { value=floats[(v+c)%floats.size()]; std::memcpy(destination,&value,4); }
            }
            expected[(v*16+reg)*4+c]=value;
        }
    }
    const unsigned rows=(bytes.size()+2047)/2048; bytes.resize(rows*2048);
    OGLShader vertex,fragment;
    vertex.Create((std::string(PackedVertexDecoderGLSL)+"out highp vec4 decoded; void main(){ decoded=cara_decode_vertex(gl_VertexID/16,gl_VertexID%16); gl_Position=vec4(0,0,0,1); }").c_str(),GL_VERTEX_SHADER);
    fragment.Create("out vec4 color;void main(){color=vec4(1);}",GL_FRAGMENT_SHADER);
    GLuint program=glCreateProgram(); glAttachShader(program,vertex.handle); glAttachShader(program,fragment.handle);
    const char* varying="decoded"; glTransformFeedbackVaryings(program,1,&varying,GL_INTERLEAVED_ATTRIBS); glLinkProgram(program);
    GLint linked=0; glGetProgramiv(program,GL_LINK_STATUS,&linked);
    if (!linked) { glDeleteProgram(program); error="packed decoder link failed"; return false; }
    OGLTexture texture; texture.Create(); glActiveTexture(GL_TEXTURE7); glBindTexture(GL_TEXTURE_2D,texture.handle); glBindSampler(7,0);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA32UI,128,rows,0,GL_RGBA_INTEGER,GL_UNSIGNED_INT,bytes.data());
    OGLVertexArray vao; vao.Create(); glBindVertexArray(vao.handle);
    OGLBuffer output; output.Create(); glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER,output.handle);
    glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER,expected.size()*sizeof(float),nullptr,GL_STREAM_READ);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER,0,output.handle); glUseProgram(program);
    glUniform1i(glGetUniformLocation(program,"cara_vertex_inputs"),7);
    glUniform4iv(glGetUniformLocation(program,"cara_vertex_layout[0]"),16,layout[0].data());
    glEnable(GL_RASTERIZER_DISCARD); glBeginTransformFeedback(GL_TRIANGLES); glDrawArrays(GL_TRIANGLES,0,count); glEndTransformFeedback();
    const auto draw_error=glGetError();
    const float* actual=static_cast<const float*>(glMapBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER,0,expected.size()*sizeof(float),GL_MAP_READ_BIT));
    bool passed=actual && draw_error==GL_NO_ERROR;
    if (actual) {
        for (unsigned i=0;i<expected.size();++i) if (std::memcmp(actual+i,expected.data()+i,sizeof(float))!=0) {
            error="packed decoder mismatch component="+std::to_string(i)+" expected="+std::to_string(expected[i])+" actual="+std::to_string(actual[i]); passed=false; break;
        }
        if (!glUnmapBuffer(GL_TRANSFORM_FEEDBACK_BUFFER)) passed=false;
    }
    if (!passed && error.empty()) error="packed decoder capture failed";
    glDisable(GL_RASTERIZER_DISCARD); glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER,0,0); glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER,0);
    glUseProgram(0); glBindVertexArray(0); glDeleteProgram(program); glBindTexture(GL_TEXTURE_2D,0); glActiveTexture(GL_TEXTURE0);
    return passed;
}

bool RunGPUVertexSelfTest(std::string& error) {
    if (!RunPackedDecodeFixture(error)) return false;
    if (!RunFeedbackCaptureFixture(error)) return false;
    for (u16 mask : {u16(0xffff), u16(0x0083), u16(0x0003)}) {
        if (!RunVertexLayoutFixture(mask, error)) {
            error += " input mask=" + std::to_string(mask);
            return false;
        }
    }
    GLint max_texture_size=0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE,&max_texture_size);
    const u16 large_mask=max_texture_size>=8192?u16(0xffff):u16(0x0083);
    for (size_t count : {size_t(41967),size_t(65535)}) {
        if (!RunVertexLayoutFixture(large_mask,error,count)) {
            error += " large vertex count="+std::to_string(count);
            return false;
        }
    }
    for (const auto& fixture : {std::pair<size_t,size_t>{41967,257}, {65535,65535}}) {
        for (unsigned mode : {1u,2u}) if (!RunVertexLayoutFixture(large_mask,error,fixture.first,mode,fixture.second)) {
            error+=" indexed vertex count="+std::to_string(fixture.first);
            return false;
        }
    }
    return true;
}
