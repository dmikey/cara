// Modified for Cara on 2026-11-12; see NOTICE for attribution.
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
#include "common/scope_exit.h"
#include "video_core/renderer_opengl/gl_state.h"
#include "video_core/renderer_opengl/gl_vars.h"
#include "video_core/regs.h"
#include "video_core/renderer_opengl/gl_resource_manager.h"
#include "video_core/renderer_opengl/gl_shader_gen.h"
#include "video_core/renderer_opengl/gl_shader_manager.h"
#include "video_core/renderer_opengl/gl_vertex_input_layout.h"
#include "video_core/renderer_opengl/gl_vertex_index_cache.h"
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

static bool RunVertexLayoutFixture(u16 input_mask, std::string& error, size_t vertex_count=99, unsigned indexed_inputs=0, size_t source_count=257, bool source_optimizations=true) {
    using namespace OpenGL;
    GLint saved_active=GL_TEXTURE0,saved_texture10=0,saved_sampler10=0;
    if (indexed_inputs==6) {
        glGetIntegerv(GL_ACTIVE_TEXTURE,&saved_active); glActiveTexture(GL_TEXTURE10);
        glGetIntegerv(GL_TEXTURE_BINDING_2D,&saved_texture10);
        glGetIntegeri_v(GL_SAMPLER_BINDING,10,&saved_sampler10);
        glActiveTexture(static_cast<GLenum>(saved_active));
    }
    SCOPE_EXIT({
        if (indexed_inputs==6) {
            glActiveTexture(GL_TEXTURE10); glBindTexture(GL_TEXTURE_2D,static_cast<GLuint>(saved_texture10));
            glBindSampler(10,static_cast<GLuint>(saved_sampler10));
            glActiveTexture(static_cast<GLenum>(saved_active));
        }
    });
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
    // The bundled inline assembler constructs MAD operands but its binary
    // compiler does not handle MultiplyAdd. Encode this original fixture
    // instruction directly, as we do for the flow-control instructions below.
    // W=1 and quaternion Z=0 preserve the expected quaternion while exercising
    // the sliced namespace's GLES multiply/add helper.
    nihstro::SwizzlePattern mad_swizzle{};
    mad_swizzle.dest_mask = 0xF;
    for (unsigned component=0; component<4; ++component) {
        mad_swizzle.SetSelectorSrc1(component,static_cast<nihstro::SwizzlePattern::Selector>(component));
        mad_swizzle.SetSelectorSrc2(component,nihstro::SwizzlePattern::Selector::w);
        mad_swizzle.SetSelectorSrc3(component,nihstro::SwizzlePattern::Selector::z);
    }
    const unsigned mad_descriptor=static_cast<unsigned>(binary.swizzle_table.size());
    setup.swizzle_data[mad_descriptor]=mad_swizzle.hex;
    nihstro::Instruction mad{};
    mad.opcode=O::Id::MAD;
    mad.mad.dest=D::MakeOutput(1);
    mad.mad.src1=S::MakeInput(1);
    mad.mad.src2=S::MakeInput(0);
    mad.mad.src3=S::MakeInput(1);
    mad.mad.operand_desc_id=mad_descriptor;
    setup.program_code[1]=mad.hex;
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
    std::string vertex_source=GenerateVertexShader(setup, config, false, &output, input_mask, indexed_inputs, source_optimizations);
    const std::string direct_vertex_source=vertex_source;
    // Exercise the entire large atlas without relying on large feedback captures.
    for (size_t pos=0;(pos=vertex_source.find("gl_VertexID",pos))!=std::string::npos;) {
        // ES transform feedback only accepts array draws. For the direct-input
        // fixture, emulate the element fetch here; the raster check below uses
        // the unmodified production shader and a real element buffer.
        const std::string replacement=indexed_inputs==6 ?
            "int(texelFetch(cara_fixture_elements, ivec2((gl_VertexID + capture_base) % 128, (gl_VertexID + capture_base) / 128), 0).r)" : indexed_inputs==3 ?
            "int(texelFetch(cara_vertex_indices, ivec2((gl_VertexID + capture_base) % 128, (gl_VertexID + capture_base) / 128), 0).r)" :
            "(gl_VertexID + capture_base)";
        vertex_source.replace(pos,11,replacement); pos+=replacement.size();
    }
    vertex_source="uniform highp int capture_base;\n"+vertex_source;
    if (indexed_inputs==6) vertex_source="uniform highp usampler2D cara_fixture_elements;\n"+vertex_source;
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
    size_t record_count=source_map->Size();
    if (indexed_inputs==3 && source_count!=vertex_count) {
        // Exercise the entire unsigned index domain without requiring 65,536
        // unique triangle vertices (the draw count must be divisible by three).
        const u16 relocated=static_cast<u16>(record_count-1);
        packed.resize(size_t(65536)*layout.stride);
        std::copy_n(packed.begin()+size_t(relocated)*layout.stride,layout.stride,
                    packed.begin()+size_t(65535)*layout.stride);
        for (auto& index:indices) if (index==relocated) index=65535;
        record_count=65536;
    }
    const size_t texture_rows=(packed.size()+IOS_VERTEX_INPUT_WIDTH-1)/IOS_VERTEX_INPUT_WIDTH;
    packed.resize(texture_rows*IOS_VERTEX_INPUT_WIDTH);
    OGLTexture texture; texture.Create();
    glActiveTexture(GL_TEXTURE7); glBindTexture(GL_TEXTURE_2D, texture.handle);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, indexed_inputs>=2?GL_RGBA32UI:GL_RGBA32F, IOS_VERTEX_INPUT_WIDTH, static_cast<GLsizei>(texture_rows), 0, indexed_inputs>=2?GL_RGBA_INTEGER:GL_RGBA, indexed_inputs>=2?GL_UNSIGNED_INT:GL_FLOAT, packed.data());
    IOSVertexIndexCache pair_cache;
    const IOSVertexIndexCache::Data* pairs=nullptr;
    OGLTexture fixture_elements, original_indices;
    if (indexed_inputs==6) {
        std::vector<u32> topology(indices.begin(),indices.begin()+vertex_count);
        pairs=pair_cache.Store({},nullptr,0,topology,true,true);
        if (!pairs || !pairs->lit_pair_count) { error="lit pair fixture topology rejected"; return false; }
        fixture_elements.Create(); glActiveTexture(GL_TEXTURE10); glBindTexture(GL_TEXTURE_2D,fixture_elements.handle);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST); glBindSampler(10,0);
        auto padded=pairs->lit_elements; padded.resize(index_rows*IOS_VERTEX_INPUT_WIDTH,0);
        glTexImage2D(GL_TEXTURE_2D,0,GL_R16UI,IOS_VERTEX_INPUT_WIDTH,index_rows,0,GL_RED_INTEGER,GL_UNSIGNED_SHORT,padded.data());
        original_indices.Create(); glActiveTexture(GL_TEXTURE8); glBindTexture(GL_TEXTURE_2D,original_indices.handle);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D,0,GL_R16UI,IOS_VERTEX_INPUT_WIDTH,index_rows,0,GL_RED_INTEGER,GL_UNSIGNED_SHORT,indices.data());
    }
    OGLTexture index_texture;
    if (indexed_inputs) {
        index_texture.Create(); glActiveTexture(GL_TEXTURE8); glBindTexture(GL_TEXTURE_2D,index_texture.handle);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glBindSampler(8,0);
        glTexImage2D(GL_TEXTURE_2D,0,indexed_inputs==6?GL_RG16UI:GL_R16UI,IOS_VERTEX_INPUT_WIDTH,
                     static_cast<GLsizei>(indexed_inputs==6?pairs->lit_pairs.size()/(IOS_VERTEX_INPUT_WIDTH*2):index_rows),0,
                     indexed_inputs==6?GL_RG_INTEGER:GL_RED_INTEGER,GL_UNSIGNED_SHORT,
                     indexed_inputs==6?pairs->lit_pairs.data():indices.data());
        glActiveTexture(GL_TEXTURE7);
    }
    OGLBuffer uniform_buffer, feedback_buffer, raster_uniforms; uniform_buffer.Create(); feedback_buffer.Create(); raster_uniforms.Create();
    OGLVertexArray vao; vao.Create(); glBindVertexArray(vao.handle);
    glUseProgram(program);
    glUniform1i(glGetUniformLocation(program, "cara_vertex_inputs"), 7);
    glUniform1i(glGetUniformLocation(program, "cara_vertex_indices"), 8);
    if (indexed_inputs==6) glUniform1i(glGetUniformLocation(program,"cara_fixture_elements"),10);
    if (indexed_inputs>=2) {
        std::array<std::array<GLint,4>,16> raw_layout{};
        for (unsigned reg=0;reg<16;++reg) if (layout.offsets[reg]>=0)
            raw_layout[reg]={layout.offsets[reg]*16,GLint(layout.stride*16),3,4};
        glUniform4iv(glGetUniformLocation(program,"cara_vertex_layout[0]"),16,raw_layout[0].data());
    }

    glUniformBlockBinding(program, glGetUniformBlockIndex(program, "vs_config"), 0);
    const GLvec4 viewport = {13.0f, 7.0f, 200.25f, 120.125f};
    UniformData raster_data{};
    raster_data.cara_viewport = viewport;
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
                        state.registers.output[1][c].ToFloat32() * (indexed_inputs!=3 && quaternion_dot < 0 ? -1 : 1),
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
    if (indexed_inputs==6 && passed) {
        // Compare exact quaternion bits with the ordinary production shader on
        // original special-value inputs. Array capture emulates the EBO lookup;
        // real glDrawElements raster equivalence is exercised below.
        const auto special_check = [&](const char* stage, int path = -1) {
            const GLenum code = glGetError();
            if (code == GL_NO_ERROR) return true;
            error = std::string("lit pair special quaternion stage=") + stage +
                    " path=" + std::to_string(path) + " GL=" + std::to_string(code);
            return false;
        };
        const auto run_special = [&]() -> bool {
        if (!special_check("preexisting-before-baseline")) return false;
        auto baseline_source=GenerateVertexShader(setup,config,false,&output,input_mask,2,false);
        for (size_t pos=0;(pos=baseline_source.find("gl_VertexID",pos))!=std::string::npos;) {
            const std::string replacement="(gl_VertexID + capture_base)";
            baseline_source.replace(pos,11,replacement); pos+=replacement.size();
        }
        baseline_source="uniform highp int capture_base;\n"+baseline_source;
        OGLShader baseline_vertex; baseline_vertex.Create(baseline_source.c_str(),GL_VERTEX_SHADER);
        if (!special_check("baseline-vertex-compile")) return false;
        const GLuint baseline=glCreateProgram();
        SCOPE_EXIT({ glDeleteProgram(baseline); });
        if (!special_check("baseline-program-create")) return false;
        glAttachShader(baseline,baseline_vertex.handle); glAttachShader(baseline,fragment.handle);
        glTransformFeedbackVaryings(baseline,3,varying,GL_INTERLEAVED_ATTRIBS); glLinkProgram(baseline);
        if (!special_check("baseline-link")) return false;
        GLint linked_baseline=0; glGetProgramiv(baseline,GL_LINK_STATUS,&linked_baseline);
        if (!special_check("baseline-link-status")) return false;
        if (!linked_baseline) { error="lit pair special quaternion stage=baseline-link-status path=-1 linked=0"; return false; }
        constexpr std::array<u32,8> patterns{0x00000000u,0x80000000u,0x7FC12345u,0xFFC54321u,
                                            0x3F800000u,0xBF800000u,0x7F800000u,0xFF800000u};
        for (size_t v=0;v<record_count;++v) {
            auto& quaternion_input=packed[v*layout.stride+layout.offsets[1]];
            for (size_t c=0;c<4;++c) {
                const u32 bits=patterns[(v+c)%patterns.size()];
                std::memcpy(&quaternion_input[c],&bits,sizeof(bits));
            }
        }
        glActiveTexture(GL_TEXTURE7); glBindTexture(GL_TEXTURE_2D,texture.handle);
        glTexSubImage2D(GL_TEXTURE_2D,0,0,0,IOS_VERTEX_INPUT_WIDTH,texture_rows,
                        GL_RGBA_INTEGER,GL_UNSIGNED_INT,packed.data());
        if (!special_check("special-input-upload")) return false;
        constexpr size_t special_vertices=99;
        std::array<std::array<u32,special_vertices*12>,2> captured{};
        const GLuint special_programs[]={baseline,program};
        for (unsigned path=0;path<2;++path) {
            glUseProgram(special_programs[path]);
            if (!special_check("use-program",path)) return false;
            glUniform1i(glGetUniformLocation(special_programs[path],"capture_base"),0);
            glUniform1i(glGetUniformLocation(special_programs[path],"cara_vertex_inputs"),7);
            glUniform1i(glGetUniformLocation(special_programs[path],"cara_vertex_indices"),8);
            if (path) glUniform1i(glGetUniformLocation(special_programs[path],"cara_fixture_elements"),10);
            if (!special_check("sampler-and-capture-uniforms",path)) return false;
            std::array<std::array<GLint,4>,16> raw_layout{};
            for (unsigned reg=0;reg<16;++reg) if (layout.offsets[reg]>=0)
                raw_layout[reg]={layout.offsets[reg]*16,GLint(layout.stride*16),3,4};
            glUniform4iv(glGetUniformLocation(special_programs[path],"cara_vertex_layout[0]"),16,raw_layout[0].data());
            if (!special_check("layout-uniform",path)) return false;
            const GLuint vertex_block=glGetUniformBlockIndex(special_programs[path],"vs_config");
            if (!special_check("vs-config-index",path)) return false;
            if (vertex_block!=GL_INVALID_INDEX) glUniformBlockBinding(special_programs[path],vertex_block,0);
            if (!special_check("vs-config-binding",path)) return false;
            const GLuint common=glGetUniformBlockIndex(special_programs[path],"shader_data");
            if (!special_check("shader-data-index",path)) return false;
            if (common!=GL_INVALID_INDEX) glUniformBlockBinding(special_programs[path],common,1);
            if (!special_check("shader-data-binding",path)) return false;
            glActiveTexture(GL_TEXTURE8); glBindTexture(GL_TEXTURE_2D,path?index_texture.handle:original_indices.handle);
            if (!special_check("index-texture-binding",path)) return false;
            glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER,feedback_buffer.handle);
            if (!special_check("feedback-buffer-binding",path)) return false;
            glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER,sizeof(captured[path]),nullptr,GL_STREAM_READ);
            if (!special_check("feedback-buffer-storage",path)) return false;
            glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER,0,feedback_buffer.handle);
            if (!special_check("feedback-indexed-binding",path)) return false;
            glBeginTransformFeedback(GL_TRIANGLES);
            if (!special_check("feedback-begin",path)) return false;
            glDrawArrays(GL_TRIANGLES,0,special_vertices);
            const bool draw_ok=special_check("feedback-draw",path);
            glEndTransformFeedback();
            if (!draw_ok) return false;
            if (!special_check("feedback-end",path)) return false;
            const void* bytes=glMapBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER,0,sizeof(captured[path]),GL_MAP_READ_BIT);
            const bool map_ok=special_check("feedback-map",path);
            if (!bytes) {
                if (map_ok) error="lit pair special quaternion stage=feedback-map path="+std::to_string(path)+" GL=0 null=1";
                return false;
            }
            if (!map_ok) { glUnmapBuffer(GL_TRANSFORM_FEEDBACK_BUFFER); return false; }
            std::memcpy(captured[path].data(),bytes,sizeof(captured[path]));
            const bool unmap_ok=glUnmapBuffer(GL_TRANSFORM_FEEDBACK_BUFFER)==GL_TRUE;
            if (!special_check("feedback-unmap",path)) return false;
            if (!unmap_ok) {
                error="lit pair special quaternion stage=feedback-unmap path="+std::to_string(path)+" GL=0 corrupted=1";
                return false;
            }
        }
        for (size_t v=0;v<special_vertices;++v) {
            if (std::memcmp(captured[0].data()+v*12+4,captured[1].data()+v*12+4,4*sizeof(u32))!=0) {
                error="lit pair signed-zero/NaN quaternion bits changed at vertex="+std::to_string(v); return false;
            }
        }
        return true;
        };
        passed=run_special();
        glUseProgram(program);
        glActiveTexture(GL_TEXTURE8); glBindTexture(GL_TEXTURE_2D,index_texture.handle);
        glActiveTexture(GL_TEXTURE7);
    }
    glDisable(GL_RASTERIZER_DISCARD);
    if (!passed && indexed_inputs==6) { glDeleteProgram(program); return false; }
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
    OGLProgram direct_program;
    OGLBuffer element_buffer;
    if (indexed_inputs==3 || indexed_inputs==6) {
        direct_program.Create(direct_vertex_source.c_str(), "out vec4 color; void main() { color=vec4(1.0); }");
        if (!direct_program.handle) { error="direct indexed fixture linkage failed"; passed=false; }
        glUseProgram(direct_program.handle);
        glUniform1i(glGetUniformLocation(direct_program.handle,"cara_vertex_inputs"),7);
        glUniform1i(glGetUniformLocation(direct_program.handle,"cara_vertex_indices"),8);
        std::array<std::array<GLint,4>,16> raw_layout{};
        for (unsigned reg=0;reg<16;++reg) if (layout.offsets[reg]>=0)
            raw_layout[reg]={layout.offsets[reg]*16,GLint(layout.stride*16),3,4};
        glUniform4iv(glGetUniformLocation(direct_program.handle,"cara_vertex_layout[0]"),16,raw_layout[0].data());
        const GLuint vs_block=glGetUniformBlockIndex(direct_program.handle,"vs_config");
        if (vs_block!=GL_INVALID_INDEX) glUniformBlockBinding(direct_program.handle,vs_block,0);
        const GLuint common_block=glGetUniformBlockIndex(direct_program.handle,"shader_data");
        if (common_block!=GL_INVALID_INDEX) glUniformBlockBinding(direct_program.handle,common_block,1);
        element_buffer.Create(); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,element_buffer.handle);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,indexed_inputs==6?pairs->lit_elements.size()*sizeof(u16):indices.size()*sizeof(u16),
                     indexed_inputs==6?pairs->lit_elements.data():indices.data(),GL_STATIC_DRAW);
    }
    for (bool outside : {false, true}) {
        for (size_t v = 0; v < record_count; ++v)
            packed[v * layout.stride + layout.offsets[0]][2] = outside ? 0.125f : -0.5f;
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, IOS_VERTEX_INPUT_WIDTH,
                        static_cast<GLsizei>(texture_rows), indexed_inputs>=2?GL_RGBA_INTEGER:GL_RGBA, indexed_inputs>=2?GL_UNSIGNED_INT:GL_FLOAT, packed.data());
        glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT);
        if (indexed_inputs==3 || indexed_inputs==6)
            glDrawElements(GL_TRIANGLES,static_cast<GLsizei>(vertex_count),GL_UNSIGNED_SHORT,nullptr);
        else glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertex_count));
        const GLenum draw_error=glGetError();
        if (draw_error!=GL_NO_ERROR) { error="indexed raster fixture GL error="+std::to_string(draw_error); passed=false; }
        std::array<u8, 4> pixel{};
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
        const u8 expected = outside ? 0 : 255;
        if (pixel[0] != expected || pixel[1] != expected || pixel[2] != expected) {
            passed = false;
            error = outside ? "PICA positive-z clipping plane missing" : "PICA in-range triangle incorrectly clipped";
        }
    }
    if ((indexed_inputs==3 || indexed_inputs==6) && passed) {
        // Compare real indexed rasterization against the expanded production
        // path. This includes duplicate/reordered IDs and the relocated 65535
        // record, with interpolated colors so a wrong element fetch is visible.
        OGLProgram expanded_color, indexed_color;
        const char* color_fragment="in highp vec4 primary_color; out vec4 color; void main() { color=primary_color; }";
        const std::string expanded_source=GenerateVertexShader(setup,config,false,&output,input_mask,2,source_optimizations);
        expanded_color.Create(expanded_source.c_str(),color_fragment);
        indexed_color.Create(direct_vertex_source.c_str(),color_fragment);
        if (!expanded_color.handle || !indexed_color.handle) {
            passed=false; error="indexed raster equivalence linkage failed";
        } else {
            glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,clip_color.handle);
            glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,8,8,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
            glViewport(0,0,8,8); glDisable(GL_BLEND); glDisable(GL_STENCIL_TEST);
            glActiveTexture(GL_TEXTURE7); glBindTexture(GL_TEXTURE_2D,texture.handle);
            for (size_t v=0;v<record_count;++v)
                packed[v*layout.stride+layout.offsets[0]][2]=-0.5f;
            glTexSubImage2D(GL_TEXTURE_2D,0,0,0,IOS_VERTEX_INPUT_WIDTH,
                           static_cast<GLsizei>(texture_rows),GL_RGBA_INTEGER,GL_UNSIGNED_INT,packed.data());
            std::array<std::array<u8,8*8*4>,2> images{};
            const GLuint programs[]={expanded_color.handle,indexed_color.handle};
            for (unsigned path=0;path<2;++path) {
                if (indexed_inputs==6) {
                    glActiveTexture(GL_TEXTURE8);
                    glBindTexture(GL_TEXTURE_2D,path?index_texture.handle:original_indices.handle);
                    glActiveTexture(GL_TEXTURE7);
                }
                glUseProgram(programs[path]);
                glUniform1i(glGetUniformLocation(programs[path],"cara_vertex_inputs"),7);
                glUniform1i(glGetUniformLocation(programs[path],"cara_vertex_indices"),8);
                std::array<std::array<GLint,4>,16> raw_layout{};
                for (unsigned reg=0;reg<16;++reg) if (layout.offsets[reg]>=0)
                    raw_layout[reg]={layout.offsets[reg]*16,GLint(layout.stride*16),3,4};
                glUniform4iv(glGetUniformLocation(programs[path],"cara_vertex_layout[0]"),16,raw_layout[0].data());
                const GLuint vs_block=glGetUniformBlockIndex(programs[path],"vs_config");
                if (vs_block!=GL_INVALID_INDEX) glUniformBlockBinding(programs[path],vs_block,0);
                const GLuint common_block=glGetUniformBlockIndex(programs[path],"shader_data");
                if (common_block!=GL_INVALID_INDEX) glUniformBlockBinding(programs[path],common_block,1);
                glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
                if (path) glDrawElements(GL_TRIANGLES,static_cast<GLsizei>(vertex_count),GL_UNSIGNED_SHORT,nullptr);
                else glDrawArrays(GL_TRIANGLES,0,static_cast<GLsizei>(vertex_count));
                glReadPixels(0,0,8,8,GL_RGBA,GL_UNSIGNED_BYTE,images[path].data());
                if (glGetError()!=GL_NO_ERROR) { passed=false; error="indexed raster equivalence GL error"; }
            }
            if (images[0]!=images[1] || std::all_of(images[0].begin(),images[0].end(),[](u8 byte){return byte==0;})) {
                passed=false; error="indexed raster colors differ from expanded draw";
            }
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
    glActiveTexture(GL_TEXTURE10); glBindTexture(GL_TEXTURE_2D,0);
    glActiveTexture(GL_TEXTURE8); glBindTexture(GL_TEXTURE_2D,0);
    glActiveTexture(GL_TEXTURE0);
    return passed;
}

