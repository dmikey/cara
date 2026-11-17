// Included inside Cara::GuestGPUTrace, after Emit. No guest reads/scheduling.
inline bool WaitOriginsEnabled(){return wait_origins_enabled;}
inline void WaitOriginStop(WaitOrigins::Reason reason){wait_origin_error=static_cast<std::uint32_t>(reason);buffer.truncated=true;active=false;complete=true;}
inline bool WaitOriginReserve(std::size_t count){
    if(buffer.size>buffer.records.size() || count>buffer.records.size()-buffer.size){
        buffer.truncated=true;active=false;complete=true;return false;}
    return true;
}
template<class Timing> inline Record WaitOriginStamp(Timing& timing){
    return {0,frame,submission,timing.GetTimer(0)->GetTicks(),timing.GetTicks(),timing.GetGlobalTicks(),
            0,0,0,timing.GetCurrentTimerId(),0,0,0,0};
}
inline void WaitOriginPush(Record r){if(!buffer.Push(r)){buffer.truncated=true;active=false;complete=true;}}
#define CARA_WAIT_COLD __attribute__((noinline,cold))
#if !defined(__GNUC__) && !defined(__clang__)
#undef CARA_WAIT_COLD
#define CARA_WAIT_COLD
#endif

template<class Timing> CARA_WAIT_COLD inline void WaitOriginArmImpl(Timing& timing,
    std::uint32_t thread,std::uint32_t status,std::uint32_t address,bool address_known,
    std::int64_t ns,std::int64_t delay,bool scheduled){
    if(!WaitOriginReserve(2))return;
    auto stamp=WaitOriginStamp(timing);std::int64_t deadline=0;
    const bool known=scheduled&&WaitOrigins::Add(stamp.current_ticks,delay,deadline);
    const auto link=wait_origin_tracker.Arm(thread,buffer.size,scheduled,known,deadline,stamp.core);
    if(link.error!=WaitOrigins::Reason::None){WaitOriginStop(link.error);return;}
    auto r=stamp;r.kind=std::uint32_t(Kind::WaitOriginArm);r.thread=thread;r.object=status;
    r.a=std::uint64_t(ns);r.b=std::uint64_t(delay);r.c=address_known?address:0;r.d=link.request.generation;
    const auto parent=buffer.size;WaitOriginPush(r);
    r=stamp;r.kind=std::uint32_t(Kind::WaitOriginArmTiming);r.thread=thread;
    r.a=parent;r.b=link.request.generation;r.c=known?std::uint64_t(deadline):0;
    r.d=(scheduled?1u:0u)|(known?2u:0u)|(address_known?4u:0u);
    WaitOriginPush(r);
}
template<class Timing> inline void ObserveWaitArm(Timing& timing,std::uint32_t thread,
    std::uint32_t status,std::uint32_t address,bool address_known,std::int64_t ns,
    std::int64_t delay,bool scheduled){
    if(!wait_origins_enabled||!Enabled())return;
    WaitOriginArmImpl(timing,thread,status,address,address_known,ns,delay,scheduled);
}
template<class Timing> CARA_WAIT_COLD inline void WaitOriginTimerImpl(Timing& timing,
    std::uint32_t thread,std::uint32_t status,std::uint32_t address,bool address_known,
    std::int64_t cycles_late,std::uint32_t actual_timer,std::int64_t actual_ticks){
    if(!wait_origins_enabled||!Enabled()||!WaitOriginReserve(1))return;
    // Advance sets KernelSystem.current_cpu to the timer actually firing. Timing's
    // selected timer may still name another core; never use it for this record.
    Record r{0,frame,submission,timing.GetTimer(0)->GetTicks(),actual_ticks,
        timing.GetGlobalTicks(),0,0,0,actual_timer,0,0,0,0};
    std::int64_t deadline=0;
    const bool known=WaitOrigins::Subtract(r.current_ticks,cycles_late,deadline);
    const auto link=wait_origin_tracker.Fire(thread,buffer.size,known,deadline,r.core);
    if(link.reason==WaitOrigins::Reason::ThreadCapacity){WaitOriginStop(link.reason);return;}
    r.kind=std::uint32_t(Kind::WaitOriginTimer);r.thread=thread;r.object=status;
    r.a=std::uint64_t(cycles_late);r.b=link.generation;r.c=link.sequence;
    r.d=std::uint64_t(link.reason)|(std::uint64_t(address_known?address:0)<<32)|
        (address_known?(std::uint64_t(1)<<31):0);WaitOriginPush(r);
}
template<class Timing> inline void ObserveWaitTimer(Timing& timing,std::uint32_t thread,
    std::uint32_t status,std::uint32_t address,bool address_known,std::int64_t late,
    std::uint32_t actual_timer,std::int64_t actual_ticks){
    if(!wait_origins_enabled)return;
    WaitOriginTimerImpl(timing,thread,status,address,address_known,late,actual_timer,actual_ticks);
}
struct WaitResumeToken { bool enabled=false,address_known=false;std::uint32_t thread=0,before=0,address=0;WaitOrigins::CauseLink link{}; std::uint64_t capture=0; };
inline WaitResumeToken BeginWaitResume(std::uint32_t thread,std::uint32_t status,
    std::uint32_t address,bool address_known){
    if(!wait_origins_enabled||!Enabled())return {};
    return {true,address_known,thread,status,address,wait_origin_tracker.ResumeLink(thread),owner_generation.load(std::memory_order_relaxed)};
}
template<class Timing> CARA_WAIT_COLD inline void WaitOriginResumeImpl(Timing& timing,const WaitResumeToken& token,
    std::uint32_t after,WaitOrigins::Resume outcome){
    if(!token.enabled || token.capture!=owner_generation.load(std::memory_order_relaxed) ||
       !wait_origins_enabled || !Enabled())return;
    wait_origin_tracker.ResumeDone(token.thread);
    if(!WaitOriginReserve(1))return;
    auto r=WaitOriginStamp(timing);r.kind=std::uint32_t(Kind::WaitOriginResume);
    r.thread=token.thread;r.object=after;r.a=(std::uint64_t(token.before)<<32)|std::uint32_t(outcome);
    r.b=token.link.arm.generation;r.c=token.link.event;
    r.d=std::uint32_t(token.link.cause)|(std::uint64_t(token.link.arm.reason)<<8)|(std::uint64_t(token.address_known?token.address:0)<<32)|
        (token.address_known?(std::uint64_t(1)<<31):0);WaitOriginPush(r);
}
template<class Timing> inline void EndWaitResume(Timing& timing,const WaitResumeToken& token,
    std::uint32_t after,WaitOrigins::Resume outcome){
    if(!token.enabled)return;WaitOriginResumeImpl(timing,token,after,outcome);
}
template<class Timing> CARA_WAIT_COLD inline void WaitOriginCancelImpl(Timing& timing,
    std::uint32_t thread,std::uint32_t status,WaitOrigins::Cancel why){
    if(!wait_origins_enabled||!Enabled()||!WaitOriginReserve(1))return;
    const auto link=wait_origin_tracker.CancelAll(thread);auto r=WaitOriginStamp(timing);
    r.kind=std::uint32_t(Kind::WaitOriginCancel);r.thread=thread;r.object=status;
    r.a=std::uint32_t(why);r.b=link.generation;r.c=link.sequence;r.d=std::uint32_t(link.reason);
    WaitOriginPush(r);
}
template<class Timing> inline void ObserveWaitCancel(Timing& timing,std::uint32_t thread,
    std::uint32_t status,WaitOrigins::Cancel why){
    if(!wait_origins_enabled)return;WaitOriginCancelImpl(timing,thread,status,why);
}
template<class Timing> struct WaitArbScope {
    Timing* timing=nullptr;WaitArbContext previous{};std::uint64_t capture=0;bool enabled=false;
    WaitArbScope()=default;
    WaitArbScope(Timing* t,WaitArbContext p,std::uint64_t c):timing(t),previous(p),capture(c),enabled(true){}
    WaitArbScope(const WaitArbScope&)=delete;
    WaitArbScope(WaitArbScope&& other)noexcept:timing(other.timing),previous(other.previous),capture(other.capture),enabled(std::exchange(other.enabled,false)){}
    ~WaitArbScope(){
        if(!enabled)return;
        if(capture!=owner_generation.load(std::memory_order_relaxed)){wait_arb_context={};return;}
        if(wait_origins_enabled&&Enabled()&&WaitOriginReserve(1)){
            auto r=WaitOriginStamp(*timing);r.kind=std::uint32_t(Kind::WaitOriginArbEnd);
            r.thread=wait_arb_context.thread;r.a=wait_arb_context.sequence;r.b=wait_arb_context.waited;WaitOriginPush(r);}
        wait_arb_context=previous;
    }
};
template<class Timing> CARA_WAIT_COLD inline WaitArbScope<Timing> WaitOriginArbBeginImpl(Timing& timing,
    std::uint32_t thread,std::uint32_t arbiter,std::uint32_t type,std::uint32_t address,
    std::int32_t value,std::uint64_t ns){
    if(!wait_origins_enabled||!Enabled()||!WaitOriginReserve(1))return {};
    auto r=WaitOriginStamp(timing);r.kind=std::uint32_t(Kind::WaitOriginArbRequest);r.thread=thread;r.object=arbiter;
    r.a=address;r.b=type;r.c=std::uint64_t(std::int64_t(value));r.d=ns;
    const auto previous=wait_arb_context;wait_arb_context={buffer.size,thread,false};WaitOriginPush(r);
    return {&timing,previous,owner_generation.load(std::memory_order_relaxed)};
}
template<class Timing> inline WaitArbScope<Timing> BeginWaitArb(Timing& timing,
    std::uint32_t thread,std::uint32_t arbiter,std::uint32_t type,std::uint32_t address,
    std::int32_t value,std::uint64_t ns){
    if(!wait_origins_enabled)return {};
    return WaitOriginArbBeginImpl(timing,thread,arbiter,type,address,value,ns);
}
template<class Timing> CARA_WAIT_COLD inline void WaitOriginArbWaitImpl(Timing& timing,
    std::uint32_t thread,std::uint32_t arbiter,std::uint32_t address,std::uint32_t status){
    if(!wait_origins_enabled||!Enabled()||!WaitOriginReserve(1))return;
    if(wait_arb_context.thread==thread)wait_arb_context.waited=true;
    const auto link=wait_origin_tracker.Pending(thread);auto r=WaitOriginStamp(timing);
    r.kind=std::uint32_t(Kind::WaitOriginArbWait);r.thread=thread;r.object=arbiter;
    r.a=(std::uint64_t(address)<<32)|status;r.b=link.generation;r.c=link.sequence;r.d=wait_arb_context.sequence;
    WaitOriginPush(r);
}
template<class Timing> inline void ObserveArbWait(Timing& timing,std::uint32_t thread,
    std::uint32_t arbiter,std::uint32_t address,std::uint32_t status){
    if(!wait_origins_enabled)return;WaitOriginArbWaitImpl(timing,thread,arbiter,address,status);
}
template<class Timing> CARA_WAIT_COLD inline void WaitOriginArbWakeImpl(Timing& timing,
    std::uint32_t thread,std::uint32_t arbiter,std::uint32_t address,std::uint32_t status,bool timeout){
    if(!wait_origins_enabled||!Enabled()||!WaitOriginReserve(1))return;
    const auto sequence=buffer.size;auto r=WaitOriginStamp(timing);
    const auto link=timeout?wait_origin_tracker.ResumeLink(thread).arm:wait_origin_tracker.Pending(thread);
    if(!timeout){const auto error=wait_origin_tracker.ArbiterSignal(thread,sequence);
        if(error!=WaitOrigins::Reason::None){WaitOriginStop(error);return;}}
    r.kind=std::uint32_t(timeout?Kind::WaitOriginArbTimeout:Kind::WaitOriginArbWake);
    r.thread=thread;r.object=arbiter;r.a=(std::uint64_t(address)<<32)|status;
    r.b=link.generation;r.c=link.sequence;r.d=timeout?wait_origin_tracker.ResumeLink(thread).event:wait_arb_context.sequence;
    WaitOriginPush(r);
}
template<class Timing> inline void ObserveArbWake(Timing& timing,std::uint32_t thread,
    std::uint32_t arbiter,std::uint32_t address,std::uint32_t status,bool timeout){
    if(!wait_origins_enabled)return;WaitOriginArbWakeImpl(timing,thread,arbiter,address,status,timeout);
}
#undef CARA_WAIT_COLD
