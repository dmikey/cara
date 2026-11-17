#!/usr/bin/env python3
"""Generate wrappers for every GLAD entry referenced by native/video render sources.
Run again after integration; --check refuses uncovered/new APIs. No GL runtime.
"""
from pathlib import Path
import argparse,re,json,hashlib
ap=argparse.ArgumentParser();ap.add_argument('--source',type=Path,default=Path(__file__).resolve().parents[1]);ap.add_argument('--output',type=Path,default=Path(__file__).resolve().parents[1]/'emulator/src/video_core/renderer_opengl');ap.add_argument('--check',action='store_true');a=ap.parse_args()
glad=a.source/'emulator/externals/glad/include/glad/glad.h';header=glad.read_text()
types={m[1]:(m[0].strip(),m[2].strip()) for m in re.findall(r'typedef\s+(.+?)\s*\(APIENTRYP\s+(PFNGL\w+PROC)\)\s*\((.*?)\);',header)}
entries={name:(ty,*types[ty]) for ty,name in re.findall(r'GLAPI\s+(PFNGL\w+PROC)\s+glad_(gl\w+)\s*;',header) if ty in types}
roots=[a.source/'emulator/src/video_core',a.source/'emulator/src/cara/native_mac']
files=[p for root in roots for p in root.rglob('*') if p.suffix in {'.h','.cpp','.mm','.c'} and 'gl_command_queue' not in p.name]
# Include references through function-pointer assignment, not only direct calls.
references={n for p in files for n in re.findall(r'\b(gl[A-Z]\w*)\b',p.read_text(errors='replace')) if n in entries}
used=references|{'glBufferSubData','glGetIntegerv','glGetVertexAttribiv','glGetPointerv','glGetPointervKHR','glDebugMessageCallback','glDebugMessageCallbackARB','glDebugMessageCallbackKHR','glDebugMessageCallbackAMD'}
required_calls={n for p in files for n in re.findall(r'\b(gl[A-Z]\w*)\s*\(',p.read_text(errors='replace'))}
missing=sorted(required_calls-set(entries));assert not missing,('Direct non-GLAD GL calls require explicit integration:',missing)
manifest_path=a.output/'gl_command_queue_coverage.json'
custom={}
def define(name,body):custom[name]=body
# State mutations are by-owned_value and tracked on the sole producer. All GL calls
# in queued closures use raw entrypoints, so the worker never reads these maps.
def state_bind(name,update):
 define(name,update+'\n    auto fn=raw_'+name+';Record([=]{fn('+', '.join(argnames(entries[name][2]))+');});')
def argnames(args):
 if args=='void' or not args:return []
 result=[]
 for p in args.split(','):
  m=re.search(r'([A-Za-z_]\w*)\s*(?:\[[^]]*\])?$',p.strip());assert m,p;result.append(m[1])
 return result
define('glGenBuffers','''    auto fn=raw_glGenBuffers;Sync([=]{fn(n,buffers);},true);
    if(n>0 && buffers)for(GLsizei i=0;i<n;++i)if(buffers[i])known_buffers.insert(buffers[i]);''')
define('glGenVertexArrays','''    auto fn=raw_glGenVertexArrays;Sync([=]{fn(n,arrays);},true);
    if(n>0 && arrays)for(GLsizei i=0;i<n;++i)if(arrays[i]){known_vaos.insert(arrays[i]);vaos.try_emplace(arrays[i]);}''')
define('glBindBuffer','''    auto fn=raw_glBindBuffer;
    GLenum binding=target==GL_ELEMENT_ARRAY_BUFFER?GL_ELEMENT_ARRAY_BUFFER_BINDING:
        target==GL_ARRAY_BUFFER?GL_ARRAY_BUFFER_BINDING:target==GL_PIXEL_UNPACK_BUFFER?GL_PIXEL_UNPACK_BUFFER_BINDING:0;
    GLuint actual=buffer;
    if(binding && ((buffer && !known_buffers.count(buffer)) || (target==GL_ELEMENT_ARRAY_BUFFER && !current_vao) || transform_feedback_active)) {
      actual=Sync([=]{fn(target,buffer);GLint b=0;raw_glGetIntegerv(binding,&b);return GLuint(b);});
    } else {Record([=]{fn(target,buffer);});}
    if(target==GL_ELEMENT_ARRAY_BUFFER)vaos[current_vao].element=actual;
    else if(target==GL_ARRAY_BUFFER)array_buffer=actual;
    else if(target==GL_PIXEL_UNPACK_BUFFER)unpack_buffer=actual;''')