static bool RunPackedDecodeFixture(std::string& error,
                                   const char* decoder=OpenGL::PackedVertexDecoderGLSL,
                                   unsigned offset_bias=0, unsigned stride=24, bool boundary=false,
                                   bool empty_attributes=false) {
    using namespace OpenGL;
    const GLenum entry_error=glGetError();
    GLint old_draw=0,old_read=0;std::array<GLint,4> old_viewport{};
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&old_draw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&old_read);
    glGetIntegerv(GL_VIEWPORT,old_viewport.data());
    OGLFramebuffer capture_fbo;capture_fbo.Create();
    OGLTexture capture_color;capture_color.Create();
    SCOPE_EXIT({
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER,old_draw);
        glBindFramebuffer(GL_READ_FRAMEBUFFER,old_read);
        glViewport(old_viewport[0],old_viewport[1],old_viewport[2],old_viewport[3]);
    });
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER,0);
    glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,capture_color.handle);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
    glBindFramebuffer(GL_FRAMEBUFFER,capture_fbo.handle);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,capture_color.handle,0);
    glViewport(0,0,1,1);
    const GLenum framebuffer_status=glCheckFramebufferStatus(GL_FRAMEBUFFER);
    constexpr unsigned count=3072, per_attribute=count/16;
    std::array<std::array<GLint,4>,16> layout{};
    std::vector<u8> bytes(16*per_attribute*32,0xA5);
    std::vector<float> expected(count*4);
    std::vector<float> floats={-32768.f,32767.f,-1.25f,0.f,-0.f,0.125f,1024.5f,1.0e-20f};
    if (boundary) {
        const std::array<u32,12> patterns={0u,0x80000000u,0x7f800000u,0xff800000u,
            0x7fc12345u,0xffc54321u,0x00000001u,0x00800000u,0x7f7fffffu,
            0x3fa00000u,0xc0200000u,0x3f800000u};
        floats.resize(patterns.size()+2);
        std::memcpy(floats.data(),patterns.data(),sizeof(patterns));
        floats[patterns.size()]=Pica::float24::FromRaw(0x3f0001).ToFloat32();
        floats[patterns.size()+1]=Pica::float24::FromRaw(0xbf8001).ToFloat32();
    }
    for (unsigned reg=0;reg<16;++reg) {
        const unsigned format=reg/4, components=empty_attributes?0:reg%4+1;
        const unsigned width=format==3?4:format==2?2:1;
        const unsigned offset=empty_attributes?unsigned(bytes.size()):
            reg*per_attribute*32+(boundary?offset_bias:reg%4);
        layout[reg]={GLint(offset),GLint(stride),GLint(format),GLint(components)};
        for (unsigned v=0;v<per_attribute;++v) for (unsigned c=0;c<4;++c) {
            float value=c==3?1.f:0.f;
            if (c<components) {
                const unsigned sample=stride?v:0;
                u8* destination=bytes.data()+offset+v*stride+c*width;
                const u32 bits=(sample*977+reg*131+c*65521)^0x8000;
                if (format==0) { s8 x=static_cast<s8>(bits); std::memcpy(destination,&x,1); value=x; }
                if (format==1) { u8 x=static_cast<u8>(bits); std::memcpy(destination,&x,1); value=x; }
                if (format==2) { s16 x=static_cast<s16>(bits); std::memcpy(destination,&x,2); value=x; }
                if (format==3) { value=floats[(sample+c)%floats.size()]; std::memcpy(destination,&value,4); }
            }
            expected[(v*16+reg)*4+c]=value;
        }
    }
    const unsigned rows=(bytes.size()+2047)/2048; bytes.resize(rows*2048);
    OGLShader vertex,fragment;
    vertex.Create((std::string(decoder)+"out highp vec4 decoded; void main(){ decoded=cara_decode_vertex(gl_VertexID/16,gl_VertexID%16); gl_Position=vec4(0,0,0,1); }").c_str(),GL_VERTEX_SHADER);
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
    const std::vector<u32> sentinel(expected.size(),0xCDCDCDCDu);
    glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER,expected.size()*sizeof(float),sentinel.data(),GL_STREAM_READ);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER,0,output.handle); glUseProgram(program);
    glUniform1i(glGetUniformLocation(program,"cara_vertex_inputs"),7);
    glUniform4iv(glGetUniformLocation(program,"cara_vertex_layout[0]"),16,layout[0].data());
    const GLenum setup_error=glGetError();
    glEnable(GL_RASTERIZER_DISCARD); glBeginTransformFeedback(GL_TRIANGLES); glDrawArrays(GL_TRIANGLES,0,count); glEndTransformFeedback();
    const auto draw_error=glGetError();
    const float* actual=static_cast<const float*>(glMapBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER,0,expected.size()*sizeof(float),GL_MAP_READ_BIT));
    bool passed=actual && entry_error==GL_NO_ERROR && setup_error==GL_NO_ERROR &&
        draw_error==GL_NO_ERROR && framebuffer_status==GL_FRAMEBUFFER_COMPLETE;
    if (actual) {
        for (unsigned i=0;i<expected.size();++i) if (std::memcmp(actual+i,expected.data()+i,sizeof(float))!=0) {
            u32 wanted_bits=0,actual_bits=0;
            std::memcpy(&wanted_bits,expected.data()+i,4);std::memcpy(&actual_bits,actual+i,4);
            error="packed decoder mismatch component="+std::to_string(i)+" expected_bits="+
                std::to_string(wanted_bits)+" actual_bits="+std::to_string(actual_bits);passed=false;break;
        }
        if (!glUnmapBuffer(GL_TRANSFORM_FEEDBACK_BUFFER)) passed=false;
    }
    if (!passed && error.empty()) error="packed decoder capture failed";
    if (!passed) error += " entryGL="+std::to_string(entry_error)+" setupGL="+
        std::to_string(setup_error)+" drawGL="+std::to_string(draw_error)+
        " framebuffer="+std::to_string(framebuffer_status)+" boundary="+std::to_string(boundary)+
        " empty="+std::to_string(empty_attributes)+" offset="+std::to_string(offset_bias)+
        " stride="+std::to_string(stride)+" fetch_reuse="+
        std::to_string(decoder==PackedVertexFetchReuseGLSL);
    glDisable(GL_RASTERIZER_DISCARD); glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER,0,0); glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER,0);
    glUseProgram(0); glBindVertexArray(0); glDeleteProgram(program); glBindTexture(GL_TEXTURE_2D,0); glActiveTexture(GL_TEXTURE0);
    return passed;
}

