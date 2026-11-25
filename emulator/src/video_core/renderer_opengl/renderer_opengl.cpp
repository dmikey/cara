#include "video_core/renderer_opengl/gl_backlog_profile.h"
// Modified for Cara on 2026-11-12; see NOTICE for attribution.
// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string_view>
#include <glad/glad.h>
#ifdef CARA_GL_CONTEXT_DISPATCH
#include "video_core/renderer_opengl/gl_command_queue.h"
#endif
#include <queue>
#ifdef __ANDROID__
#include <android/log.h>
#endif
#include "cara/display_scaling.h"
#include "common/frame_timing.h"
#include "common/bit_field.h"
#include "common/logging/log.h"
#include "core/3ds.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/framebuffer_layout.h"
#include "core/hw/gpu.h"
#include "core/hw/hw.h"
#include "core/hw/lcd.h"
#include "core/memory.h"
#include "core/settings.h"
#include "core/tracer/recorder.h"
#include "video_core/debug_utils/debug_utils.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_opengl/gl_lcd_upload.h"
#include "video_core/renderer_opengl/gl_state.h"
#include "video_core/renderer_opengl/gl_vars.h"
#include "video_core/renderer_opengl/on_screen_display.h"
#include "video_core/renderer_opengl/renderer_opengl.h"
#include "video_core/video_core.h"

namespace OpenGL {

#ifdef __ANDROID__
static thread_local GLESRefreshTimer* g_command_trace = nullptr;

struct GLESRefreshTimer {
    using Clock = std::chrono::steady_clock;

    // A zero bit count selects the existing elapsed query.
    GLESRefreshTimer(unsigned timestamp_bits, bool command_trace, bool list_trace = false)
        : timestamp_bits(timestamp_bits), command_trace(command_trace), list_trace(list_trace) {}

    ~GLESRefreshTimer() {
        if (g_command_trace == this)
            g_command_trace = nullptr;
        if (active && !timestamp_bits)
            glEndQueryEXT(GL_TIME_ELAPSED_EXT);
        for (const auto& sample : samples) {
            if (sample.begin)
                glDeleteQueriesEXT(1, &sample.begin);
            if (sample.end)
                glDeleteQueriesEXT(1, &sample.end);
            if (sample.section_begin)
                glDeleteQueriesEXT(1, &sample.section_begin);
            if (sample.section_end)
                glDeleteQueriesEXT(1, &sample.section_end);
            for (const auto& list : sample.lists) {
                if (list.begin)
                    glDeleteQueriesEXT(1, &list.begin);
                if (list.end)
                    glDeleteQueriesEXT(1, &list.end);
            }
        }
    }

    void FinishAndPoll() {
        if (active) {
            if (g_command_trace == this)
                g_command_trace = nullptr;
            if (timestamp_bits) {
                if (list_trace)
                    active->host_end = SinceSubmission();
                glQueryCounterEXT(active->end, GL_TIMESTAMP_EXT);
            } else {
                glEndQueryEXT(GL_TIME_ELAPSED_EXT);
            }
            active->pending = true;
            active = nullptr;
        }
        if (std::none_of(samples.begin(), samples.end(), [](const Sample& sample) {
                return sample.pending;
            }))
            return;
        GLint disjoint = GL_FALSE;
        glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
        for (auto& sample : samples) {
            if (!sample.pending)
                continue;
            sample.disjoint |= disjoint != GL_FALSE;
            if (!Ready(sample.begin) || (timestamp_bits && !Ready(sample.end)))
                continue;
            if (!sample.disjoint)
                Report(sample);
            sample.pending = false;
        }
    }

    void Begin(u32 frame) {
        if (frame % 16 != 0)
            return;
        for (auto& sample : samples) {
            if (sample.pending)
                continue;
            if (!sample.begin)
                glGenQueriesEXT(1, &sample.begin);
            if (timestamp_bits && !sample.end)
                glGenQueriesEXT(1, &sample.end);
            if (!sample.begin || (timestamp_bits && !sample.end))
                return;
            sample.frame = frame;
            sample.disjoint = false;
            sample.has_section = false;
            sample.list_count = 0;
            sample.lists_seen = 0;
            if (timestamp_bits) {
                sample.submitted = Clock::now();
                glQueryCounterEXT(sample.begin, GL_TIMESTAMP_EXT);
            } else {
                glBeginQueryEXT(GL_TIME_ELAPSED_EXT, sample.begin);
            }
            active = &sample;
            if (command_trace) {
                const auto sample_number = frame / 16;
                section_kind = sample_number % 2 ? GLESCommandTraceScope::Kind::Draw
                                                 : GLESCommandTraceScope::Kind::LUTUpload;
                section_index = (sample_number / 2 * 17) %
                                (section_kind == GLESCommandTraceScope::Kind::Draw ? 256 : 32);
                sections_seen = 0;
            }
            if (command_trace || list_trace)
                g_command_trace = this;
            return;
        }
    }

    bool BeginSection(GLESCommandTraceScope::Kind kind, bool lighting, bool fog, std::size_t bytes) {
        if (!command_trace || kind != section_kind || active->has_section ||
            sections_seen++ != section_index)
            return false;
        if (!active->section_begin)
            glGenQueriesEXT(1, &active->section_begin);
        if (!active->section_end)
            glGenQueriesEXT(1, &active->section_end);
        if (!active->section_begin || !active->section_end)
            return false;
        active->lighting = lighting;
        active->fog = fog;
        active->bytes = bytes;
        glQueryCounterEXT(active->section_begin, GL_TIMESTAMP_EXT);
        return true;
    }

    void EndSection() {
        glQueryCounterEXT(active->section_end, GL_TIMESTAMP_EXT);
        active->has_section = true;
    }

    bool BeginList(u32 bytes) {
        if (!list_trace)
            return false;
        ++active->lists_seen;
        if (list_active || active->list_count == active->lists.size())
            return false;
        auto& list = active->lists[active->list_count];
        if (!list.begin)
            glGenQueriesEXT(1, &list.begin);
        if (!list.end)
            glGenQueriesEXT(1, &list.end);
        if (!list.begin || !list.end)
            return false;
        list.bytes = bytes;
        list.draws = Common::FrameTiming::counters.pica_draw_requests;
        list.host_begin = SinceSubmission();
        glQueryCounterEXT(list.begin, GL_TIMESTAMP_EXT);
        list_active = true;
        return true;
    }

    void EndList() {
        auto& list = active->lists[active->list_count++];
        list.host_end = SinceSubmission();
        glQueryCounterEXT(list.end, GL_TIMESTAMP_EXT);
        list.draws = Common::FrameTiming::counters.pica_draw_requests - list.draws;
        list_active = false;
    }

private:
    struct List {
        GLuint begin = 0;
        GLuint end = 0;
        u32 bytes = 0;
        u64 draws = 0;
        u64 host_begin = 0;
        u64 host_end = 0;
    };

    struct Sample {
        GLuint begin = 0;
        GLuint end = 0;
        u32 frame = 0;
        Clock::time_point submitted{};
        bool pending = false;
        bool disjoint = false;
        GLuint section_begin = 0;
        GLuint section_end = 0;
        bool has_section = false;
        bool lighting = false;
        bool fog = false;
        std::size_t bytes = 0;
        std::array<List, 6> lists{};
        std::size_t list_count = 0;
        std::size_t lists_seen = 0;
        u64 host_end = 0;
    };

    static bool Ready(GLuint query) {
        GLuint available = GL_FALSE;
        glGetQueryObjectuivEXT(query, GL_QUERY_RESULT_AVAILABLE_EXT, &available);
        return available == GL_TRUE;
    }

    static GLuint64 Read(GLuint query) {
        GLuint64 result = 0;
        glGetQueryObjectui64vEXT(query, GL_QUERY_RESULT_EXT, &result);
        return result;
    }

    u64 SinceSubmission() const {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   Clock::now() - active->submitted)
            .count();
    }