define('glBindVertexArray','''    auto fn=raw_glBindVertexArray;
    if((array && !known_vaos.count(array)) || transform_feedback_active) {
      current_vao=Sync([=]{fn(array);GLint b=0;raw_glGetIntegerv(GL_VERTEX_ARRAY_BINDING,&b);return GLuint(b);});
      // Query this VAO's EBO after any rejected/unknown bind, without clearing GL errors.
      vaos[current_vao].element=Sync([]{GLint b=0;raw_glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING,&b);return GLuint(b);});
    } else {current_vao=array;Record([=]{fn(array);});}
    vaos.try_emplace(current_vao);''')
state_bind('glBeginTransformFeedback','    transform_feedback_active=true;')
state_bind('glEndTransformFeedback','    transform_feedback_active=false;')
state_bind('glEnableVertexAttribArray','    if(index<GLuint(max_attributes))vaos[current_vao].attributes[index].enabled=true;')
state_bind('glDisableVertexAttribArray','    if(index<GLuint(max_attributes))vaos[current_vao].attributes[index].enabled=false;')
define('glVertexAttribPointer','''    auto fn=raw_glVertexAttribPointer;
    const bool plain_type=type==GL_BYTE||type==GL_UNSIGNED_BYTE||type==GL_SHORT||type==GL_UNSIGNED_SHORT||
        type==GL_INT||type==GL_UNSIGNED_INT||type==GL_FLOAT||type==GL_HALF_FLOAT||type==GL_DOUBLE;
    if(index>=GLuint(max_attributes)||!current_vao||!array_buffer||size<1||size>4||stride<0||!plain_type) {
      Sync([=]{fn(index,size,type,normalized,stride,pointer);},true);
      if(index<GLuint(max_attributes)) {
        const auto binding=Sync([=]{GLint b=0;raw_glGetVertexAttribiv(index,GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING,&b);return b;});
        vaos[current_vao].attributes[index].client=binding==0;
      }
      return;
    }
    vaos[current_vao].attributes[index].client=false;
    Record([=]{fn(index,size,type,normalized,stride,pointer);});''')
state_bind('glPixelStorei','''    if(pname==GL_UNPACK_ALIGNMENT && (param==1||param==2||param==4||param==8))unpack.alignment=param;
    else if(pname==GL_UNPACK_ROW_LENGTH && param>=0)unpack.row_length=param;
    else if(pname==GL_UNPACK_SKIP_ROWS && param>=0)unpack.skip_rows=param;
    else if(pname==GL_UNPACK_SKIP_PIXELS && param>=0)unpack.skip_pixels=param;''')
for name in ['glDrawArrays','glDrawElements','glDrawRangeElementsBaseVertex']:
 names=argnames(entries[name][2]);args=', '.join(names);client='ClientArrays()'+(' || vaos[current_vao].element==0' if name!='glDrawArrays' else '')
 define(name,f'''    auto fn=raw_{name};
    if({client}){{++stats.client_array_rpcs;Sync([=]{{fn({args});}},true);return;}}
    Record([=]{{fn({args});}});''')
for name in ['glFinish','glFlush','glFlushMappedBufferRange']:
 args=', '.join(argnames(entries[name][2]));define(name,f'    auto fn=raw_{name};Sync([=]{{fn({args});}});')
