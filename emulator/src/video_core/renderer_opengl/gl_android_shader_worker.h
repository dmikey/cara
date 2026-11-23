// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <EGL/egl.h>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

#include "common/thread.h"
#include "video_core/renderer_opengl/gl_resource_manager.h"

namespace OpenGL {

// Optional indexed programs are built in a separate context in the same share
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

    OGLProgram Request(u64 key, const std::string& vertex, const std::string& fragment) {
        std::lock_guard lock(mutex);
        if (!thread.joinable() || unavailable) {
            return {};
        }
        if (auto result = results.find(key); result != results.end()) {
            return std::move(result->second);
        }
        if (jobs.size() >= MaxQueued) {
            return {};
        }
        results.try_emplace(key);
        jobs.push_back({key, vertex, fragment});
        wake.notify_one();
        return {};
    }

private:
    struct Job {
        u64 key;
        std::string vertex;
        std::string fragment;
    };
    static constexpr std::size_t MaxQueued = 8;

    void Run() {
        Common::SetCurrentThreadName("CaraShaderBuild");
        if (!eglMakeCurrent(display, surface, surface, context)) {
            std::lock_guard lock(mutex);
            unavailable = true;
            jobs.clear();
            return;
        }
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
            program.Create(job.vertex.c_str(), job.fragment.c_str());
            if (program.handle) {
                // Complete compilation and publish across contexts before the
                // emulation thread may bind the shared program.
                glFinish();
            }
            {
                std::lock_guard lock(mutex);
                results.at(job.key) = std::move(program);
            }
        }
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglReleaseThread();
    }

    EGLDisplay display = EGL_NO_DISPLAY;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Job> jobs;
    std::unordered_map<u64, OGLProgram> results;
    bool stopping = false;
    bool unavailable = false;
    std::thread thread;
};

} // namespace OpenGL
