// Original-data ROM-free test; included in command_processor.cpp to use the
// ACTUAL unchanged WritePicaReg scalar oracle. GPL-2.0-or-later.
namespace {
std::string WholeShaderStreamPicaBytes() {
    std::ostringstream s(std::ios::out|std::ios::binary);
    {boost::archive::binary_oarchive ar(s);ar<<g_state;}
    return s.str();
}
struct ShaderStreamHooks final : VideoCore::RasterizerInterface {
    u32 calls=0;bool mutate=false;std::vector<std::array<u32,3>> writes;
    void PrePicaRegisterWrite(u32 id,u32 value,u32 mask) override {++calls;writes.push_back({id,value,mask});if(mutate&&calls==2){g_state.regs.vs.program.offset=5;g_state.regs.pipeline.gs_unit_exclusive_configuration.Assign(1);}}
    void AddTriangle(const Shader::OutputVertex&,const Shader::OutputVertex&,const Shader::OutputVertex&) override {}
    void DrawTriangles()override {} void NotifyPicaRegisterChanged(u32)override {}
    void FlushAll()override{} void FlushRegion(PAddr,u32)override{} void InvalidateRegion(PAddr,u32)override{}
    void FlushAndInvalidateRegion(PAddr,u32)override{}
};
struct ShaderStreamSaved {
    Regs regs; Shader::ShaderSetup vs,gs; decltype(g_state.cmd_list) list;
    Cara::P3DWorkEstimate p3d_work;
    static ShaderStreamSaved Take() {
        return {g_state.regs, g_state.vs, g_state.gs, g_state.cmd_list, g_state.experimental_p3d_work};
    }
    void Restore() const {
        g_state.regs = regs;
        g_state.vs = vs;
        g_state.gs = gs;
        g_state.cmd_list = list;
        g_state.experimental_p3d_work = p3d_work;
    }
    std::string Bytes()const {
        std::ostringstream s(std::ios::out|std::ios::binary);
        {boost::archive::binary_oarchive ar(s);
         ar<<boost::serialization::make_array(regs.reg_array.data(),Regs::NUM_REGS)<<vs<<gs<<p3d_work;}
        return s.str();
    }
};
}
bool RunShaderStreamBatchSelfTest(std::string& result) {
    if(g_debug_context || RepeatStateEnabled() || CommandProfileEnabled() || PostIRQProfileEnabled() ||
       Cara::GuestGPUTrace::active.load(std::memory_order_relaxed) || Cara::ExperimentalP3DCompletionEnabled()) {
        result="FAIL selftest requires tracing/profiling/repeat/P3D observers disabled";return false;
    }
    const auto original=ShaderStreamSaved::Take();const auto original_stats=shader_stream_batch_stats;
    SCOPE_EXIT({original.Restore();shader_stream_batch_stats=original_stats;});
    u64 cases=0, compared_words=0, hits=0, declined=0;
    const auto require=[](bool ok,const char* message){if(!ok)throw std::runtime_error(message);};
    try {
        const auto run=[&](u32 id,u32 mask,u32 start,u32 count,bool exclusive,bool use_gs,
                           bool grouped=false,bool truncated=false,unsigned alias=0) {
            std::vector<u32> payload(count+4);
            for(u32 i=0;i<payload.size();++i)payload[i]=0x9e3779b9u*(i+1)^0x7fc12345u;
            if(payload.size()>3){payload[0]=0;payload[1]=0x80000000u;payload[2]=0x7fc12345u;payload[3]=0xff800000u;}
            std::fill(std::begin(g_state.regs.reg_array),std::end(g_state.regs.reg_array),0x16325478u);
            g_state.regs.pipeline.gs_unit_exclusive_configuration.Assign(exclusive);
            g_state.regs.pipeline.use_gs.Assign(use_gs?PipelineRegs::UseGS::Yes:PipelineRegs::UseGS::No);
            g_state.regs.vs.program.offset=start;g_state.regs.vs.swizzle_patterns.offset=start;
            for(unsigned i=0;i<g_state.vs.program_code.size();++i){g_state.vs.program_code[i]=0xabc00000u+i;g_state.gs.program_code[i]=0xdef00000u+i;}
            for(unsigned i=0;i<g_state.vs.swizzle_data.size();++i){g_state.vs.swizzle_data[i]=0x12300000u+i;g_state.gs.swizzle_data[i]=0x45600000u+i;}
            // Seed private hash-dirty state false, then compare serialization after
            // each word. Repeated identical payloads must still mark dirty OFF.
            g_state.vs.GetProgramCodeHash();g_state.vs.GetSwizzleDataHash();
            g_state.gs.GetProgramCodeHash();g_state.gs.GetSwizzleDataHash();
            u32* source=payload.data();
            if(alias){source=alias==1?g_state.vs.program_code.data():g_state.gs.program_code.data();
                require(count+4<=g_state.vs.program_code.size(),"alias test capacity");
                std::copy(payload.begin(),payload.end(),source);}
            g_state.cmd_list.head_ptr=g_state.cmd_list.current_ptr=source;
            g_state.cmd_list.length=truncated&&count?count-1:count;
            const auto beginning=ShaderStreamSaved::Take();const auto initial_whole=WholeShaderStreamPicaBytes();auto expected=beginning;
            const auto observe=[&](u32) {
                const auto candidate=ShaderStreamSaved::Take();const auto candidate_whole=WholeShaderStreamPicaBytes();
                expected.Restore();
                const u32 value=*g_state.cmd_list.current_ptr++;
                WritePicaReg(id,value,mask,nullptr);
                expected=ShaderStreamSaved::Take();
                require(expected.Bytes()==candidate.Bytes(),"per-word scalar/batch state mismatch");
                require(WholeShaderStreamPicaBytes()==candidate_whole,"whole PICA state mismatch");
                require(expected.list.current_ptr==candidate.list.current_ptr,"cursor mismatch");
                require(expected.vs.engine_data.cached_shader==candidate.vs.engine_data.cached_shader &&
                        expected.gs.engine_data.cached_shader==candidate.gs.engine_data.cached_shader,"host shader state mismatch");
                candidate.Restore();++compared_words;
            };
            const bool used=TryShaderStreamExtras(id,count,mask,grouped,nullptr,false,false,observe);
            if(used){++hits;require(g_state.cmd_list.current_ptr==source+count,"accepted cursor count");}
            else {++declined;require(initial_whole==WholeShaderStreamPicaBytes(),"decline changed whole PICA state");require(beginning.Bytes()==ShaderStreamSaved::Take().Bytes() && g_state.cmd_list.current_ptr==source,"decline mutated state");
                // Execute original fallback, including its valid capacity prefix
                // and payload consumption past the logical (padded) truncation.
                for(u32 i=0;i<count;++i){const u32 value=*g_state.cmd_list.current_ptr++;WritePicaReg(id+(grouped?i:0),value,mask,nullptr);}
                const auto actual=ShaderStreamSaved::Take();const auto actual_whole=WholeShaderStreamPicaBytes();beginning.Restore();
                for(u32 i=0;i<count;++i){const u32 value=*g_state.cmd_list.current_ptr++;WritePicaReg(id+(grouped?i:0),value,mask,nullptr);}
                require(actual.Bytes()==ShaderStreamSaved::Take().Bytes() && actual.list.current_ptr==g_state.cmd_list.current_ptr,"fallback mismatch");
                require(actual_whole==WholeShaderStreamPicaBytes(),"fallback whole state mismatch");
            }
            ++cases;
        };
        for(bool swizzle:{false,true})for(u32 port=0;port<8;++port)for(u32 mask=0;mask<16;++mask)
            for(bool exclusive:{false,true})for(bool gs:{false,true})
                run((swizzle?PICA_REG_INDEX(vs.swizzle_patterns.set_word[0]):PICA_REG_INDEX(vs.program.set_word[0]))+port,mask,4,3,exclusive,gs);
        for(bool swizzle:{false,true}) {
            const u32 id=swizzle?PICA_REG_INDEX(vs.swizzle_patterns.set_word[0]):PICA_REG_INDEX(vs.program.set_word[0]);
            const u32 cap=swizzle?u32(g_state.vs.swizzle_data.size()):512;
            for(u32 start:{0u,cap-1,cap,cap+1})for(u32 count:{0u,1u,2u,7u})run(id,15,start,count,false,false);
            for(u32 count:{64u,511u,2047u})run(id,15,0,count,true,false);
            run(id,15,0,7,false,false,false,true);
            run(id,15,0,3,false,false,true);
        }
        // Program source aliases target and mirrored GS storage; retain exact
        // read/store order rather than using memcpy/restrict/read-ahead.
        for(unsigned alias:{1u,2u})run(PICA_REG_INDEX(vs.program.set_word[0]),15,1,7,false,false,false,false,alias);
        {
            const auto policy = Cara::gpu_completion_policy;
            const auto estimate = g_state.experimental_p3d_work;
            SCOPE_EXIT({Cara::gpu_completion_policy = policy; g_state.experimental_p3d_work = estimate;});
            Cara::gpu_completion_policy = {true, true, true};
            g_state.experimental_p3d_work.fifth_ns = 4096;
            run(PICA_REG_INDEX(vs.program.set_word[0]), 15, 16, 8, false, false);
            require(g_state.experimental_p3d_work.fifth_ns == 4096 + 8 * 112,
                    "P3D charge changed for a timed program stream");
            run(PICA_REG_INDEX(vs.swizzle_patterns.set_word[0]), 15, 32, 8, true, false);
            require(g_state.experimental_p3d_work.fifth_ns == 4096 + 16 * 112,
                    "P3D charge changed for a timed swizzle stream");
        }
        // Nonstream and GS-only ports must remain on original path.
        run(PICA_REG_INDEX(gs.program.set_word[0]),15,0,2,true,true);
        {
            std::array<u32,4> words{0x12345678,0x7fc01234,0x80000000,0};
            const u32 id=PICA_REG_INDEX(vs.program.set_word[0]);
            g_state.regs.vs.program.offset=0;g_state.cmd_list.head_ptr=g_state.cmd_list.current_ptr=words.data();g_state.cmd_list.length=3;
            g_state.regs.pipeline.gs_unit_exclusive_configuration.Assign(0);
            const u32 old_gs5=g_state.gs.program_code[5];
            const auto before=WholeShaderStreamPicaBytes();ShaderStreamHooks hooks;hooks.mutate=true;
            require(!TryShaderStreamExtras(id,3,15,false,&hooks,false,false),"active hook admitted");
            require(before==WholeShaderStreamPicaBytes()&&hooks.calls==0,"decline called hook or changed state");
            for(u32 i=0;i<3;++i){const auto v=*g_state.cmd_list.current_ptr++;WritePicaReg(id,v,15,&hooks);}
            require(hooks.calls==3&&hooks.writes.size()==3,"fallback lost logical hook calls");
            for(u32 i=0;i<3;++i)require(hooks.writes[i]==std::array<u32,3>{id,words[i],15},"hook order/payload");
            require(g_state.vs.program_code[0]==words[0] && g_state.vs.program_code[5]==words[1] && g_state.vs.program_code[6]==words[2],"hook-updated index ignored");
            require(g_state.gs.program_code[0]==words[0] && g_state.gs.program_code[5]==old_gs5,"hook-updated mirroring ignored");
            ++cases;
            g_state.regs.vs.program.offset=0;g_state.cmd_list.current_ptr=words.data();
            const auto origin_before=WholeShaderStreamPicaBytes();
            require(!TryShaderStreamExtras(id,3,15,false,nullptr,true,false),"origin admitted");
            require(origin_before==WholeShaderStreamPicaBytes(),"origin decline changed state");++cases;
            const auto saved_debug=g_debug_context;g_debug_context=DebugContext::Construct();
            require(!TryShaderStreamExtras(id,3,15,false,nullptr,false,false),"debug admitted");
            g_debug_context=saved_debug;require(origin_before==WholeShaderStreamPicaBytes(),"debug decline changed state");++cases;
        }
        {
            std::array<u32,4> words{0,0x80000000u,0x7fc01234u,0xff800000u};
            const u32 id=PICA_REG_INDEX(vs.program.set_word[0]);
            g_state.regs.vs.program.offset=0;g_state.regs.pipeline.gs_unit_exclusive_configuration.Assign(0);
            g_state.cmd_list.head_ptr=g_state.cmd_list.current_ptr=words.data();g_state.cmd_list.length=4;
            const auto stats=shader_stream_batch_stats;
            require(TryShaderStreamExtras(id,4,15,false,nullptr,false,true),"counter case not admitted");
            require(shader_stream_batch_stats.packets==stats.packets+1 && shader_stream_batch_stats.words==stats.words+4 && shader_stream_batch_stats.mirrored_words==stats.mirrored_words+4,"packet/word/mirror accounting");
            require(g_state.regs.reg_array[id]==words[3] && g_state.regs.vs.program.offset==4,"last backing/index");
            require(g_state.vs.program_code[3]==words[3] && g_state.gs.program_code[3]==words[3],"last port payload");++cases;
            g_state.regs.vs.program.offset=0;g_state.cmd_list.current_ptr=words.data()+4;g_state.cmd_list.length=3;
            require(!TryShaderStreamExtras(id,1,15,false,nullptr,false,false),"past logical cursor admitted");++cases;
            g_state.cmd_list.current_ptr=reinterpret_cast<u32*>(reinterpret_cast<std::uintptr_t>(words.data())+1);
            require(!TryShaderStreamExtras(id,1,15,false,nullptr,false,false),"unaligned cursor admitted");++cases;
            // Relatively aligned pointers can both be absolutely misaligned.
            // Decline only: never execute the baseline's unsupported UB fallback.
            for(unsigned displacement:{1u,2u,3u}) {
                auto* bad=reinterpret_cast<u32*>(reinterpret_cast<std::uintptr_t>(words.data())+displacement);
                g_state.cmd_list.head_ptr=g_state.cmd_list.current_ptr=bad;
                g_state.cmd_list.length=3;g_state.regs.vs.program.offset=0;
                const auto before_bad=WholeShaderStreamPicaBytes();
                require(!TryShaderStreamExtras(id,1,15,false,nullptr,false,false),"absolute misalignment admitted");
                require(before_bad==WholeShaderStreamPicaBytes() && g_state.cmd_list.current_ptr==bad,
                        "absolute misalignment decline mutated state");++cases;
            }
            g_state.cmd_list.head_ptr=g_state.cmd_list.current_ptr=words.data();
        }
        {
            namespace T=Cara::GuestGPUTrace;
            const auto old_active=T::active.load(std::memory_order_relaxed);
            const auto old_owner=T::owner_generation.load(std::memory_order_relaxed);
            const auto old_foreign=T::foreign_events.load(std::memory_order_relaxed);
            const auto old_local=T::local_generation;
            SCOPE_EXIT({T::active.store(old_active,std::memory_order_relaxed);T::owner_generation.store(old_owner,std::memory_order_relaxed);T::foreign_events.store(old_foreign,std::memory_order_relaxed);T::local_generation=old_local;});
            std::array<u32,3> words{0x01234567,0x80000000,0x7fc01234};
            const u32 id=PICA_REG_INDEX(vs.program.set_word[0]);
            g_state.regs.vs.program.offset=0;g_state.regs.pipeline.gs_unit_exclusive_configuration.Assign(1);
            g_state.cmd_list.head_ptr=g_state.cmd_list.current_ptr=words.data();g_state.cmd_list.length=3;
            T::local_generation=0;T::owner_generation.store(0x1234,std::memory_order_relaxed);T::active.store(true,std::memory_order_relaxed);
            const auto before=WholeShaderStreamPicaBytes();
            require(!TryShaderStreamExtras(id,3,15,false,nullptr,false,false),"foreign-owner active trace admitted");
            require(T::foreign_events.load(std::memory_order_relaxed)==old_foreign,"eligibility incremented foreign trace counter");
            require(before==WholeShaderStreamPicaBytes(),"foreign trace decline mutated PICA/cursor");
            require(T::active.load(std::memory_order_relaxed)&&T::owner_generation.load(std::memory_order_relaxed)==0x1234&&T::local_generation==0,"trace ownership changed");
            // Original fallback alone owns its existing per-word foreign-event
            // observations. The fast-path eligibility must add NONE.
            for(u32 i=0;i<3;++i){const u32 value=*g_state.cmd_list.current_ptr++;WritePicaReg(id,value,15,nullptr);}
            require(T::foreign_events.load(std::memory_order_relaxed)==old_foreign+3,"scalar fallback foreign-event count changed");
            ++cases;
        }
        result="PASS "+std::to_string(cases)+" actual scalar-vs-stream cases; "+std::to_string(compared_words)+
            " per-word serialized state comparisons; hits="+std::to_string(hits)+" declines="+std::to_string(declined)+
            "; includes masks, ports, mirroring/useGS, dirty/hash state, raw NaN/zero, capacity, padded truncation and source alias and P3D command charges; foreign trace guard adds zero observations";
        return true;
    }catch(const std::exception& e){result=std::string("FAIL ")+e.what();return false;}
}
