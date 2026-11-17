// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <limits>
namespace Cara::GuestGPUTrace::WaitOrigins {
constexpr std::uint64_t UnknownSequence=std::numeric_limits<std::uint64_t>::max();
enum class Reason : std::uint32_t { None, NoObservedArm, RearmAmbiguous, DeadlineUnknown,
    DeadlineMismatch, CancelledArm, ThreadCapacity, GenerationOverflow, ClockMismatch };
enum class Cause : std::uint32_t { OtherUnknown, TimerCallback, ArbiterSignal };
enum class Cancel : std::uint32_t { ContextSwitch=1, ThreadStop=2 };
enum class Resume : std::uint32_t { ReadyQueued=1, AlreadyReady=2, AlreadyRunning=3, Dead=4 };
struct Link { std::uint64_t generation=0,sequence=UnknownSequence;Reason reason=Reason::NoObservedArm; };
struct CauseLink { Link arm{};Cause cause=Cause::OtherUnknown;std::uint64_t event=UnknownSequence; };
struct ArmResult { Link request{};Reason error=Reason::None; };
inline bool Add(std::int64_t a,std::int64_t b,std::int64_t& result) {
    if ((b>0&&a>std::numeric_limits<std::int64_t>::max()-b) ||
        (b<0&&a<std::numeric_limits<std::int64_t>::min()-b))return false;
    result=a+b;return true;
}
inline bool Subtract(std::int64_t a,std::int64_t b,std::int64_t& result) {
    if ((b>0&&a<std::numeric_limits<std::int64_t>::min()+b) ||
        (b<0&&a>std::numeric_limits<std::int64_t>::max()+b))return false;
    result=a-b;return true;
}
// Diagnostic side state only. No Kernel::Thread members or guest serialization.
// Re-arm before consumption/cancellation is conservatively ambiguous, never
// attributed to the newest timer solely because userdata contains a thread ID.
template<std::size_t Capacity=256,std::uint64_t GenerationLimit=std::numeric_limits<std::uint64_t>::max()> class Tracker {
    struct Slot {
        std::uint32_t thread=0,timer=0,cancelled_timer=0;std::uint64_t next_generation=0;
        Link pending{},cancelled{};std::int64_t deadline=0,cancelled_deadline=0;
        bool has_pending=false,ambiguous=false,history_ambiguous=false,deadline_known=false,cancelled_known=false;
        CauseLink cause{};
    };
    std::array<Slot,Capacity> slots{};
    Slot* Find(std::uint32_t id,bool create) {
        for(auto& slot:slots)if(slot.thread==id&&id)return &slot;
        if(create&&id)for(auto& slot:slots)if(!slot.thread){slot.thread=id;return &slot;}
        return nullptr;
    }
public:
    void Reset(){slots={};}
    ArmResult Arm(std::uint32_t id,std::uint64_t sequence,bool scheduled,
                  bool deadline_known,std::int64_t deadline,std::uint32_t timer=0) {
        auto* s=Find(id,true);if(!s)return {{},Reason::ThreadCapacity};
        if(s->next_generation==GenerationLimit)return {{},Reason::GenerationOverflow};
        Link request{++s->next_generation,sequence,Reason::None};
        if(scheduled){s->history_ambiguous|=s->has_pending;s->ambiguous|=s->has_pending||s->history_ambiguous;s->pending=request;s->has_pending=true;
            s->deadline=deadline;s->timer=timer;s->deadline_known=deadline_known;
            if(s->cancelled_known&&deadline_known&&s->cancelled_deadline==deadline&&s->cancelled_timer==timer)s->ambiguous=true;}
        return {request,Reason::None};
    }
    Link Pending(std::uint32_t id) {
        auto* s=Find(id,false);if(!s||!s->has_pending)return {};
        if(s->ambiguous||s->history_ambiguous)return {0,UnknownSequence,Reason::RearmAmbiguous};
        return s->pending;
    }
    Link Fire(std::uint32_t id,std::uint64_t event,bool deadline_known,std::int64_t deadline,std::uint32_t timer=0) {
        auto* s=Find(id,true);if(!s)return {0,UnknownSequence,Reason::ThreadCapacity};
        Link link;
        if(s->ambiguous||s->history_ambiguous)link.reason=Reason::RearmAmbiguous;
        else if(!deadline_known)link.reason=Reason::DeadlineUnknown;
        else if(s->has_pending&&s->deadline_known&&s->deadline==deadline&&s->timer==timer){link=s->pending;s->has_pending=false;}
        else if(s->cancelled_known&&s->cancelled_deadline==deadline&&s->cancelled_timer==timer){link=s->cancelled;link.reason=Reason::CancelledArm;}
        else if(s->has_pending)link.reason=s->timer!=timer?Reason::ClockMismatch:Reason::DeadlineMismatch;
        s->cause={link,Cause::TimerCallback,event};return link;
    }
    Link CancelAll(std::uint32_t id) {
        auto* s=Find(id,false);if(!s)return {};
        Link link=Pending(id);
        if(s->has_pending&&!s->ambiguous){s->cancelled=s->pending;s->cancelled_deadline=s->deadline;s->cancelled_timer=s->timer;s->cancelled_known=s->deadline_known;}
        s->has_pending=false;s->ambiguous=false;s->cause={};return link;
    }
    Reason ArbiterSignal(std::uint32_t id,std::uint64_t sequence) {
        auto* s=Find(id,true);if(!s)return Reason::ThreadCapacity;
        s->cause={Pending(id),Cause::ArbiterSignal,sequence};return Reason::None;
    }
    CauseLink ResumeLink(std::uint32_t id) {
        auto* s=Find(id,false);if(!s)return {};
        if(s->cause.event!=UnknownSequence)return s->cause;
        return {Pending(id),Cause::OtherUnknown,UnknownSequence};
    }
    void ResumeDone(std::uint32_t id){auto* s=Find(id,false);if(s)s->cause={};}
};
} // namespace Cara::GuestGPUTrace::WaitOrigins
