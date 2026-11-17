// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <exception>
#include <stdexcept>
#include <glad/glad.h>
namespace Cara::NativeProfile {
// A failed Core load may already own renderer resources; treat load-start as live.
constexpr bool MustExitWithoutGLDestructors(bool core_load_started,bool owned_context_current) {
    return core_load_started&&!owned_context_current;
}
struct BoundaryOps {
    PFNGLFENCESYNCPROC fence=nullptr;
    PFNGLCLIENTWAITSYNCPROC wait=nullptr;
    PFNGLDELETESYNCPROC erase=nullptr;
    void (*drain)()=nullptr;
};
// Diagnostic-only terminal prefix completion; never advances a guest clock.
inline bool CompleteGPU(bool requested,bool diagnostics,bool benchmark,bool terminal,
                        const BoundaryOps& ops) {
    if(!requested||!terminal)return false;
    if(!diagnostics||!benchmark)throw std::runtime_error("GPU boundary completion requires diagnostics and benchmark");
    if(!ops.fence||!ops.wait||!ops.erase||!ops.drain)throw std::runtime_error("GPU completion sync entrypoint missing");
    GLsync sync=ops.fence(GL_SYNC_GPU_COMMANDS_COMPLETE,0);
    if(!sync)throw std::runtime_error("GPU completion fence returned null");
    bool drain_attempted=false;
    const auto cleanup=[&] {
        const auto old=sync;sync=nullptr;if(old)ops.erase(old);
        if(!drain_attempted){drain_attempted=true;ops.drain();}
    };
    try {
        bool done=false;
        for(unsigned attempt=0;attempt<100;++attempt) {
            const auto status=ops.wait(sync,attempt==0?GL_SYNC_FLUSH_COMMANDS_BIT:0,10000000ull);
            if(status==GL_ALREADY_SIGNALED||status==GL_CONDITION_SATISFIED){done=true;break;}
            if(status==GL_WAIT_FAILED)throw std::runtime_error("GPU completion wait failed");
            if(status!=GL_TIMEOUT_EXPIRED)throw std::runtime_error("GPU completion wait returned unexpected status");
        }
        if(!done)throw std::runtime_error("GPU completion bounded wait timed out");
        cleanup();return true;
    }catch(...) {const auto failure=std::current_exception();try{cleanup();}catch(...){}std::rethrow_exception(failure);}
}
}