    GLuint64 Difference(GLuint64 start, GLuint64 end) const {
        const GLuint64 elapsed = end - start;
        return timestamp_bits < 64 ? elapsed & ((GLuint64{1} << timestamp_bits) - 1) : elapsed;
    }

    void Report(const Sample& sample) const {
        const GLuint64 begin = Read(sample.begin);
        GLuint64 elapsed = begin;
        GLuint64 end = 0;
        GLuint64 upper_bound = static_cast<GLuint64>(std::numeric_limits<GLint64>::max());
        if (timestamp_bits) {
            end = Read(sample.end);
            elapsed = Difference(begin, end);
            // Both GPU timestamps must fall within this host interval. Allow
            // for modest differences between the two clocks.
            upper_bound = std::chrono::duration_cast<std::chrono::nanoseconds>(
                Clock::now() - sample.submitted + std::chrono::milliseconds(1)).count();
        }
        const char* method = timestamp_bits ? "timestamp" : "elapsed";
        if (elapsed > upper_bound) {
            __android_log_print(ANDROID_LOG_INFO, "CaraGPU",
                                "guest_interval_invalid frame=%u method=%s",
                                static_cast<unsigned>(sample.frame), method);
            return;
        }
        __android_log_print(ANDROID_LOG_INFO, "CaraGPU",
                            "guest_interval frame=%u ns=%llu method=%s",
                            static_cast<unsigned>(sample.frame),
                            static_cast<unsigned long long>(elapsed), method);
        if (list_trace && sample.list_count) {
            __android_log_print(ANDROID_LOG_INFO, "CaraGPU",
                                "guest_lists frame=%u lists=%zu captured=%zu host_ns=%llu",
                                static_cast<unsigned>(sample.frame), sample.lists_seen,
                                sample.list_count, static_cast<unsigned long long>(sample.host_end));
            u64 previous_end = 0;
            for (std::size_t index = 0; index < sample.list_count; ++index) {
                const auto& list = sample.lists[index];
                const auto list_begin = Difference(begin, Read(list.begin));
                const auto list_end = Difference(begin, Read(list.end));
                if (list_begin < previous_end || list_end < list_begin || list_end > elapsed)
                    break;
                previous_end = list_end;
                __android_log_print(
                    ANDROID_LOG_INFO, "CaraGPU",
                    "guest_list frame=%u list=%zu bytes=%u draws=%llu gpu_begin_ns=%llu "
                    "gpu_end_ns=%llu host_begin_ns=%llu host_end_ns=%llu",
                    static_cast<unsigned>(sample.frame), index, static_cast<unsigned>(list.bytes),
                    static_cast<unsigned long long>(list.draws),
                    static_cast<unsigned long long>(list_begin),
                    static_cast<unsigned long long>(list_end),
                    static_cast<unsigned long long>(list.host_begin),
                    static_cast<unsigned long long>(list.host_end));
            }
        }
        if (!sample.has_section)
            return;
        // Readiness of the frame's end marker implies readiness of earlier timestamps.
        const auto start_delta = Difference(begin, Read(sample.section_begin));
        const auto finish_delta = Difference(begin, Read(sample.section_end));
        if (start_delta > finish_delta || finish_delta > elapsed)
            return;
        __android_log_print(
            ANDROID_LOG_INFO, "CaraGPU",
            "guest_section frame=%u kind=%s lighting=%d fog=%d bytes=%zu before_ns=%llu "
            "section_ns=%llu after_ns=%llu",
            static_cast<unsigned>(sample.frame), sample.bytes ? "lut_upload" : "draw",
            sample.lighting, sample.fog, sample.bytes, static_cast<unsigned long long>(start_delta),
            static_cast<unsigned long long>(finish_delta - start_delta),
            static_cast<unsigned long long>(elapsed - finish_delta));
    }

    const unsigned timestamp_bits;
    const bool command_trace;
    const bool list_trace;
    bool list_active = false;
    GLESCommandTraceScope::Kind section_kind{};
    u32 section_index = 0;
    u32 sections_seen = 0;
    std::array<Sample, 4> samples{};
    Sample* active = nullptr;
};

GLESCommandTraceScope::GLESCommandTraceScope(Kind kind, bool lighting, bool fog, std::size_t bytes) {
    if (g_command_trace && g_command_trace->BeginSection(kind, lighting, fog, bytes))
        timer = g_command_trace;
}

GLESCommandTraceScope::~GLESCommandTraceScope() {
    if (timer)
        timer->EndSection();
}

GLESCommandListTraceScope::GLESCommandListTraceScope(u32 bytes) {
    if (g_command_trace && g_command_trace->BeginList(bytes))
        timer = g_command_trace;
}

GLESCommandListTraceScope::~GLESCommandListTraceScope() {
    if (timer)
        timer->EndList();
}
#endif

// If the size of this is too small, it ends up creating a soft cap on FPS as the renderer will have
// to wait on available presentation frames. There doesn't seem to be much of a downside to a larger
// number but 9 swap textures at 60FPS presentation allows for 800% speed so thats probably fine
constexpr std::size_t SWAP_CHAIN_SIZE = 6;

struct OGLFrame {
    u32 width;              /// Width of the frame (to detect resize)
    u32 height;             /// Height of the frame
    bool color_reloaded;    /// Texture attachment was recreated (ie: resized)
    bool dual_atlas = false; /// Canonical combined LCD atlas, not a physical window layout
    OGLRenderbuffer color;  /// Buffer shared between the render/present FBO
    OGLFramebuffer render;  /// FBO created on the render thread
    OGLFramebuffer present; /// FBO created on the present thread
    GLsync render_fence;    /// Fence created on the render thread
    GLsync present_fence;   /// Fence created on the presentation thread
};

struct OGLTextureMailbox {
    std::mutex swap_chain_lock;
    std::condition_variable free_cv;
    std::array<OGLFrame, SWAP_CHAIN_SIZE> swap_chain{};
    std::queue<OGLFrame*> free_queue;
    std::deque<OGLFrame*> present_queue;
    OGLFrame* previous_frame = nullptr;
    u64 presentation_generation = 0;
    std::chrono::milliseconds elapsed{};

    OGLTextureMailbox() {
        if (Settings::values.use_frame_limit) {
            // min to 1, speed 200%
            u16 time = std::max(200 - Settings::values.frame_limit, 2) >> 1;
            elapsed = std::chrono::milliseconds{time};
        }
        for (auto& frame : swap_chain) {
            free_queue.push(&frame);
        }
    }

    ~OGLTextureMailbox() {
        // lock the mutex and clear out the present and free_queues and notify any people who are
        // blocked to prevent deadlock on shutdown
        std::scoped_lock lock(swap_chain_lock);
        std::queue<OGLFrame*>().swap(free_queue);
        present_queue.clear();
        free_cv.notify_all();
    }

    /// called in core thread
    void ResetPresent() {
        std::scoped_lock lock(swap_chain_lock);
        ++presentation_generation;
        for (auto* frame : present_queue) {
            free_queue.push(frame);
        }
        present_queue.clear();
        free_cv.notify_all();
    }

