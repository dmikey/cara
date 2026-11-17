// SPDX-License-Identifier: GPL-2.0-or-later
// Original generated GPU oracle. Run after System.Load, before guest execution;
// the native caller must report the result and exit without entering guest Run.
#include <algorithm>
#include <array>
#include <cstring>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <vector>
#include "common/frame_timing.h"
#include "common/logging/log.h"
#include "common/scope_exit.h"
#include "core/core.h"
#include "core/memory.h"
#include "core/settings.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/process.h"
#include "video_core/pica_state.h"
#include "video_core/debug_utils/debug_utils.h"
#include "video_core/renderer_opengl/gl_rasterizer.h"
#include "video_core/renderer_opengl/gl_state.h"
#include "video_core/video_core.h"

namespace OpenGL {
// Narrow test-only access: validate unused scratch, inspect actual ownership,
// and retire only surfaces created inside that reserved scratch interval.
struct SoftwareBatchVisibilityTestAccess {
 static bool Unused(RasterizerCacheOpenGL& cache,SurfaceInterval interval) {
  auto surfaces=cache.surface_cache.equal_range(interval);auto dirty=cache.dirty_regions.equal_range(interval);
  return surfaces.first==surfaces.second&&dirty.first==dirty.second;
 }
 static bool Owned(RasterizerCacheOpenGL& cache,SurfaceInterval interval) {
  const auto dirty=cache.dirty_regions.equal_range(interval);return dirty.first!=dirty.second;
 }
 static void Retire(RasterizerCacheOpenGL& cache,SurfaceInterval interval) {
  std::vector<Surface> remove;const auto range=cache.surface_cache.equal_range(interval);
  for(auto i=range.first;i!=range.second;++i)for(const auto& surface:i->second)
   if(std::find(remove.begin(),remove.end(),surface)==remove.end())remove.push_back(surface);
  cache.dirty_regions.erase(interval);
  for(const auto& surface:remove){
   LOG_INFO(Render_OpenGL,"SW_VIS_RETIRE surface=0x{:x} texture={} registered={} addr=0x{:x} bytes={}",reinterpret_cast<std::uintptr_t>(surface.get()),surface->texture.handle,surface->registered,surface->addr,surface->size);
   OpenGLState::ResetTexture(surface->texture.handle);cache.UnregisterSurface(surface);cache.remove_surfaces.erase(surface);
  }
  // Local strong references release/deallocate textures on function return.

 }
 static bool Retain(RasterizerCacheOpenGL& cache,bool value) {
  bool old=cache.retain_small_cpu_writes;cache.retain_small_cpu_writes=value;return old;
 }
};

bool RasterizerOpenGL::RunSoftwareBatchVisibilitySelfTest(std::string& error) {
 using namespace SoftwareBatching;
 constexpr u32 scratch_bytes=64*1024,target_bytes=8*8*4;
 constexpr PAddr scratch=Memory::VRAM_PADDR_END-scratch_bytes;
 constexpr VAddr virtual_scratch=Memory::VRAM_VADDR_END-scratch_bytes;
 constexpr PAddr command_scratch=scratch+scratch_bytes-4096;
 const SurfaceInterval interval(scratch,scratch+scratch_bytes),target_interval(scratch,scratch+target_bytes);
 const auto enabled=[](const char* name){const char* value=std::getenv(name);return value&&std::strcmp(value,"1")==0;};
 if(!enabled("CARA_SW_BATCH_VISIBILITY_TEST")||!Enabled()||!DeferFramebufferFlushEnabled()) {
  error="visibility oracle requires its opt-in, SW batching and FB deferral before startup";return false;
 }
 for(const char* flag:{"CARA_GL_COMMAND_QUEUE","CARA_GPU_ABLATE_FRAGMENT_LIGHTING","CARA_GPU_ABLATE_PARTICLE_FAMILY","CARA_GPU_ABLATE_COMPOSITE_PASS","CARA_GPU_ABLATE_RAY_CANDIDATE"})
  if(enabled(flag)){error=std::string("visibility oracle requires disabled ")+flag;return false;}
 if(res_cache.GetScaleFactor()!=1){error="visibility oracle requires existing resolution scale1";return false;}
 if(Pica::g_debug_context||!vertex_batch.empty()||software_framebuffer_visibility.pending||software_batch_planner.in_list) {
  error="visibility oracle requires pristine rasterizer outside guest execution";return false;
 }
 if(!SoftwareBatchVisibilityTestAccess::Unused(res_cache,interval)||glGetError()!=GL_NO_ERROR) {
  error="visibility oracle requires unused scratch/cache and no prior GL error";return false;
 }
 auto& memory=*VideoCore::Memory();auto process=Core::System::GetInstance().Kernel().GetCurrentProcess();
 if(!process){error="visibility oracle has no loaded process for canonical VRAM mapping";return false;}
 const auto old_page_table=memory.GetCurrentPageTable();memory.SetCurrentPageTable(process->vm_manager.page_table);
 bool mapped_here=false;
 SCOPE_EXIT({if(mapped_here)process->vm_manager.UnmapRange(virtual_scratch,scratch_bytes);memory.SetCurrentPageTable(old_page_table);});
 auto table=memory.GetCurrentPageTable();unsigned unmapped=0;
 for(u32 offset=0;offset<scratch_bytes;offset+=Memory::PAGE_SIZE)
  unmapped+=table->attributes[(virtual_scratch+offset)>>Memory::PAGE_BITS]==Memory::PageType::Unmapped;
 if(unmapped) {
  if(unmapped!=scratch_bytes/Memory::PAGE_SIZE){error="visibility oracle scratch has mixed existing mappings";return false;}
  auto mapped=process->vm_manager.MapBackingMemory(virtual_scratch,memory.GetPhysicalRef(scratch),scratch_bytes,Kernel::MemoryState::IO);
  if(mapped.Failed()){error="visibility oracle could not map own VRAM scratch";return false;}
  mapped_here=true;
  if(process->vm_manager.ReprotectRange(virtual_scratch,scratch_bytes,Kernel::VMAPermission::ReadWrite).IsError()) {
   error="visibility oracle could not make scratch read/write";return false;
  }
 }
 u8* raw=memory.GetPhysicalPointer(scratch);
 if(!raw){error="visibility oracle physical scratch missing";return false;}
 for(u32 offset=0;offset<scratch_bytes;offset+=Memory::PAGE_SIZE)
  if(memory.GetPointer(virtual_scratch+offset)!=raw+offset){error="visibility oracle VRAM VA/PTE alias not canonical";return false;}
 const std::vector<u8> backup(raw,raw+scratch_bytes);
 auto& regs=Pica::g_state.regs;const auto saved_regs=regs;const auto saved_command=Pica::g_state.cmd_list;
 const auto saved_gl=OpenGLState::GetCurState();const auto saved_stats=stats;const auto saved_counters=Common::FrameTiming::counters;
 const auto saved_planner=software_batch_planner;const auto saved_visibility=software_framebuffer_visibility;
 const bool saved_skip=Settings::values.skip_cpu_write;
 const bool saved_retain=SoftwareBatchVisibilityTestAccess::Retain(res_cache,true);
 
 Settings::values.skip_cpu_write=false;Common::FrameTiming::counters.enabled=true;stats={};
 SCOPE_EXIT({
  Pica::g_debug_context.reset();
  try{FlushDeferredDraws();}catch(...){vertex_batch.clear();} // caller still reports failure and exits
  SoftwareBatchVisibilityTestAccess::Retire(res_cache,interval);
  std::memcpy(raw,backup.data(),backup.size());regs=saved_regs;Pica::g_state.cmd_list=saved_command;
  software_batch_planner=saved_planner;software_framebuffer_visibility=saved_visibility;
  software_has_deferred=software_batch_draw_safe=software_batch_flushing=false;
  software_vertices_since_request=software_completed_requests=0;
  SoftwareBatchVisibilityTestAccess::Retain(res_cache,saved_retain);Settings::values.skip_cpu_write=saved_skip;
  stats=saved_stats;Common::FrameTiming::counters=saved_counters;
  framebuffer_info={};shader_dirty=true;uniform_block_data.dirty=uniform_block_data.light_dirty=true;
  OpenGLState::InvalidateRasterState();saved_gl.Apply();
 });
 software_visibility_oracle_trace_sequence=0;software_visibility_first_gl_error=software_visibility_first_query_error=0;
 const auto require=[&](bool pass,const char* stage){if(!pass){const auto current=glGetError();throw std::runtime_error(std::string(stage)+" GL="+std::to_string(software_visibility_first_gl_error?software_visibility_first_gl_error:current)+" queryGL="+std::to_string(software_visibility_first_query_error));}};
 const auto write=[&](u32 id,u32 value,u32 mask){
  PrePicaRegisterWrite(id,value,mask);
  const u32 expanded=(mask&1?0xffu:0u)|(mask&2?0xff00u:0u)|(mask&4?0xff0000u:0u)|(mask&8?0xff000000u:0u);
  regs.reg_array[id]=(regs.reg_array[id]&~expanded)|(value&expanded);
  NotifyPicaRegisterChanged(id);
 };
 const auto configure=[&] {
  require(vertex_batch.empty()&&!software_framebuffer_visibility.pending,"reset with live vertices/visibility");
  TraceSoftwareBatchVisibilityFBO("configure_before_retire");
  SoftwareBatchVisibilityTestAccess::Retire(res_cache,interval);
  TraceSoftwareBatchVisibilityFBO("configure_after_retire");
  std::fill_n(raw,scratch_bytes,0);regs={};software_batch_planner={};software_vertices_since_request=software_completed_requests=0;
  software_has_deferred=software_batch_draw_safe=false;
  regs.framebuffer.framebuffer.color_buffer_address.Assign(scratch/8);
  regs.framebuffer.framebuffer.width.Assign(8);regs.framebuffer.framebuffer.height.Assign(7);
  regs.framebuffer.framebuffer.color_format.Assign(Pica::FramebufferRegs::ColorFormat::RGBA8);
  regs.framebuffer.framebuffer.allow_color_write.Assign(15);
  auto& output=regs.framebuffer.output_merger;
  output.red_enable.Assign(1);output.green_enable.Assign(1);output.blue_enable.Assign(1);output.alpha_enable.Assign(1);
  output.alphablend_enable.Assign(1);
  output.alpha_blending.factor_source_rgb.Assign(Pica::FramebufferRegs::BlendFactor::SourceAlpha);
  output.alpha_blending.factor_dest_rgb.Assign(Pica::FramebufferRegs::BlendFactor::OneMinusSourceAlpha);
  output.alpha_blending.factor_source_a.Assign(Pica::FramebufferRegs::BlendFactor::One);
  output.alpha_blending.factor_dest_a.Assign(Pica::FramebufferRegs::BlendFactor::OneMinusSourceAlpha);
  regs.rasterizer.viewport_size_x.Assign(0x410000u);
  regs.rasterizer.viewport_size_y.Assign(0x410000u);
  regs.rasterizer.viewport_depth_range.Assign(0x3f0000u);
  regs.lighting.disable.Assign(1);
  // Generated primary-color-only fragment pipeline. No guest shader program.
  using T=Pica::TexturingRegs::TevStageConfig;
  for(auto* stage:{&regs.texturing.tev_stage0,&regs.texturing.tev_stage1,&regs.texturing.tev_stage2,&regs.texturing.tev_stage3,&regs.texturing.tev_stage4,&regs.texturing.tev_stage5}) {
   stage->color_source1.Assign(T::Source::PrimaryColor);stage->alpha_source1.Assign(T::Source::PrimaryColor);
   stage->color_op.Assign(T::Operation::Replace);stage->alpha_op.Assign(T::Operation::Replace);
  }
  auto* commands=reinterpret_cast<u32*>(raw+(command_scratch-scratch));
  Pica::g_state.cmd_list.head_ptr=Pica::g_state.cmd_list.current_ptr=commands;Pica::g_state.cmd_list.length=2;
  Pica::g_state.cmd_list.addr=command_scratch;
  shader_dirty=true;SyncEntireState();BeginPicaCommandList();TraceSoftwareBatchVisibilityFBO("configure_complete");
 };
 using Color=std::array<float,4>;const Color red{1.f,0.f,0.25f,0.5f},green{0.f,1.f,0.25f,0.5f};
 const auto quad=[&](const Color& color,bool deferred,bool indexed=false,bool expect_first_input_rejection=false){
  PrePicaRegisterWrite(indexed?0x22f:0x22e,1,1); // actual resource guard, before synthetic owned outputs
  const auto vertex=[&](float x,float y){Pica::Shader::OutputVertex v{};
   v.pos={Pica::float24::FromFloat32(x),Pica::float24::FromFloat32(y),Pica::float24::FromFloat32(-.5f),Pica::float24::FromFloat32(1.f)};
   v.quat.w=Pica::float24::FromFloat32(1.f);
   v.color={Pica::float24::FromFloat32(color[0]),Pica::float24::FromFloat32(color[1]),Pica::float24::FromFloat32(color[2]),Pica::float24::FromFloat32(color[3])};return v;};
  auto a=vertex(-1,-1),b=vertex(1,-1),c=vertex(1,1),d=vertex(-1,1);
  AddTriangle(a,b,c);
  if(expect_first_input_rejection)
   require(!software_batch_draw_safe&&vertex_batch.size()==3&&!software_has_deferred&&!software_framebuffer_visibility.pending,
           "first SW triangle failed to reject unread input alias before any capacity flush");
  AddTriangle(a,c,d);
  TraceSoftwareBatchVisibilityFBO(deferred?"quad_before_deferred_request":"quad_before_immediate_draw");
  if(deferred)DrawTriangles();else {require(Draw(false,false),"forced immediate production Draw");software_vertices_since_request=0;}
  TraceSoftwareBatchVisibilityFBO(deferred?"quad_after_deferred_request":"quad_after_immediate_draw");
  require(!software_visibility_first_gl_error&&!software_visibility_first_query_error,"GL error at quad boundary");
 };
 const auto read=[&] {
  TraceSoftwareBatchVisibilityFBO("before_canonical_pixel_read");
  require(!software_visibility_first_gl_error&&!software_visibility_first_query_error,"GL error before pixel read");
  std::vector<u8> bytes(target_bytes);for(u32 i=0;i<target_bytes;++i)bytes[i]=memory.Read8(virtual_scratch+i);
  TraceSoftwareBatchVisibilityFBO("after_canonical_pixel_read");
  require(!software_visibility_first_gl_error&&!software_visibility_first_query_error,"GL error during canonical pixel read");return bytes;
 };
 const auto texture_read=[&] {
  Pica::Texture::TextureInfo info{};info.physical_address=scratch;info.width=info.height=8;
  info.format=Pica::TexturingRegs::TextureFormat::RGBA8;info.SetDefaultStride();
  const auto texture=res_cache.GetTextureSurface(info,0);require(bool(texture),"actual texture-alias lookup failed");
  texture->DownloadGLTexture(texture->GetRect());require(glGetError()==GL_NO_ERROR,"actual texture-alias GPU download failed");
  return texture->gl_buffer;
 };
 const auto live=[&] {return !vertex_batch.empty()&&software_framebuffer_visibility.pending;};
 const auto cleared=[&] {require(vertex_batch.empty()&&!software_framebuffer_visibility.pending,"barrier failed to retire live work");};
 try {
  // Independent immediate baseline and reversed-order witness reject black/no-op
  // fixtures and cases where the draw-order test would be vacuous.
  configure();quad(red,false);quad(green,false);EndPicaCommandList();const auto baseline=read();const auto baseline_texture=texture_read();
  const auto nonzero=std::count_if(baseline.begin(),baseline.end(),[](u8 b){return b!=0;});
  require(nonzero>64,"baseline produced no nonblack RGB coverage");
  configure();quad(green,false);quad(red,false);EndPicaCommandList();require(read()!=baseline,"blend order witness is vacuous");
  unsigned cases=0,partial_cases=0,alias_cases=0;
  configure();quad(red,false);EndPicaCommandList();const auto red_baseline=read();
  const auto start=[&] {configure();quad(red,true);require(vertex_batch.size()==6,"first draw was not deferred");
   write(0x111,1,1);require(live(),"111 did not create an owned obligation");
   write(0x111,1,2);cleared(); // non-trigger partial mask remains conservative
   EndPicaCommandList();
   configure();quad(red,true);write(0x111,1,1);quad(green,true);write(0x111,1,15);
   require(live()&&vertex_batch.size()==12,"two software requests did not coexist");};
  for(unsigned barrier=0;barrier<4;++barrier) {
   start();const auto before=stats.actual_flushes;
   if(barrier==0)EndPicaCommandList();
   if(barrier==1){FlushDeferredDraws();EndPicaCommandList();}
   if(barrier==2){write(0x10,0,15);EndPicaCommandList();} // real pre-IRQ fence; no guest IRQ injected
   if(barrier==3){write(0x103,0,15);EndPicaCommandList();}
   cleared();require(stats.actual_flushes>before,"barrier lacked actual Draw submission");
   require(read()==baseline,"deferred barrier pixels differ from immediate baseline");++cases;
  }
  // Defensive alias guards are probed with deliberate test-only state changes;
  // these are NOT claimed to be guest command sequences bypassing prior fences.
  for(unsigned alias=0;alias<3;++alias) {
   start();const auto before=stats.actual_flushes;
   if(alias==0){regs.texturing.main_config.texture0_enable.Assign(1);regs.texturing.texture0.width.Assign(8);regs.texturing.texture0.height.Assign(8);regs.texturing.texture0.address.Assign(scratch/8);}
   if(alias==1){regs.pipeline.vertex_attributes.base_address.Assign(scratch/16);regs.pipeline.vertex_attributes.attribute_loaders[0].component_count.Assign(1);regs.pipeline.vertex_attributes.attribute_loaders[0].byte_count.Assign(16);regs.pipeline.num_vertices=3;}
   if(alias==2){Pica::g_state.cmd_list.head_ptr=Pica::g_state.cmd_list.current_ptr=reinterpret_cast<u32*>(raw);}
   PrePicaRegisterWrite(0x22e,1,1);cleared();require(stats.actual_flushes>before,"alias guard lacked Draw");
   EndPicaCommandList();if(alias==0)require(texture_read()==baseline_texture,"actual texture-alias GPU bytes differ");
   require(read()==baseline,"alias guard lost queued GPU output");++alias_cases;
  }
  // First-request admission, with NO previously owned/deferred work. The target
  // is later in our own scratch region; initial inputs do not alias it. Only
  // the unread tail of this >4095-vertex request reaches the framebuffer.
  require(!CensusEnabled(),"first-request alias oracle requires actual lazy mode, census off");
  unsigned first_request_alias_cases=0;
  for(bool indexed:{false,true}) {
   configure();constexpr u32 target_offset=0x4000;
   const PAddr later_target=scratch+target_offset;
   regs.framebuffer.framebuffer.color_buffer_address.Assign(later_target/8);
   regs.pipeline.num_vertices=Capacity+3;
   if(indexed) {
    // 4098 ushort indices from +0x2000: only the final two reach +0x4000.
    regs.pipeline.vertex_attributes.base_address.Assign((scratch+0x2000)/16);
    using IndexFormat=decltype(regs.pipeline.index_array.format.Value());
    regs.pipeline.index_array.format.Assign(IndexFormat::SHORT);
   } else {
    // Four-byte vertex stride: vertex4096 first reaches +0x4000.
    regs.pipeline.vertex_attributes.base_address.Assign(scratch/16);
    regs.pipeline.vertex_attributes.attribute_loaders[0].component_count.Assign(1);
    regs.pipeline.vertex_attributes.attribute_loaders[0].byte_count.Assign(4);
   }
   require(vertex_batch.empty()&&!software_has_deferred&&!software_framebuffer_visibility.pending,
           "first-request alias case started with previous owned work");
   const auto eager=stats.pre_input_resource_checks,skips=stats.lazy_empty_input_skips;
   const auto first=stats.first_software_admission_checks,input_aliases=stats.unsafe_vertex_index;
   const auto actual_caps=stats.actual_flush_reasons[unsigned(Reason::SizeCap)];
   const auto predicted_caps=stats.predicted_flush_reasons[unsigned(Reason::SizeCap)];
   quad(red,true,indexed,true);
   require(stats.pre_input_resource_checks==eager&&stats.lazy_empty_input_skips==skips+1,
           "first-request input case did not exercise skipped empty prehook");
   require(stats.first_software_admission_checks==first+1&&stats.unsafe_vertex_index>input_aliases,
           "first-request SW admission did not inspect the full remaining vertex/index envelope");
   require(stats.actual_flush_reasons[unsigned(Reason::SizeCap)]==actual_caps&&
           stats.predicted_flush_reasons[unsigned(Reason::SizeCap)]==predicted_caps,
           "first-request input alias reached capacity retirement before rejection");
   require(!software_batch_draw_safe&&vertex_batch.empty()&&!software_framebuffer_visibility.pending,
           "first-request input alias unexpectedly retained a deferred draw");
   const auto deferred_triggers=stats.framebuffer_flush_deferred;write(0x111,1,1);
   require(stats.framebuffer_flush_deferred==deferred_triggers&&!software_framebuffer_visibility.pending,
           "rejected first-request alias admitted a framebuffer visibility obligation");
   EndPicaCommandList();
   std::vector<u8> actual(target_bytes);
   for(u32 i=0;i<target_bytes;++i)actual[i]=memory.Read8(virtual_scratch+target_offset+i);
   require(actual==red_baseline,"first-request alias rejection changed actual immediate GPU pixels");
   LOG_INFO(Render_OpenGL,"SW_VIS_FIRST_REQUEST_ALIAS indexed={} declared_vertices={} first_triangle_rejected=true eager_checks_delta={} lazy_skips_delta={} first_checks_delta={} unsafe_input_delta={} capacity_flush_delta=0 pixels_equal=true",indexed,regs.pipeline.num_vertices,stats.pre_input_resource_checks-eager,stats.lazy_empty_input_skips-skips,stats.first_software_admission_checks-first,stats.unsafe_vertex_index-input_aliases);
   ++first_request_alias_cases;
  }
  // Actual CPU PTE write path, BEFORE initial GPU readback. Entire 256-byte
  // oracle must survive except for the deliberately overwritten 1+4 bytes.
  for(bool retain:{false,true}) {
   start();EndPicaCommandList();cleared();
   require(SoftwareBatchVisibilityTestAccess::Owned(res_cache,target_interval),"Draw did not publish GPU dirty ownership");
   require(table->attributes[virtual_scratch>>Memory::PAGE_BITS]==Memory::PageType::RasterizerCachedMemory,"GPU owner did not mark actual CPU PTE");
   SoftwareBatchVisibilityTestAccess::Retain(res_cache,retain);
   auto expected=baseline;expected[3]=0x5a;const u32 patch=0x6c27a190u;std::memcpy(expected.data()+9,&patch,4);
   memory.Write8(virtual_scratch+3,0x5a);memory.Write32(virtual_scratch+9,patch);
   require(read()==expected,"partial CPU PTE write lost untouched GPU bytes");++partial_cases;
  }
  // Conservative target-vs-target guard: depth is configured to alias color,
  // but depth testing/writes stay disabled so the immediate color oracle is well-defined.
  configure();regs.framebuffer.framebuffer.depth_buffer_address.Assign(scratch/8);
  const auto overlap_deferred=stats.framebuffer_flush_deferred;
  quad(red,true);require(!software_batch_draw_safe&&vertex_batch.empty(),"overlapping targets admitted SW deferral");
  write(0x111,1,1);require(!software_framebuffer_visibility.pending&&stats.framebuffer_flush_deferred==overlap_deferred,"overlapping targets admitted111 obligation");
  EndPicaCommandList();require(read()==red_baseline,"overlap rejection changed immediate color draw");
  configure();Pica::g_debug_context=Pica::DebugContext::Construct();
  const auto deferred_before=stats.framebuffer_flush_deferred;quad(red,true);write(0x111,1,1);
  require(vertex_batch.empty()&&!software_framebuffer_visibility.pending&&stats.framebuffer_flush_deferred==deferred_before,"debug observer admitted deferral");
  Pica::g_debug_context.reset();EndPicaCommandList();
  require(stats.actual_merged_completed_requests>0&&stats.framebuffer_flush_materialized>0&&stats.framebuffer_flush_pending==0,"oracle did not exercise actual merging/materialization");
  require(glGetError()==GL_NO_ERROR,"oracle GL error");
  LOG_INFO(Render_OpenGL,"SW_BATCH_VISIBILITY_ORACLE passed=true barrier_cases={} alias_cases={} partial_cpu_cases={} target_overlap_cases=1 debug_cases=1 first_request_alias_cases={} nonzero_bytes={} actual_flushes={} merged_requests={} logical_triggers={} deferred_triggers={} materialized={} pending={} mapped_own_vram={}",cases,alias_cases,partial_cases,first_request_alias_cases,nonzero,stats.actual_flushes,stats.actual_merged_completed_requests,stats.framebuffer_flush_triggers,stats.framebuffer_flush_deferred,stats.framebuffer_flush_materialized,stats.framebuffer_flush_pending,mapped_here);
  error.clear();return true;
 } catch(const std::exception& e) {error=std::string("software batch visibility oracle: ")+e.what();return false;}
}
} // namespace OpenGL
