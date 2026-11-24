// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <EGL/egl.h>
#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

#include "common/thread.h"
#include "video_core/renderer_opengl/gl_resource_manager.h"

namespace OpenGL {

// Program builds run in a separate context in the same share
// group. Only the renderer installs programs and changes their uniforms.
class AndroidShaderWorker {
public:
    AndroidShaderWorker() {
        display = eglGetCurrentDisplay();
        const auto parent = eglGetCurrentContext();
        EGLint config_id = 0;
        if (display == EGL_NO_DISPLAY || parent == EGL_NO_CONTEXT ||
            !eglQueryContext(display, parent, EGL_CONFIG_ID, &config_id)) {
            return;
        }
        const EGLint config_attribs[] = {EGL_CONFIG_ID, config_id, EGL_NONE};
        EGLConfig config{};
        EGLint count = 0;
        if (!eglChooseConfig(display, config_attribs, &config, 1, &count) || count != 1) {
            return;
        }
        const EGLint surface_attribs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
        surface = eglCreatePbufferSurface(display, config, surface_attribs);
        if (surface == EGL_NO_SURFACE) {
            return;
        }
        const EGLint context_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        context = eglCreateContext(display, config, parent, context_attribs);
        if (context != EGL_NO_CONTEXT) {
            thread = std::thread(&AndroidShaderWorker::Run, this);
        }
    }

    ~AndroidShaderWorker() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        wake.notify_one();
        if (thread.joinable()) {
            thread.join();
        }
        if (context != EGL_NO_CONTEXT) {
            eglDestroyContext(display, context);
        }
        if (surface != EGL_NO_SURFACE) {
            eglDestroySurface(display, surface);
        }
    }

    enum class Status { Unavailable, Busy, Pending, Ready };

    struct Result {
        Status status = Status::Unavailable;
        OGLProgram program;
    };

    Result Request(u64 key, const std::string& vertex, const std::string& fragment, bool reuse_stages) {
        std::lock_guard lock(mutex);
        if (!thread.joinable() || unavailable) {
            return {};
        }
        if (auto result = results.find(key); result != results.end()) {
            if (result->second.status != Status::Ready)
                return {result->second.status, {}};
            auto ready = std::move(result->second);
            results.erase(result);
            return ready;
        }
        if (jobs.size() >= MaxQueued) {
            return {Status::Busy, {}};
        }
        if (results.size() >= MaxTracked) {
            const auto stale = std::find_if(results.begin(), results.end(), [](const auto& entry) {
                return entry.second.status == Status::Unavailable;
            });
            if (stale != results.end()) {
                results.erase(stale);
            } else {
                return {Status::Busy, {}};
            }
        }
        results.emplace(key, Result{Status::Pending, {}});
        jobs.push_back({key, vertex, fragment, reuse_stages});
        wake.notify_one();
        return {Status::Pending, {}};
    }

    OGLProgram TakeRequiredVertexStage(u64 key) {
        std::unique_lock lock(mutex);
        // The owner can build immediately if the worker hasn't claimed this
        // request. Only a compile already in progress can delay the draw.
        const auto queued = std::find_if(jobs.begin(), jobs.end(), [&](const Job& job) {
            return job.key == key;
        });
        if (queued != jobs.end()) {
            jobs.erase(queued);
            results.erase(key);
            return {};
        }
        completed.wait(lock, [&] {
            const auto result = results.find(key);
            return unavailable || result == results.end() || result->second.status != Status::Pending;
        });
        const auto result = results.find(key);
        if (result == results.end())
            return {};
        auto program = std::move(result->second.program);
        results.erase(result);
        return program;
    }

private:
    struct Job {
        u64 key;
        std::string vertex;
        std::string fragment;
        bool reuse_stages;
    };
    static constexpr std::size_t MaxQueued = 8;
    static constexpr std::size_t MaxTracked = 64;

    void Run() {
        Common::SetCurrentThreadName("CaraShaderBuild");
        if (!eglMakeCurrent(display, surface, surface, context)) {
            std::lock_guard lock(mutex);
            unavailable = true;
            jobs.clear();
            completed.notify_all();
            return;
        }
        const char* reuse = std::getenv("CARA_ANDROID_SHADER_STAGE_REUSE");
        const bool force_stage_reuse = reuse && std::strcmp(reuse, "1") == 0;
        std::unordered_map<std::string, OGLShader> vertices, fragments;
        const auto get_stage = [](auto& stages, const std::string& source, GLenum type) {
            auto found = stages.find(source);
            if (found != stages.end())
                return found->second.handle;
            if (stages.size() == 16)
                stages.clear();
            auto& shader = stages[source];
            shader.Create(source.c_str(), type);
            return shader.handle;
        };
        while (true) {
            Job job;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [&] { return stopping || !jobs.empty(); });
                if (stopping) {
                    // Destroy unpublished GL objects while a context is current.
                    results.clear();
                    break;
                }
                job = std::move(jobs.front());
                jobs.pop_front();
            }
            OGLProgram program;
            if (job.fragment.empty()) {
                OGLShader shader;
                shader.Create(job.vertex.c_str(), GL_VERTEX_SHADER, true);
                if (shader.handle)
                    program.Create(true, {shader.handle});
            } else if (job.reuse_stages || force_stage_reuse) {
                const auto vertex = get_stage(vertices, job.vertex, GL_VERTEX_SHADER);
                const auto fragment = get_stage(fragments, job.fragment, GL_FRAGMENT_SHADER);
                if (vertex && fragment)
                    program.Create(false, {vertex, fragment});
            } else {
                program.Create(job.vertex.c_str(), job.fragment.c_str());
            }
            if (program.handle) {
                // Complete compilation and publish across contexts before the
                // emulation thread may bind the shared program.
                glFinish();
            }
            {
                std::lock_guard lock(mutex);
                const auto status = program.handle ? Status::Ready : Status::Unavailable;
                if (results.size() == MaxTracked && status == Status::Ready) {
                    const auto unclaimed = std::find_if(results.begin(), results.end(),
                                                        [](const auto& entry) {
                        return entry.second.status == Status::Ready;
                    });
                    if (unclaimed != results.end())
                        results.erase(unclaimed);
                }
                results.at(job.key) = {status, std::move(program)};
            }
            completed.notify_all();
        }
        vertices.clear();
        fragments.clear();
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglReleaseThread();
    }

    EGLDisplay display = EGL_NO_DISPLAY;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable completed;
    std::deque<Job> jobs;
    std::unordered_map<u64, Result> results;
    bool stopping = false;
    bool unavailable = false;
    std::thread thread;
};

} // namespace OpenGL