    /// called in core thread
    /// Recreate the frame if the size of the window has changed
    void ReloadRenderFrame(OGLFrame* frame, u32 width, u32 height) {
        // Recreate the color texture attachment
        frame->color.Release();
        frame->color.Create();
        GLuint prev_render_buffer = OpenGLState::BindRenderbuffer(frame->color.handle);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
        // Recreate the FBO for the render target
        frame->render.Release();
        frame->render.Create();
        OpenGLState::BindReadFramebuffer(frame->render.handle);
        OpenGLState::BindDrawFramebuffer(frame->render.handle);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
                                  frame->color.handle);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            LOG_CRITICAL(Render_OpenGL, "Failed to recreate render FBO!");
        }
        OpenGLState::BindRenderbuffer(prev_render_buffer);
        frame->width = width;
        frame->height = height;
        frame->color_reloaded = true;
    }

    /// called in core thread
    OGLFrame* GetRenderFrame(u64& generation) {
        std::unique_lock<std::mutex> lock(swap_chain_lock);
        if (free_queue.empty()) {
            free_cv.wait_for(lock, elapsed, [this] { return !free_queue.empty(); });
        }
        generation = presentation_generation;
        if (!free_queue.empty()) {
            auto* frame = free_queue.front();
            free_queue.pop();
            return frame;
        }
        if (present_queue.empty()) {
            return nullptr;
        }
        auto* frame = present_queue.front();
        present_queue.pop_front();
        if (!present_queue.empty()) {
            free_queue.push(present_queue.front());
            present_queue.pop_front();
        }
        return frame;
    }

    // Called by the producer or by the worker after flushing the fence.
    void ReleaseRenderFrame(OGLFrame* frame, u64 generation) {
        std::scoped_lock lock(swap_chain_lock);
        if (generation == presentation_generation) {
            present_queue.push_back(frame);
        } else {
            free_queue.push(frame);
        }
        free_cv.notify_one();
    }

    /// called in present thread
    OGLFrame* GetPresentFrame() {
        std::unique_lock<std::mutex> lock(swap_chain_lock);

        if (present_queue.empty()) {
            return nullptr;
        }
        // free the previous frame and add it back to the free queue
        if (previous_frame) {
            free_queue.push(previous_frame);
            free_cv.notify_one();
        }

        // the newest entries are pushed to the front of the queue
        previous_frame = present_queue.front();
        present_queue.pop_front();

        if (present_queue.size() > 1) {
            // skip frame if pending present frames more than one
            free_queue.push(present_queue.front());
            present_queue.pop_front();
        }

        return previous_frame;
    }

    /// called in present thread
    /// Recreate the presentation FBO if the color attachment was changed
    void ReloadPresentFrame(OGLFrame* frame, u32 height, u32 width) {
        frame->present.Release();
        frame->present.Create();
        GLint previous_draw_fbo{};
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previous_draw_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, frame->present.handle);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
                                  frame->color.handle);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            LOG_CRITICAL(Render_OpenGL, "Failed to recreate present FBO!");
        }
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, previous_draw_fbo);
        frame->color_reloaded = false;
    }
};

static const char vertex_shader[] = R"(
in vec2 vert_position;
in vec2 vert_tex_coord;
out vec2 frag_tex_coord;

// This is a truncated 3x3 matrix for 2D transformations:
// The upper-left 2x2 submatrix performs scaling/rotation/mirroring.
// The third column performs translation.
// The third row could be used for projection, which we don't need in 2D. It hence is assumed to
// implicitly be [0, 0, 1]
uniform mat3x2 modelview_matrix;

void main() {
    // Multiply input position by the rotscale part of the matrix and then manually translate by
    // the last column. This is equivalent to using a full 3x3 matrix and expanding the vector
    // to `vec3(vert_position.xy, 1.0)`
    gl_Position = vec4(mat2(modelview_matrix) * vert_position + modelview_matrix[2], 0.0, 1.0);
    frag_tex_coord = vert_tex_coord;
}
)";

static const char fragment_shader[] = R"(
in vec2 frag_tex_coord;
out vec4 color;
uniform sampler2D color_texture;
void main() {
    color = texture(color_texture, frag_tex_coord);
}
)";

static const char post_processing_header[] = R"(
// hlsl to glsl types
#define float2 vec2
#define float3 vec3
#define float4 vec4
#define uint2 uvec2
#define uint3 uvec3
#define uint4 uvec4
#define int2 ivec2
#define int3 ivec3
#define int4 ivec4

in float2 frag_tex_coord;
out float4 output_color;

uniform float4 resolution;
uniform sampler2D color_texture;

float4 Sample() { return texture(color_texture, frag_tex_coord); }
float4 SampleLocation(float2 location) { return texture(color_texture, location); }
float4 SampleFetch(int2 location) { return texelFetch(color_texture, location, 0); }
int2 SampleSize() { return textureSize(color_texture, 0); }
float2 GetResolution() { return resolution.zw; }
float2 GetInvResolution() { return 1.0 / resolution.zw; }
float2 GetOnScreenSize() { return resolution.xy; }
float2 GetCoordinates() { return frag_tex_coord; }
void SetOutput(float4 color) { output_color = color; }
)";

/**
 * Vertex structure that the drawn screen rectangles are composed of.
 */
struct ScreenRectVertex {
    ScreenRectVertex(GLfloat x, GLfloat y, GLfloat u, GLfloat v) {
        position[0] = x;
        position[1] = y;
        tex_coord[0] = u;
        tex_coord[1] = v;
    }

    GLfloat position[2];
    GLfloat tex_coord[2];
};

/**
 * Defines a 1:1 pixel ortographic projection matrix with (0,0) on the top-left
 * corner and (width, height) on the lower-bottom.
 *
 * The projection part of the matrix is trivial, hence these operations are represented
 * by a 3x2 matrix.
 */
static std::array<GLfloat, 3 * 2> MakeOrthographicMatrix(float width, float height) {
    std::array<GLfloat, 3 * 2> matrix; // Laid out in column-major order

    // clang-format off
    matrix[0] = 2.f / width; matrix[2] = 0.f;           matrix[4] = -1.f;
    matrix[1] = 0.f;         matrix[3] = -2.f / height; matrix[5] = 1.f;
    // Last matrix row is implicitly assumed to be [0, 0, 1].
    // clang-format on

    return matrix;
}

/**
 * option example: //! key = value
 * @param shader
 * @param options
 */
static void ParsePostShaderOptions(const std::string& shader,
                                   std::unordered_map<std::string, std::string>& options) {
    std::size_t i = 0;
    std::size_t size = shader.size();

    std::string key;
    std::string value;

    bool is_line_begin = true;
    u32 slash_counter = 0;
    bool is_option_key = false;
    bool is_option_value = false;
    while (i < size) {
        char c = shader[i++];
        switch (c) {
        case '/':
            slash_counter += 1;
            break;
        case '!':
            if (is_line_begin && (slash_counter == 2)) {
                is_option_key = true;
            }
            is_line_begin = false;
            break;
        case '=':
            if (is_option_key) {
                is_option_key = false;
                is_option_value = true;
            }
            is_line_begin = false;
            break;
        case ' ':
        case '\t':
            is_line_begin = false;
            break;
        case '\n':
        case '\r':
            is_line_begin = true;
            is_option_key = false;
            is_option_value = false;
            slash_counter = 0;
            if (!key.empty() && !value.empty()) {
                options[key] = value;
                key.clear();
                value.clear();
            }
            break;
        default:
            if (is_option_key) {
                key += c;
            } else if (is_option_value) {
                value += c;
            }
            is_line_begin = false;
            break;
        }
    }
}

RendererOpenGL::RendererOpenGL(Frontend::EmuWindow& window, bool use_gles) : RendererBase{window} {
    OpenGL::GLES = use_gles;
    mailbox = std::make_unique<OGLTextureMailbox>();
}

RendererOpenGL::~RendererOpenGL() {
#if defined(__ANDROID__) && defined(CARA_GL_CONTEXT_DISPATCH)
    if (CommandQueue::Enabled()) {
        try {
            CommandQueue::Drain();
        } catch (const std::exception& error) {
            LOG_ERROR(Render_OpenGL, "Could not drain GLES mailbox work: {}", error.what());
        }
    }
#endif
    OSD::Shutdown();
}