// Exercise the exact feedback -> pixel-unpack -> texture-fetch path using only
// generated PICA instructions and vertex data. Readback is confined to this test.
static bool RunLitVertexReuseFixture(unsigned source_count, std::string& error) {
    using namespace OpenGL;
    using S=nihstro::SourceRegister;
    using D=nihstro::DestRegister;
    using O=nihstro::OpCode;
    using Semantic=Pica::RasterizerRegs::VSOutputAttributes;
    const auto binary=nihstro::InlineAsm::CompileToRawBinary({
        {O::Id::MOV,D::MakeOutput(0),S::MakeInput(0)},
        {O::Id::MOV,D::MakeOutput(1),S::MakeInput(1)},
        {O::Id::MOV,D::MakeOutput(2),S::MakeInput(2)},
        {O::Id::MOV,D::MakeOutput(3),S::MakeInput(3)},
        {O::Id::MOV,D::MakeOutput(4),S::MakeInput(4)},
        {O::Id::MOV,D::MakeOutput(5),S::MakeInput(5)},
        {O::Id::END},
    });
    Pica::Shader::ShaderSetup setup{};
    std::transform(binary.program.begin(),binary.program.end(),setup.program_code.begin(),[](auto v){return v.hex;});
    std::transform(binary.swizzle_table.begin(),binary.swizzle_table.end(),setup.swizzle_data.begin(),[](auto v){return v.hex;});
    Pica::Regs regs{};
    regs.vs.output_mask.Assign(0x3f); regs.vs.max_input_attribute_index.Assign(5);
    regs.vs.input_attribute_to_register_map_low=0x543210;
    regs.rasterizer.vs_output_total.Assign(6);
    const std::array<std::array<Semantic::Semantic,4>,6> semantics{{
        {Semantic::POSITION_X,Semantic::POSITION_Y,Semantic::POSITION_Z,Semantic::POSITION_W},
        {Semantic::QUATERNION_X,Semantic::QUATERNION_Y,Semantic::QUATERNION_Z,Semantic::QUATERNION_W},
        {Semantic::COLOR_R,Semantic::COLOR_G,Semantic::COLOR_B,Semantic::COLOR_A},
        {Semantic::TEXCOORD0_U,Semantic::TEXCOORD0_V,Semantic::TEXCOORD1_U,Semantic::TEXCOORD1_V},
        {Semantic::TEXCOORD2_U,Semantic::TEXCOORD2_V,Semantic::TEXCOORD0_W,Semantic::INVALID},
        {Semantic::VIEW_X,Semantic::VIEW_Y,Semantic::VIEW_Z,Semantic::INVALID},
    }};
    for (unsigned i=0;i<semantics.size();++i) {
        auto& a=regs.rasterizer.vs_output_attributes[i];
        a.map_x.Assign(semantics[i][0]); a.map_y.Assign(semantics[i][1]);
        a.map_z.Assign(semantics[i][2]); a.map_w.Assign(semantics[i][3]);
    }
    const PicaVSConfig config(regs,setup);
    const PicaFixedGSConfig mapping(regs);
    const char* captured[]={"gl_Position","normquat","primary_color","texcoord0","texcoord1","texcoord2","texcoord0_w","view"};
    const char* feedback_names[]={"cara_feedback0","cara_feedback1","cara_feedback2","cara_feedback3","cara_feedback4","cara_feedback5"};
    const auto link=[&](unsigned mode,bool capture_base) {
        OGLProgram result;
        std::string source=GenerateVertexShader(setup,config,false,&mapping,0x3f,mode);
        if (capture_base) {
            for (size_t pos=0;(pos=source.find("gl_VertexID",pos))!=std::string::npos;) {
                const std::string replacement="(gl_VertexID+capture_base)";
                source.replace(pos,11,replacement);pos+=replacement.size();
            }
            source="uniform highp int capture_base;\n"+source;
        }
        OGLShader vertex,fragment;
        vertex.Create(source.c_str(),GL_VERTEX_SHADER);
        fragment.Create("out vec4 color;void main(){color=vec4(1.0);}",GL_FRAGMENT_SHADER);
        if (!vertex.handle || !fragment.handle) return result;
        result.handle=glCreateProgram(); glAttachShader(result.handle,vertex.handle);glAttachShader(result.handle,fragment.handle);
        glTransformFeedbackVaryings(result.handle,mode==4?6:8,mode==4?feedback_names:captured,GL_INTERLEAVED_ATTRIBS);
        glLinkProgram(result.handle);
        GLint linked=0;glGetProgramiv(result.handle,GL_LINK_STATUS,&linked);
        if (!linked) { glDeleteProgram(result.handle);result.handle=0; }
        return result;
    };
    auto feedback=link(4,false), expanded=link(2,true), reused=link(5,true);
    if (!feedback.handle || !expanded.handle || !reused.handle) { error="lit reuse fixture linkage failed";return false; }
    const unsigned rows=(source_count*6+127)/128;
    std::vector<GLvec4> inputs(rows*128);
    for (unsigned v=0;v<source_count;++v) {
        const float w=1.0f+float(v%3)*0.25f;
        inputs[v*6]={float(int(v%13)-6)/8.0f,float(int(v%7)-3)/8.0f,-w/2.0f,w};
        const float sign=v%2?-1.0f:1.0f;
        inputs[v*6+1]={sign,sign*float(v%3)/4.0f,sign*float(v%5)/8.0f,0.25f*sign};
        inputs[v*6+2]={float(v%17)/8.0f,-float(v%11)/8.0f,float(v%7)/8.0f,1.0f};
        inputs[v*6+3]={float(v%19)/16.0f,float(v%23)/32.0f,-float(v%29)/8.0f,float(v%31)/32.0f};
        inputs[v*6+4]={float(v%37)/32.0f,-float(v%41)/16.0f,float(v%43)/32.0f,0.5f};
        inputs[v*6+5]={float(v%47)/32.0f,float(v%53)/32.0f,float(v%59)/32.0f,0.5f};
    }
    const unsigned vertex_count=source_count*3;
    std::vector<u16> indices((vertex_count+127)/128*128);
    for (unsigned i=0;i<vertex_count;++i) indices[i]=static_cast<u16>((i*7+(i/3)*13)%source_count);
    OGLTexture input_texture,index_texture,output_texture;
    const auto allocate=[&](OGLTexture& texture,unsigned unit,GLint internal,GLenum format,GLenum type,unsigned height,const void* data) {
        texture.Create();glActiveTexture(GL_TEXTURE0+unit);glBindTexture(GL_TEXTURE_2D,texture.handle);glBindSampler(unit,0);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D,0,internal,128,height,0,format,type,data);
    };
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER,0);glPixelStorei(GL_UNPACK_ROW_LENGTH,0);
    allocate(input_texture,7,GL_RGBA32UI,GL_RGBA_INTEGER,GL_UNSIGNED_INT,rows,inputs.data());
    allocate(index_texture,8,GL_R16UI,GL_RED_INTEGER,GL_UNSIGNED_SHORT,indices.size()/128,indices.data());
    allocate(output_texture,9,GL_RGBA32F,GL_RGBA,GL_FLOAT,rows,nullptr);
    OGLVertexArray vao;vao.Create();glBindVertexArray(vao.handle);
    OGLBuffer transformed,captured_output,common_buffer;
    transformed.Create();captured_output.Create();common_buffer.Create();
    UniformData common{};common.cara_viewport={13.0f,7.0f,200.25f,120.125f};
    glBindBuffer(GL_UNIFORM_BUFFER,common_buffer.handle);glBufferData(GL_UNIFORM_BUFFER,sizeof(common),&common,GL_STREAM_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER,1,common_buffer.handle);
    const auto bind_program=[&](GLuint program) {
        glUseProgram(program);
        glUniform1i(glGetUniformLocation(program,"cara_vertex_inputs"),7);
        glUniform1i(glGetUniformLocation(program,"cara_vertex_indices"),8);
        glUniform1i(glGetUniformLocation(program,"cara_vertex_outputs"),9);
        std::array<std::array<GLint,4>,16> layout{};
        for (unsigned reg=0;reg<6;++reg) layout[reg]={GLint(reg*16),96,3,4};
        glUniform4iv(glGetUniformLocation(program,"cara_vertex_layout[0]"),16,layout[0].data());
        const GLuint block=glGetUniformBlockIndex(program,"shader_data");
        if (block!=GL_INVALID_INDEX) glUniformBlockBinding(program,block,1);
    };
    bind_program(feedback.handle);
    glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER,transformed.handle);
    glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER,inputs.size()*sizeof(GLvec4),nullptr,GL_STREAM_COPY);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER,0,transformed.handle);
    glEnable(GL_RASTERIZER_DISCARD);glBeginTransformFeedback(GL_POINTS);glDrawArrays(GL_POINTS,0,source_count);glEndTransformFeedback();
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER,0,0);glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER,0);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER,transformed.handle);glActiveTexture(GL_TEXTURE9);
    glTexSubImage2D(GL_TEXTURE_2D,0,0,0,128,rows,GL_RGBA,GL_FLOAT,nullptr);glBindBuffer(GL_PIXEL_UNPACK_BUFFER,0);
    bool passed=glGetError()==GL_NO_ERROR;
    std::array<std::vector<float>,2> values;
    const GLuint programs[]={expanded.handle,reused.handle};
    for (unsigned path=0;path<2 && passed;++path) {
        values[path].resize(size_t(vertex_count)*22);
        bind_program(programs[path]);
        for (unsigned first=0;first<vertex_count && passed;first+=3072) {
            const unsigned count=std::min(3072u,vertex_count-first);
            glUniform1i(glGetUniformLocation(programs[path],"capture_base"),first);
            glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER,captured_output.handle);
            glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER,size_t(count)*22*sizeof(float),nullptr,GL_STREAM_READ);
            glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER,0,captured_output.handle);
            glBeginTransformFeedback(GL_TRIANGLES);glDrawArrays(GL_TRIANGLES,0,count);glEndTransformFeedback();
            const auto* data=static_cast<const float*>(glMapBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER,0,size_t(count)*22*sizeof(float),GL_MAP_READ_BIT));
            if (!data) passed=false;
            else { std::copy_n(data,size_t(count)*22,values[path].begin()+size_t(first)*22);if(!glUnmapBuffer(GL_TRANSFORM_FEEDBACK_BUFFER))passed=false; }
            if (glGetError()!=GL_NO_ERROR) passed=false;
        }
    }
    // Independent scalar oracle for the generated MOV program. This also
    // rejects a driver that silently drops the same feedback range in both paths.
    for (unsigned v=0;v<vertex_count && passed;++v) {
        const unsigned source=indices[v], first=indices[v-v%3];
        std::array<float,22> expected{};
        for (unsigned c=0;c<4;++c) expected[c]=inputs[source*6][c];
        for (unsigned c=0;c<2;++c) if (!std::getenv("CARA_UNSNAPPED_VERTICES")) {
            const auto& viewport=common.cara_viewport;
            const float screen=(expected[c]/expected[3]+1.0f)*viewport[c+2]+viewport[c];
            expected[c]=((std::round(screen*16.0f)/16.0f-viewport[c])/viewport[c+2]-1.0f)*expected[3];
        }
        expected[2]=2.0f*expected[2]+expected[3];
        float dot=0;
        for (unsigned c=0;c<4;++c) dot+=inputs[first*6+1][c]*inputs[source*6+1][c];
        for (unsigned c=0;c<4;++c) {
            expected[4+c]=inputs[source*6+1][c]*(dot<0?-1.0f:1.0f);
            expected[8+c]=std::min(1.0f,std::abs(inputs[source*6+2][c]));
            expected[12+c]=inputs[source*6+3][c];
        }
        for (unsigned c=0;c<3;++c) {
            expected[16+c]=inputs[source*6+4][c];
            expected[19+c]=inputs[source*6+5][c];
        }
        for (unsigned c=0;c<22 && passed;++c)
            if (!std::isfinite(values[0][size_t(v)*22+c]) ||
                std::abs(values[0][size_t(v)*22+c]-expected[c])>1e-5f) {
                passed=false;error="lit reuse reference mismatch vertex="+std::to_string(v)+" component="+std::to_string(c);
            }
    }
    if (passed && values[0]!=values[1]) {
        passed=false;
        for (size_t i=0;i<values[0].size();++i) if (values[0][i]!=values[1][i]) {
            error="lit reuse mismatch component="+std::to_string(i)+" expected="+std::to_string(values[0][i])+" actual="+std::to_string(values[1][i]);break;
        }
    }
    if (!passed && error.empty()) error="lit reuse transfer or capture failed";
    glDisable(GL_RASTERIZER_DISCARD);glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER,0,0);glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER,0);
    glBindBufferBase(GL_UNIFORM_BUFFER,1,0);glBindBuffer(GL_UNIFORM_BUFFER,0);glBindBuffer(GL_PIXEL_UNPACK_BUFFER,0);
    glBindVertexArray(0);glUseProgram(0);
    for (unsigned unit : {7u,8u,9u}) {glActiveTexture(GL_TEXTURE0+unit);glBindTexture(GL_TEXTURE_2D,0);}
    glActiveTexture(GL_TEXTURE0);
    return passed;
}