for name in ['glBufferData','glBufferSubData','glProgramBinary']:
 names=argnames(entries[name][2]);ptr='binary' if name=='glProgramBinary' else 'data';size='length' if name=='glProgramBinary' else 'size';args=', '.join(names);ownedargs=', '.join('owned_value.data()' if n==ptr else n for n in names)
 define(name,f'''    auto fn=raw_{name};std::size_t bytes=0;
    if({size}<0 || ({ptr} && !Small(std::size_t({size}),1,bytes))){{Sync([=]{{fn({args});}},true);return;}}
    if(!{ptr}){{Record([=]{{fn({args});}});return;}}
    auto owned_value=Copy({ptr},bytes);bytes=owned_value.capacity();Record([=,owned_value=std::move(owned_value)]{{fn({ownedargs});}},bytes);''')
for name in sorted(used):
 if not name.startswith('glDelete') or '*' not in entries[name][2]:continue
 names=argnames(entries[name][2]);assert len(names)==2,(name,names);count,ptr=names;args=', '.join(names)
 cleanup=''
 if name=='glDeleteBuffers':cleanup='''    for(GLsizei i=0;i<n;++i){const auto id=buffers[i];if(!id)continue;known_buffers.erase(id);if(array_buffer==id)array_buffer=0;if(unpack_buffer==id)unpack_buffer=0;for(auto& v:vaos)if(v.second.element==id)v.second.element=0;}'''
 elif name=='glDeleteVertexArrays':cleanup='''    for(GLsizei i=0;i<n;++i){if(!arrays[i])continue;known_vaos.erase(arrays[i]);vaos.erase(arrays[i]);if(current_vao==arrays[i])current_vao=0;}'''
 define(name,f'''    auto fn=raw_{name};std::size_t bytes=0;
    if({count}<0 || !{ptr} || !Small(std::size_t({count}),sizeof(GLuint),bytes)){{Sync([=]{{fn({args});}},true);if({count}>0 && {ptr}){{
{cleanup}
}}return;}}
{cleanup}
    auto owned_value=std::vector<GLuint>({ptr},{ptr}+{count});bytes=owned_value.capacity()*sizeof(GLuint);Record([=,owned_value=std::move(owned_value)]{{fn({count},owned_value.data());}},bytes);''')
# Pointer input whose exact element count is explicit in its API.
for name in sorted(used):
 m=re.fullmatch(r'glUniform([1-4])([fidu])v',name);mat=re.fullmatch(r'glUniformMatrix([2-4])(?:x([2-4]))?([fd])v',name)
 if not (m or mat):continue
 names=argnames(entries[name][2]);ptr=names[-1];count='count';mult=int(m[1]) if m else int(mat[1])*int(mat[2] or mat[1]);ty={'f':'GLfloat','d':'GLdouble','i':'GLint','u':'GLuint'}[m[2] if m else mat[3]]
 args=', '.join(names);ownedargs=', '.join('owned_value.data()' if x==ptr else x for x in names)
 define(name,f'''    auto fn=raw_{name};std::size_t bytes=0;
    if({count}<0 || !{ptr} || !Small(std::size_t({count}),sizeof({ty})*{mult},bytes)){{Sync([=]{{fn({args});}},true);return;}}
    std::vector<{ty}> owned_value({ptr},{ptr}+std::size_t({count})*{mult});bytes=owned_value.capacity()*sizeof({ty});Record([=,owned_value=std::move(owned_value)]{{fn({ownedargs});}},bytes);''')
for name in ['glClearBufferfv','glClearBufferiv']:
 names=argnames(entries[name][2]);args=', '.join(names);ty='GLfloat' if name.endswith('fv') else 'GLint';ptr=names[-1];scalar='GL_DEPTH' if ty=='GLfloat' else 'GL_STENCIL'
 define(name,f'''    auto fn=raw_{name};const std::size_t n=buffer==GL_COLOR?4:(buffer=={scalar}?1:0);
    if(!n||!{ptr}){{Sync([=]{{fn({args});}},true);return;}}
    std::vector<{ty}> owned_value({ptr},{ptr}+n);const auto charge=owned_value.capacity()*sizeof({ty});Record([=,owned_value=std::move(owned_value)]{{fn(buffer,drawbuffer,owned_value.data());}},charge);''')