/// Swap buffers (render frame)
void RendererOpenGL::SwapBuffers() {
#ifdef __ANDROID__
    if (!gpu_refresh_timer_checked) {
        gpu_refresh_timer_checked = true;
        const char* enabled = std::getenv("CARA_TRACE_GLES_GPU_TIMING");
        const char* timestamp_flag = std::getenv("CARA_TRACE_GLES_GPU_TIMESTAMPS");
        const char* command_flag = std::getenv("CARA_TRACE_GLES_GPU_COMMANDS");
        const char* list_flag = std::getenv("CARA_TRACE_GLES_GPU_LISTS");
        const bool commands = command_flag && std::strcmp(command_flag, "1") == 0;
        const bool lists = list_flag && std::strcmp(list_flag, "1") == 0;
        const bool timestamps = commands || lists ||
                                (timestamp_flag && std::strcmp(timestamp_flag, "1") == 0);
        const bool elapsed = enabled && std::strcmp(enabled, "1") == 0;
        if ((timestamps || elapsed) && GLES &&
            GLAD_GL_EXT_disjoint_timer_query && glGetQueryivEXT && glGenQueriesEXT &&
            glDeleteQueriesEXT && glGetQueryObjectuivEXT && glGetQueryObjectui64vEXT &&
            (timestamps ? bool(glQueryCounterEXT) : glBeginQueryEXT && glEndQueryEXT)
#ifdef CARA_GL_CONTEXT_DISPATCH
            && !CommandQueue::Enabled()
#endif
        ) {
            GLint bits = 0;
            glGetQueryivEXT(timestamps ? GL_TIMESTAMP_EXT : GL_TIME_ELAPSED_EXT,
                            GL_QUERY_COUNTER_BITS_EXT, &bits);
            if (bits > 0 && bits <= 64)
                gpu_refresh_timer = std::make_unique<GLESRefreshTimer>(timestamps ? bits : 0, commands, lists);
        }
    }
    if (gpu_refresh_timer)
        gpu_refresh_timer->FinishAndPoll();
#endif
    if (!VideoCore::g_screenshot_complete_callback && !render_window.ShouldPresentFrame()) {
        VideoCore::FrameUpdate();
        return;
    }
    const auto& layout = render_window.GetFramebufferLayout();
    // Maintain the rasterizer's state as a priority
    OpenGLState prev_state = OpenGLState::GetCurState();
    state.viewport.x = 0;
    state.viewport.y = 0;
    state.viewport.width = layout.width;
    state.viewport.height = layout.height;
    state.Apply();

    for (int i : {0, 2}) {
        int fb_id = i == 2 ? 1 : 0;
        const auto& framebuffer = GPU::g_regs.framebuffer_config[fb_id];

        // Main LCD (0): 0x1ED02204, Sub LCD (1): 0x1ED02A04
        u32 lcd_color_addr =
            (fb_id == 0) ? LCD_REG_INDEX(color_fill_top) : LCD_REG_INDEX(color_fill_bottom);
        lcd_color_addr = HW::VADDR_LCD + 4 * lcd_color_addr;
        LCD::Regs::ColorFill color_fill = {0};
        LCD::Read(color_fill.raw, lcd_color_addr);

        if (color_fill.is_enabled) {
            LoadColorToActiveGLTexture(color_fill.color_r, color_fill.color_g, color_fill.color_b,
                                       screen_infos[i].texture);

            // Resize the texture in case the framebuffer size has changed
            screen_infos[i].texture.width = 1;
            screen_infos[i].texture.height = 1;
        } else {
            if (screen_infos[i].texture.width != (GLsizei)framebuffer.width ||
                screen_infos[i].texture.height != (GLsizei)framebuffer.height ||
                screen_infos[i].texture.format != framebuffer.color_format) {
                // Reallocate texture if the framebuffer size has changed.
                // This is expected to not happen very often and hence should not be a
                // performance problem.
                ConfigureFramebufferTexture(screen_infos[i].texture, framebuffer);
            }
            LoadFBToScreenInfo(framebuffer, screen_infos[i], i == 1);
        }
    }

    RenderScreenshot();

#ifdef __ANDROID__
    const bool dual=render_window.ExternalDisplayMode()!=0;
    if (dual) {
        // Capture completed GPU LCD textures once at the internal render scale.
        // Presentation may then crop either LCD without rerunning guest draws.
        const u32 scale=std::max<u32>(1,VideoCore::GetResolutionScaleFactor());
        const auto canonical=Layout::DefaultFrameLayout(Core::kScreenTopWidth*scale,
            (Core::kScreenTopHeight+Core::kScreenBottomHeight)*scale,false);
        auto atlas_state=OpenGLState::GetCurState();
        atlas_state.viewport={0,0,static_cast<GLsizei>(canonical.width),static_cast<GLsizei>(canonical.height)};
        atlas_state.Apply();
        if (Settings::values.use_present_thread) {
            RenderToMailbox(canonical, true);
        } else if (DrawDualAtlas(canonical)) {
            render_window.PresentDualFrame(dual_framebuffer.handle,canonical.width,canonical.height);
        } else {
            render_window.DisableExternalDisplay();
            auto fallback_state=OpenGLState::GetCurState();
            fallback_state.viewport={0,0,static_cast<GLsizei>(layout.width),static_cast<GLsizei>(layout.height)};
            fallback_state.Apply();
            OpenGLState::BindDrawFramebuffer(render_window.GetDefaultFramebuffer());
            DrawScreens(layout,render_window.GetIntegerOutputScale()!=0);
            render_window.SwapBuffers();
        }
    } else
#endif
    if (Settings::values.use_present_thread) {
        RenderToMailbox(layout);
    } else {
        OpenGLState::BindDrawFramebuffer(render_window.GetDefaultFramebuffer());
        DrawScreens(layout, render_window.GetIntegerOutputScale() != 0);
        render_window.SwapBuffers();
    }
    prev_state.Apply();

#ifdef __ANDROID__
    // Time the next guest GPU interval. Composition and swap are outside the query.
    if (gpu_refresh_timer)
        gpu_refresh_timer->Begin(VideoCore::GetCurrentFrame() + 1);
#endif
    VideoCore::FrameUpdate();
}

#ifdef __ANDROID__
bool RendererOpenGL::DrawDualAtlas(const Layout::FramebufferLayout& canonical) {
    if (!dual_framebuffer.handle || dual_width!=canonical.width || dual_height!=canonical.height) {
        dual_framebuffer.Release();dual_color.Release();
        dual_framebuffer.Create();dual_color.Create();
        const auto prior=OpenGLState::BindRenderbuffer(dual_color.handle);
        glRenderbufferStorage(GL_RENDERBUFFER,GL_RGBA8,canonical.width,canonical.height);
        OpenGLState::BindDrawFramebuffer(dual_framebuffer.handle);
        glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_RENDERBUFFER,dual_color.handle);
        const auto status=glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
        OpenGLState::BindRenderbuffer(prior);
        if(status!=GL_FRAMEBUFFER_COMPLETE) {
            LOG_ERROR(Render_OpenGL,"External LCD atlas framebuffer incomplete: {}",status);
            dual_framebuffer.Release();dual_color.Release();dual_width=dual_height=0;
            return false;
        }
        dual_width=canonical.width;dual_height=canonical.height;
    }
    OpenGLState::BindDrawFramebuffer(dual_framebuffer.handle);
    DrawScreens(canonical,true);
    return true;
}
#endif

void RendererOpenGL::RenderScreenshot() {
    if (VideoCore::g_screenshot_complete_callback) {
        // Draw this frame to the screenshot framebuffer
        OGLFramebuffer screenshot_framebuffer;
        screenshot_framebuffer.Create();
        OpenGLState::BindReadFramebuffer(screenshot_framebuffer.handle);
        OpenGLState::BindDrawFramebuffer(screenshot_framebuffer.handle);

        const auto& layout = render_window.GetFramebufferLayout();
        GLuint renderbuffer;
        glGenRenderbuffers(1, &renderbuffer);
        glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, layout.width, layout.height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
                                  renderbuffer);

        DrawScreens(layout);

        std::vector<u32> pixels(layout.width * layout.height);
        glReadPixels(0, 0, layout.width, layout.height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        FlipPixels(pixels.data(), layout.width, layout.height);

        screenshot_framebuffer.Release();
        glDeleteRenderbuffers(1, &renderbuffer);
        VideoCore::g_screenshot_complete_callback(layout.width, layout.height, pixels);
        VideoCore::g_screenshot_complete_callback = nullptr;
    }
}

