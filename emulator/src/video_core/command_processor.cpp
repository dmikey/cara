// Copyright 2014 Citra Emulator Project
// Modified for Cara on 2026-11-12; see NOTICE for attribution.
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/cpu_category_timing.h"
#include "cara/pica_submission_probe.h"
#include "cara/guest_gpu_trace.h"
#include "video_core/pica_shader_packet_profile.h"
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <unordered_set>
#include <memory>
#include <utility>
#include "common/assert.h"
#include "common/frame_timing.h"
#include "common/logging/log.h"
#include "common/microprofile.h"
#include "common/vector_math.h"
#include "core/core.h"
#include "core/hle/service/gsp/gsp.h"
#include "core/hw/gpu.h"
#include "core/memory.h"
#include "core/settings.h"
#include "core/tracer/recorder.h"
#include "video_core/command_processor.h"
#include "video_core/debug_utils/debug_utils.h"
#include "video_core/pica_state.h"
#include "video_core/pica_types.h"
#include "video_core/primitive_assembly.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/regs.h"
#include "video_core/regs_pipeline.h"
#include "video_core/regs_texturing.h"
#include "video_core/renderer_base.h"
#include "video_core/shader/shader.h"
#include "video_core/vertex_loader.h"
#include "video_core/video_core.h"

