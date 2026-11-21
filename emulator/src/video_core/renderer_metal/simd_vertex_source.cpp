// SPDX-License-Identifier: GPL-2.0-or-later
#include "simd_vertex_source.h"
#include "batched_vertex_source.h"
#include <stdexcept>
#include <vector>
namespace Cara {
namespace {
void Require(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
std::string Trim(std::string_view s) {
 const auto a=s.find_first_not_of(" \n\r\t"),b=s.find_last_not_of(" \n\r\t");
 return a==s.npos ? "" : std::string(s.substr(a,b-a+1));
}
std::string WithoutReference(std::string source) {
 constexpr std::string_view start="void main() {\nvec4 first_quat=vec4(0.0);\nif (gl_VertexID % 3 != 0) { cara_run_vertex(gl_VertexID - gl_VertexID % 3); first_quat = ";
 constexpr std::string_view actual="; }\ncara_run_vertex(gl_VertexID);\nnormquat = ";
 constexpr std::string_view fix=";\nif (gl_VertexID % 3 != 0 && dot(first_quat, normquat) < 0.0) normquat = -normquat;\n";
 const auto a=source.find(start),b=source.find(actual,a),c=source.find(fix,b);
 Require(a!=source.npos && b!=source.npos && c!=source.npos,"Unexpected real generated reference form");
 const auto reference=source.substr(a+start.size(),b-a-start.size());
 const auto quaternion=source.substr(b+actual.size(),c-b-actual.size());
 Require(reference==quaternion,"Generated reference and actual quaternion expressions differ");
 source.replace(a,c+fix.size()-a,"void main() {\ncara_run_vertex(gl_VertexID);\nnormquat = "+quaternion+";\n");
 Require(source.find("int input_vertex_id = int(texelFetch(cara_vertex_indices")!=source.npos,
         "Diagnostic removed raw indexed input lookup");
 return source;
}
std::string Build(std::string_view original,bool simd,bool diagnostic) {
 constexpr std::string_view entry="vertex cara_pica_vert_out cara_pica_vert(";
 const auto begin=original.find(entry),end=original.find(")\n{",begin);
 Require(begin!=original.npos && end!=original.npos,"Unknown generated MSL entry");
 const auto signature=original.substr(begin+entry.size(),end-begin-entry.size());
 std::vector<std::string> args; std::string id;
 for(std::size_t at=0;at<signature.size();) {
  const auto comma=signature.find(',',at);auto parameter=signature.substr(at,comma==signature.npos ? signature.size()-at : comma-at);
  const auto annotation=parameter.find("[[");Require(annotation!=parameter.npos,"Unknown MSL parameter annotation");
  const auto declaration=Trim(parameter.substr(0,annotation));
  const auto name=declaration.substr(declaration.find_last_of(" \t")+1);
  args.push_back(name);if(parameter.find("[[vertex_id]]")!=parameter.npos) id=name;
  if(comma==signature.npos)break;at=comma+1;
 }
 Require(!id.empty() && signature.find("[[buffer(3)]]")==signature.npos,"Unexpected diagnostic buffer collision");
 auto call=[&](bool reference) {
  std::string result="cara_compute_vertex(";
  for(unsigned i=0;i<args.size();++i){if(i)result+=",";result+=reference && args[i]==id ? "(cara_id-cara_id%3u)" : args[i];}
  return result+")";
 };
 auto body=Cara::MakeBatchedVertexSource(original,false);Require(bool(body),"Generated body rejected by production wrapper");
 const auto cutoff=body->find("\nstruct CaraComputeJob {");
 Require(cutoff!=body->npos,"Unknown actual-evaluator wrapper boundary");body->resize(cutoff);
 *body+=simd ? std::string(SimdVertexMslTag) : "\n";
 *body+="vertex cara_pica_vert_out cara_pica_vert("+std::string(signature)+",\n constant ulong& cara_draw_token [[buffer(3)]]";
 if(diagnostic)*body+=", device float4* cara_values [[buffer(4)]], device uint4* cara_stats [[buffer(5)]]";
 *body+=", uint cara_instance [[instance_id]])\n{\n";
 *body+="uint cara_id="+id+";\nauto out="+call(false)+";\n";
 if(simd) {
  *body+=R"(
 auto candidates=static_cast<simd_vote::vote_t>(simd_ballot(cara_id%3u==0));
 const auto active=static_cast<simd_vote::vote_t>(simd_ballot(true));
 ushort source=0; bool found=false;
 float4 reference=out.normquat;
 if(active) { // Uniform vote result; never shuffle from an inactive/undefined lane.
 source=ushort(ctz(active));
 while(candidates) {
  const ushort lane=ushort(ctz(candidates));
  const uint candidate_id=simd_shuffle(cara_id,lane);
  const uint2 candidate_draw=simd_shuffle(as_type<uint2>(cara_draw_token),lane);
  const uint candidate_instance=simd_shuffle(cara_instance,lane);
  const bool match=candidate_id==cara_id-cara_id%3u &&
       all(candidate_draw==as_type<uint2>(cara_draw_token)) && candidate_instance==cara_instance;
  source=match ? lane : source;found=found || match;
  candidates &= candidates-1;
 }
 reference=simd_shuffle(out.normquat,source);
 }
 )";
  *body+="if(!found) reference="+call(true)+".normquat;\n";
  *body+="if(cara_id%3u && dot(reference,out.normquat)<0) out.normquat=-out.normquat;\n";
 } else *body+="bool found=cara_id%3u==0;\n";
 if(diagnostic)*body+=R"(
 if(cara_id<256 && cara_instance<2) {
  const uint slot=cara_instance*256+cara_id;
  cara_values[slot*6+0]=out.gl_Position;
  cara_values[slot*6+1]=out.primary_color;
  cara_values[slot*6+2]=float4(out.texcoord0,out.texcoord1);
  cara_values[slot*6+3]=float4(out.texcoord2,out.texcoord0_w,out.gl_ClipDistance[0]);
  cara_values[slot*6+4]=out.normquat;
  cara_values[slot*6+5]=float4(out.view,out.gl_ClipDistance[1]);
  cara_stats[slot]=uint4(found ? 0u : 1u,found ? 1u : 0u,cara_id,cara_instance);
 }
 )";
 *body+="return out;\n}\n";
 return *body;
}

} // namespace
bool HasSimdVertexTag(std::string_view source) {return source.ends_with(SimdVertexTag);}
std::string StripSimdVertexTag(std::string_view source) {
 if(HasSimdVertexTag(source))source.remove_suffix(SimdVertexTag.size());return std::string(source);
}
std::optional<std::string> RemoveVertexReference(std::string_view source) {
 try {return WithoutReference(StripSimdVertexTag(source));}catch(const std::runtime_error&){return std::nullopt;}
}
bool TagSimdVertexSource(std::string& source) {
 if(HasSimdVertexTag(source))return bool(RemoveVertexReference(source));
 if(!RemoveVertexReference(source))return false;source+=SimdVertexTag;return true;
}
std::optional<std::string> MakeVertexReferenceSource(std::string_view source,bool simd,bool diagnostic) {
 try {return Build(source,simd,diagnostic);}catch(const std::runtime_error&){return std::nullopt;}
}
bool ValidSimdVertexMsl(std::string_view source) {
 // Rebuild the same generated wrapper tail from its original annotated signature.
 // The content-addressed disk record protects its actual-evaluator prefix.
 const auto tag=source.find(SimdVertexMslTag);
 if(tag==source.npos)return false;
 constexpr std::string_view entry="vertex cara_pica_vert_out cara_pica_vert(";
 const auto begin=source.find(entry,tag),split=source.find(",\n constant ulong& cara_draw_token [[buffer(3)]]",begin);
 if(begin==source.npos || split==source.npos)return false;
 const auto signature=source.substr(begin+entry.size(),split-begin-entry.size());
 // A synthetic entry lets the SAME builder regenerate the expected complete tail.
 std::string original(source.substr(0,tag));
 const auto helper=original.find("static inline cara_pica_vert_out cara_compute_vertex(");
 if(helper==original.npos)return false;
 original.resize(helper);
 original+=std::string(entry)+std::string(signature)+")\n{ return {}; }\n";
 const auto rebuilt=MakeVertexReferenceSource(original,true,false);
 if(!rebuilt)return false;
 const auto rebuilt_tag=rebuilt->find(SimdVertexMslTag);
 return rebuilt_tag!=rebuilt->npos && source.substr(tag)==std::string_view(*rebuilt).substr(rebuilt_tag);
}
} // namespace Cara