/// run in core thread
void RendererOpenGL::RenderToMailbox(const Layout::FramebufferLayout& layout, bool dual_atlas) {
#if defined(__ANDROID__) && defined(CARA_GL_CONTEXT_DISPATCH)
    CommandQueue::Backlog::Scope scope(CommandQueue::Backlog::Source::Display);
#endif
    u64 generation = 0;
    OGLFrame* frame = mailbox->GetRenderFrame(generation);

#if defined(__ANDROID__) && defined(CARA_GL_CONTEXT_DISPATCH)
    static const bool async_requested = [] {
        const char* value = std::getenv("CARA_MAILBOX_ASYNC_PUBLISH");
        return value && std::strcmp(value, "1") == 0;
    }();
    const bool async_publish = async_requested && GLES && CommandQueue::Enabled();
    const bool packet_fences = async_publish || CommandQueue::MailboxFencePacketEnabled();
    if (!frame && async_publish) {
        // The bounded mailbox is full. Drain outside the mailbox lock.
        CommandQueue::Drain();
        frame = mailbox->GetRenderFrame(generation);
    }
#endif
    if (!frame) {
        return;
    }

    // Clean up sync objects before drawing

    // INTEL driver workaround. We can't delete the previous render sync object until we are
    // sure that the presentation is done
    if (frame->present_fence) {
        Common::FrameTiming::Scope timing(Common::FrameTiming::Phase::GPUWait);
#if defined(__ANDROID__) && defined(CARA_GL_CONTEXT_DISPATCH)
        if (packet_fences) {
            CommandQueue::QueueConsumedPresentFence(frame->present_fence, async_publish);
        } else
#endif
        {
            glClientWaitSync(frame->present_fence, 0, GL_TIMEOUT_IGNORED);
            glWaitSync(frame->present_fence, 0, GL_TIMEOUT_IGNORED);
            glDeleteSync(frame->present_fence);
        }
        frame->present_fence = nullptr;
    }

    // drawing

    // Recreate the frame if the size of the window has changed
    if (layout.width != frame->width || layout.height != frame->height) {
        LOG_DEBUG(Render_OpenGL, "Reloading render frame");
        mailbox->ReloadRenderFrame(frame, layout.width, layout.height);
    }
    OpenGLState::BindDrawFramebuffer(frame->render.handle);

    // The mailbox slot owns its source layout until both presentation blits retire.
    frame->dual_atlas=dual_atlas;
    DrawScreens(layout, dual_atlas);

    // delete the draw fence if the frame wasn't presented
    if (frame->render_fence) {
        glDeleteSync(frame->render_fence);
    }
    // Create a fence for the frontend to wait on and swap this frame to OffTex
#if defined(__ANDROID__) && defined(CARA_GL_CONTEXT_DISPATCH)
    if (async_publish) {
        CommandQueue::QueuePublishedRenderFence(
            [owner = mailbox.get(), frame, generation](GLsync fence) {
                frame->render_fence = fence;
                owner->ReleaseRenderFrame(frame, generation);
            });
        return;
    }
    if (packet_fences) {
        frame->render_fence = CommandQueue::CreatePublishedRenderFence();
    } else
#endif
    {
        frame->render_fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        glFlush();
#ifdef CARA_GL_CONTEXT_DISPATCH
        CommandQueue::Drain();
#endif
    }
    mailbox->ReleaseRenderFrame(frame, generation);
}

/// run in present thread
bool RendererOpenGL::TryPresent() {
    auto* frame = mailbox->GetPresentFrame();
    if (!frame) {
        LOG_TRACE(Render_OpenGL, "mailbox no frame to present");
        return false;
    }
    const auto& layout = render_window.GetFramebufferLayout();
#ifdef __ANDROID__
    // A pre-attach combined frame can have an arbitrary old user layout.
    // Wait for a newly composed canonical atlas before splitting it.
    if(render_window.ExternalDisplayMode()!=0 && !frame->dual_atlas)
        return false;
#endif

    // Clearing before a full overwrite of a fbo can signal to drivers that they can avoid a
    // readback since we won't be doing any blending
#ifdef __ANDROID__
    if(!frame->dual_atlas)
#endif
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    // Recreate the presentation FBO if the color attachment was changed
    if (frame->color_reloaded) {
        LOG_DEBUG(Render_OpenGL, "Reloading present frame");
        mailbox->ReloadPresentFrame(frame, frame->height, frame->width);
    }
    glWaitSync(frame->render_fence, 0, GL_TIMEOUT_IGNORED);
    // INTEL workaround.
    // Normally we could just delete the draw fence here, but due to driver bugs, we can just delete
    // it on the emulation thread without too much penalty
    // glDeleteSync(frame.render_sync);
    // frame.render_sync = 0;

#ifdef __ANDROID__
    if (frame->dual_atlas) {
        // Both crops use this retained completed mailbox attachment. Insert
        // its normal reuse fence only after both source reads have been issued.
        const bool handled=render_window.PresentDualFrame(frame->present.handle,frame->width,frame->height);
        if(!render_window.IsPresentationContextCurrent())
            return false; // Lost context: do not submit GL calls to no owner.
        frame->present_fence=glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE,0);
        glFlush();
        return !handled; // Native dual packet already swapped the handheld.
    }
#endif
    glBindFramebuffer(GL_READ_FRAMEBUFFER, frame->present.handle);
    glBlitFramebuffer(0, 0, frame->width, frame->height, 0, 0, layout.width, layout.height,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);

    /* insert fence for the main thread to block on */
    frame->present_fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();

    return true;
}

///
void RendererOpenGL::ResetPresent() {
    mailbox->ResetPresent();
}

void RendererOpenGL::LoadBackgroundImage(u32* pixels, u32 width, u32 height) {
    u32 diff = 0;
    u32 size = width * height;
    for (u32 i = 0; i < size - 1; ++i) {
        diff |= pixels[i] ^ pixels[i + 1];
    }
    if (diff == 0) {
        // use solid color
        u32 pixel = pixels[0];
        float b = static_cast<float>(pixel & 255) / 255.0f;
        float g = static_cast<float>((pixel >> 8) & 255) / 255.0f;
        float r = static_cast<float>((pixel >> 16) & 255) / 255.0f;
        glClearColor(r, g, b, 0.0f);
        bg_texture.Release();
        return;
    }

    if (bg_shader.handle == 0) {
        const char bg_vertex_shader[] = R"(
out vec2 frag_tex_coord;
void main() {
    vec2 rawpos = vec2(gl_VertexID & 1, (gl_VertexID & 2) >> 1);
    frag_tex_coord = vec2(rawpos.x, rawpos.y);
    mat2 rotate = mat2(0, -1, 1, 0); // rotate -90°
    gl_Position = vec4((rawpos * 2.0 - 1.0) * rotate, 0.0, 1.0);
}
)";
        const char bg_fragment_shader[] = R"(
in vec2 frag_tex_coord;
out vec4 color;
layout(binding = 0) uniform sampler2D color_texture;
void main() {
    color = texture(color_texture, frag_tex_coord);
}
)";
        std::string frag_source;
        if (GLES) {
            frag_source = fragment_shader_precision_OES;
            frag_source += bg_fragment_shader;
        } else {
            frag_source = bg_fragment_shader;
        }
        bg_shader.Create(bg_vertex_shader, frag_source.c_str());
    }

    if (bg_texture.handle == 0) {
        bg_texture.Create();
    }

    auto old_tex = OpenGLState::BindTexture2D(0, bg_texture.handle);
    FlipPixels(pixels, width, height);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    OpenGLState::BindTexture2D(0, old_tex);
}

/**
 * Loads framebuffer from emulated memory into the active OpenGL texture.
 */