namespace Pica::CommandProcessor {

MICROPROFILE_DEFINE(GPU_Drawing, "GPU", "Drawing", MP_RGB(50, 50, 240));

// Experiments are deliberately independent of frame-timing diagnostics. Neither
// mode changes serialized PICA state or the command stream / trigger ordering.
static bool RepeatStateEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("CARA_PICA_REPEAT_STATE");
        return value && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

static bool CommandProfileEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("CARA_PICA_COMMAND_PROFILE");
        return value && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

struct CommandProfile {
    std::uint64_t lists = 0, writes = 0, draws = 0;
    std::uint64_t list_cpu_ns = 0, draw_cpu_ns = 0, clock_failures = 0;
    std::uint64_t notifications = 0, repeated_shader_notifications = 0;
    std::uint64_t code_words = 0, repeated_code_words = 0;
    std::uint64_t swizzle_words = 0, repeated_swizzle_words = 0;
    std::array<std::uint64_t, Regs::NUM_REGS> registers{};
};
static CommandProfile command_profile;
static Cara::PicaShaderPackets::Counters shader_packet_profile;
static_assert(PICA_REG_INDEX(vs.program.set_word[0]) == Cara::PicaShaderPackets::VSProgramFirst);
static_assert(PICA_REG_INDEX(vs.swizzle_patterns.set_word[0]) == Cara::PicaShaderPackets::VSSwizzleFirst);

static void LogShaderPacketProfile() {
    static const auto session = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto snapshot = command_profile.lists;
    LOG_INFO(HW_GPU, "PICA VS packet begin schema=1 session={} snapshot={} all_packets={}",
             session, snapshot, shader_packet_profile.all_packets);
    for (std::size_t kind = 0; kind < 2; ++kind) {
        for (std::size_t grouped = 0; grouped < 2; ++grouped) {
            const auto& group = shader_packet_profile.groups[kind][grouped];
            const auto& t = group.total;
            const char* name = kind == 0 ? "program" : "swizzle";
            LOG_INFO(HW_GPU, "PICA VS packet total session={} snapshot={} kind={} grouped={} "
                     "packets={} extra_words={} complete_packets={} repeated_capacity_packets={} "
                     "eligible_packets={} eligible_extra_words={}",
                     session, snapshot, name, grouped, t.packets, t.extra_words, t.complete_packets,
                     t.capacity_packets, t.eligible_packets, t.eligible_extra_words);
            for (std::size_t bucket = 0; bucket < group.lengths.size(); ++bucket) {
                const auto& b = group.lengths[bucket];
                LOG_INFO(HW_GPU, "PICA VS packet length session={} snapshot={} kind={} grouped={} "
                         "bucket={} packets={} extra_words={} complete_packets={} repeated_capacity_packets={} "
                         "eligible_packets={} eligible_extra_words={}",
                         session, snapshot, name, grouped, bucket, b.packets, b.extra_words,
                         b.complete_packets, b.capacity_packets, b.eligible_packets, b.eligible_extra_words);
            }
            const auto& m = group.masks;
            LOG_INFO(HW_GPU, "PICA VS packet masks session={} snapshot={} kind={} grouped={} "
                     "exclusive_packets={} mirrored_packets={} m0={} m1={} m2={} m3={} "
                     "m4={} m5={} m6={} m7={} m8={} m9={} m10={} m11={} m12={} m13={} m14={} m15={}",
                     session, snapshot, name, grouped, group.mirror_packets[0], group.mirror_packets[1],
                     m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7],
                     m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
        }
    }
    LOG_INFO(HW_GPU, "PICA VS packet end session={} snapshot={} all_packets={}",
             session, snapshot, shader_packet_profile.all_packets);
}

class CommandCPUProfileScope {
public:
    explicit CommandCPUProfileScope(bool draw) : draw(draw) {
        if (!CommandProfileEnabled()) return;
        active = Common::CPUCategoryTiming::ReadThreadCPUTime(nullptr, start);
        if (!active) ++command_profile.clock_failures;
        if (draw) ++command_profile.draws;
    }
    ~CommandCPUProfileScope() {
        if (!active) return;
        std::uint64_t end;
        if (!Common::CPUCategoryTiming::ReadThreadCPUTime(nullptr, end) || end < start) {
            ++command_profile.clock_failures;
            return;
        }
        (draw ? command_profile.draw_cpu_ns : command_profile.list_cpu_ns) += end - start;
        if (!draw && ++command_profile.lists % 256 == 0) {
            // Cumulative values. Difference two complete records to exclude boot
            // and checkpoint loading. Draw CPU is nested in list CPU, not additive.
            std::array<std::pair<std::uint64_t, u32>, 4> hottest{};
            for (u32 id = 0; id < command_profile.registers.size(); ++id) {
                std::pair<std::uint64_t, u32> entry{command_profile.registers[id], id};
                for (auto& slot : hottest) {
                    if (entry.first > slot.first) std::swap(entry, slot);
                }
            }
            LOG_INFO(HW_GPU, "PICA command hottest r{}={} r{}={} r{}={} r{}={}",
                     hottest[0].second, hottest[0].first, hottest[1].second, hottest[1].first,
                     hottest[2].second, hottest[2].first, hottest[3].second, hottest[3].first);
            LOG_INFO(HW_GPU,
                     "PICA command profile lists={} writes={} draws={} list_cpu_ns={} "
                     "draw_cpu_ns={} notifications={} repeat_shader_notifications={} "
                     "code_words={} repeat_code_words={} swizzle_words={} "
                     "repeat_swizzle_words={} clock_failures={}",
                     command_profile.lists, command_profile.writes, command_profile.draws,
                     command_profile.list_cpu_ns, command_profile.draw_cpu_ns,
                     command_profile.notifications, command_profile.repeated_shader_notifications,
                     command_profile.code_words, command_profile.repeated_code_words,
                     command_profile.swizzle_words, command_profile.repeated_swizzle_words,
                     command_profile.clock_failures);
            LogShaderPacketProfile();
        }
    }
private:
    bool draw, active = false;
    std::uint64_t start = 0;
};

// Only notifications that exclusively dirty the shader configuration are eligible.
// In particular, do not skip fixed-function Sync calls: some derive their result
// from other registers (e.g. logic op / color masks), even on an unchanged write.
static bool ShaderConfigNotification(u32 id) {
    switch (id) {
    case PICA_REG_INDEX(rasterizer.depthmap_enable):
    case PICA_REG_INDEX(rasterizer.scissor_test.mode):
    case PICA_REG_INDEX(texturing.main_config):
    case PICA_REG_INDEX(texturing.texture0.type):
    case PICA_REG_INDEX(texturing.tev_stage0.color_source1):
    case PICA_REG_INDEX(texturing.tev_stage0.color_modifier1):
    case PICA_REG_INDEX(texturing.tev_stage0.color_op):
    case PICA_REG_INDEX(texturing.tev_stage0.color_scale):
    case PICA_REG_INDEX(texturing.tev_stage1.color_source1):
    case PICA_REG_INDEX(texturing.tev_stage1.color_modifier1):
    case PICA_REG_INDEX(texturing.tev_stage1.color_op):
    case PICA_REG_INDEX(texturing.tev_stage1.color_scale):
    case PICA_REG_INDEX(texturing.tev_stage2.color_source1):
    case PICA_REG_INDEX(texturing.tev_stage2.color_modifier1):
    case PICA_REG_INDEX(texturing.tev_stage2.color_op):
    case PICA_REG_INDEX(texturing.tev_stage2.color_scale):
    case PICA_REG_INDEX(texturing.tev_stage3.color_source1):
    case PICA_REG_INDEX(texturing.tev_stage3.color_modifier1):
    case PICA_REG_INDEX(texturing.tev_stage3.color_op):
    case PICA_REG_INDEX(texturing.tev_stage3.color_scale):
    case PICA_REG_INDEX(texturing.tev_stage4.color_source1):
    case PICA_REG_INDEX(texturing.tev_stage4.color_modifier1):
    case PICA_REG_INDEX(texturing.tev_stage4.color_op):
    case PICA_REG_INDEX(texturing.tev_stage4.color_scale):
    case PICA_REG_INDEX(texturing.tev_stage5.color_source1):
    case PICA_REG_INDEX(texturing.tev_stage5.color_modifier1):
    case PICA_REG_INDEX(texturing.tev_stage5.color_op):
    case PICA_REG_INDEX(texturing.tev_stage5.color_scale):
    case PICA_REG_INDEX(texturing.tev_combiner_buffer_input):
    case PICA_REG_INDEX(lighting.light[0].config):
    case PICA_REG_INDEX(lighting.light[1].config):
    case PICA_REG_INDEX(lighting.light[2].config):
    case PICA_REG_INDEX(lighting.light[3].config):
    case PICA_REG_INDEX(lighting.light[4].config):
    case PICA_REG_INDEX(lighting.light[5].config):
    case PICA_REG_INDEX(lighting.light[6].config):
    case PICA_REG_INDEX(lighting.light[7].config):
        return true;
    default:
        return false;
    }
}

static void WriteShaderWord(Shader::ShaderSetup& setup, u32 offset, u32 value, bool swizzle) {
    auto& word = swizzle ? setup.swizzle_data[offset] : setup.program_code[offset];
    if (Cara::GuestGPUTrace::DrawsEnabled() && word != value)
        Cara::GuestGPUTrace::ShaderWordChanged(&setup == &g_state.gs);
    if (!RepeatStateEnabled() && !CommandProfileEnabled()) {
        word = value;
        if (swizzle) setup.MarkSwizzleDataDirty();
        else setup.MarkProgramCodeDirty();
        return;
    }
    const bool repeated = word == value;
    if (CommandProfileEnabled()) {
        if (swizzle) {
            ++command_profile.swizzle_words;
            command_profile.repeated_swizzle_words += repeated;
        } else {
            ++command_profile.code_words;
            command_profile.repeated_code_words += repeated;
        }
    }
    word = value;
    if (!RepeatStateEnabled() || !repeated) {
        if (swizzle) setup.MarkSwizzleDataDirty();
        else setup.MarkProgramCodeDirty();
    }
}

static const char* GetShaderSetupTypeName(Shader::ShaderSetup& setup) {
    if (&setup == &g_state.vs) {
        return "vertex shader";
    }
    if (&setup == &g_state.gs) {
        return "geometry shader";
    }
    return "unknown shader";
}

static void WriteUniformBoolReg(Shader::ShaderSetup& setup, u32 value) {
    for (u32 i = 0; i < setup.uniforms.b.size(); ++i)
        setup.uniforms.b[i] = (value & (1 << i)) != 0;
}

static void WriteUniformIntReg(Shader::ShaderSetup& setup, unsigned index,
                               const Common::Vec4<u8>& values) {
    ASSERT(index < setup.uniforms.i.size());
    setup.uniforms.i[index] = values;
    LOG_TRACE(HW_GPU, "Set {} integer uniform {} to {:02x} {:02x} {:02x} {:02x}",
              GetShaderSetupTypeName(setup), index, values.x, values.y, values.z, values.w);
}

static void WriteUniformFloatReg(ShaderRegs& config, Shader::ShaderSetup& setup,
                                 int& float_regs_counter, u32 uniform_write_buffer[4], u32 value) {
    auto& uniform_setup = config.uniform_setup;

    // TODO: Does actual hardware indeed keep an intermediate buffer or does
    //       it directly write the values?
    uniform_write_buffer[float_regs_counter++] = value;

    // Uniforms are written in a packed format such that four float24 values are encoded in
    // three 32-bit numbers. We write to internal memory once a full such vector is
    // written.
    if ((float_regs_counter >= 4 && uniform_setup.IsFloat32()) ||
        (float_regs_counter >= 3 && !uniform_setup.IsFloat32())) {
        float_regs_counter = 0;

        auto& uniform = setup.uniforms.f[uniform_setup.index];

        if (uniform_setup.index >= 96) {
            LOG_ERROR(HW_GPU, "Invalid {} float uniform index {}", GetShaderSetupTypeName(setup),
                      (int)uniform_setup.index);
        } else {

            // NOTE: The destination component order indeed is "backwards"
            if (uniform_setup.IsFloat32()) {
                for (auto i : {0, 1, 2, 3})
                    uniform[3 - i] = float24::FromFloat32(*(float*)(&uniform_write_buffer[i]));
            } else {
                // TODO: Untested
                uniform.w = float24::FromRaw(uniform_write_buffer[0] >> 8);
                uniform.z = float24::FromRaw(((uniform_write_buffer[0] & 0xFF) << 16) |
                                             ((uniform_write_buffer[1] >> 16) & 0xFFFF));
                uniform.y = float24::FromRaw(((uniform_write_buffer[1] & 0xFFFF) << 8) |
                                             ((uniform_write_buffer[2] >> 24) & 0xFF));
                uniform.x = float24::FromRaw(uniform_write_buffer[2] & 0xFFFFFF);
            }

            LOG_TRACE(HW_GPU, "Set {} float uniform {:x} to ({} {} {} {})",
                      GetShaderSetupTypeName(setup), (int)uniform_setup.index,
                      uniform.x.ToFloat32(), uniform.y.ToFloat32(), uniform.z.ToFloat32(),
                      uniform.w.ToFloat32());

            // TODO: Verify that this actually modifies the register!
            uniform_setup.index.Assign(uniform_setup.index + 1);
        }
    }
}

static bool PostIRQProfileEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("CARA_PICA_POST_IRQ_PROFILE");
        return value && std::strcmp(value, "1") == 0;
    }();
    return enabled && Common::FrameTiming::counters.enabled;
}
static Cara::PicaSubmissionProbe post_irq_submission;