for name in ['glSamplerParameterfv']:
 names=argnames(entries[name][2]);args=', '.join(names);ptr=names[-1]
 define(name,f'''    auto fn=raw_{name};
    // Uncommon vector pnames are conservatively synchronous, never guessed.
    if(pname!=GL_TEXTURE_BORDER_COLOR){{Sync([=]{{fn({args});}},true);return;}}
    if(!{ptr}){{Sync([=]{{fn({args});}},true);return;}}
    std::vector<GLfloat> owned_value({ptr},{ptr}+4);const auto charge=owned_value.capacity()*sizeof(GLfloat);Record([=,owned_value=std::move(owned_value)]{{fn(sampler,pname,owned_value.data());}},charge);''')
for name in ['glTexImage2D','glTexSubImage2D']:
 names=argnames(entries[name][2]);args=', '.join(names);ownedargs=', '.join('owned_value.data()' if n=='pixels' else n for n in names)
 define(name,f'''    auto fn=raw_{name};
    if(unpack_buffer){{++stats.texture_pbo_offsets;Record([=]{{fn({args});}});return;}}
    if(!pixels){{Record([=]{{fn({args});}});return;}}
    std::size_t bytes=0;
    if(!TextureSpan(width,height,format,type,bytes)){{++stats.texture_pointer_rpcs;Sync([=]{{fn({args});}},true);return;}}
    auto owned_value=Copy(pixels,bytes);bytes=owned_value.capacity();++stats.texture_owned_uploads;
    Record([=,owned_value=std::move(owned_value)]{{fn({ownedargs});}},bytes);''')
define('glShaderSource','''    auto fn=raw_glShaderSource;std::size_t bytes=0;
    if(count<0||!string||std::size_t(count)>MaxBatchBytes/(sizeof(std::string)+sizeof(GLint)+sizeof(void*))){Sync([=]{fn(shader,count,string,length);},true);return;}
    std::vector<std::string> text;std::vector<GLint> lengths;text.reserve(count);lengths.reserve(count);
    for(GLsizei i=0;i<count;++i){
      if(!string[i]){Sync([=]{fn(shader,count,string,length);},true);return;}
      const auto n=length&&length[i]>=0?std::size_t(length[i]):std::strlen(string[i]);
      if(n>std::size_t(std::numeric_limits<GLint>::max())||n>MaxBatchBytes-sizeof(std::string)-sizeof(GLint)-sizeof(void*) || bytes>MaxBatchBytes-n-sizeof(std::string)-sizeof(GLint)-sizeof(void*)){Sync([=]{fn(shader,count,string,length);},true);return;}
      bytes+=n+sizeof(std::string)+sizeof(GLint)+sizeof(void*);text.emplace_back(string[i],n);lengths.push_back(GLint(n));
    }
    bytes=text.capacity()*sizeof(std::string)+lengths.capacity()*sizeof(GLint);for(const auto& t:text)bytes+=t.capacity()+1;
    Record([=,text=std::move(text),lengths=std::move(lengths)]{
      std::vector<const GLchar*> pointers;pointers.reserve(text.size());for(const auto& t:text)pointers.push_back(t.data());
      fn(shader,count,pointers.data(),lengths.data());},bytes);''')