void RendererOpenGL::LoadFBToScreenInfo(const GPU::Regs::FramebufferConfig& framebuffer,
                                        ScreenInfo& screen_info, bool right_eye) {

    if (framebuffer.address_right1 == 0 || framebuffer.address_right2 == 0)
        right_eye = false;

    const PAddr framebuffer_addr =
        framebuffer.active_fb == 0
            ? (!right_eye ? framebuffer.address_left1 : framebuffer.address_right1)
            : (!right_eye ? framebuffer.address_left2 : framebuffer.address_right2);

    LOG_TRACE(Render_OpenGL, "0x{:08x} bytes from 0x{:08x}({}x{}), fmt {:x}",
              framebuffer.stride * framebuffer.height, framebuffer_addr, (int)framebuffer.width,
              (int)framebuffer.height, (int)framebuffer.format);

    int bpp = GPU::Regs::BytesPerPixel(framebuffer.color_format);
    std::size_t pixel_stride = framebuffer.stride / bpp;

    // OpenGL only supports specifying a stride in units of pixels, not bytes, unfortunately
    ASSERT(pixel_stride * bpp == framebuffer.stride);

    if (!VideoCore::Rasterizer()->AccelerateDisplay(framebuffer, framebuffer_addr,
                                                    static_cast<u32>(pixel_stride), screen_info)) {
        // Reset the screen info's display texture to its own permanent texture
        screen_info.display_texture = screen_info.texture.resource.handle;
        screen_info.display_texcoords = Common::Rectangle<float>(0.f, 0.f, 1.f, 1.f);

        Memory::RasterizerFlushRegion(framebuffer_addr, framebuffer.stride * framebuffer.height);

        const u8* framebuffer_data = VideoCore::Memory()->GetPhysicalPointer(framebuffer_addr);

        GLuint old_tex = OpenGLState::BindTexture2D(0, screen_info.texture.resource.handle);

        GLint unpack_alignment{}, unpack_row_length{}, unpack_skip_rows{}, unpack_skip_pixels{};
        GLint unpack_buffer{};
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack_buffer);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack_alignment);
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &unpack_row_length);
        glGetIntegerv(GL_UNPACK_SKIP_ROWS, &unpack_skip_rows);
        glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &unpack_skip_pixels);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);

        // Update existing texture. The LCD buffers remain sideways; DrawScreens
        // performs the same rotation for both accelerated and fallback textures.
        // TODO: Applications could specify oversized framebuffer dimensions; mapped
        // source bounds and texture dimensions remain the producer's contract.
        if (GLES) {
            static_assert(static_cast<unsigned>(GPU::Regs::PixelFormat::RGBA8) == 0 &&
                          static_cast<unsigned>(GPU::Regs::PixelFormat::RGB8) == 1 &&
                          static_cast<unsigned>(GPU::Regs::PixelFormat::RGB565) == 2 &&
                          static_cast<unsigned>(GPU::Regs::PixelFormat::RGB5A1) == 3 &&
                          static_cast<unsigned>(GPU::Regs::PixelFormat::RGBA4) == 4);
            std::vector<u8> pixels;
            const bool staged = LcdUpload::Stage(
                framebuffer_data, std::size_t(framebuffer.stride) * framebuffer.height,
                static_cast<unsigned>(framebuffer.color_format.Value()), framebuffer.width,
                framebuffer.height, framebuffer.stride, pixels);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            if (staged) {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, framebuffer.width, framebuffer.height,
                               screen_info.texture.gl_format, screen_info.texture.gl_type,
                               pixels.data());
            } else {
                LOG_ERROR(Render_OpenGL, "Invalid LCD framebuffer upload layout");
            }
        } else {
            glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(pixel_stride));
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, framebuffer.width, framebuffer.height,
                           screen_info.texture.gl_format, screen_info.texture.gl_type,
                           framebuffer_data);
        }

        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpack_buffer);
        glPixelStorei(GL_UNPACK_ALIGNMENT, unpack_alignment);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, unpack_row_length);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, unpack_skip_rows);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, unpack_skip_pixels);

        OpenGLState::BindTexture2D(0, old_tex);
    }
}

/**
 * Fills active OpenGL texture with the given RGB color. Since the color is solid, the texture can
 * be 1x1 but will stretch across whatever it's rendered on.
 */
void RendererOpenGL::LoadColorToActiveGLTexture(u8 color_r, u8 color_g, u8 color_b,
                                                const TextureInfo& texture) {
    u8 framebuffer_data[3] = {color_r, color_g, color_b};
    GLuint old_tex = OpenGLState::BindTexture2D(0, texture.resource.handle);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 1, 1, 0, GL_RGB, GL_UNSIGNED_BYTE, framebuffer_data);
    OpenGLState::BindTexture2D(0, old_tex);
}

/**
 * Initializes the OpenGL state and creates persistent objects.
 */
void RendererOpenGL::InitOpenGLObjects() {
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);

    // Generate VBO handle for drawing
    vertex_buffer.Create();

    // Generate VAO
    vertex_array.Create();

    // sampler for post shader
    filter_sampler.Create();

    // Link shaders and get variable locations
    std::string frag_source;
    bool linear_mag_filter = true;
    bool linear_min_filter = true;
    if (GLES) {
        frag_source += fragment_shader_precision_OES;
    }
    if (!Settings::values.pp_shader_name.empty()) {
        std::string pp_shader = FileUtil::GetUserPath(FileUtil::UserPath::ShaderDir) +
                                Settings::values.pp_shader_name + ".glsl";
        std::size_t size = FileUtil::ReadFileToString(true, pp_shader, pp_shader);
        if (size > 0 && size == pp_shader.size()) {
            std::unordered_map<std::string, std::string> options;
            ParsePostShaderOptions(pp_shader, options);
            linear_mag_filter = options["mag_filter"] != "nearest";
            linear_min_filter = options["min_filter"] != "nearest";
            frag_source += post_processing_header;
            frag_source += pp_shader;
        } else {
            frag_source += fragment_shader;
        }
    } else {
        frag_source += fragment_shader;
    }

    shader.Create(vertex_shader, frag_source.data());
    if (!shader.handle) {
        // use default vertex and fragment shader
        frag_source = fragment_shader_precision_OES;
        frag_source += fragment_shader;
        shader.Create(vertex_shader, frag_source.data());
    }

    // sampler
    glSamplerParameteri(filter_sampler.handle, GL_TEXTURE_MAG_FILTER,
                        linear_mag_filter ? GL_LINEAR : GL_NEAREST);
    glSamplerParameteri(filter_sampler.handle, GL_TEXTURE_MIN_FILTER,
                        linear_min_filter ? GL_LINEAR : GL_NEAREST);
    glSamplerParameteri(filter_sampler.handle, GL_TEXTURE_WRAP_S, GLES ? GL_CLAMP_TO_EDGE : GL_CLAMP_TO_BORDER);
    glSamplerParameteri(filter_sampler.handle, GL_TEXTURE_WRAP_T, GLES ? GL_CLAMP_TO_EDGE : GL_CLAMP_TO_BORDER);
    state.texture_units[0].sampler = filter_sampler.handle;

    // apply
    state.draw.shader_program = shader.handle;
    state.draw.vertex_array = vertex_array.handle;
    state.draw.vertex_buffer = vertex_buffer.handle;
    state.Apply();

    uniform_modelview_matrix = glGetUniformLocation(shader.handle, "modelview_matrix");
    uniform_resolution = glGetUniformLocation(shader.handle, "resolution");
    GLuint color_texture = glGetUniformLocation(shader.handle, "color_texture");
    GLuint attrib_position = glGetAttribLocation(shader.handle, "vert_position");
    GLuint attrib_tex_coord = glGetAttribLocation(shader.handle, "vert_tex_coord");

    // Bind texture in Texture Unit 0
    glUniform1i(color_texture, 0);

    // Attach vertex data to VAO
    glVertexAttribPointer(attrib_position, 2, GL_FLOAT, GL_FALSE, sizeof(ScreenRectVertex),
                          (GLvoid*)offsetof(ScreenRectVertex, position));
    glVertexAttribPointer(attrib_tex_coord, 2, GL_FLOAT, GL_FALSE, sizeof(ScreenRectVertex),
                          (GLvoid*)offsetof(ScreenRectVertex, tex_coord));
    glEnableVertexAttribArray(attrib_position);
    glEnableVertexAttribArray(attrib_tex_coord);

    // Allocate textures for each screen
    for (auto& screen_info : screen_infos) {
        screen_info.texture.resource.Create();

        // Allocation of storage is deferred until the first frame, when we
        // know the framebuffer size.

        OpenGLState::BindTexture2D(0, screen_info.texture.resource.handle);

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        screen_info.display_texture = screen_info.texture.resource.handle;
    }

    // init
    OSD::Initialize();
    if (Settings::values.is_new_3ds) {
        OSD::AddMessage("New 3DS Model", OSD::MessageType::New3DS, OSD::Duration::NORMAL,
                        OSD::Color::YELLOW);
    }
    if (!Settings::values.use_hw_shader) {
        OSD::AddMessage("HW Shader Off", OSD::MessageType::HWShader, OSD::Duration::NORMAL,
                        OSD::Color::YELLOW);
    }
    if (!Settings::values.use_cpu_jit) {
        OSD::AddMessage("CPU JIT Off", OSD::MessageType::CPUJit, OSD::Duration::NORMAL,
                        OSD::Color::YELLOW);
    }

    OpenGLState::BindTexture2D(0, 0);
}