static void WritePicaReg(u32 id, u32 value, u32 mask) {
    auto& regs = g_state.regs;

    if (id >= Regs::NUM_REGS) {
        LOG_ERROR(
            HW_GPU,
            "Commandlist tried to write to invalid register 0x{:03X} (value: {:08X}, mask: {:X})",
            id, value, mask);
        return;
    }

    if (Cara::ExperimentalP3DCompletionEnabled()) g_state.experimental_p3d_work.Command();

    // Expand a 4-bit mask to 4-byte mask, e.g. 0b0101 -> 0x00FF00FF
    static const u32 expand_bits_to_bytes[] = {
        0x00000000, 0x000000ff, 0x0000ff00, 0x0000ffff, 0x00ff0000, 0x00ff00ff,
        0x00ffff00, 0x00ffffff, 0xff000000, 0xff0000ff, 0xff00ff00, 0xff00ffff,
        0xffff0000, 0xffff00ff, 0xffffff00, 0xffffffff,
    };
    // TODO: Figure out how register masking acts on e.g. vs.uniform_setup.set_value
    if (CommandProfileEnabled()) {
        ++command_profile.writes;
        ++command_profile.registers[id];
    }
    const u32 old_value = regs.reg_array[id];
    const u32 write_mask = expand_bits_to_bytes[mask];
    regs.reg_array[id] = (old_value & ~write_mask) | (value & write_mask);
    if (PostIRQProfileEnabled()) {
        const bool irq = id == PICA_REG_INDEX(trigger_irq);
        std::uint64_t remaining_words = 0;
        if (irq && !post_irq_submission.saw_irq) {
            const auto used = g_state.cmd_list.current_ptr - g_state.cmd_list.head_ptr;
            if (used >= 0 && std::uint64_t(used) <= g_state.cmd_list.length)
                remaining_words = g_state.cmd_list.length - used;
        }
        post_irq_submission.Write(irq,
            id == PICA_REG_INDEX(pipeline.trigger_draw) || id == PICA_REG_INDEX(pipeline.trigger_draw_indexed),
            id == PICA_REG_INDEX(pipeline.command_buffer.trigger[0]) ||
                id == PICA_REG_INDEX(pipeline.command_buffer.trigger[1]), remaining_words);
    }

    switch (id) {
    // Trigger IRQ
    case PICA_REG_INDEX(trigger_irq):
        if (Cara::GuestGPUTrace::Enabled()) Cara::GuestGPUTrace::Emit(Core::System::GetInstance().CoreTiming(),
            Cara::GuestGPUTrace::Kind::PicaIRQ, 0, 0, value, mask);
        Service::GSP::SignalInterrupt(Service::GSP::InterruptId::P3D);
        break;

    case PICA_REG_INDEX(pipeline.triangle_topology):
        g_state.primitive_assembler.Reconfigure(regs.pipeline.triangle_topology);
        break;

    case PICA_REG_INDEX(pipeline.restart_primitive):
        g_state.primitive_assembler.Reset();
        break;

    case PICA_REG_INDEX(pipeline.vs_default_attributes_setup.index):
        g_state.immediate.current_attribute = 0;
        g_state.immediate.reset_geometry_pipeline = true;
        g_state.default_attr_counter = 0;
        break;

    // Load default vertex input attributes
    case PICA_REG_INDEX(pipeline.vs_default_attributes_setup.set_value[0]):
    case PICA_REG_INDEX(pipeline.vs_default_attributes_setup.set_value[1]):
    case PICA_REG_INDEX(pipeline.vs_default_attributes_setup.set_value[2]): {
        // TODO: Does actual hardware indeed keep an intermediate buffer or does
        //       it directly write the values?
        g_state.default_attr_write_buffer[g_state.default_attr_counter++] = value;

        // Default attributes are written in a packed format such that four float24 values are
        // encoded in
        // three 32-bit numbers. We write to internal memory once a full such vector is
        // written.
        if (g_state.default_attr_counter >= 3) {
            g_state.default_attr_counter = 0;

            auto& setup = regs.pipeline.vs_default_attributes_setup;

            if (setup.index >= 16) {
                LOG_ERROR(HW_GPU, "Invalid VS default attribute index {}", (int)setup.index);
                break;
            }

            Common::Vec4<float24> attribute;

            // NOTE: The destination component order indeed is "backwards"
            attribute.w = float24::FromRaw(g_state.default_attr_write_buffer[0] >> 8);
            attribute.z = float24::FromRaw(((g_state.default_attr_write_buffer[0] & 0xFF) << 16) |
                                           ((g_state.default_attr_write_buffer[1] >> 16) & 0xFFFF));
            attribute.y = float24::FromRaw(((g_state.default_attr_write_buffer[1] & 0xFFFF) << 8) |
                                           ((g_state.default_attr_write_buffer[2] >> 24) & 0xFF));
            attribute.x = float24::FromRaw(g_state.default_attr_write_buffer[2] & 0xFFFFFF);

            LOG_TRACE(HW_GPU, "Set default VS attribute {:x} to ({} {} {} {})", (int)setup.index,
                      attribute.x.ToFloat32(), attribute.y.ToFloat32(), attribute.z.ToFloat32(),
                      attribute.w.ToFloat32());

            // TODO: Verify that this actually modifies the register!
            if (setup.index < 15) {
                g_state.input_default_attributes.attr[setup.index] = attribute;
                setup.index++;
            } else {
                // Put each attribute into an immediate input buffer.  When all specified immediate
                // attributes are present, the Vertex Shader is invoked and everything is sent to
                // the primitive assembler.

                auto& immediate_input = g_state.immediate.input_vertex;
                auto& immediate_attribute_id = g_state.immediate.current_attribute;

                immediate_input.attr[immediate_attribute_id] = attribute;

                if (immediate_attribute_id < regs.pipeline.max_input_attrib_index) {
                    immediate_attribute_id += 1;
                } else {
                    MICROPROFILE_SCOPE(GPU_Drawing);
                    immediate_attribute_id = 0;

                    Shader::OutputVertex::ValidateSemantics(regs.rasterizer);

                    auto* shader_engine = Shader::GetEngine();
                    shader_engine->SetupBatch(g_state.vs, regs.vs.main_offset);

                    // Send to vertex shader
                    if (g_debug_context)
                        g_debug_context->OnEvent(DebugContext::Event::VertexShaderInvocation,
                                                 static_cast<void*>(&immediate_input));
                    Shader::UnitState shader_unit;
                    Shader::AttributeBuffer output{};

                    shader_unit.LoadInput(regs.vs, immediate_input);
                    shader_engine->Run(g_state.vs, shader_unit);
                    shader_unit.WriteOutput(regs.vs, output);

                    // Send to geometry pipeline
                    if (g_state.immediate.reset_geometry_pipeline) {
                        g_state.geometry_pipeline.Reconfigure();
                        g_state.immediate.reset_geometry_pipeline = false;
                    }
                    ASSERT(!g_state.geometry_pipeline.NeedIndexInput());
                    g_state.geometry_pipeline.Setup(shader_engine);
                    g_state.geometry_pipeline.SubmitVertex(output);

                    // TODO: If drawing after every immediate mode triangle kills performance,
                    // change it to flush triangles whenever a drawing config register changes
                    // See: https://github.com/citra-emu/citra/pull/2866#issuecomment-327011550
                    VideoCore::Rasterizer()->DrawTriangles();
                }
            }
        }
        break;
    }

    case PICA_REG_INDEX(pipeline.gpu_mode):
        // This register likely just enables vertex processing and doesn't need any special handling
        break;

    case PICA_REG_INDEX(pipeline.command_buffer.trigger[0]):
    case PICA_REG_INDEX(pipeline.command_buffer.trigger[1]): {
        unsigned index =
            static_cast<unsigned>(id - PICA_REG_INDEX(pipeline.command_buffer.trigger[0]));
        u32* head_ptr = (u32*)VideoCore::Memory()->GetPhysicalPointer(
            regs.pipeline.command_buffer.GetPhysicalAddress(index));
        g_state.cmd_list.head_ptr = g_state.cmd_list.current_ptr = head_ptr;
        g_state.cmd_list.length = regs.pipeline.command_buffer.GetSize(index) / sizeof(u32);
        break;
    }

    // It seems like these trigger vertex rendering
    case PICA_REG_INDEX(pipeline.trigger_draw):
    case PICA_REG_INDEX(pipeline.trigger_draw_indexed): {
        if (Cara::GuestGPUTrace::ReserveDrawRecords()) {
            auto& timing = Core::System::GetInstance().CoreTiming();
            const auto& fb = regs.framebuffer.framebuffer;
            const auto& pipe = regs.pipeline;
            const auto& tex = regs.texturing;
            const u64 flags = u64(id == PICA_REG_INDEX(pipeline.trigger_draw_indexed)) |
                (u64(pipe.triangle_topology.Value()) << 1) |
                (u64(pipe.use_gs.Value()) << 3) | (u64(pipe.variable_primitive.Value()) << 5);
            Cara::GuestGPUTrace::Emit(timing, Cara::GuestGPUTrace::Kind::Draw, 0, 0,
                (u64(fb.GetColorBufferPhysicalAddress()) << 32) | fb.GetDepthBufferPhysicalAddress(),
                u64(pipe.num_vertices) | (flags << 32), Cara::GuestGPUTrace::vs_generation,
                Cara::GuestGPUTrace::gs_generation);
            const u64 dimensions = u64(fb.GetWidth()) | (u64(fb.GetHeight()) << 16) |
                (u64(fb.color_format.Value()) << 32) | (u64(fb.depth_format.Value()) << 36) |
                (u64(fb.allow_color_write.Value()) << 40) |
                (u64(fb.allow_depth_stencil_write.Value()) << 44) |
                (u64(regs.rasterizer.cull_mode.Value()) << 48);
            Cara::GuestGPUTrace::Emit(timing, Cara::GuestGPUTrace::Kind::DrawState, 0, 0,
                dimensions,
                u64(regs.reg_array[PICA_REG_INDEX(framebuffer.output_merger.depth_test_enable)]) |
                    (u64(regs.framebuffer.output_merger.stencil_test.raw_func) << 32),
                u64(regs.vs.main_offset) | (u64(regs.gs.main_offset) << 32),
                u64(regs.reg_array[PICA_REG_INDEX(framebuffer.output_merger.fragment_operation_mode)]) |
                    (u64(regs.reg_array[PICA_REG_INDEX(framebuffer.output_merger.alpha_blending)]) << 32));
            const u64 texture_formats = u64(tex.texture0_format.Value()) |
                (u64(tex.texture1_format.Value()) << 4) | (u64(tex.texture2_format.Value()) << 8) |
                (u64(tex.texture0.type.Value()) << 12);
            Cara::GuestGPUTrace::Emit(timing, Cara::GuestGPUTrace::Kind::DrawInput, 0, 0,
                (u64(pipe.vertex_attributes.GetPhysicalBaseAddress()) << 32) |
                    regs.reg_array[PICA_REG_INDEX(pipeline.index_array)],
                u64(pipe.vertex_offset) | (u64(regs.reg_array[PICA_REG_INDEX(texturing.main_config)]) << 32),
                (u64(tex.texture0.GetPhysicalAddress()) << 32) | tex.texture1.GetPhysicalAddress(),
                u64(tex.texture2.GetPhysicalAddress()) | (texture_formats << 32));
        }
        if (Cara::ExperimentalP3DCompletionEnabled())
            g_state.experimental_p3d_work.Draw(regs.pipeline.num_vertices,
                                             static_cast<unsigned>(regs.pipeline.triangle_topology.Value()));
        if (Common::FrameTiming::counters.enabled)
            ++Common::FrameTiming::counters.pica_draw_requests;
        MICROPROFILE_SCOPE(GPU_Drawing);
        CommandCPUProfileScope command_draw_profile(true);

#if PICA_LOG_TEV
        DebugUtils::DumpTevStageConfig(regs.GetTevStages());
#endif
        PrimitiveAssembler<Shader::OutputVertex>& primitive_assembler = g_state.primitive_assembler;
        bool accelerate_draw = VideoCore::g_hw_shader_enabled && primitive_assembler.IsEmpty();
        if (accelerate_draw) {
            if (regs.pipeline.use_gs == PipelineRegs::UseGS::No) {
                auto topology = primitive_assembler.GetTopology();
                if (topology == PipelineRegs::TriangleTopology::Shader ||
                    topology == PipelineRegs::TriangleTopology::List) {
                    accelerate_draw = (regs.pipeline.num_vertices % 3) == 0;
                }
                // TODO (wwylele): for Strip/Fan topology, if the primitive assember is not
                // restarted after this draw call, the buffered vertex from this draw should "leak"
                // to the next draw, in which case we should buffer the vertex into the software
                // primitive assember, or disable accelerate draw completely. However, there is not
                // game found yet that does this, so this is left unimplemented for now. Revisit
                // this when an issue is found in games.
            } else {
                accelerate_draw = false;
            }
        }

        bool is_indexed = (id == PICA_REG_INDEX(pipeline.trigger_draw_indexed));

        if (accelerate_draw && VideoCore::Rasterizer()->AccelerateDrawBatch(is_indexed)) {
            break;
        } else if (Settings::values.skip_slow_draw) {
            break;
        }

#ifdef CARA_IOS_GLES
        ++Common::FrameTiming::counters.cpu_vertex_draws;
        static const bool trace_fallback=std::getenv("CARA_VERTEX_FALLBACK_TRACE")!=nullptr;
        if (trace_fallback) {
            static std::unordered_set<u64> reported;
            const u32 count=regs.pipeline.num_vertices;
            const u32 topology=static_cast<u32>(primitive_assembler.GetTopology());
            const u32 gs=static_cast<u32>(regs.pipeline.use_gs.Value());
            const u64 key=u64(count)|(u64(topology)<<32)|(u64(gs)<<36)|(u64(accelerate_draw)<<40);
            if (reported.size()<64 && reported.insert(key).second)
                LOG_INFO(Render_OpenGL,"CPU vertex fallback: vertices={} topology={} gs={} attempted={} assembler_empty={}",
                         count,topology,gs,accelerate_draw,primitive_assembler.IsEmpty());
        }
#endif
        // Processes information about internal vertex attributes to figure out how a vertex is
        // loaded.
        // Later, these can be compiled and cached.
        const u32 base_address = regs.pipeline.vertex_attributes.GetPhysicalBaseAddress();
        VertexLoader loader(regs.pipeline);
        Shader::OutputVertex::ValidateSemantics(regs.rasterizer);

        // Load vertices
        const auto& index_info = regs.pipeline.index_array;
        const u8* index_address_8 =
            VideoCore::Memory()->GetPhysicalPointer(base_address + index_info.offset);
        const u16* index_address_16 = reinterpret_cast<const u16*>(index_address_8);
        bool index_u16 = index_info.format != 0;

        DebugUtils::MemoryAccessTracker memory_accesses;
        Shader::AttributeBuffer vs_output;
        auto* shader_engine = Shader::GetEngine();
        Shader::UnitState shader_unit;

        shader_engine->SetupBatch(g_state.vs, regs.vs.main_offset);

        g_state.geometry_pipeline.Reconfigure();
        g_state.geometry_pipeline.Setup(shader_engine);

        if (is_indexed) {
            // Simple circular-replacement vertex cache
            // The size has been tuned for optimal balance between hit-rate and the cost of lookup
            const u32 VERTEX_CACHE_SIZE = 32;
            std::array<bool, VERTEX_CACHE_SIZE> vertex_cache_valid{};
            std::array<u16, VERTEX_CACHE_SIZE> vertex_cache_ids;
            std::array<Shader::AttributeBuffer, VERTEX_CACHE_SIZE> vertex_cache;
            u32 vertex_cache_pos = 0;
            u32 cache_lookup_limit = std::min(VERTEX_CACHE_SIZE, regs.pipeline.num_vertices - 1);

            for (u32 index = 0; index < regs.pipeline.num_vertices; ++index) {
                // Indexed rendering doesn't use the start offset
                u32 vertex = index_u16 ? index_address_16[index] : index_address_8[index];

                if (g_state.geometry_pipeline.NeedIndexInput()) {
                    g_state.geometry_pipeline.SubmitIndex(vertex);
                    continue;
                }

                bool vertex_cache_hit = false;
                for (u32 i = 0; i < cache_lookup_limit; ++i) {
                    if (vertex_cache_valid[i] && vertex == vertex_cache_ids[i]) {
                        vs_output = vertex_cache[i];
                        vertex_cache_hit = true;
                        break;
                    }
                }

                if (!vertex_cache_hit) {
                    // Initialize data for the current vertex
                    Shader::AttributeBuffer input;
                    loader.LoadVertex(base_address, index, vertex, input, memory_accesses);
                    shader_unit.LoadInput(regs.vs, input);
                    shader_engine->Run(g_state.vs, shader_unit);
                    shader_unit.WriteOutput(regs.vs, vs_output);

                    vertex_cache[vertex_cache_pos] = vs_output;
                    vertex_cache_valid[vertex_cache_pos] = true;
                    vertex_cache_ids[vertex_cache_pos] = vertex;
                    vertex_cache_pos = (vertex_cache_pos + 1) % VERTEX_CACHE_SIZE;
                }

                // Send to geometry pipeline
                g_state.geometry_pipeline.SubmitVertex(vs_output);
            }
        } else {
            for (u32 index = 0; index < regs.pipeline.num_vertices; ++index) {
                // Indexed rendering doesn't use the start offset
                u32 vertex = index + regs.pipeline.vertex_offset;

                // Initialize data for the current vertex
                Shader::AttributeBuffer input;
                loader.LoadVertex(base_address, index, vertex, input, memory_accesses);
                shader_unit.LoadInput(regs.vs, input);
                shader_engine->Run(g_state.vs, shader_unit);
                shader_unit.WriteOutput(regs.vs, vs_output);

                // Send to geometry pipeline
                g_state.geometry_pipeline.SubmitVertex(vs_output);
            }
        }

        VideoCore::Rasterizer()->DrawTriangles();
        break;
    }

    case PICA_REG_INDEX(gs.bool_uniforms):
        WriteUniformBoolReg(g_state.gs, g_state.regs.gs.bool_uniforms.Value());
        break;

    case PICA_REG_INDEX(gs.int_uniforms[0]):
    case PICA_REG_INDEX(gs.int_uniforms[1]):
    case PICA_REG_INDEX(gs.int_uniforms[2]):
    case PICA_REG_INDEX(gs.int_uniforms[3]): {
        unsigned index = (id - PICA_REG_INDEX(gs.int_uniforms[0]));
        auto values = regs.gs.int_uniforms[index];
        WriteUniformIntReg(g_state.gs, index,
                           Common::Vec4<u8>(values.x, values.y, values.z, values.w));
        break;
    }

    case PICA_REG_INDEX(gs.uniform_setup.set_value[0]):
    case PICA_REG_INDEX(gs.uniform_setup.set_value[1]):
    case PICA_REG_INDEX(gs.uniform_setup.set_value[2]):
    case PICA_REG_INDEX(gs.uniform_setup.set_value[3]):
    case PICA_REG_INDEX(gs.uniform_setup.set_value[4]):
    case PICA_REG_INDEX(gs.uniform_setup.set_value[5]):
    case PICA_REG_INDEX(gs.uniform_setup.set_value[6]):
    case PICA_REG_INDEX(gs.uniform_setup.set_value[7]): {
        WriteUniformFloatReg(g_state.regs.gs, g_state.gs, g_state.gs_float_regs_counter,
                             g_state.gs_uniform_write_buffer, value);
        break;
    }

    case PICA_REG_INDEX(gs.program.set_word[0]):
    case PICA_REG_INDEX(gs.program.set_word[1]):
    case PICA_REG_INDEX(gs.program.set_word[2]):
    case PICA_REG_INDEX(gs.program.set_word[3]):
    case PICA_REG_INDEX(gs.program.set_word[4]):
    case PICA_REG_INDEX(gs.program.set_word[5]):
    case PICA_REG_INDEX(gs.program.set_word[6]):
    case PICA_REG_INDEX(gs.program.set_word[7]): {
        u32& offset = g_state.regs.gs.program.offset;
        if (offset >= 4096) {
            LOG_ERROR(HW_GPU, "Invalid GS program offset {}", offset);
        } else {
            WriteShaderWord(g_state.gs, offset, value, false);
            offset++;
        }
        break;
    }

    case PICA_REG_INDEX(gs.swizzle_patterns.set_word[0]):
    case PICA_REG_INDEX(gs.swizzle_patterns.set_word[1]):
    case PICA_REG_INDEX(gs.swizzle_patterns.set_word[2]):
    case PICA_REG_INDEX(gs.swizzle_patterns.set_word[3]):
    case PICA_REG_INDEX(gs.swizzle_patterns.set_word[4]):
    case PICA_REG_INDEX(gs.swizzle_patterns.set_word[5]):
    case PICA_REG_INDEX(gs.swizzle_patterns.set_word[6]):
    case PICA_REG_INDEX(gs.swizzle_patterns.set_word[7]): {
        u32& offset = g_state.regs.gs.swizzle_patterns.offset;
        if (offset >= g_state.gs.swizzle_data.size()) {
            LOG_ERROR(HW_GPU, "Invalid GS swizzle pattern offset {}", offset);
        } else {
            WriteShaderWord(g_state.gs, offset, value, true);
            offset++;
        }
        break;
    }

    case PICA_REG_INDEX(vs.bool_uniforms):
        // TODO (wwylele): does regs.pipeline.gs_unit_exclusive_configuration affect this?
        WriteUniformBoolReg(g_state.vs, g_state.regs.vs.bool_uniforms.Value());
        break;

    case PICA_REG_INDEX(vs.int_uniforms[0]):
    case PICA_REG_INDEX(vs.int_uniforms[1]):
    case PICA_REG_INDEX(vs.int_uniforms[2]):
    case PICA_REG_INDEX(vs.int_uniforms[3]): {
        // TODO (wwylele): does regs.pipeline.gs_unit_exclusive_configuration affect this?
        u32 index = (id - PICA_REG_INDEX(vs.int_uniforms[0]));
        auto values = regs.vs.int_uniforms[index];
        WriteUniformIntReg(g_state.vs, index,
                           Common::Vec4<u8>(values.x, values.y, values.z, values.w));
        break;
    }

    case PICA_REG_INDEX(vs.uniform_setup.set_value[0]):
    case PICA_REG_INDEX(vs.uniform_setup.set_value[1]):
    case PICA_REG_INDEX(vs.uniform_setup.set_value[2]):
    case PICA_REG_INDEX(vs.uniform_setup.set_value[3]):
    case PICA_REG_INDEX(vs.uniform_setup.set_value[4]):
    case PICA_REG_INDEX(vs.uniform_setup.set_value[5]):
    case PICA_REG_INDEX(vs.uniform_setup.set_value[6]):
    case PICA_REG_INDEX(vs.uniform_setup.set_value[7]): {
        // TODO (wwylele): does regs.pipeline.gs_unit_exclusive_configuration affect this?
        WriteUniformFloatReg(g_state.regs.vs, g_state.vs, g_state.vs_float_regs_counter,
                             g_state.vs_uniform_write_buffer, value);
        break;
    }

    case PICA_REG_INDEX(vs.program.set_word[0]):
    case PICA_REG_INDEX(vs.program.set_word[1]):
    case PICA_REG_INDEX(vs.program.set_word[2]):
    case PICA_REG_INDEX(vs.program.set_word[3]):
    case PICA_REG_INDEX(vs.program.set_word[4]):
    case PICA_REG_INDEX(vs.program.set_word[5]):
    case PICA_REG_INDEX(vs.program.set_word[6]):
    case PICA_REG_INDEX(vs.program.set_word[7]): {
        u32& offset = g_state.regs.vs.program.offset;
        if (offset >= 512) {
            LOG_ERROR(HW_GPU, "Invalid VS program offset {}", offset);
        } else {
            WriteShaderWord(g_state.vs, offset, value, false);
            if (!g_state.regs.pipeline.gs_unit_exclusive_configuration) {
                WriteShaderWord(g_state.gs, offset, value, false);
            }
            offset++;
        }
        break;
    }

    case PICA_REG_INDEX(vs.swizzle_patterns.set_word[0]):
    case PICA_REG_INDEX(vs.swizzle_patterns.set_word[1]):
    case PICA_REG_INDEX(vs.swizzle_patterns.set_word[2]):
    case PICA_REG_INDEX(vs.swizzle_patterns.set_word[3]):
    case PICA_REG_INDEX(vs.swizzle_patterns.set_word[4]):
    case PICA_REG_INDEX(vs.swizzle_patterns.set_word[5]):
    case PICA_REG_INDEX(vs.swizzle_patterns.set_word[6]):
    case PICA_REG_INDEX(vs.swizzle_patterns.set_word[7]): {
        u32& offset = g_state.regs.vs.swizzle_patterns.offset;
        if (offset >= g_state.vs.swizzle_data.size()) {
            LOG_ERROR(HW_GPU, "Invalid VS swizzle pattern offset {}", offset);
        } else {
            WriteShaderWord(g_state.vs, offset, value, true);
            if (!g_state.regs.pipeline.gs_unit_exclusive_configuration) {
                WriteShaderWord(g_state.gs, offset, value, true);
            }
            offset++;
        }
        break;
    }

    case PICA_REG_INDEX(lighting.lut_data[0]):
    case PICA_REG_INDEX(lighting.lut_data[1]):
    case PICA_REG_INDEX(lighting.lut_data[2]):
    case PICA_REG_INDEX(lighting.lut_data[3]):
    case PICA_REG_INDEX(lighting.lut_data[4]):
    case PICA_REG_INDEX(lighting.lut_data[5]):
    case PICA_REG_INDEX(lighting.lut_data[6]):
    case PICA_REG_INDEX(lighting.lut_data[7]): {
        auto& lut_config = regs.lighting.lut_config;
        ASSERT_MSG(lut_config.index < 256, "lut_config.index exceeded maximum value of 255!");
        g_state.lighting.luts[lut_config.type][lut_config.index].raw = value;
        lut_config.index.Assign(lut_config.index + 1);
        VideoCore::Rasterizer()->SyncLightingLutData();
        break;
    }

    case PICA_REG_INDEX(texturing.fog_lut_data[0]):
    case PICA_REG_INDEX(texturing.fog_lut_data[1]):
    case PICA_REG_INDEX(texturing.fog_lut_data[2]):
    case PICA_REG_INDEX(texturing.fog_lut_data[3]):
    case PICA_REG_INDEX(texturing.fog_lut_data[4]):
    case PICA_REG_INDEX(texturing.fog_lut_data[5]):
    case PICA_REG_INDEX(texturing.fog_lut_data[6]):
    case PICA_REG_INDEX(texturing.fog_lut_data[7]): {
        g_state.fog.lut[regs.texturing.fog_lut_offset % 128].raw = value;
        regs.texturing.fog_lut_offset.Assign(regs.texturing.fog_lut_offset + 1);
        VideoCore::Rasterizer()->SyncFogLutData();
        break;
    }

    case PICA_REG_INDEX(texturing.proctex_lut_data[0]):
    case PICA_REG_INDEX(texturing.proctex_lut_data[1]):
    case PICA_REG_INDEX(texturing.proctex_lut_data[2]):
    case PICA_REG_INDEX(texturing.proctex_lut_data[3]):
    case PICA_REG_INDEX(texturing.proctex_lut_data[4]):
    case PICA_REG_INDEX(texturing.proctex_lut_data[5]):
    case PICA_REG_INDEX(texturing.proctex_lut_data[6]):
    case PICA_REG_INDEX(texturing.proctex_lut_data[7]): {
        auto& index = regs.texturing.proctex_lut_config.index;
        auto& pt = g_state.proctex;
        switch (regs.texturing.proctex_lut_config.ref_table.Value()) {
        case TexturingRegs::ProcTexLutTable::Noise:
            pt.noise_table[index % pt.noise_table.size()].raw = value;
            break;
        case TexturingRegs::ProcTexLutTable::ColorMap:
            pt.color_map_table[index % pt.color_map_table.size()].raw = value;
            break;
        case TexturingRegs::ProcTexLutTable::AlphaMap:
            pt.alpha_map_table[index % pt.alpha_map_table.size()].raw = value;
            break;
        case TexturingRegs::ProcTexLutTable::Color:
            pt.color_table[index % pt.color_table.size()].raw = value;
            break;
        case TexturingRegs::ProcTexLutTable::ColorDiff:
            pt.color_diff_table[index % pt.color_diff_table.size()].raw = value;
            break;
        }
        index.Assign(index + 1);
        VideoCore::Rasterizer()->SyncProcTexLutData();
        break;
    }
    default: {
        const bool repeated_shader_state = (RepeatStateEnabled() || CommandProfileEnabled()) &&
                                           old_value == regs.reg_array[id] &&
                                           ShaderConfigNotification(id);
        if (CommandProfileEnabled()) {
            ++command_profile.notifications;
            command_profile.repeated_shader_notifications += repeated_shader_state;
        }
        if (!RepeatStateEnabled() || !repeated_shader_state)
            VideoCore::Rasterizer()->NotifyPicaRegisterChanged(id);
        break;
    }
    }
}