define('glTransformFeedbackVaryings','''    auto fn=raw_glTransformFeedbackVaryings;std::size_t bytes=0;
    if(count<0||!varyings||std::size_t(count)>MaxBatchBytes/(sizeof(std::string)+sizeof(void*)+1)){Sync([=]{fn(program,count,varyings,bufferMode);},true);return;}
    std::vector<std::string> text;text.reserve(count);
    for(GLsizei i=0;i<count;++i){
      if(!varyings[i]){Sync([=]{fn(program,count,varyings,bufferMode);},true);return;}
      const auto n=std::strlen(varyings[i]);
      if(n>MaxBatchBytes-sizeof(std::string)-sizeof(void*)-1 || bytes>MaxBatchBytes-n-sizeof(std::string)-sizeof(void*)-1){Sync([=]{fn(program,count,varyings,bufferMode);},true);return;}
      bytes+=n+sizeof(std::string)+sizeof(void*)+1;text.emplace_back(varyings[i]);
    }
    bytes=text.capacity()*sizeof(std::string);for(const auto& t:text)bytes+=t.capacity()+1;
    Record([=,text=std::move(text)]{
      std::vector<const GLchar*> pointers;pointers.reserve(text.size());for(const auto& t:text)pointers.push_back(t.c_str());
      fn(program,count,pointers.data(),bufferMode);},bytes);''')
# Immutable linked-program metadata cache. Invalid/unlinked/null queries keep real RPC semantics.
define('glCreateProgram','''    auto fn=raw_glCreateProgram;
    const auto result=Sync([=]{return fn();});
    if(result)programs.insert_or_assign(result,ProgramQueries{});return result;''')
define('glDeleteProgram','''    programs.erase(program);
    auto fn=raw_glDeleteProgram;Record([=]{fn(program);});''')
define('glLinkProgram','''    InvalidateProgram(program);auto fn=raw_glLinkProgram;Record([=]{fn(program);});''')
custom['glProgramBinary']='    InvalidateProgram(program);\n'+custom['glProgramBinary']
define('glGetProgramiv','''    auto fn=raw_glGetProgramiv;Sync([=]{fn(program,pname,params);},true);
    const auto p=programs.find(program);
    // Known creation plus no subsequent deletion establishes object validity;
    // no preseeded caller output from an invalid-program query can enable caching.
    if(pname==GL_LINK_STATUS && params && p!=programs.end()) {
      if(*params==GL_TRUE)p->second.linked=true;
      else {p->second.linked=false;p->second.uniforms.clear();p->second.attributes.clear();p->second.blocks.clear();}
    }''')
for name,argument,member,hit in [
 ('glGetUniformLocation','name','uniforms','uniform_location_cache_hits'),
 ('glGetAttribLocation','name','attributes','attrib_location_cache_hits'),
 ('glGetUniformBlockIndex','uniformBlockName','blocks','uniform_block_index_cache_hits')]:
 define(name,f'''    auto fn=raw_{name};auto* cache=LinkedProgram(program,{argument});
    const bool bounded_name={argument} && std::strlen({argument})<=4096;
    if(cache && bounded_name){{const auto i=cache->{member}.find(std::string_view({argument}));if(i!=cache->{member}.end()){{++stats.{hit};return i->second;}}}}
    const auto result=Sync([=]{{return fn(program,{argument});}},true);
    cache=LinkedProgram(program,{argument});
    if(cache && bounded_name && cache->{member}.size()<2048)cache->{member}.emplace({argument},result);
    return result;''')
# Generated wrappers: unknown pointer APIs always block through original call.
lines=['// Generated; do not hand-edit. Run generate_gl_command_queue.py.\n']
for name in sorted(used):lines.append(f'static {entries[name][0]} raw_{name}=nullptr;\n')
policies={}
for name in sorted(used):
 ty,ret,params=entries[name];args=', '.join(argnames(params));lines.append(f'static {ret} APIENTRY Wrap_{name}({params}) {{\n')
 if name in {'glDebugMessageCallback','glDebugMessageCallbackARB','glDebugMessageCallbackKHR','glDebugMessageCallbackAMD'}:
  lines.append('    throw std::logic_error("GL callback registration is unsupported while transactions are active");\n}\n');policies[name]='reject_callback_registration';continue

 lines.append(f'    if(Owner().IsWorkerThread()){{if(DriverTransactions::Executing()){{++stats.transaction_inline_calls;return raw_{name}({args});}}++stats.worker_rejected_calls;throw std::logic_error("GL recorder requires producer outside transaction");}}\n    Producer();\n')
 if name in custom:body=custom[name];policy='owned_or_state_aware'
 elif ret=='void' and '*' not in params and all(not x in params for x in ['GLDEBUGPROC','GLVULKANPROC']):body=f'    auto fn=raw_{name};Record([=]{{fn({args});}});';policy='async_scalar'
 else:body=f'    auto fn=raw_{name};return Sync([=]{{return fn({args});}},'+('true' if '*' in params else 'false')+');';policy='synchronous_rpc'
 body=re.sub(r'\bSync\(', 'SyncApi(RpcApi::'+name+', ',body);body=re.sub(r'\bRecord\(', 'RecordApi(RpcApi::'+name+', ',body)
 import re as _re
 body=_re.sub(r'(auto owned_value=(?:Copy\(|std::vector<)|std::vector<[^;]+?\s+(?:owned_value|text)[;(])',lambda m:'OwnedCopyScope payload_scope;'+m.group(0),body,count=1)
 lines.append(body+'\n}\n');policies[name]=policy