void RendererOpenGL::ConfigureFramebufferTexture(TextureInfo& texture,
                                                 const GPU::Regs::FramebufferConfig& framebuffer) {
    GPU::Regs::PixelFormat format = framebuffer.color_format;
    GLint internal_format;

    texture.format = format;
    texture.width = framebuffer.width;
    texture.height = framebuffer.height;

    switch (format) {
    case GPU::Regs::PixelFormat::RGBA8:
        internal_format = GL_RGBA;
        texture.gl_format = GL_RGBA;
        texture.gl_type = GLES ? GL_UNSIGNED_BYTE : GL_UNSIGNED_INT_8_8_8_8;
        break;

    case GPU::Regs::PixelFormat::RGB8:
        // This pixel format uses BGR since GL_UNSIGNED_BYTE specifies byte-order, unlike every
        // specific OpenGL type used in this function using native-endian (that is, little-endian
        // mostly everywhere) for words or half-words.
        // TODO: check how those behave on big-endian processors.
        internal_format = GL_RGB;

        // GLES Dosen't support BGR , Use RGB instead
        texture.gl_format = GLES ? GL_RGB : GL_BGR;
        texture.gl_type = GL_UNSIGNED_BYTE;
        break;

    case GPU::Regs::PixelFormat::RGB565:
        internal_format = GL_RGB;
        texture.gl_format = GL_RGB;
        texture.gl_type = GL_UNSIGNED_SHORT_5_6_5;
        break;

    case GPU::Regs::PixelFormat::RGB5A1:
        internal_format = GL_RGBA;
        texture.gl_format = GL_RGBA;
        texture.gl_type = GL_UNSIGNED_SHORT_5_5_5_1;
        break;

    case GPU::Regs::PixelFormat::RGBA4:
        internal_format = GL_RGBA;
        texture.gl_format = GL_RGBA;
        texture.gl_type = GL_UNSIGNED_SHORT_4_4_4_4;
        break;

    default:
        UNIMPLEMENTED();
    }

    GLuint old_tex = OpenGLState::BindTexture2D(0, texture.resource.handle);

    glTexImage2D(GL_TEXTURE_2D, 0, internal_format, texture.width, texture.height, 0,
                 texture.gl_format, texture.gl_type, nullptr);

    OpenGLState::BindTexture2D(0, old_tex);
}

/**
 * Draws the emulated screens to the emulator window.
 */
