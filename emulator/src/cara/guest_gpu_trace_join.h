// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <ostream>
#include "cara/guest_gpu_trace_buffer.h"

namespace Cara::GuestGPUTrace::Join {
enum class Reason : std::uint32_t {
    None, Disabled, Inactive, ForeignOwner, GenerationMismatch, NoGX,
    TraceOverflow, ContextOverflow, InvalidAnchor, DeferredBatch
};
struct Snapshot {
    Reason reason=Reason::NoGX;
    bool gx_valid=false,draw_valid=false,span_valid=false,execution_owner_valid=false;
    std::uint64_t generation=0;
    Record gx{},anchor{}; // Existing committed records; no clock/timer read.
    std::uint32_t execution_thread=0,execution_core=0;
};
inline Snapshot Unknown(Reason reason,std::uint64_t generation=0) {
    Snapshot s;s.reason=reason;s.generation=generation;return s;
}
struct Token { std::uint64_t generation=0;std::size_t depth=0;bool armed=false; };
class State {
public:
    static constexpr std::size_t Capacity=8;
    void Reset(std::uint64_t generation) {
        generation_=generation;depth_=0;poisoned_=false;current_=Unknown(Reason::NoGX,generation);
    }
    Token GX(std::uint64_t generation,const Record& begin,bool owner_valid,
             std::uint32_t execution_thread,std::uint32_t execution_core) {
        if(generation_!=generation)Reset(generation);
        Snapshot next;
        if(begin.kind!=static_cast<std::uint32_t>(Kind::GXBegin)||!generation)
            next=Unknown(Reason::InvalidAnchor,generation);
        else {
            next.reason=Reason::None;next.generation=generation;next.gx_valid=true;
            next.span_valid=begin.a==1;next.gx=next.anchor=begin;
            next.execution_owner_valid=owner_valid;
            next.execution_thread=execution_thread;next.execution_core=execution_core;
        }
        return Push(next);
    }
    Token Draw(std::uint64_t generation,const Record& draw,bool owner_valid,
               std::uint32_t execution_thread,std::uint32_t execution_core) {
        Snapshot next=current_;
        if(generation_!=generation || !next.gx_valid || draw.kind!=static_cast<std::uint32_t>(Kind::Draw)||
           !next.span_valid||draw.submission!=next.gx.submission||draw.sequence<=next.gx.sequence)
            next=Unknown(Reason::InvalidAnchor,generation);
        else {
            next.draw_valid=true;next.anchor=draw;next.execution_owner_valid=owner_valid;
            next.execution_thread=execution_thread;next.execution_core=execution_core;
        }
        return Push(next);
    }
    Token Deferred(std::uint64_t generation) {
        if(generation_!=generation)Reset(generation);
        // Neither current submission nor current draw owns an older deferred batch.
        return Push(Unknown(Reason::DeferredBatch,generation));
    }
    void Pop(Token token) {
        if(!token.armed||token.generation!=generation_)return;
        if(!depth_||token.depth!=depth_){Poison();return;}
        current_=saved_[--depth_];
        if(poisoned_)current_=Unknown(Reason::ContextOverflow,generation_);
    }
    Snapshot Read(std::uint64_t generation,bool active,bool owner,bool trace_ok) const {
        if(!active)return Unknown(Reason::Inactive,generation);
        if(!owner)return Unknown(Reason::ForeignOwner,generation);
        if(!trace_ok)return Unknown(Reason::TraceOverflow,generation);
        if(generation_!=generation)return Unknown(Reason::GenerationMismatch,generation);
        if(poisoned_)return Unknown(Reason::ContextOverflow,generation);
        return current_;
    }
    bool Poisoned() const {return poisoned_;}
private:
    Token Push(Snapshot next) {
        if(poisoned_||depth_==Capacity){Poison();return {};}
        saved_[depth_++]=current_;current_=next;return {generation_,depth_,true};
    }
    void Poison(){poisoned_=true;current_=Unknown(Reason::ContextOverflow,generation_);}
    std::uint64_t generation_=0;
    std::size_t depth_=0;
    bool poisoned_=false;
    Snapshot current_{};
    std::array<Snapshot,Capacity> saved_{};
};
inline thread_local State state;
class Scope {
public:
    Scope()=default;
    Scope(const Scope&)=delete;Scope& operator=(const Scope&)=delete;
    ~Scope(){if(token_.armed)state.Pop(token_);}
    void Set(Token token){token_=token;}
private:
    Token token_{};
};
inline void WriteJSON(std::ostream& out,const Snapshot& s) {
    const auto& a=s.anchor;const auto& g=s.gx;
    out<<"{\"reason\":"<<static_cast<std::uint32_t>(s.reason)
       <<",\"capture_generation\":"<<s.generation
       <<",\"gx_valid\":"<<(s.gx_valid?"true":"false")
       <<",\"draw_valid\":"<<(s.draw_valid?"true":"false")
       <<",\"command_span_valid\":"<<(s.span_valid?"true":"false")
       <<",\"execution_owner_valid\":"<<(s.execution_owner_valid?"true":"false");
    if(s.gx_valid) {
        out<<",\"submission\":"<<g.submission<<",\"gx_begin_sequence\":"<<g.sequence
           <<",\"command_id\":"<<g.a<<",\"queue_caller\":"<<g.thread<<",\"queue_owner\":"<<g.object
           <<",\"trace_frame\":"<<a.frame<<",\"anchor_sequence\":"<<a.sequence
           <<",\"timer_id\":"<<a.core<<",\"timer0_ticks\":"<<a.timer0_ticks
           <<",\"current_ticks\":"<<a.current_ticks<<",\"global_ticks\":"<<a.global_ticks;
        if(s.span_valid)out<<",\"command_address\":"<<(g.d>>32)<<",\"command_bytes\":"<<std::uint32_t(g.d);
        if(s.draw_valid)out<<",\"draw_sequence\":"<<a.sequence;
        if(s.execution_owner_valid)out<<",\"execution_thread\":"<<s.execution_thread<<",\"execution_core\":"<<s.execution_core;
    }
    out<<'}';
}
} // namespace Cara::GuestGPUTrace::Join
