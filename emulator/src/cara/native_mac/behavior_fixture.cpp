// Copyright 2026 Cara contributors. Licensed under GPL-2.0-or-later.
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <nihstro/inline_assembly.h>
#include "cara/native_mac/behavior_fixture.h"
#include "common/frame_timing.h"
#include "core/arm/arm_interface.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/process.h"
#include "core/hw/gpu.h"
#include "core/hw/hw.h"
#include "core/loader/loader.h"
#include "core/memory.h"
#include "core/settings.h"
#include "video_core/command_processor.h"
#include "video_core/pica_state.h"
#include "video_core/video_core.h"
#include "video_core/renderer_opengl/gl_vars.h"
#include "video_core/renderer_opengl/gl_command_queue.h"
#include <glad/glad.h>
namespace Cara::NativeBehavior {
namespace {
std::array<u8,80> Bootstrap(){std::array<u8,80>b{};std::memcpy(b.data(),"3DSX",4);b[4]=44;b[6]=8;const auto w=[&](unsigned at,u32 x){for(unsigned i=0;i<4;i++)b[at+i]=u8(x>>(8*i));};w(16,4);w(20,4);w(24,4);w(68,0xeafffffe);w(72,0x11223344);w(76,0x55667788);return b;}
void Require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
struct CPUWitness {std::array<std::array<u32,81>,4> regs{};std::array<s64,4> ticks{};bool operator==(const CPUWitness&)const=default;};
CPUWitness Capture(Core::System& s){CPUWitness w;for(unsigned c=0;c<4;c++){auto& cpu=s.GetCore(c);for(unsigned r=0;r<16;r++)w.regs[c][r]=cpu.GetReg(r);for(unsigned r=0;r<64;r++)w.regs[c][16+r]=cpu.GetVFPReg(r);w.regs[c][80]=cpu.GetCPSR();w.ticks[c]=s.CoreTiming().GetTimer(c)->GetTicks();}return w;}
struct Packets {std::vector<u32> words;unsigned writes=0;void Write(u32 id,u32 value,u32 mask=15){Pica::CommandProcessor::CommandHeader h{};h.cmd_id.Assign(id);h.parameter_mask.Assign(mask);h.extra_data_length.Assign(0);h.group_commands.Assign(0);words.push_back(value);words.push_back(h.hex);++writes;}void Range(const Pica::Regs& r,u32 first,u32 count){for(u32 i=0;i<count;i++)Write(first+i,r.reg_array[first+i]);}};
unsigned PixelOffset(unsigned x,unsigned y){unsigned m=0;for(unsigned i=0;i<3;i++)m|=((x>>i)&1)<<(2*i)|((y>>i)&1)<<(2*i+1);return (((y/8)*4+x/8)*64+m)*4;}
}
bool ValidateBootstrap(const std::string& path,std::string& error){std::ifstream f(path,std::ios::binary);std::array<u8,81> bytes{};f.read(reinterpret_cast<char*>(bytes.data()),bytes.size());const auto expected=Bootstrap();if(f.gcount()!=80||!std::equal(expected.begin(),expected.end(),bytes.begin())){error="behavior fixture requires its exact authored 80-byte bootstrap";return false;}return true;}
Result RunOneDraw(Core::System& system,bool software,bool indexed){Result result;std::vector<std::string> milestones;u64 pica_lists=0,pica_draws=0,gpu_draws=0,cpu_draws=0,triangle_entries=0,shader_calls=0;unsigned packet_writes=0;bool cpu_unchanged=false,cpu_checked=false,cached_page=false,cached_checked=false,coherence=false,coherence_checked=false;GLenum error=GL_NO_ERROR;
 const bool original_hw_setting=Settings::values.use_hw_shader,original_hw_flag=VideoCore::g_hw_shader_enabled.load();
 try{
  Require(system.GetAppLoader().GetFileType()==Loader::FileType::THREEDSX,"3DSX loader was not selected");
  Require(!OpenGL::GLES&&VideoCore::g_hw_shader_enabled,"requires default native hardware shader path");
  Require(!Settings::values.skip_cpu_write&&!Settings::values.skip_slow_draw,"default coherence/draw settings required");
  Require(!indexed||software,"indexed fixture is software-only");
  if(software) {
   Require(!OpenGL::CommandQueue::Enabled(),"software fixture requires actual queue OFF");
   Require(!Settings::values.use_shader_jit,"software fixture needs interpreter shader-call witness");
  }
  struct RestoreShaderMode {
   bool active=false,setting=false,hardware=false;
   ~RestoreShaderMode(){if(active){Settings::values.use_hw_shader=setting;VideoCore::g_hw_shader_enabled.store(hardware);}}
  } restore_shader_mode;
  if(software) {
   restore_shader_mode.active=true;restore_shader_mode.setting=Settings::values.use_hw_shader;restore_shader_mode.hardware=VideoCore::g_hw_shader_enabled.load();
   Settings::values.use_hw_shader=false;VideoCore::g_hw_shader_enabled.store(false);
  }
  auto process=system.Kernel().GetCurrentProcess();Require(bool(process),"bootstrap process absent");milestones.push_back("Core::Load completed; no ARM RunLoop invoked");
  const auto before=Capture(system);auto& memory=system.Memory();
  constexpr PAddr target=Memory::VRAM_PADDR+0x10000,vertices=Memory::VRAM_PADDR+0x12000,list=Memory::VRAM_PADDR+0x13000;
  constexpr VAddr target_va=Memory::VRAM_VADDR+0x10000,vertices_va=Memory::VRAM_VADDR+0x12000,list_va=Memory::VRAM_VADDR+0x13000;
  Require(memory.GetPointer(target_va)==memory.GetPhysicalPointer(target),"bootstrap VRAM mapping is not canonical");
  std::array<u8,4096> initial;initial.fill(0x2d);memory.WriteBlock(*process,target_va,initial.data(),initial.size());
  struct Vertex{std::array<float,4> position,color;};const std::array<Vertex,3> data{{{{-.75f,-.75f,-.5f,1.f},{1.f,.25f,.75f,.875f}},{{.75f,-.75f,-.5f,1.f},{1.f,.25f,.75f,.875f}},{{0,.75f,-.5f,1.f},{1.f,.25f,.75f,.875f}}}};static_assert(sizeof(Vertex)==32);memory.WriteBlock(*process,vertices_va,data.data(),sizeof data);
  if(indexed) {
   // ID3 duplicates ID2's authored position/color, but remains a distinct VS cache key.
   memory.WriteBlock(*process,vertices_va+sizeof data,&data[2],sizeof(Vertex));
   const std::array<u8,9> indices{0,1,2,0,1,2,0,1,3};
   memory.WriteBlock(*process,vertices_va+0x800,indices.data(),indices.size());
  }
  milestones.push_back("authored target and vertex bytes written via Memory::WriteBlock");
  Pica::Regs r{};r.framebuffer.framebuffer.color_buffer_address.Assign(target/8);r.framebuffer.framebuffer.width.Assign(32);r.framebuffer.framebuffer.height.Assign(31);r.framebuffer.framebuffer.color_format.Assign(Pica::FramebufferRegs::ColorFormat::RGBA8);r.framebuffer.framebuffer.allow_color_write.Assign(15);
  auto& o=r.framebuffer.output_merger;o.red_enable.Assign(1);o.green_enable.Assign(1);o.blue_enable.Assign(1);o.alpha_enable.Assign(1);o.logic_op.Assign(Pica::FramebufferRegs::LogicOp::Copy);
  r.rasterizer.viewport_size_x.Assign(0x430000);r.rasterizer.viewport_size_y.Assign(0x430000);r.rasterizer.viewport_depth_range.Assign(0x3f0000);r.lighting.disable.Assign(1);
  using T=Pica::TexturingRegs::TevStageConfig;for(auto* t:{&r.texturing.tev_stage0,&r.texturing.tev_stage1,&r.texturing.tev_stage2,&r.texturing.tev_stage3,&r.texturing.tev_stage4,&r.texturing.tev_stage5}){t->color_source1.Assign(T::Source::PrimaryColor);t->alpha_source1.Assign(T::Source::PrimaryColor);t->color_op.Assign(T::Operation::Replace);t->alpha_op.Assign(T::Operation::Replace);}
  auto& a=r.pipeline.vertex_attributes;a.base_address.Assign(vertices/16);a.format0.Assign(Pica::PipelineRegs::VertexAttributeFormat::FLOAT);a.size0.Assign(3);a.format1.Assign(Pica::PipelineRegs::VertexAttributeFormat::FLOAT);a.size1.Assign(3);a.max_attribute_index.Assign(1);a.attribute_loaders[0].comp0.Assign(0);a.attribute_loaders[0].comp1.Assign(1);a.attribute_loaders[0].byte_count.Assign(32);a.attribute_loaders[0].component_count.Assign(2);
  r.pipeline.num_vertices=indexed?9:3;r.pipeline.use_gs.Assign(Pica::PipelineRegs::UseGS::No);r.pipeline.triangle_topology.Assign(Pica::PipelineRegs::TriangleTopology::List);r.vs.max_input_attribute_index.Assign(1);r.vs.shader_mode.Assign(Pica::ShaderRegs::VS);r.vs.input_attribute_to_register_map_low=0x10;r.vs.output_mask.Assign(3);r.rasterizer.vs_output_total.Assign(2);
  using S=Pica::RasterizerRegs::VSOutputAttributes;const std::array<std::array<S::Semantic,4>,2> map{{{S::POSITION_X,S::POSITION_Y,S::POSITION_Z,S::POSITION_W},{S::COLOR_R,S::COLOR_G,S::COLOR_B,S::COLOR_A}}};for(unsigned i=0;i<2;i++){auto& v=r.rasterizer.vs_output_attributes[i];v.map_x.Assign(map[i][0]);v.map_y.Assign(map[i][1]);v.map_z.Assign(map[i][2]);v.map_w.Assign(map[i][3]);}
  if(indexed){r.pipeline.index_array.offset.Assign(0x800);r.pipeline.index_array.format.Assign(decltype(r.pipeline.index_array)::BYTE);}
  Packets packets;
#define EMIT(field) packets.Write(PICA_REG_INDEX(field),r.reg_array[PICA_REG_INDEX(field)])
  EMIT(framebuffer.framebuffer.color_buffer_address);EMIT(framebuffer.framebuffer.width);EMIT(framebuffer.framebuffer.color_format);EMIT(framebuffer.framebuffer.allow_color_write);EMIT(framebuffer.output_merger.red_enable);EMIT(framebuffer.output_merger.alphablend_enable);EMIT(framebuffer.output_merger.logic_op);EMIT(rasterizer.viewport_size_x);EMIT(rasterizer.viewport_size_y);EMIT(rasterizer.viewport_depth_range);EMIT(rasterizer.viewport_depth_near_plane);EMIT(rasterizer.viewport_corner);EMIT(rasterizer.vs_output_total);EMIT(lighting.disable);
  packets.Range(r,PICA_REG_INDEX(rasterizer.vs_output_attributes[0]),2);for(u32 first:{PICA_REG_INDEX(texturing.tev_stage0),PICA_REG_INDEX(texturing.tev_stage1),PICA_REG_INDEX(texturing.tev_stage2),PICA_REG_INDEX(texturing.tev_stage3),PICA_REG_INDEX(texturing.tev_stage4),PICA_REG_INDEX(texturing.tev_stage5)})packets.Range(r,first,5);
  if(indexed)packets.Write(PICA_REG_INDEX(pipeline.index_array),r.reg_array[PICA_REG_INDEX(pipeline.index_array)]);
  packets.Range(r,PICA_REG_INDEX(pipeline.vertex_attributes),sizeof(r.pipeline.vertex_attributes)/4);EMIT(pipeline.use_gs);EMIT(pipeline.num_vertices);EMIT(pipeline.vertex_offset);EMIT(pipeline.triangle_topology);packets.Write(PICA_REG_INDEX(pipeline.restart_primitive),1);EMIT(vs.max_input_attribute_index);EMIT(vs.main_offset);EMIT(vs.input_attribute_to_register_map_low);EMIT(vs.input_attribute_to_register_map_high);EMIT(vs.output_mask);
#undef EMIT
  using O=nihstro::OpCode;using D=nihstro::DestRegister;using I=nihstro::SourceRegister;const auto shader=nihstro::InlineAsm::CompileToRawBinary({{O::Id::MOV,D::MakeOutput(0),I::MakeInput(0)},{O::Id::MOV,D::MakeOutput(1),I::MakeInput(1)},{O::Id::END}});
  packets.Write(PICA_REG_INDEX(vs.program.offset),0);for(const auto x:shader.program)packets.Write(PICA_REG_INDEX(vs.program.set_word[0]),x.hex);packets.Write(PICA_REG_INDEX(vs.swizzle_patterns.offset),0);for(const auto x:shader.swizzle_table)packets.Write(PICA_REG_INDEX(vs.swizzle_patterns.set_word[0]),x.hex);packets.Write(indexed?PICA_REG_INDEX(pipeline.trigger_draw_indexed):PICA_REG_INDEX(pipeline.trigger_draw),1);
  packet_writes=packets.writes;Require(packets.words.size()*4<=4096,"authored list exceeds private page");memory.WriteBlock(*process,list_va,packets.words.data(),packets.words.size()*4);milestones.push_back("original PICA MOV program and masked register packets stored");
  if(software)Require(!Settings::values.use_hw_shader&&!VideoCore::g_hw_shader_enabled.load(),"software mode inactive at real draw submission");
  auto& c=Common::FrameTiming::counters;
  struct RestoreCounterEnabled {
   bool& enabled;const bool saved;
   void Restore(){enabled=saved;}
   ~RestoreCounterEnabled(){Restore();}
  } restore_counter_enabled{c.enabled,c.enabled};
  c.enabled=true;const auto old=c;
  GPU::Write<u32>(HW::VADDR_GPU+GPU_REG_INDEX(command_processor_config.address)*4,list>>3);GPU::Write<u32>(HW::VADDR_GPU+GPU_REG_INDEX(command_processor_config.size)*4,u32(packets.words.size()*4));GPU::Write<u32>(HW::VADDR_GPU+GPU_REG_INDEX(command_processor_config.trigger)*4,1);
  pica_lists=c.pica_command_lists-old.pica_command_lists;pica_draws=c.pica_draw_requests-old.pica_draw_requests;gpu_draws=c.gpu_vertex_draws-old.gpu_vertex_draws;cpu_draws=c.cpu_vertex_draws-old.cpu_vertex_draws;triangle_entries=c.gpu_vertices-old.gpu_vertices;shader_calls=c.shader_calls-old.shader_calls;
  // Counter witness ends before the separate CPU/coherence transaction.
  restore_counter_enabled.Restore();
  if(software) {
   Require(pica_lists==1&&pica_draws==1&&gpu_draws==0&&cpu_draws==1&&triangle_entries==0,"expected one actual software PICA request and no accelerated draw");
   Require(shader_calls==(indexed?4u:3u),"software VS calls do not prove expected cache hit/miss pattern");
   milestones.push_back(indexed?"actual indexed software draw: nine indices, four VS misses and five cached hits":"actual unindexed software draw: three interpreter VS invocations");
  } else {
  Require(pica_lists==1&&pica_draws==1&&gpu_draws==1&&cpu_draws==0&&triangle_entries==3,"expected exactly one default accelerated triangle");milestones.push_back("GPU::Write triggered real ProcessCommandList and one accelerated draw");
  }
  cached_checked=true;cached_page=memory.GetCurrentPageTable()->attributes[target_va>>Memory::PAGE_BITS]==Memory::PageType::RasterizerCachedMemory;Require(cached_page,"draw target page did not become rasterizer-cached");
  // Poison ONLY the two known covered interior texels; untouched RAM remains initial.
  // Raw poisoning is an oracle witness, not a simulated guest CPU write/invalidation.
  const auto patch_offset=PixelOffset(16,16),neighbor=PixelOffset(15,16);
  auto* backing=memory.GetPhysicalPointer(target);
  std::fill_n(backing+patch_offset,4,0xa5);std::fill_n(backing+neighbor,4,0xa5);
  const std::array<u8,4> patch{7,19,31,43};memory.WriteBlock(*process,target_va+patch_offset,patch.data(),patch.size());std::array<u8,4096> read{};memory.ReadBlock(*process,target_va,read.data(),read.size());
  const std::array<u8,4> expected_gpu{223,191,64,255};coherence_checked=true;coherence=std::equal(patch.begin(),patch.end(),read.begin()+patch_offset)&&std::equal(expected_gpu.begin(),expected_gpu.end(),read.begin()+neighbor)&&std::all_of(read.begin(),read.begin()+4,[](u8 x){return x==0x2d;});Require(coherence,"CPU partial write/read did not preserve GPU neighbor and original outside pixel");milestones.push_back("real cached CPU partial-write/read coherence preserved three witnesses");
  cpu_checked=true;cpu_unchanged=Capture(system)==before;Require(cpu_unchanged,"ARM/VFP registers or core ticks advanced despite no ARM RunLoop");milestones.push_back("ARM/VFP/ticks unchanged");result.passed=true;
 }catch(const std::exception& e){result.error=e.what();}
 const bool shader_mode_restored=Settings::values.use_hw_shader==original_hw_setting&&VideoCore::g_hw_shader_enabled.load()==original_hw_flag;
 if(!shader_mode_restored){result.passed=false;result.error+="; shader mode did not restore";}
 error=glGetError();if(error!=GL_NO_ERROR){result.passed=false;result.error+="; GL error at fixture boundary";}else milestones.push_back("GL boundary checked clean");
 std::ostringstream out;out<<"{\"label\":\"single synthetic host batch; not guest refresh\",\"guest_ARM_Run_calls_by_fixture\":0,\"GSP_queue_or_waits_exercised\":false,\"virtual_time_advance_calls_by_fixture\":0,\"milestones\":[";for(unsigned i=0;i<milestones.size();i++){if(i)out<<',';out<<'"'<<milestones[i]<<'"';}out<<"],\"software_forced\":"<<(software?"true":"false")<<",\"indexed\":"<<(indexed?"true":"false")<<",\"shader_mode_restored\":"<<(shader_mode_restored?"true":"false")<<",\"input_index_count\":"<<(indexed?9:0)<<",\"software_shader_calls\":"<<shader_calls<<",\"expected_cache_misses\":"<<(software?(indexed?4:3):0)<<",\"expected_cache_hits\":"<<(software&&indexed?5:0)<<",\"PICA_register_packets\":"<<packet_writes<<",\"PICA_lists\":"<<pica_lists<<",\"PICA_draw_requests\":"<<pica_draws<<",\"accelerated_draws\":"<<gpu_draws<<",\"software_draws\":"<<cpu_draws<<",\"triangle_order_entries\":"<<triangle_entries<<",\"rasterizer_cached_page\":"<<(cached_checked?(cached_page?"true":"false"):"null")<<",\"CPU_write_read_coherence\":"<<(coherence_checked?(coherence?"true":"false"):"null")<<",\"ARM_VFP_ticks_unchanged\":"<<(cpu_checked?(cpu_unchanged?"true":"false"):"null")<<",\"GL_error\":"<<error<<'}';result.detail_json=out.str();return result;
}
} // namespace Cara::NativeBehavior