static bool RunRetainedInputBindingFixture(std::string& error) {
    using namespace OpenGL;
    // This oracle runs before renderer initialization sets the GLES globals.
    const bool previous_gles = GLES;
    GLES = true;
    auto previous = OpenGLState::GetCurState();
    // Before renderer initialization, the window's actual framebuffer can
    // differ from the renderer's cached defaults. Preserve the real bindings.
    GLint previous_draw=0,previous_read=0;
    std::array<GLint,4> previous_viewport{};
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&previous_draw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&previous_read);
    glGetIntegerv(GL_VIEWPORT,previous_viewport.data());
    previous.draw.draw_framebuffer=previous_draw;
    previous.draw.read_framebuffer=previous_read;
    previous.viewport={previous_viewport[0],previous_viewport[1],previous_viewport[2],previous_viewport[3]};
    const GLboolean dither = glIsEnabled(GL_DITHER);
    GLint active = GL_TEXTURE0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    std::array<GLint, 2> previous_inputs{};
    for (unsigned i = 0; i < 2; ++i) {
        glActiveTexture(GL_TEXTURE7 + i);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_inputs[i]);
    }
    SCOPE_EXIT({
        for (unsigned i = 0; i < 2; ++i) {
            glActiveTexture(GL_TEXTURE7 + i);
            glBindTexture(GL_TEXTURE_2D, previous_inputs[i]);
        }
        if (dither) glEnable(GL_DITHER);
        OpenGLState::InvalidateRasterState();
        previous.Apply();
        glActiveTexture(active);
        GLES = previous_gles;
    });
    constexpr const char* positions = R"(
