// SPDX-License-Identifier: GPL-2.0-or-later
#include "gl_pipeline_capture.h"
#include <algorithm>
#include <atomic>
#include <sys/stat.h>
#include <sys/mman.h>
#include <new>
#include <stdexcept>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <fcntl.h>
#include <unistd.h>
namespace OpenGL::PipelineCapture {
bool Requested() noexcept {const char* p=std::getenv("CARA_GL_PIPELINE_CAPTURE");return p&&std::strcmp(p,"1")==0;}
struct Recorder::Impl {
 struct Shader {std::uint64_t generation=0,revision=0;unsigned type=0;bool deleted=false;};
 struct Program {std::uint64_t generation=0,link=0;std::set<unsigned> attached;};
 std::filesystem::path directory;std::ofstream events;Journal* journal=nullptr;std::uint64_t publication=0;
 std::map<unsigned,Shader> shaders;std::map<unsigned,Program> programs;
 std::size_t source_bytes=0,event_bytes=0,event_count=0,shader_creates=0,program_creates=0,source_files=0;
 std::uint64_t generation=0,link=0,state=0,sequence=0;
 unsigned current=0,vao=0,framebuffer=0;bool failed=false,truncated=false;
 DrawRecord draw{};
 explicit Impl(const std::string& path) {
  directory=path;
  if(path.empty()||!directory.is_absolute()||!std::filesystem::create_directory(directory))
   throw std::runtime_error("capture requires a fresh absolute directory");
  ::chmod(path.c_str(),0700);
  events.open(directory/"events.jsonl",std::ios::out|std::ios::binary);
  if(!events)throw std::runtime_error("capture events unavailable");
  Event("{\"event\":\"begin\",\"schema\":1,\"pid\":"+std::to_string(getpid())+",\"binding_scope\":\"executed_API_requests_no_new_GL_queries\",\"state_scope\":\"program_VAO_drawFBO_only_not_complete_pipeline\"}");
  const int fd=::open((directory/"last-draw.bin").c_str(),O_CREAT|O_EXCL|O_RDWR,0600);
  if(fd<0)throw std::runtime_error("capture journal unavailable");
  if(ftruncate(fd,sizeof(Journal))!=0){::close(fd);throw std::runtime_error("capture journal sizing failed");}
  void* memory=mmap(nullptr,sizeof(Journal),PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
  ::close(fd);
  if(memory==MAP_FAILED)throw std::runtime_error("capture journal mapping failed");
  journal=new(memory) Journal{};std::memcpy(journal->magic,"CARAPIP1",8);
  Status();
 }
 ~Impl(){if(journal){msync(journal,sizeof(Journal),MS_SYNC);munmap(journal,sizeof(Journal));}}
 void Status() noexcept {try{std::ofstream out(directory/"status.json",std::ios::trunc);out<<"{\"schema\":1,\"failed\":"<<(failed?"true":"false")<<",\"truncated\":"<<(truncated?"true":"false")<<",\"source_bytes\":"<<source_bytes<<",\"event_count\":"<<event_count<<",\"events_bytes\":"<<event_bytes<<",\"shader_creates\":"<<shader_creates<<",\"program_creates\":"<<program_creates<<",\"draw_sequence\":"<<sequence<<",\"last_draw_record_bytes\":"<<sizeof(DrawRecord)<<"}\n";}catch(...){}}
 void Fail() noexcept {if(!failed){failed=true;Status();}}
 void Truncate() noexcept {if(!truncated){truncated=true;Status();}}
 void Event(const std::string& row) {
  if(failed)return;
  if(event_count>=MaxEvents||row.size()+1>MaxEventBytes-event_bytes){Truncate();return;}
  events<<row<<'\n';events.flush();if(!events){Fail();return;}++event_count;event_bytes+=row.size()+1;
 }
 std::string ShaderFields(unsigned id) {auto it=shaders.find(id);if(it==shaders.end())return "\"shader\":"+std::to_string(id)+",\"unknown\":true";auto& s=it->second;return "\"shader\":"+std::to_string(id)+",\"generation\":"+std::to_string(s.generation)+",\"revision\":"+std::to_string(s.revision);}
 void WriteDraw(){if(failed||!journal)return;const auto n=++publication;journal->slots[n&1]=draw;std::atomic_ref<std::uint64_t>(journal->committed).store(n,std::memory_order_release);}
};
#define CAPTURE_BODY(...) if(!impl||impl->failed)return;try{auto& x=*impl; __VA_ARGS__ }catch(...){impl->Fail();}
Recorder::Recorder(const std::string& path) noexcept {try{impl=std::make_unique<Impl>(path);}catch(const std::exception& e){std::fprintf(stderr,"CARA pipeline capture unavailable: %s\n",e.what());}catch(...){std::fprintf(stderr,"CARA pipeline capture unavailable\n");}}
Recorder::~Recorder(){if(impl){try{impl->Event("{\"event\":\"end\"}");}catch(...){impl->Fail();}impl->Status();}}
void Recorder::CreateShader(unsigned id,unsigned type) noexcept {CAPTURE_BODY(
 if(!id)return;if(x.shader_creates>=MaxShaders){x.Truncate();return;}++x.shader_creates;
 x.shaders[id]={++x.generation,0,type,false};x.Event("{\"event\":\"create_shader\","+x.ShaderFields(id)+",\"type\":"+std::to_string(type)+"}");
)}
void Recorder::Source(unsigned id,int count,const char* const* strings,const int* lengths) noexcept {CAPTURE_BODY(
 if(x.source_files>=MaxSources||x.truncated){x.Truncate();return;}auto it=x.shaders.find(id);if(it==x.shaders.end()||count<0||count>1024||(count&&!strings)){x.Truncate();return;}
 std::string source;std::string parts="[";
 for(int i=0;i<count;++i){if(!strings[i]){x.Truncate();return;}std::size_t n=lengths&&lengths[i]>=0?std::size_t(lengths[i]):strnlen(strings[i],MaxSource+1);
  if(n>MaxSource-source.size()){x.Truncate();return;}source.append(strings[i],n);if(i)parts+=',';parts+=std::to_string(n);}
 parts+=']';if(source.size()>MaxSourceBytes-x.source_bytes){x.Truncate();return;}
 auto& s=it->second;++s.revision;const std::string filename="shader-"+std::to_string(s.generation)+"-"+std::to_string(s.revision)+".glsl";
 std::ofstream out(x.directory/filename,std::ios::binary);out.write(source.data(),source.size());out.close();if(!out){x.Fail();return;}
 ++x.source_files;x.source_bytes+=source.size();x.Event("{\"event\":\"shader_source\","+x.ShaderFields(id)+",\"bytes\":"+std::to_string(source.size())+",\"parts\":"+parts+",\"file\":\""+filename+"\"}");x.Status();
)}
void Recorder::Compile(unsigned id) noexcept {CAPTURE_BODY(x.Event("{\"event\":\"compile_before\","+x.ShaderFields(id)+"}");)}
void Recorder::ShaderStatus(unsigned id,int value) noexcept {CAPTURE_BODY(x.Event("{\"event\":\"existing_compile_status\","+x.ShaderFields(id)+",\"value\":"+std::to_string(value)+"}");)}
void Recorder::DeleteShader(unsigned id) noexcept {CAPTURE_BODY(auto it=x.shaders.find(id);if(it!=x.shaders.end())it->second.deleted=true;x.Event("{\"event\":\"delete_shader_request\","+x.ShaderFields(id)+"}");)}
void Recorder::CreateProgram(unsigned id) noexcept {CAPTURE_BODY(if(!id)return;if(x.program_creates>=MaxPrograms){x.Truncate();return;}++x.program_creates;x.programs[id]={++x.generation,0,{}};x.Event("{\"event\":\"create_program\",\"program\":"+std::to_string(id)+",\"generation\":"+std::to_string(x.programs[id].generation)+"}");)}
void Recorder::Attach(unsigned p,unsigned s,bool attach) noexcept {CAPTURE_BODY(auto it=x.programs.find(p);if(it==x.programs.end()){x.Truncate();return;}if(attach){if(!x.shaders.count(s)||it->second.attached.size()>=16){x.Truncate();return;}it->second.attached.insert(s);}else it->second.attached.erase(s);x.Event("{\"event\":\""+std::string(attach?"attach_request":"detach_request")+"\",\"program\":"+std::to_string(p)+",\"shader\":"+std::to_string(s)+"}");)}
void Recorder::Link(unsigned p) noexcept {CAPTURE_BODY(auto it=x.programs.find(p);if(it==x.programs.end()){x.Truncate();return;}auto& a=it->second;a.link=++x.link;std::string list="[";bool comma=false;for(unsigned s:a.attached){if(comma)list+=',';comma=true;list+="{"+x.ShaderFields(s)+"}";}list+=']';x.Event("{\"event\":\"link_before\",\"program\":"+std::to_string(p)+",\"generation\":"+std::to_string(a.generation)+",\"link_serial\":"+std::to_string(a.link)+",\"shaders\":"+list+"}");)}
void Recorder::ProgramStatus(unsigned p,int value) noexcept {CAPTURE_BODY(auto it=x.programs.find(p);x.Event("{\"event\":\"existing_link_status\",\"program\":"+std::to_string(p)+",\"link_serial\":"+std::to_string(it==x.programs.end()?0:it->second.link)+",\"value\":"+std::to_string(value)+"}");)}
void Recorder::DeleteProgram(unsigned p) noexcept {CAPTURE_BODY(x.Event("{\"event\":\"delete_program_request\",\"program\":"+std::to_string(p)+"}");)}
void Recorder::Use(unsigned p) noexcept {CAPTURE_BODY(x.current=p;++x.state;)}
void Recorder::BindVAO(unsigned p) noexcept {CAPTURE_BODY(x.vao=p;++x.state;)}
void Recorder::BindFramebuffer(unsigned target,unsigned p) noexcept {CAPTURE_BODY(if(target==0x8D40||target==0x8CA9){x.framebuffer=p;++x.state;})}
void Recorder::Draw(unsigned api,unsigned mode,int first,int count,unsigned type,std::uint64_t offset,unsigned start,unsigned end,int base) noexcept {CAPTURE_BODY(
 x.draw={};std::memcpy(x.draw.magic,"CARADRW1",8);x.draw.sequence=++x.sequence;x.draw.state_serial=x.state;
 x.draw.phase=1;x.draw.program=x.current;x.draw.vao=x.vao;x.draw.draw_framebuffer=x.framebuffer;x.draw.api=api;x.draw.mode=mode;x.draw.first=first;x.draw.count=count;x.draw.base_vertex=base;x.draw.index_type=type;x.draw.index_offset=offset;x.draw.range_start=start;x.draw.range_end=end;
 auto p=x.programs.find(x.current);if(p!=x.programs.end()){x.draw.program_generation=p->second.generation;x.draw.link_serial=p->second.link;}else x.draw.flags|=1;
 if(x.failed)x.draw.flags|=2;if(x.truncated)x.draw.flags|=4;x.WriteDraw();
)}
void Recorder::DrawReturned() noexcept {CAPTURE_BODY(x.draw.phase=2;x.WriteDraw();)}
#undef CAPTURE_BODY
}