lines.append('static void CaptureAndInstall(){\n')
for name in sorted(used):lines.append(f'    raw_{name}=glad_{name};if(raw_{name})glad_{name}=&Wrap_{name};\n')
lines.append('}\nstatic void RestoreEntrypoints(){\n')
for name in sorted(used):lines.append(f'    if(raw_{name})glad_{name}=raw_{name};\n')
lines.append('}\n');generated=''.join(lines)
manifest=dict(glad_header=str(glad.relative_to(a.source)),glad_sha256=hashlib.sha256(glad.read_bytes()).hexdigest(),scan_roots=[str(r.relative_to(a.source)) for r in roots],covered_entrypoints=sorted(used),policies=policies,coverage='Every GLAD symbol referenced anywhere in selected render/native source files, including function-pointer references; unavailable raw entrypoints remain null',worker_rule='Closures call captured raw GL entrypoints only, never PICA/cache/render C++ metadata',limits='CPU packing stays producer; large/unknown payload pointers use synchronous RPC; only 2D known unpack formats owned asynchronously')
api_names=['Internal','UploadBufferOwned','InstallMaxAttributes']+sorted(used)
api_header='#pragma once\n#include <array>\n#include <cstddef>\nnamespace OpenGL::CommandQueue {\nenum class RpcApi : std::size_t {\n'+''.join('    '+n+',\n' for n in api_names)+'    Count\n};\ninline constexpr std::size_t RpcApiCount=std::size_t(RpcApi::Count);\ninline constexpr std::array<const char*,RpcApiCount> RpcApiNames{{\n'+''.join('    "'+n+'",\n' for n in api_names)+'}};\ninline const char* RpcApiName(RpcApi api){const auto i=std::size_t(api);return i<RpcApiCount?RpcApiNames[i]:"Unknown";}\n}\n'
outputs={'gl_command_queue_generated.inc':generated,'gl_command_queue_api.h':api_header}
if a.check:
 for name,expected in outputs.items():
  actual=a.output/name
  if not actual.is_file() or actual.read_bytes()!=expected.encode():
   raise SystemExit('Generated GL command queue drift: '+str(actual))
 print(json.dumps(dict(covered=len(used),referenced=len(references),byte_exact=True)))
 raise SystemExit
# --check never writes files. Generation is an explicit developer action.
a.output.mkdir(parents=True,exist_ok=True)
for name,value in outputs.items():(a.output/name).write_bytes(value.encode())
manifest['query_cache']='Known-created program + actual LINK_STATUS true; location/attribute/block-name caches only. Invalidated on create/link/binary/delete; worker-wrapper calls fail closed; no active-block-data-size cache.'
manifest['rpc_counters']='Fixed array per actual blocking API RPC; cache hits separate; includes oversize owned jobs and installation query.'
manifest_path.write_text(json.dumps(manifest,indent=2)+'\n');print(json.dumps(dict(generated=len(used),policies={p:sum(x==p for x in policies.values()) for p in set(policies.values())})))