GLuint RendererOpenGL::ScreenSampler(bool nearest) {
    if (nearest && !nearest_output_sampler.handle) {
        nearest_output_sampler.Create();
        glSamplerParameteri(nearest_output_sampler.handle, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glSamplerParameteri(nearest_output_sampler.handle, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glSamplerParameteri(nearest_output_sampler.handle, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(nearest_output_sampler.handle, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    return nearest ? nearest_output_sampler.handle : filter_sampler.handle;
}

void RendererOpenGL::DrawScreens(const Layout::FramebufferLayout& layout, bool integer_output) {
    integer_output = integer_output && layout.top_screen.GetWidth() >= Core::kScreenTopWidth;
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    if (bg_texture.handle) {
        OpenGLState::BindSampler(0, ScreenSampler(integer_output));
        // background image
        GLuint handle = OpenGLState::BindShaderProgram(bg_shader.handle);
        OpenGLState::BindTexture2D(0, bg_texture.handle);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        OpenGLState::BindShaderProgram(handle);
    }

    const auto requested_scale = Cara::LCDUpscaleFactor(Settings::values.lossless_output_scale);
    if (!GLES || requested_scale == 1 || !DrawLosslessLCDs(layout, requested_scale)) {
        DrawLCDTextures(layout, integer_output);
    }
    OSD::DrawMessage(render_window, layout);
}

bool RendererOpenGL::DrawLosslessLCDs(const Layout::FramebufferLayout& layout, u32 requested_scale) {
    const u32 source_scale = VideoCore::GetResolutionScaleFactor();
    const auto integral = [](const auto& screen, u32 native_width, u32 scale) {
        return Cara::FinalScaleIsIntegral(native_width * scale, Core::kScreenTopHeight * scale,
                                          screen.GetWidth(), screen.GetHeight());
    };
    const bool top_integral = integral(layout.top_screen, Core::kScreenTopWidth, source_scale);
    const bool bottom_integral = integral(layout.bottom_screen, Core::kScreenBottomWidth, source_scale);
    if ((!layout.top_screen_enabled || top_integral) && (!layout.bottom_screen_enabled || bottom_integral)) {
        DrawLCDTextures(layout, true);
        return true;
    }

    // Expand the finished LCDs before fitting them into their unchanged display
    // and touch rectangles, as in the iOS presentation path.
    const u32 scale = std::max(source_scale, requested_scale);
    if (scale > 4) {
        return false;
    }
    const u32 width = Core::kScreenTopWidth * scale;
    const u32 height = (Core::kScreenTopHeight + Core::kScreenBottomHeight) * scale;
    const auto source = Layout::DefaultFrameLayout(width, height, false);

    if (lossless_storage_scale == scale && !lossless_storage_complete) {
        return false;
    }

    const auto output_state = OpenGLState::GetCurState();
    lossless_framebuffer.Create();
    lossless_color.Create();
    OpenGLState::BindDrawFramebuffer(lossless_framebuffer.handle);
    if (lossless_storage_scale != scale) {
        OpenGLState::BindRenderbuffer(lossless_color.handle);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
        glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
                                  lossless_color.handle);
        lossless_storage_scale = scale;
        lossless_storage_complete =
            glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        if (!lossless_storage_complete) {
            output_state.Apply();
            return false;
        }
    }

    auto stage_state = OpenGLState::GetCurState();
    stage_state.viewport = {0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height)};
    stage_state.scissor.enabled = false;
    stage_state.Apply();
    DrawLCDTextures(source, true);
    output_state.Apply();

    const auto previous_read = OpenGLState::BindReadFramebuffer(lossless_framebuffer.handle);
    const auto blit = [&](const auto& from, const auto& to, u32 native_width) {
        if (to.GetWidth() == 0 || to.GetHeight() == 0) {
            return;
        }
        const GLenum filter = integral(to, native_width, scale) ? GL_NEAREST : GL_LINEAR;
        glBlitFramebuffer(from.left, height - from.bottom, from.right, height - from.top,
                          to.left, layout.height - to.bottom, to.right, layout.height - to.top,
                          GL_COLOR_BUFFER_BIT, filter);
    };
    if (layout.top_screen_enabled) {
        blit(source.top_screen, layout.top_screen, Core::kScreenTopWidth);
    }
    if (layout.bottom_screen_enabled) {
        blit(source.bottom_screen, layout.bottom_screen, Core::kScreenBottomWidth);
    }
    OpenGLState::BindReadFramebuffer(previous_read);
    return true;
}

void RendererOpenGL::DrawLCDTextures(const Layout::FramebufferLayout& layout, bool nearest) {
    OpenGLState::BindSampler(0, ScreenSampler(nearest));

    // Set projection matrix
    std::array<GLfloat, 3 * 2> ortho_matrix = MakeOrthographicMatrix(layout.width, layout.height);
    glUniformMatrix3x2fv(uniform_modelview_matrix, 1, GL_FALSE, ortho_matrix.data());

    // Set vertices
    const auto& top_screen = layout.top_screen;
    const auto& top_texcoords = screen_infos[0].display_texcoords;

    const auto& bottom_screen = layout.bottom_screen;
    const auto& bottom_texcoords = screen_infos[2].display_texcoords;

    const std::array<ScreenRectVertex, 8> vertices = {{
        // top screen
        ScreenRectVertex(top_screen.left, top_screen.top, top_texcoords.bottom, top_texcoords.left),
        ScreenRectVertex(top_screen.left, top_screen.top + top_screen.GetHeight(),
                         top_texcoords.top, top_texcoords.left),
        ScreenRectVertex(top_screen.left + top_screen.GetWidth(), top_screen.top,
                         top_texcoords.bottom, top_texcoords.right),
        ScreenRectVertex(top_screen.left + top_screen.GetWidth(),
                         top_screen.top + top_screen.GetHeight(), top_texcoords.top,
                         top_texcoords.right),
        // bottom screen
        ScreenRectVertex(bottom_screen.left, bottom_screen.top, bottom_texcoords.bottom,
                         bottom_texcoords.left),
        ScreenRectVertex(bottom_screen.left, bottom_screen.top + bottom_screen.GetHeight(),
                         bottom_texcoords.top, bottom_texcoords.left),
        ScreenRectVertex(bottom_screen.left + bottom_screen.GetWidth(), bottom_screen.top,
                         bottom_texcoords.bottom, bottom_texcoords.right),
        ScreenRectVertex(bottom_screen.left + bottom_screen.GetWidth(),
                         bottom_screen.top + bottom_screen.GetHeight(), bottom_texcoords.top,
                         bottom_texcoords.right),
    }};
    // prefer `glBufferData` than `glBufferSubData` on mobile device
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices.data(), GL_STREAM_DRAW);

    // Draws texture to the emulator window,
    // rotating the texture to correct for the 3DS's LCD rotation.

    if (layout.top_screen_enabled) {
        const ScreenInfo& screen_info = screen_infos[0];
        OpenGLState::BindTexture2D(0, screen_info.display_texture);
        float res_width = top_screen.GetHeight();
        float res_height = top_screen.GetWidth();
        float src_width = screen_info.texture.width * Settings::values.resolution_factor;
        float src_height = screen_info.texture.height * Settings::values.resolution_factor;
        glUniform4f(uniform_resolution, res_width, res_height, src_width, src_height);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    if (layout.bottom_screen_enabled) {
        const ScreenInfo& screen_info = screen_infos[2];
        OpenGLState::BindTexture2D(0, screen_info.display_texture);
        float res_width = bottom_screen.GetHeight();
        float res_height = bottom_screen.GetWidth();
        float src_width = screen_info.texture.width * Settings::values.resolution_factor;
        float src_height = screen_info.texture.height * Settings::values.resolution_factor;
        glUniform4f(uniform_resolution, res_width, res_height, src_width, src_height);
        glDrawArrays(GL_TRIANGLE_STRIP, 4, 4);
    }
}

static void APIENTRY ObserveGLESDebugMessage(GLenum source, GLenum type, GLuint id,
                                             GLenum severity, GLsizei length,
                                             const GLchar* message, const void*) {
    if (type != GL_DEBUG_TYPE_ERROR && type != GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR)
        return;

    // Keep repeating draw-time validation errors from flooding the log.
    const std::array<GLuint, 3> key{source, type, id};
    static thread_local std::array<std::array<GLuint, 3>, 64> seen{};
    static thread_local std::size_t count = 0;
    const auto end = seen.begin() + count;
    if (count == seen.size() || std::find(seen.begin(), end, key) != end)
        return;
    seen[count++] = key;
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_WARN, "CaraGLES", "source=0x%x type=0x%x id=%u severity=0x%x %.*s",
                        source, type, id, severity, length, message);
#else
    LOG_WARNING(Render_OpenGL, "GLES source={:x} type={:x} id={} severity={:x} {}",
                source, type, id, severity, std::string_view(message, length));
#endif
}

static void InstallGLESDebugObserver() {
    const char* enabled = std::getenv("CARA_GLES_DEBUG");
    if (!GLES || !enabled || std::strcmp(enabled, "1") != 0)
        return;

    if (GLAD_GL_ES_VERSION_3_2 && glDebugMessageCallback) {
        glDebugMessageCallback(ObserveGLESDebugMessage, nullptr);
    } else if (GLAD_GL_KHR_debug && glDebugMessageCallbackKHR) {
        glDebugMessageCallbackKHR(ObserveGLESDebugMessage, nullptr);
    } else {
        LOG_INFO(Render_OpenGL, "GLES debug output is unavailable in this context");
        return;
    }
    glEnable(GL_DEBUG_OUTPUT);
    glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
#ifdef __ANDROID__
    __android_log_write(ANDROID_LOG_INFO, "CaraGLES", "GLES debug observer installed on renderer context");
#endif
}

/// Initialize the renderer
VideoCore::ResultStatus RendererOpenGL::Init() {
    const char* gl_version{reinterpret_cast<char const*>(glGetString(GL_VERSION))};
    const char* gpu_vendor{reinterpret_cast<char const*>(glGetString(GL_VENDOR))};
    const char* gpu_model{reinterpret_cast<char const*>(glGetString(GL_RENDERER))};

    if (!gl_version || !gpu_vendor || !gpu_model) {
        LOG_ERROR(Render_OpenGL, "No usable GL context during renderer initialization");
        return VideoCore::ResultStatus::ErrorGenericDrivers;
    }

    LOG_INFO(Render_OpenGL, "GL_VERSION: {}", gl_version);
    LOG_INFO(Render_OpenGL, "GL_VENDOR: {}", gpu_vendor);
    LOG_INFO(Render_OpenGL, "GL_RENDERER: {}", gpu_model);

    if (!strcmp(gpu_vendor, "GDI Generic")) {
        return VideoCore::ResultStatus::ErrorGenericDrivers;
    }

#if defined(CARA_MAC_ATLAS_PIPELINE)
    GLint native_major{}, native_minor{};
    glGetIntegerv(GL_MAJOR_VERSION, &native_major);
    glGetIntegerv(GL_MINOR_VERSION, &native_minor);
    if (GLES || native_major < 4 || (native_major == 4 && native_minor < 1)) {
        LOG_ERROR(Render_OpenGL,"Mac atlas prototype requires a native OpenGL4.1 context, not ES");
#elif defined(CARA_IOS_GLES)
    if (!GLAD_GL_ES_VERSION_3_0) {
#else
    if (!(GLAD_GL_VERSION_3_3 || GLAD_GL_ES_VERSION_3_1)) {
#endif
        return VideoCore::ResultStatus::ErrorBelowGL33;
    }

#ifdef CARA_MAC_ATLAS_PIPELINE
    // Rasterizer recomputes this exact capability conjunction. Do not allow an
    // unvalidated image/shadow path to enter this GL4.1-only prototype.
    if ((GLAD_GL_ARB_shader_image_load_store && GLAD_GL_ARB_shader_image_size &&
         GLAD_GL_ARB_framebuffer_no_attachments) || Settings::values.allow_shadow) {
        LOG_ERROR(Render_OpenGL,"Mac atlas prototype does not support shadow/image paths");
        return VideoCore::ResultStatus::ErrorGenericDrivers;
    }
#endif
    InstallGLESDebugObserver();
    InitOpenGLObjects();

    return VideoCore::ResultStatus::Success;
}

} // namespace OpenGL