u64 TakeExperimentalP3DNanoseconds() {
    return g_state.experimental_p3d_work.TakeNanoseconds();
}

void ProcessCommandList(PAddr list, u32 size) {
    if (PostIRQProfileEnabled()) post_irq_submission = {};
    if (Common::FrameTiming::counters.enabled)
        ++Common::FrameTiming::counters.pica_command_lists;
    static const bool reported_options = [] {
        LOG_INFO(HW_GPU, "PICA command options repeat_state={} command_profile={}",
                 RepeatStateEnabled(), CommandProfileEnabled());
        return true;
    }();
    (void)reported_options;
    CommandCPUProfileScope command_list_profile(false);
    Common::CPUCategoryTiming::Scope cpu_category_scope(Common::CPUCategoryTiming::Category::GPUCommand);
    u32* buffer = (u32*)VideoCore::Memory()->GetPhysicalPointer(list);
    g_state.cmd_list.addr = list;
    g_state.cmd_list.head_ptr = g_state.cmd_list.current_ptr = buffer;
    g_state.cmd_list.length = size / sizeof(u32);

    const bool collect_shader_packets = CommandProfileEnabled();
    while (g_state.cmd_list.current_ptr < g_state.cmd_list.head_ptr + g_state.cmd_list.length) {
        // Align read pointer to 8 bytes
        g_state.cmd_list.current_ptr +=
            (g_state.cmd_list.head_ptr - g_state.cmd_list.current_ptr) & 1;

        u32 value = *g_state.cmd_list.current_ptr++;
        const CommandHeader header = {*g_state.cmd_list.current_ptr++};
        const u32 group_factor = header.group_commands;
        u32 cmd_id = header.cmd_id;

        WritePicaReg(cmd_id, value, header.parameter_mask);
        // Observe only metadata AFTER the original scalar header. In particular,
        // never read ahead, move the cursor, or reject legacy truncated packets.
        if (collect_shader_packets) {
            const auto kind = Cara::PicaShaderPackets::PortKind(cmd_id);
            const u32 offset = kind == Cara::PicaShaderPackets::Kind::Program
                ? g_state.regs.vs.program.offset
                : kind == Cara::PicaShaderPackets::Kind::Swizzle
                    ? g_state.regs.vs.swizzle_patterns.offset : 0;
            shader_packet_profile.Observe({cmd_id, bool(group_factor), header.extra_data_length,
                offset, g_state.cmd_list.length,
                g_state.cmd_list.current_ptr - g_state.cmd_list.head_ptr,
                header.parameter_mask, !g_state.regs.pipeline.gs_unit_exclusive_configuration});
        }
        for (u32 i = 0; i < header.extra_data_length; ++i) {
            cmd_id += group_factor;
            WritePicaReg(cmd_id, *g_state.cmd_list.current_ptr++, header.parameter_mask);
        }
    }
    if (PostIRQProfileEnabled()) {
        auto& counters = Common::FrameTiming::counters;
        counters.pica_post_irq_submissions += post_irq_submission.saw_irq;
        counters.pica_post_irq_writes += post_irq_submission.writes;
        counters.pica_post_irq_draw_requests += post_irq_submission.draws;
        counters.pica_post_irq_jumps += post_irq_submission.jumps;
        counters.pica_first_irq_remaining_words += post_irq_submission.first_irq_remaining_words;
        if (counters.pica_command_lists % 256 == 0)
            LOG_INFO(HW_GPU, "PICA post IRQ profile submissions={} with_irq={} writes={} draws={} jumps={} first_irq_remaining_words={}",
                     counters.pica_command_lists, counters.pica_post_irq_submissions,
                     counters.pica_post_irq_writes, counters.pica_post_irq_draw_requests,
                     counters.pica_post_irq_jumps, counters.pica_first_irq_remaining_words);
    }
}

} // namespace Pica::CommandProcessor
