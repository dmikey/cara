// Captured-generator subset only. Unsupported input retains the whole original.
// Modified for Cara on 2026-11-17; see NOTICE for attribution.
#pragma once
#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <utility>
namespace OpenGL::ShaderSuffixSharing {
inline constexpr unsigned SourceOption = 0x53534631; // SSF1, source policy revision 1
inline bool One(const char* value) { return value && std::string_view(value) == "1"; }
inline bool Requested(const char* value, const char* diagnostics) {
    return One(value) && One(diagnostics);
}
namespace Detail {
inline void Require(bool ok) { if (!ok) throw std::invalid_argument("unsupported suffix source"); }
inline bool Space(char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }
inline std::string Trim(std::string_view s) {
    while (!s.empty() && Space(s.front())) s.remove_prefix(1);
    while (!s.empty() && Space(s.back())) s.remove_suffix(1);
    return std::string(s);
}
struct Token { std::string text; std::size_t a, b; bool comment=false, directive=false; };
inline std::vector<Token> Lex(std::string_view s) {
    std::vector<Token> out;
    for (std::size_t i=0;i<s.size();) {
        if (Space(s[i])) { ++i; continue; }
        const auto a=i;
        if (s.substr(i,2)=="//") {
            auto end=s.find('\n',i); i=end==s.npos?s.size():end;
            out.push_back({std::string(s.substr(a,i-a)),a,i,true,false}); continue;
        }
        if (s.substr(i,2)=="/*") {
            auto end=s.find("*/",i+2); Require(end!=s.npos); i=end+2;
            out.push_back({std::string(s.substr(a,i-a)),a,i,true,false}); continue;
        }
        if (s[i]=='#') {
            auto line=s.rfind('\n',i); auto start=line==s.npos?0:line+1;
            Require(Trim(s.substr(start,i-start)).empty());
            auto end=s.find('\n',i); i=end==s.npos?s.size():end;
            const auto text=Trim(s.substr(a,i-a)); Require(text.empty() || text.back()!='\\');
            out.push_back({text,a,i,false,true}); continue;
        }
        const auto word=[](char c){return std::isalnum(static_cast<unsigned char>(c)) || c=='_';};
        if (word(s[i])) { while(i<s.size() && word(s[i])) ++i; }
        else { Require(std::string_view("{}()[];,.?:+-*/%<>=!&|^~").find(s[i])!=std::string_view::npos); ++i; }
        out.push_back({std::string(s.substr(a,i-a)),a,i,false,false});
    }
    return out;
}
inline bool Sub(const std::string& n) { return std::regex_match(n,std::regex("sub_[0-9]+_[0-9]+")); }
struct Function { std::size_t a,b; unsigned begin,end; };
using Functions=std::map<std::string,Function>;
inline Functions Parse(const std::string& source) {
    static const std::set<std::string> allowed={
        "#version 410 core","#define CARA_GLES","#define uniforms vs_uniforms",
        "#define NUM_TEV_STAGES 6","#define NUM_LIGHTS 8","#define NUM_LIGHTING_SAMPLERS 24",
        "#define mul_safe(x, y) mix(x * y, vec4(0.0), isnan(x * y))",
        "#define rcp_safe(x) (1.0f / x)","#define rsq_safe(x) inversesqrt(x)",
        "#define fma_safe(x, y, z) (mul_safe(x, y) + z)"};
    std::vector<Token> t; std::set<std::string> directives,prototypes;
    for (auto x:Lex(source)) {
        if (x.comment) continue;
        if (x.directive) { Require(allowed.count(x.text) && directives.insert(x.text).second); continue; }
        t.push_back(std::move(x));
    }
    Functions result; int depth=0;
    for (std::size_t i=0;i<t.size();) {
        if (depth==0 && t[i].text=="bool" && i+1<t.size() && Sub(t[i+1].text)) {
            Require(i+4<t.size() && t[i+2].text=="(" && t[i+3].text==")");
            auto name=t[i+1].text;
            if (t[i+4].text==";") { Require(prototypes.insert(name).second); i+=5; continue; }
            Require(t[i+4].text=="{"); const auto a=t[i+4].b; int d=1; auto j=i+5;
            while(j<t.size() && d) { d+=(t[j].text=="{")-(t[j].text=="}"); ++j; }
            Require(d==0 && !result.count(name));
            auto split=name.find('_',4); auto begin=std::stoul(name.substr(4,split-4));auto end=std::stoul(name.substr(split+1));
            Require(begin<=4096 && end<=4096);
            result.emplace(name,Function{a,t[j-1].a,static_cast<unsigned>(begin),static_cast<unsigned>(end)});i=j;continue;
        }
        depth+=(t[i].text=="{")-(t[i].text=="}");Require(depth>=0);++i;
    }
    Require(depth==0 && !result.empty());
    for(const auto& [name,f]:result) Require(prototypes.count(name));
    // Preprocessor directives inside functions are never accepted, including known ones.
    for(const auto& x:Lex(source)) if(x.directive)
        for(const auto& [name,f]:result) Require(x.a<f.a || x.a>=f.b);
    return result;
}
inline void Graph(const std::string& source,const Functions& functions) {
    std::map<std::string,std::set<std::string>> graph;
    for(const auto& [name,f]:functions) {
        std::vector<Token> t;
        for (auto x:Lex(std::string_view(source).substr(f.a,f.b-f.a))) if(!x.comment) t.push_back(std::move(x));
        for(std::size_t i=0;i+1<t.size();++i) if(!t[i].comment && Sub(t[i].text) && t[i+1].text=="(") {
            Require(functions.count(t[i].text));graph[name].insert(t[i].text);
        }
    }
    std::set<std::string> active,done;
    std::function<void(const std::string&)> visit=[&](const std::string& n){
        Require(!active.count(n));if(done.count(n)) return;active.insert(n);
        for(const auto& child:graph[n]) visit(child);
        active.erase(n);done.insert(n);
    };
    for(const auto& [name,f]:functions) visit(name);
}
inline bool Prefix(const std::string& s) {
    std::vector<std::string> stack; std::string clean;
    for(const auto& x:Lex(s)) {
        if(x.comment) continue; if(x.directive) return false;
        if(x.text=="{" || x.text=="(" || x.text=="[") stack.push_back(x.text);
        else if(x.text=="}" || x.text==")" || x.text=="]") {
            const auto open=x.text=="}"?"{":x.text==")"?"(":"[";
            if(stack.empty() || stack.back()!=open) return false;stack.pop_back();
        }
        if(x.text!="{" && x.text!="}") clean+=x.text+" ";
    }
    if(!stack.empty()) return false;
    std::size_t start=0;
    while(start<clean.size()) {
        auto end=clean.find(';',start);if(end==std::string::npos) return Trim(std::string_view(clean).substr(start)).empty();
        auto statement=Trim(std::string_view(clean).substr(start,end-start));start=end+1;
        if(statement.empty()) return false;
        if(std::regex_match(statement,std::regex("sub_[0-9]+_[0-9]+ \\( \\)"))) continue;
        std::smatch match;
        if(!std::regex_match(statement,match,std::regex("(?:reg_tmp[0-9]+|vs_out_attr[0-9]+)(?: \\. [xyzwrgba]{1,4})? = (.+)"))) return false;
        const std::string rhs=match[1];
        if(std::regex_search(rhs,std::regex("[=;{}?:]|\\+ \\+|- -|\\b(return|if|else|for|while|do|break|continue|discard)\\b"))) return false;
    }
    return true;
}
struct Edit { std::string low,high,removed,replacement;std::size_t a,b; };
inline std::string Transform(const std::string& original) {
    const auto functions=Parse(original);Graph(original,functions);std::vector<Edit> edits;
    for(const auto& [low,f]:functions) {
        std::vector<std::pair<std::string,Function>> targets;
        for(const auto& h:functions) if(h.second.begin>f.begin && h.second.end==f.end) targets.push_back(h);
        std::sort(targets.begin(),targets.end(),[](const auto& a,const auto& b){return a.second.begin<b.second.begin;});
        const auto body=original.substr(f.a,f.b-f.a);
        for(const auto& [high,h]:targets) {
            const auto target=Trim(std::string_view(original).substr(h.a,h.b-h.a));
            if(!std::regex_search(target,std::regex("\\breturn false;\\s*$"))) continue;
            std::vector<std::size_t> cuts;
            for(const auto& x:Lex(body)) if(x.comment && std::regex_search(x.text,std::regex("^//\\s*"+std::to_string(h.begin)+":"))) {
                auto line=body.rfind('\n',x.a);auto cut=line==std::string::npos?0:line+1;
                if(Trim(std::string_view(body).substr(cut))==target) cuts.push_back(cut);
            }
            if(cuts.size()!=1 || !Prefix(body.substr(0,cuts[0]))) continue;
            const auto cut=f.a+cuts[0];
            edits.push_back({low,high,original.substr(cut,f.b-cut),"    return "+high+"();\n",cut,f.b});break;
        }
    }
    if(edits.empty()) return original;
    auto output=original;std::sort(edits.begin(),edits.end(),[](const auto& a,const auto& b){return a.a>b.a;});
    for(const auto& e:edits) output.replace(e.a,e.b-e.a,e.replacement);
    const auto after=Parse(output);Require(after.size()==functions.size());Graph(output,after);
    std::map<std::string,const Edit*> changed;
    for(const auto& e:edits) Require(changed.emplace(e.low,&e).second);
    std::function<std::string(const std::string&,std::set<std::string>)> expand;
    expand=[&](const std::string& name,std::set<std::string> active){
        Require(active.insert(name).second);const auto f=after.at(name);
        auto body=output.substr(f.a,f.b-f.a);auto it=changed.find(name);
        if(it==changed.end()) return Trim(body);
        const auto& e=*it->second;
        Require(body.size()>=e.replacement.size() && body.compare(body.size()-e.replacement.size(),e.replacement.size(),e.replacement)==0);
        body.resize(body.size()-e.replacement.size());
        std::size_t lead=0,tail=e.removed.size();while(lead<tail && Space(e.removed[lead])) ++lead;
        while(tail>lead && Space(e.removed[tail-1])) --tail;
        return Trim(body+e.removed.substr(0,lead)+expand(e.high,std::move(active))+e.removed.substr(tail));
    };
    for(const auto& [name,f]:functions) Require(expand(name,{})==Trim(std::string_view(original).substr(f.a,f.b-f.a)));
    return output;
}
} // namespace Detail
inline std::string Apply(const std::string& source) {
    try { return Detail::Transform(source); }
    catch (const std::exception&) { return source; } // Fail closed, no partial edits.
}
} // namespace OpenGL::ShaderSuffixSharing