void main() {
    const vec2 p[3] = vec2[3](vec2(-1.0,-1.0),vec2(3.0,-1.0),vec2(-1.0,3.0));
    gl_Position=vec4(p[gl_VertexID],0.0,1.0);
}
)";
    constexpr const char* input_vertex = R"(
uniform highp usampler2D inputs;
uniform highp usampler2D indices;
out highp vec4 value;
void main() {
    const vec2 p[3] = vec2[3](vec2(-1.0,-1.0),vec2(3.0,-1.0),vec2(-1.0,3.0));
    gl_Position=vec4(p[gl_VertexID],0.0,1.0);
    int index=int(texelFetch(indices,ivec2(0),0).r);
    value=vec4(texelFetch(inputs,ivec2(index,0),0))/255.0;
}
)";
    OGLProgram input_program, copy_program;
    input_program.Create(input_vertex, "in highp vec4 value;out vec4 color;void main(){color=value;}");
    copy_program.Create(positions, "uniform sampler2D image;out vec4 color;void main(){color=texelFetch(image,ivec2(0),0);}");
    if (!input_program.handle || !copy_program.handle) {
        error="retained input fixture linkage failed"; return false;
    }
    OpenGLState::BindShaderProgram(input_program.handle);
    glUniform1i(glGetUniformLocation(input_program.handle,"inputs"),7);
    glUniform1i(glGetUniformLocation(input_program.handle,"indices"),8);
    OpenGLState::BindShaderProgram(copy_program.handle);
    glUniform1i(glGetUniformLocation(copy_program.handle,"image"),0);
    std::array<OGLTexture,2> inputs, indices, outputs;
    const std::array<std::array<u32,8>,2> colors{{{11,29,47,251,67,83,101,239},
                                               {127,149,163,227,181,199,211,223}}};
    const std::array<u16,2> selected{1,0};
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER,0);
    glPixelStorei(GL_UNPACK_ROW_LENGTH,0);
    glActiveTexture(GL_TEXTURE0);
    const auto allocate=[&](OGLTexture& texture,GLint internal,GLenum format,GLenum type,
                            unsigned width,const void* data) {
        texture.Create();glBindTexture(GL_TEXTURE_2D,texture.handle);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D,0,internal,width,1,0,format,type,data);
    };
    for (unsigned i=0;i<2;++i) {
        allocate(inputs[i],GL_RGBA32UI,GL_RGBA_INTEGER,GL_UNSIGNED_INT,2,colors[i].data());
        allocate(indices[i],GL_R16UI,GL_RED_INTEGER,GL_UNSIGNED_SHORT,1,&selected[i]);
        allocate(outputs[i],GL_RGBA8,GL_RGBA,GL_UNSIGNED_BYTE,1,nullptr);
    }
    glBindTexture(GL_TEXTURE_2D,previous.texture_units[0].texture_2d);
    OGLFramebuffer framebuffer;framebuffer.Create();
    OGLVertexArray vao;vao.Create();
    OpenGLState state;
    state.cull.enabled=false;
    state.draw.draw_framebuffer=state.draw.read_framebuffer=framebuffer.handle;
    state.draw.vertex_array=vao.handle;
    state.viewport={0,0,1,1};
    glDisable(GL_DITHER);
    bool passed=true;
    for (unsigned path : {0u,1u,0u}) {
        // Explicit begin binds match production; no end-of-draw unbinds.
        glActiveTexture(GL_TEXTURE7);glBindTexture(GL_TEXTURE_2D,inputs[path].handle);glBindSampler(7,0);
        glActiveTexture(GL_TEXTURE8);glBindTexture(GL_TEXTURE_2D,indices[path].handle);glBindSampler(8,0);
        glActiveTexture(GL_TEXTURE0);
        state.texture_units[0].texture_2d=0;
        state.draw.shader_program=input_program.handle;state.Apply();
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,outputs[0].handle,0);
        const GLenum framebuffer_status=glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
        if (framebuffer_status!=GL_FRAMEBUFFER_COMPLETE) {
            passed=false; error="retained input incomplete framebuffer="+std::to_string(framebuffer_status);
        }
        glDrawArrays(GL_TRIANGLES,0,3);
        // An ordinary display-style copy changes program, FBO attachment and
        // texture unit zero while the packed inputs remain bound on 7/8.
        state.texture_units[0].texture_2d=outputs[0].handle;
        state.draw.shader_program=copy_program.handle;state.Apply();
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,outputs[1].handle,0);
        glDrawArrays(GL_TRIANGLES,0,3);
        std::array<u8,4> pixel{};
        glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel.data());
        for (unsigned c=0;c<4;++c)
            if (pixel[c]!=colors[path][selected[path]*4+c] && passed) {
                passed=false; error="retained input path="+std::to_string(path)+
                    " component="+std::to_string(c)+" expected="+
                    std::to_string(colors[path][selected[path]*4+c])+" actual="+std::to_string(pixel[c]);
            }
        const GLenum draw_error=glGetError();
        if (draw_error!=GL_NO_ERROR) {
            passed=false; error="retained input draw/copy GL error="+std::to_string(draw_error);
        }
    }
    // Bound texture deletion must invalidate the actual driver binding; the
    // next production draw always supplies its new object explicitly.
    glActiveTexture(GL_TEXTURE7);inputs[0].Release();
    GLint binding=-1;glGetIntegerv(GL_TEXTURE_BINDING_2D,&binding);
    if (binding!=0) { passed=false;error="retained input deleted binding7="+std::to_string(binding); }
    glActiveTexture(GL_TEXTURE8);indices[0].Release();
    glGetIntegerv(GL_TEXTURE_BINDING_2D,&binding);
    if (binding!=0) { passed=false;error="retained input deleted binding8="+std::to_string(binding); }
    glActiveTexture(GL_TEXTURE0);
    OpenGLState::ResetTexture(outputs[0].handle);
    if (!passed && error.empty()) error="retained input draw/copy/deletion fixture mismatch";
    return passed;
}

