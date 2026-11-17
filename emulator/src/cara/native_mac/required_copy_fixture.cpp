// SPDX-License-Identifier: GPL-2.0-or-later
// Original-data native diagnostic. Pattern/layout/state checks adapt the existing
// Cara gl_native_required_copy_self_test.cpp (GPL-2.0-or-later), not a game asset.
#include "cara/native_mac/required_copy_fixture.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <vector>
#include "common/frame_timing.h"
#include "common/scope_exit.h"
#include "core/arm/arm_interface.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/process.h"
#include "core/memory.h"
#include "core/settings.h"
#include "core/savestate.h"
#include "video_core/renderer_opengl/gl_command_queue.h"
#include "video_core/renderer_opengl/gl_rasterizer.h"
#include "video_core/renderer_opengl/gl_rasterizer_cache.h"
#include "video_core/renderer_opengl/gl_state.h"
#include "video_core/renderer_opengl/gl_vars.h"
#include "video_core/video_core.h"

namespace OpenGL {
// Existing intended test friend. Its other definitions are separate executable
// sources mac_lut_self_test.cpp / ios/gles_lut_self_test.mm, NEVER native/core TUs.
struct LUTResidencyTestAccess {
    static RasterizerCacheOpenGL& Cache(RasterizerOpenGL& renderer) { return renderer.res_cache; }
};
}
namespace Cara::NativeBehavior {
namespace {
void Require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
bool Flag(const char* name){const char* v=std::getenv(name);return v&&std::strcmp(v,"1")==0;}
struct CPUWitness {
 std::array<std::array<u32,82>,4> registers{};std::array<s64,4> ticks{};
 bool operator==(const CPUWitness&)const=default;
};
CPUWitness Capture(Core::System& s){CPUWitness w;for(unsigned c=0;c<4;++c){auto& a=s.GetCore(c);for(unsigned r=0;r<16;++r)w.registers[c][r]=a.GetReg(r);for(unsigned r=0;r<64;++r)w.registers[c][r+16]=a.GetVFPReg(r);w.registers[c][80]=a.GetCPSR();w.registers[c][81]=a.GetVFPSystemReg(VFP_FPSCR);w.ticks[c]=s.CoreTiming().GetTimer(c)->GetTicks();}return w;}

enum class CopyMode { Direct, Validate, Pending };
struct CopyQueuePoint { u64 submitted=0,pending=0,recorded=0,coarse_sync=0; };
CopyQueuePoint ObserveCopyQueue(){const auto q=OpenGL::CommandQueue::SnapshotStats();return {q.submitted_batches,q.pending_commands,OpenGL::CommandQueue::GetUncheckedBlitProducerStats().blit_commands_recorded,q.transaction_sync_calls};}
struct PendingCopy {
 std::vector<u8> expected;u64 recorded=0,submitted_batches_before=0;
 CopyQueuePoint before{},after{},boundary{};bool before_valid=false,after_valid=false,boundary_valid=false;
};
std::string CopyQueueJSON(const PendingCopy& p){
 const auto point=[](const CopyQueuePoint& q){std::ostringstream o;o<<"{\"submitted_batches\":"<<q.submitted<<",\"pending_commands\":"<<q.pending<<",\"recorded_blits\":"<<q.recorded<<",\"coarse_sync_calls\":"<<q.coarse_sync<<"}";return o.str();};
 std::ostringstream o;o<<"{\"before_valid\":"<<(p.before_valid?"true":"false")<<",\"after_valid\":"<<(p.after_valid?"true":"false")<<",\"boundary_valid\":"<<(p.boundary_valid?"true":"false")<<",\"before\":"<<point(p.before)<<",\"after\":"<<point(p.after)<<",\"boundary\":"<<point(p.boundary)<<"}";return o.str();
}
}
static Result RunCopyCases(Core::System& system,CopyMode mode,PendingCopy* pending=nullptr,unsigned pending_seed=17) {
 using namespace OpenGL;
 Result result;u64 cases=0,sync_witnesses=0,record_witnesses=0;bool cpu_unchanged=false,cleanup_ok=false,state_ok=false,restore_ok=false;u64 cpu_write_cases=0;
 std::array<GLenum,2> cpu_write_read_GL_errors{};u64 cpu_write_read_GL_queries=0;
 GLenum final_error=GL_NO_ERROR;
 try {
  Require(!GLES&&!Common::FrameTiming::counters.enabled,"required-copy fixture needs desktop GL and timing counters OFF");
  const char* queries=std::getenv("CARA_NATIVE_COPY_RUNTIME_QUERIES");
  const bool record_expected=CommandQueue::Enabled()&&CommandQueue::UncheckedBlitProducerRequested()&&
      queries&&std::strcmp(queries,"0")==0&&!Flag("CARA_NATIVE_COPY_FORCE_QUERIES");
  Require(!Settings::values.skip_cpu_write&&!Settings::values.skip_texture_copy&&Settings::values.resolution_factor==1&&!Settings::values.custom_textures,"default 1x memory/copy settings required");
  Require(!Flag("CARA_RETAIN_SMALL_CPU_WRITES")&&!Flag("CARA_NATIVE_SHADER_SURFACE_COPY")&&!Flag("CARA_NATIVE_COPYTEX_SURFACE_COPY"),"requires default copy backend/retirement");
  auto* renderer=dynamic_cast<RasterizerOpenGL*>(VideoCore::Rasterizer());Require(renderer,"native rasterizer missing");
  auto& cache=LUTResidencyTestAccess::Cache(*renderer);auto& memory=system.Memory();auto process=system.Kernel().GetCurrentProcess();Require(bool(process),"process missing");
  constexpr u32 scratch_size=1024*1024;constexpr PAddr scratch=Memory::VRAM_PADDR_END-scratch_size;
  constexpr VAddr scratch_va=Memory::VRAM_VADDR+(scratch-Memory::VRAM_PADDR);
  // Current renderer owns this cache; reject even one preexisting cached page.
  for(u32 i=0;i<scratch_size;i+=Memory::PAGE_SIZE)Require(memory.GetCurrentPageTable()->attributes[(scratch_va+i)>>Memory::PAGE_BITS]!=Memory::PageType::RasterizerCachedMemory,"scratch page already cache-owned");
  u8* ram=memory.GetPhysicalPointer(scratch);Require(ram&&memory.GetPointer(scratch_va)==ram,"scratch mapping invalid");
  const auto before_cpu=Capture(system);const std::vector<u8> backup(ram,ram+scratch_size);
  const auto old_state=OpenGLState::GetCurState();
  GLint active{},read_buffer{},pack_state[5]{},unpack_state[5]{};
  constexpr GLenum pack_names[]{GL_PIXEL_PACK_BUFFER_BINDING,GL_PACK_ALIGNMENT,GL_PACK_ROW_LENGTH,GL_PACK_SKIP_ROWS,GL_PACK_SKIP_PIXELS};
  constexpr GLenum unpack_names[]{GL_PIXEL_UNPACK_BUFFER_BINDING,GL_UNPACK_ALIGNMENT,GL_UNPACK_ROW_LENGTH,GL_UNPACK_SKIP_ROWS,GL_UNPACK_SKIP_PIXELS};
  glGetIntegerv(GL_ACTIVE_TEXTURE,&active);glGetIntegerv(GL_READ_BUFFER,&read_buffer);
  for(unsigned i=0;i<5;++i){glGetIntegerv(pack_names[i],&pack_state[i]);glGetIntegerv(unpack_names[i],&unpack_state[i]);}
  const bool old_dither=glIsEnabled(GL_DITHER),old_discard=glIsEnabled(GL_RASTERIZER_DISCARD);
  constexpr GLenum binding_names[]{GL_READ_FRAMEBUFFER_BINDING,GL_DRAW_FRAMEBUFFER_BINDING,GL_CURRENT_PROGRAM,GL_VERTEX_ARRAY_BINDING,GL_ARRAY_BUFFER_BINDING,GL_ELEMENT_ARRAY_BUFFER_BINDING};
  std::array<GLint,6> binding_entry{};for(unsigned i=0;i<6;++i)glGetIntegerv(binding_names[i],&binding_entry[i]);
  std::array<GLint,4> viewport_entry{},scissor_entry{};std::array<GLboolean,4> mask_entry{};
  glGetIntegerv(GL_VIEWPORT,viewport_entry.data());glGetIntegerv(GL_SCISSOR_BOX,scissor_entry.data());glGetBooleanv(GL_COLOR_WRITEMASK,mask_entry.data());
  const bool scissor_enabled_entry=glIsEnabled(GL_SCISSOR_TEST);
  GLint tex0_entry{},sampler0_entry{};glActiveTexture(GL_TEXTURE0);glGetIntegerv(GL_TEXTURE_BINDING_2D,&tex0_entry);glGetIntegerv(GL_SAMPLER_BINDING,&sampler0_entry);glActiveTexture(active);
  OGLFramebuffer framebuffer;OGLVertexArray vao;OGLBuffer pack;OGLSampler sampler;OGLProgram producer;
  std::vector<Surface> owned;
  const auto normal_pack=[] {glBindBuffer(GL_PIXEL_PACK_BUFFER,0);glPixelStorei(GL_PACK_ALIGNMENT,1);glPixelStorei(GL_PACK_ROW_LENGTH,0);glPixelStorei(GL_PACK_SKIP_ROWS,0);glPixelStorei(GL_PACK_SKIP_PIXELS,0);};
  const auto retire=[&] {
   // Known scratch surfaces only; retain_small_cpu_writes is rejected above.
   for(const auto& s:owned)cache.InvalidateRegion(s->addr,4,nullptr);
   owned.clear();
  };
  SCOPE_EXIT({
   // Exception cleanup remains visible as failure; never leave registered owners
   // over restored CPU RAM or delete the renderer's process-static helper FBOs.
   try {
   if(mode!=CopyMode::Pending){retire();CommandQueue::Drain();memory.WriteBlock(*process,scratch_va,backup.data(),backup.size());}
   // Pending mode deliberately leaves only cache-owned registered surfaces.
   // This entire GL object/state scope ends BEFORE the caller may LoadState.
   OpenGLState::InvalidateRasterState();old_state.Apply();glReadBuffer(read_buffer);
   glBindBuffer(GL_PIXEL_PACK_BUFFER,pack_state[0]);glBindBuffer(GL_PIXEL_UNPACK_BUFFER,unpack_state[0]);
   for(unsigned i=1;i<5;++i){glPixelStorei(pack_names[i],pack_state[i]);glPixelStorei(unpack_names[i],unpack_state[i]);}
   if(old_dither)glEnable(GL_DITHER);else glDisable(GL_DITHER);
   if(old_discard)glEnable(GL_RASTERIZER_DISCARD);else glDisable(GL_RASTERIZER_DISCARD);
   glActiveTexture(active);
   restore_ok=true;
   if(mode!=CopyMode::Pending){
   const auto check=[&](GLenum name,GLint expected){GLint actual{};glGetIntegerv(name,&actual);restore_ok&=actual==expected;};
   check(GL_ACTIVE_TEXTURE,active);check(GL_READ_BUFFER,read_buffer);
   for(unsigned i=0;i<6;++i)check(binding_names[i],binding_entry[i]);
   for(unsigned i=0;i<5;++i){check(pack_names[i],pack_state[i]);check(unpack_names[i],unpack_state[i]);}
   glActiveTexture(GL_TEXTURE0);check(GL_TEXTURE_BINDING_2D,tex0_entry);check(GL_SAMPLER_BINDING,sampler0_entry);glActiveTexture(active);
   decltype(viewport_entry) viewport{};decltype(scissor_entry) scissor{};decltype(mask_entry) mask{};
   glGetIntegerv(GL_VIEWPORT,viewport.data());glGetIntegerv(GL_SCISSOR_BOX,scissor.data());glGetBooleanv(GL_COLOR_WRITEMASK,mask.data());
   restore_ok&=viewport==viewport_entry&&scissor==scissor_entry&&mask==mask_entry&&bool(glIsEnabled(GL_DITHER))==old_dither&&bool(glIsEnabled(GL_RASTERIZER_DISCARD))==old_discard&&bool(glIsEnabled(GL_SCISSOR_TEST))==scissor_enabled_entry;
   }
   cleanup_ok&=restore_ok;
   } catch(...) {cleanup_ok=false;}
  });
  std::vector<u8> initial(scratch_size,0x6B);memory.WriteBlock(*process,scratch_va,initial.data(),initial.size());
  Require(glGetError()==GL_NO_ERROR,"incoming GL error");
  framebuffer.Create();vao.Create();pack.Create();sampler.Create();
  normal_pack();glBindBuffer(GL_PIXEL_PACK_BUFFER,pack.handle);const std::vector<u8> pbo_canary(4096,0xB4);glBufferData(GL_PIXEL_PACK_BUFFER,pbo_canary.size(),pbo_canary.data(),GL_STREAM_READ);normal_pack();
  producer.Create(R"(
void main(){const vec2 p[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));gl_Position=vec4(p[gl_VertexID],0,1);}
)",R"(
uniform int seed;out vec4 color;
void main(){ivec2 p=ivec2(gl_FragCoord.xy);ivec4 c=ivec4(p.x+3*p.y+seed,5*p.x+p.y+17+seed,7*p.x+11*p.y+91+seed,13*p.x+19*p.y+151+seed)&ivec4(255);color=vec4(c)/255.0;}
)");Require(producer.handle!=0,"authored producer shader missing");
  const GLint seed_location=glGetUniformLocation(producer.handle,"seed");Require(seed_location>=0,"producer uniform missing");
  const auto params=[](PAddr addr,u32 height,bool tiled){SurfaceParams p;p.addr=addr;p.width=p.stride=256;p.height=height;p.is_tiled=tiled;p.res_scale=1;p.pixel_format=SurfaceParams::PixelFormat::RGBA8;p.UpdateParams();return p;};
  const auto bytes=[](const SurfaceParams& p,unsigned seed){std::vector<u8> out(p.size);for(u32 y=0;y<p.height;++y)for(u32 x=0;x<p.width;++x){const u32 gy=p.is_tiled?p.height-1-y:y;u32 pixel=gy*p.stride+x;if(p.is_tiled){u32 m=0;for(unsigned bit=0;bit<3;++bit){m|=((x>>bit)&1)<<(2*bit);m|=((gy>>bit)&1)<<(2*bit+1);}pixel=(gy/8)*p.stride*8+(x/8)*64+m;}const std::array<u8,4> c{u8(x+3*y+seed),u8(5*x+y+17+seed),u8(7*x+11*y+91+seed),u8(13*x+19*y+151+seed)};for(unsigned k=0;k<4;++k)out[pixel*4+k]=c[p.is_tiled?3-k:k];}return out;};
  const auto draw=[&](const Surface& s,unsigned seed){OpenGLState state;state.draw.draw_framebuffer=framebuffer.handle;state.draw.read_framebuffer=framebuffer.handle;state.draw.shader_program=producer.handle;state.draw.vertex_array=vao.handle;state.viewport={0,0,GLsizei(s->width),GLsizei(s->height)};state.Apply();glDisable(GL_DITHER);glDisable(GL_RASTERIZER_DISCARD);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,s->texture.handle,0);glReadBuffer(GL_COLOR_ATTACHMENT0);Require(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"producer FBO incomplete");glUniform1i(seed_location,seed);glDrawArrays(GL_TRIANGLES,0,3);Require(glGetError()==GL_NO_ERROR,"producer error");cache.InvalidateRegion(s->addr,s->size,s);};
  const auto hostile=[&]{OpenGLState state;state.draw.draw_framebuffer=framebuffer.handle;state.draw.read_framebuffer=framebuffer.handle;state.draw.shader_program=producer.handle;state.draw.vertex_array=vao.handle;state.texture_units[0].texture_2d=owned.front()->texture.handle;state.texture_units[0].sampler=sampler.handle;state.viewport={3,5,7,9};state.scissor={true,1,2,3,4};state.color_mask={false,true,false,true};state.Apply();glReadBuffer(GL_COLOR_ATTACHMENT0);glEnable(GL_DITHER);glEnable(GL_RASTERIZER_DISCARD);glActiveTexture(GL_TEXTURE3);glBindBuffer(GL_PIXEL_PACK_BUFFER,pack.handle);glPixelStorei(GL_PACK_ALIGNMENT,8);glPixelStorei(GL_PACK_ROW_LENGTH,23);glPixelStorei(GL_PACK_SKIP_ROWS,2);glPixelStorei(GL_PACK_SKIP_PIXELS,3);};
  const auto state_preserved=[&]{const auto check=[](GLenum p,GLint expected){GLint v{};glGetIntegerv(p,&v);Require(v==expected,"copy changed scalar raw GL state");};check(GL_ACTIVE_TEXTURE,GL_TEXTURE3);glActiveTexture(GL_TEXTURE0);check(GL_TEXTURE_BINDING_2D,owned.front()->texture.handle);check(GL_SAMPLER_BINDING,sampler.handle);glActiveTexture(GL_TEXTURE3);check(GL_DRAW_FRAMEBUFFER_BINDING,framebuffer.handle);check(GL_READ_FRAMEBUFFER_BINDING,framebuffer.handle);check(GL_READ_BUFFER,GL_COLOR_ATTACHMENT0);check(GL_CURRENT_PROGRAM,producer.handle);check(GL_VERTEX_ARRAY_BINDING,vao.handle);check(GL_PIXEL_PACK_BUFFER_BINDING,pack.handle);check(GL_PACK_ALIGNMENT,8);check(GL_PACK_ROW_LENGTH,23);check(GL_PACK_SKIP_ROWS,2);check(GL_PACK_SKIP_PIXELS,3);Require(glIsEnabled(GL_DITHER)&&glIsEnabled(GL_RASTERIZER_DISCARD)&&glIsEnabled(GL_SCISSOR_TEST),"copy changed raw enable state");GLint viewport[4]{},scissor[4]{};GLboolean mask[4]{};glGetIntegerv(GL_VIEWPORT,viewport);glGetIntegerv(GL_SCISSOR_BOX,scissor);glGetBooleanv(GL_COLOR_WRITEMASK,mask);Require(viewport[0]==3&&viewport[1]==5&&viewport[2]==7&&viewport[3]==9&&scissor[0]==1&&scissor[1]==2&&scissor[2]==3&&scissor[3]==4&&!mask[0]&&mask[1]&&!mask[2]&&mask[3],"copy changed vector raw GL state");std::vector<u8> check_pack(4096);glGetBufferSubData(GL_PIXEL_PACK_BUFFER,0,check_pack.size(),check_pack.data());Require(check_pack==pbo_canary,"copy wrote pack PBO");Require(glGetError()==GL_NO_ERROR,"copy state error");};
  for(bool tiled:{false,true})for(unsigned part=0;part<(mode==CopyMode::Direct?3u:2u);++part) {
   if(mode==CopyMode::Pending&&(tiled||part))continue;
   const auto sp=params(scratch,912,tiled),dp=params(scratch+400*256*4,512,tiled);
   auto source=cache.GetSurface(sp,ScaleMatch::Exact,true);Require(bool(source)&&source->GetInterval()==sp.GetInterval()&&source->width==sp.width&&source->height==sp.height&&source->res_scale==1,"source acquisition failed");owned.push_back(source);
   auto destination=cache.GetSurface(dp,ScaleMatch::Exact,true);Require(bool(destination)&&destination!=source&&destination->texture.handle!=source->texture.handle&&destination->GetInterval()==dp.GetInterval()&&destination->width==dp.width&&destination->height==dp.height&&destination->res_scale==1,"destination acquisition alias");owned.push_back(destination);
   const unsigned seed=mode==CopyMode::Pending?pending_seed:17+part*37;draw(destination,seed+97);draw(source,seed);
   const auto original=bytes(sp,seed);auto expected=bytes(dp,seed+97);
   const u32 unit=tiled?256u:4u,row=256*4*(tiled?8:1);
   const SurfaceInterval interval=(mode!=CopyMode::Direct||part==0)?dp.GetInterval():part==1?SurfaceInterval(dp.addr+row*2,dp.addr+row*4):SurfaceInterval(dp.addr+row*3+unit*3,dp.addr+row*3+unit*5);
   const auto sub=dp.FromInterval(interval);Require(sub.GetInterval()==interval&&source->CanSubRect(sub)&&source->IsRegionValid(interval),"copy geometry/owner precondition failed");
   const auto begin=boost::icl::first(interval),end=boost::icl::last_next(interval);std::copy(original.begin()+(begin-sp.addr),original.begin()+(end-sp.addr),expected.begin()+(begin-dp.addr));
   // Only the FULL GPU-written source span becomes deliberately stale; outside
   // scratch canary bytes remain unchanged. There is no simulated CPU visibility.
   std::fill_n(ram,sp.size,0x6B);
   if(mode!=CopyMode::Pending)hostile();
   if(mode!=CopyMode::Direct) {
    Require(!destination->IsRegionValid(dp.GetInterval()),"alias destination was not genuinely invalidated");
    if(mode==CopyMode::Validate)CommandQueue::Flush();
   }
   if(mode==CopyMode::Pending){
    // SETUP ONLY. Return after all state restoration/private GL destruction.
    // The actual natural acquisition is performed later by PreparePendingCopy.
    Require(pending,"pending witness missing");pending->expected=initial;
    std::copy(original.begin(),original.end(),pending->expected.begin());
    ++cases;continue;
   }
   const auto before_sync=CommandQueue::Enabled()?CommandQueue::SnapshotStats().transaction_sync_calls:0;
   const auto before_record=CommandQueue::GetUncheckedBlitProducerStats().blit_commands_recorded;
   if(mode==CopyMode::Direct)cache.CopySurface(source,destination,interval);
   else {
    auto validated=cache.GetSurface(dp,ScaleMatch::Exact,true);
    Require(validated==destination&&destination->IsRegionValid(dp.GetInterval()),"natural ValidateSurface did not validate the same registered destination");
   }
   const auto after_record=CommandQueue::GetUncheckedBlitProducerStats().blit_commands_recorded;
   const auto after_sync=CommandQueue::Enabled()?CommandQueue::SnapshotStats().transaction_sync_calls:0;
   if(CommandQueue::Enabled()){
    Require(after_sync-before_sync==u64(!record_expected)&&after_record-before_record==u64(record_expected),"actual CopySurface sync/producer-record witness mismatch");
    sync_witnesses+=!record_expected;record_witnesses+=record_expected;
   }
   Require(!Common::FrameTiming::counters.enabled,"fixture enabled timing counters");
   if(mode==CopyMode::Direct||part==0)state_preserved();
   Require(std::all_of(ram,ram+scratch_size,[](u8 b){return b==0x6B;}),"CopySurface exported CPU RAM");
   if(mode==CopyMode::Validate&&part==1){
    // Real CPU coherence observation comes BEFORE any manual texture readback.
    // Pixel byte-address is interior in either layout; the other bytes retain
    // independently expected GPU values. There is no simulated RAM visibility.
    constexpr u32 offset=65536+128;
    const std::array<u8,4> patch{0x19,0xA7,0x62,0xE3};
    // This CPU observer uses the same pack-state precondition as the explicit
    // download branches. Setters only: no query, drain or materialization here.
    normal_pack();
    memory.WriteBlock(*process,scratch_va+(dp.addr-scratch)+offset,patch.data(),patch.size());
    std::copy(patch.begin(),patch.end(),expected.begin()+offset);
    std::vector<u8> observed(dp.size);
    memory.ReadBlock(*process,scratch_va+(dp.addr-scratch),observed.data(),observed.size());
    // Query AFTER the real WriteBlock/ReadBlock and BEFORE any assertion, so
    // a failing byte comparison retains the actual numeric GL boundary error.
    const GLenum coherence_error=glGetError();
    Require(cpu_write_read_GL_queries<cpu_write_read_GL_errors.size(),"CPU coherence error record overflow");
    cpu_write_read_GL_errors[cpu_write_read_GL_queries++]=coherence_error;
    Require(coherence_error==GL_NO_ERROR,"GL error at actual CPU WriteBlock/ReadBlock boundary");
    Require(observed==expected,"4-byte CPU write/read lost GPU neighbors or destination bytes");
    Require(std::all_of(ram+sp.size,ram+scratch_size,[](u8 b){return b==0x6B;}),"coherence wrote outside known GPU-owned source");
    ++cpu_write_cases;
   } else {
   normal_pack();destination->DownloadGLTexture(destination->GetRect());destination->FlushGLBuffer(destination->addr,destination->end);
   Require(std::equal(expected.begin(),expected.end(),ram+(dp.addr-scratch)),"full destination GPU bytes differ from independent expected partial copy");
   Require(std::all_of(ram,ram+(dp.addr-scratch),[](u8 b){return b==0x6B;})&&std::all_of(ram+(dp.end-scratch),ram+scratch_size,[](u8 b){return b==0x6B;}),"GPU readback touched outside destination canaries");
   }
   Require(glGetError()==GL_NO_ERROR,"copy/readback error");++cases;
   retire();source.reset();destination.reset();CommandQueue::Drain();
   for(u32 i=0;i<scratch_size;i+=Memory::PAGE_SIZE)Require(memory.GetCurrentPageTable()->attributes[(scratch_va+i)>>Memory::PAGE_BITS]!=Memory::PageType::RasterizerCachedMemory,"scratch owner failed to retire");
   // No old owners remain; next case starts from the original canary via real CPU write.
   memory.WriteBlock(*process,scratch_va,initial.data(),initial.size());
  }
  const u64 expected_cases=mode==CopyMode::Direct?6:mode==CopyMode::Validate?4:1;
  Require(cases==expected_cases&&(!CommandQueue::Enabled()||sync_witnesses+record_witnesses==(mode==CopyMode::Pending?0:expected_cases)),"incomplete copy matrix/witness");
  Require(mode!=CopyMode::Validate||cpu_write_cases==2,"small CPU write cases missing");
  cleanup_ok=true;cpu_unchanged=Capture(system)==before_cpu;Require(cpu_unchanged,"ARM/VFP/core ticks changed");state_ok=true;result.passed=true;
 } catch(const std::exception& e){result.error=e.what();}
 if(!cleanup_ok&&result.passed){result.passed=false;result.error="scratch cleanup failed";}
 final_error=mode==CopyMode::Pending?GL_NO_ERROR:glGetError();if(final_error!=GL_NO_ERROR){result.passed=false;result.error+="; GL error at native copy boundary";}
 std::ostringstream out;out<<"{\"mode\":"<<int(mode)<<",\"small_CPU_write_cases\":"<<cpu_write_cases<<",\"cpu_write_read_GL_error_queries\":"<<cpu_write_read_GL_queries<<",\"cpu_write_read_GL_errors\":[";for(u64 i=0;i<cpu_write_read_GL_queries;++i)out<<(i?",":"")<<cpu_write_read_GL_errors[i];out<<"],\"label\":\"original-data current-cache CopySurface host batch\",\"cases\":"<<cases<<",\"layouts\":"<<(mode==CopyMode::Pending?1:2)<<",\"partial_modes_per_layout\":"<<(mode==CopyMode::Direct?3:mode==CopyMode::Validate?2:1)<<",\"coarse_sync_witnesses\":"<<sync_witnesses<<",\"producer_record_witnesses\":"<<record_witnesses<<",\"timing_counters_enabled\":"<<(Common::FrameTiming::counters.enabled?"true":"false")<<",\"current_renderer_cache\":true,\"ARM_VFP_ticks_unchanged\":"<<(cpu_unchanged?"true":"false")<<",\"owned_surfaces_retired\":"<<(cleanup_ok&&mode!=CopyMode::Pending?"true":"false")<<",\"raw_state_scope\":\"direct:all;validation:readback-cases-only;pending:setter-restore-no-query\",\"per_copy_raw_state_preserved\":"<<(state_ok&&mode==CopyMode::Direct?"true":"false")<<",\"raw_entry_state_restored\":"<<(restore_ok?"true":"false")<<",\"GL_error\":"<<final_error<<",\"guest_ARM_Run_calls_by_fixture\":0,\"virtual_time_advance_calls_by_fixture\":0,\"ValidateSurface_integration_matrix\":"<<(mode==CopyMode::Validate?"true":"false")<<"}";result.detail_json=out.str();return result;
}

// Cache metadata/local Surface refs only after RunCopyCases returns: all
// fixture GL objects and captured OpenGLState snapshots have already died.
static Result PreparePendingCopy(Core::System& system,PendingCopy& p,unsigned seed){
 using namespace OpenGL;Result result=RunCopyCases(system,CopyMode::Pending,&p,seed);
 if(!result.passed)return result;result.passed=false;
 try {
  auto* renderer=dynamic_cast<RasterizerOpenGL*>(VideoCore::Rasterizer());Require(renderer,"pending current renderer missing");
  auto& cache=LUTResidencyTestAccess::Cache(*renderer);
  SurfaceParams dp;dp.addr=Memory::VRAM_PADDR_END-1024*1024+400*256*4;dp.width=dp.stride=256;dp.height=512;dp.is_tiled=false;dp.res_scale=1;dp.pixel_format=SurfaceParams::PixelFormat::RGBA8;dp.UpdateParams();
  auto destination=cache.GetSurface(dp,ScaleMatch::Exact,false);
  Require(destination&&destination->registered&&destination->GetInterval()==dp.GetInterval()&&!destination->IsRegionValid(dp.GetInterval()),"prepared registered destination missing/valid");
  // Publish ALL setup/restores/deletes before measuring the target copy.
  // Flush submits only; no GPU completion or extra wait is requested.
  CommandQueue::Flush();p.before=ObserveCopyQueue();p.before_valid=true;p.submitted_batches_before=p.before.submitted;
  {
   auto validated=cache.GetSurface(dp,ScaleMatch::Exact,true);
   Require(validated==destination&&destination->registered&&destination->IsRegionValid(dp.GetInterval()),"pending natural validation changed destination or failed");
  }
  // Dropping these cache-held refs cannot delete GL objects: registration and
  // the public cache still own the destination. No post-copy GL teardown follows.
  destination.reset();p.after=ObserveCopyQueue();p.after_valid=true;p.recorded=p.after.recorded-p.before.recorded;
  const char* queries=std::getenv("CARA_NATIVE_COPY_RUNTIME_QUERIES");
  const bool recording=CommandQueue::UncheckedBlitProducerRequested()&&queries&&std::strcmp(queries,"0")==0&&!Flag("CARA_NATIVE_COPY_FORCE_QUERIES");
  Require(p.recorded==u64(recording)&&p.after.coarse_sync-p.before.coarse_sync==u64(!recording),"pending natural copy sync/record witness mismatch");
  result.passed=true;
 }catch(const std::exception& e){
  if(p.before_valid){p.after=ObserveCopyQueue();p.after_valid=true;p.recorded=p.after.recorded-p.before.recorded;}
  result.error=e.what();
 }
 return result;
}
Result RunRequiredCopy(Core::System& system){return RunCopyCases(system,CopyMode::Direct);}
Result RunRequiredCopyIntegration(Core::System& system){
 using namespace OpenGL;
 Result result;u64 pending_save=0,pending_load=0,epoch_before=0,epoch_after=0;
 bool restored=false,archive_ok=false,cleanup_ok=false,post_save_scratch_retired=false;std::string validation_detail;PendingCopy saved,mutated;
 constexpr u32 size=1024*1024;constexpr PAddr pa=Memory::VRAM_PADDR_END-size;
 constexpr VAddr va=Memory::VRAM_VADDR+(pa-Memory::VRAM_PADDR);
 std::vector<u8> backup;
 try {
  Require(CommandQueue::Enabled()&&!Common::FrameTiming::counters.enabled,"integration requires queue and counters OFF");
  Require(!Flag("CARA_RETAIN_SMALL_CPU_WRITES"),"integration covers retain_small_CPU_writes=0 only");
  const char* user=std::getenv("CARA_USER_DIR");Require(user&&std::filesystem::path(user).is_absolute(),"private absolute user path missing");
  const std::string path=std::string(user)+"/native-unchecked-boundary.carastate";
  Require(!std::filesystem::exists(path),"fixture archive already exists");
  const auto cpu=Capture(system);
  // No cache/Memory/process/raw-RAM reference escapes this expression.
  for(u32 i=0;i<size;i+=Memory::PAGE_SIZE)Require(system.Memory().GetCurrentPageTable()->attributes[(va+i)>>Memory::PAGE_BITS]!=Memory::PageType::RasterizerCachedMemory,"integration scratch already owned");
  backup.resize(size);system.Memory().ReadBlock(*system.Kernel().GetCurrentProcess(),va,backup.data(),backup.size());
  auto validation=RunCopyCases(system,CopyMode::Validate);validation_detail=validation.detail_json;Require(validation.passed,validation.error.c_str());
  auto first=PreparePendingCopy(system,saved,41);Require(first.passed,first.error.c_str());
  // RunCopyCases has returned: ALL fixture GL objects, old state snapshots and
  // local Surface refs are gone. Only cache ownership and pure values survive.
  saved.boundary=ObserveCopyQueue();saved.boundary_valid=true;
  pending_save=CommandQueue::SnapshotStats().pending_commands;
  const bool recording=saved.recorded!=0;
  Require(!recording||(saved.recorded==1&&pending_save>0&&CommandQueue::SnapshotStats().submitted_batches==saved.submitted_batches_before),"recorder Save copy was not demonstrably still in the unsubmitted prefix");
  system.SaveStateToPath(path);
  Require(CommandQueue::SnapshotStats().pending_commands==0,"Save boundary left pending records");
  std::array<char,256> header{};std::ifstream archive(path,std::ios::binary);archive.read(header.data(),header.size());
  u32 backend{};std::memcpy(&backend,header.data()+180,4);
  archive_ok=archive.gcount()==256&&std::memcmp(header.data(),"CARAST01",8)==0&&backend==0&&
      std::string(header.data()+48,64)==Core::GetSaveStateBuildFingerprint();
  Require(archive_ok,"own archive header/FP/backend mismatch");
  std::vector<u8> observed(size);system.Memory().ReadBlock(*system.Kernel().GetCurrentProcess(),va,observed.data(),observed.size());
  Require(observed==saved.expected,"Save did not materialize registered GPU RAM");
  // Save flushes/invalidates but intentionally retains ordinary registrations.
  // Retire ONLY our two known scratch owners after checking the saved bytes.
  // Current-owner references are scoped here and never survive a later Load.
  {
   auto* current_renderer=dynamic_cast<RasterizerOpenGL*>(VideoCore::Rasterizer());
   Require(current_renderer,"current renderer missing after Save");
   auto& current_cache=LUTResidencyTestAccess::Cache(*current_renderer);
   current_cache.InvalidateRegion(pa,4,nullptr);
   current_cache.InvalidateRegion(pa+400*256*4,4,nullptr);
   CommandQueue::Drain();
   for(u32 i=0;i<size;i+=Memory::PAGE_SIZE)
    Require(system.Memory().GetCurrentPageTable()->attributes[(va+i)>>Memory::PAGE_BITS]!=Memory::PageType::RasterizerCachedMemory,"known scratch owners retained after explicit retirement");
   post_save_scratch_retired=true;
  }
  auto second=PreparePendingCopy(system,mutated,139);Require(second.passed,second.error.c_str());
  mutated.boundary=ObserveCopyQueue();mutated.boundary_valid=true;
  pending_load=CommandQueue::SnapshotStats().pending_commands;
  Require(!recording||(mutated.recorded==1&&pending_load>0&&CommandQueue::SnapshotStats().submitted_batches==mutated.submitted_batches_before),"recorder Load copy was not demonstrably still in the unsubmitted prefix");
  epoch_before=CommandQueue::Queue()->GetStats().epoch;
  system.LoadStateFromPath(path);
  // Every owner below is freshly resolved from current System AFTER Load.
  Require(system.IsPoweredOn(),"Load rollback closed System");
  epoch_after=CommandQueue::Queue()->GetStats().epoch;
  Require(epoch_after>epoch_before&&CommandQueue::SnapshotStats().pending_commands==0,"Load epoch/drain witness failed");
  for(u32 i=0;i<size;i+=Memory::PAGE_SIZE)Require(system.Memory().GetCurrentPageTable()->attributes[(va+i)>>Memory::PAGE_BITS]!=Memory::PageType::RasterizerCachedMemory,"loaded scratch page retains old cache ownership");
  system.Memory().ReadBlock(*system.Kernel().GetCurrentProcess(),va,observed.data(),observed.size());
  Require(observed==saved.expected&&Capture(system)==cpu,"Load changed saved RAM/selected CPU/FPSCR/timers");restored=true;
  // Fresh public acquisition/validation/readback after the recreated renderer.
  auto reacquired=RunCopyCases(system,CopyMode::Validate);Require(reacquired.passed,reacquired.error.c_str());
  Require(Capture(system)==cpu,"reacquisition advanced architecture/ticks");
  system.Memory().WriteBlock(*system.Kernel().GetCurrentProcess(),va,backup.data(),backup.size());cleanup_ok=true;
  Require(glGetError()==GL_NO_ERROR,"integration final GL error");result.passed=true;
 } catch(const std::exception& e){result.error=e.what();
  // Cleanup intentionally resolves ONLY current owners. Never reuse pre-Load
  // Cache/Memory/process/Surface/GLuint/OpenGLState snapshots after recreation.
  try {if(system.IsPoweredOn()&&!backup.empty()){
   auto* r=dynamic_cast<RasterizerOpenGL*>(VideoCore::Rasterizer());Require(r,"current renderer absent during cleanup");
   auto& c=LUTResidencyTestAccess::Cache(*r);c.InvalidateRegion(pa,4,nullptr);c.InvalidateRegion(pa+400*256*4,4,nullptr);
   CommandQueue::Drain();system.Memory().WriteBlock(*system.Kernel().GetCurrentProcess(),va,backup.data(),backup.size());cleanup_ok=true;
  }}catch(...){cleanup_ok=false;}
 }
 std::ostringstream out;out<<"{\"save_copy_queue_points\":"<<CopyQueueJSON(saved)<<",\"load_copy_queue_points\":"<<CopyQueueJSON(mutated)<<",\"validation\":"<<(validation_detail.empty()?"null":validation_detail)
  <<",\"pending_commands_at_Save\":"<<pending_save<<",\"pending_commands_at_Load\":"<<pending_load
  <<",\"epoch_before_Load\":"<<epoch_before<<",\"epoch_after_Load\":"<<epoch_after
  <<",\"post_Save_known_scratch_owners_retired\":"<<(post_save_scratch_retired?"true":"false")<<",\"archive_FP_backend_checked\":"<<(archive_ok?"true":"false")<<",\"saved_RAM_selected_CPU_FPSCR_timers_restored\":"<<(restored?"true":"false")
  <<",\"current_owner_cleanup\":"<<(cleanup_ok?"true":"false")<<",\"retain_small_CPU_writes\":false,\"guest_ARM_Run_calls_by_fixture\":0}";
 result.detail_json=out.str();return result;
}
} // namespace Cara::NativeBehavior