bool RunGPUVertexSelfTest(std::string& error) {
    const char* retain_inputs=std::getenv("CARA_GPU_RETAIN_INPUT_BINDINGS");
    if (retain_inputs && std::strcmp(retain_inputs,"1")==0 && !RunRetainedInputBindingFixture(error)) return false;
    const char* lit_reuse=std::getenv("CARA_GPU_LIT_VERTEX_REUSE");
    if (lit_reuse && std::strcmp(lit_reuse,"1")==0)
        for (unsigned count : {128u,3072u}) if (!RunLitVertexReuseFixture(count,error)) return false;
    if (!RunPackedDecodeFixture(error)) return false;
    const char* reuse_fetch=std::getenv("CARA_GPU_PACKED_FETCH_REUSE");
    if (reuse_fetch && std::strcmp(reuse_fetch,"1")==0) {
        for (unsigned offset=0;offset<16;++offset)
            for (unsigned stride : {0u,31u})
                for (const char* decoder : {OpenGL::PackedVertexDecoderGLSL,OpenGL::PackedVertexFetchReuseGLSL})
                    if (!RunPackedDecodeFixture(error,decoder,offset,stride,true)) return false;
        // Four floats end exactly at the allocation's final texel boundary.
        for (const char* decoder : {OpenGL::PackedVertexDecoderGLSL,OpenGL::PackedVertexFetchReuseGLSL})
            if (!RunPackedDecodeFixture(error,decoder,16,32,true)) return false;
        // Empty attributes return defaults without accessing their unused address.
        for (const char* decoder : {OpenGL::PackedVertexDecoderGLSL,OpenGL::PackedVertexFetchReuseGLSL})
            if (!RunPackedDecodeFixture(error,decoder,0,0,true,true)) return false;
    }
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
        for (unsigned mode : {1u,2u,3u}) if (!RunVertexLayoutFixture(large_mask,error,fixture.first,mode,fixture.second)) {
            error+=" indexed vertex count="+std::to_string(fixture.first)+" mode="+std::to_string(mode);
            return false;
        }
    }
    const char* lit_pairs=std::getenv("CARA_GPU_LIT_PAIR_REUSE");
    if (lit_pairs && std::strcmp(lit_pairs,"1")==0) {
        for (const auto& fixture : {std::pair<size_t,size_t>{99,17}, {41967,257}, {65535,65535}})
            if (!RunVertexLayoutFixture(large_mask,error,fixture.first,6,fixture.second)) {
                error+=" lit-pair vertex count="+std::to_string(fixture.first); return false;
            }
    }
    // Compile and execute the GPU retry source with optional flags still set.
    if (!RunVertexLayoutFixture(0x0083,error,99,2,257,false)) {
        error += " baseline GPU retry source"; return false;
    }
    return true;
}
