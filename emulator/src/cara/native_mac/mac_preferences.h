// Copyright 2026 Cara contributors. GPLv2 or later; see license.txt.
#pragma once
#include <array>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <SDL_scancode.h>
#include "common/param_package.h"
namespace Cara::MacPreferences {
inline std::string Trim(std::string s) { const auto a=s.find_first_not_of(" \t\r");if(a==s.npos)return {};return s.substr(a,s.find_last_not_of(" \t\r")-a+1); }
class Ini {
public:
 explicit Ini(std::string text={}):text_(std::move(text)){}
 std::string Get(const std::string& section,const std::string& key,std::string fallback={}) const {
  std::istringstream in(text_);std::string line,current,value=std::move(fallback);
  while(std::getline(in,line)){auto t=Trim(line);if(t.empty()||t[0]=='#'||t[0]==';')continue;
   if(t.front()=='['&&t.back()==']'){current=t.substr(1,t.size()-2);continue;}
   const auto eq=t.find('=');if(current==section&&eq!=t.npos&&Trim(t.substr(0,eq))==key)value=Trim(t.substr(eq+1));}
  return value;
 }
 void Set(const std::string& section,const std::string& key,const std::string& value) {
  if(value.find_first_of("\r\n")!=value.npos)throw std::invalid_argument("Invalid preference value");
  std::istringstream in(text_);std::ostringstream out;std::string line,current;bool wrote=false,found=false;
  const auto emit=[&]{out<<key<<" = "<<value<<'\n';wrote=true;};
  while(std::getline(in,line)){auto t=Trim(line);
   if(!t.empty()&&t.front()=='['&&t.back()==']'){
    if(current==section&&!wrote)emit();current=t.substr(1,t.size()-2);found|=current==section;
   }
   const auto eq=t.find('=');
   if(current==section&&eq!=t.npos&&Trim(t.substr(0,eq))==key){if(!wrote)emit();continue;}
   out<<line<<'\n';
  }
  if(!found){out<<'\n'<<'['<<section<<"]\n";emit();}else if(!wrote)emit();text_=out.str();
 }
 const std::string& Text()const{return text_;}
private:std::string text_;
};
inline constexpr bool JitAvailable(){
#if defined(CARA_MAC_CPU_JIT) || defined(CARA_ENABLE_JIT)
 return true;
#else
 return false;
#endif
}
inline void ValidateBackend(const std::string& name,bool jit) {
 if(name!="fastinterp"&&name!="dyncom"&&name!="jit")throw std::invalid_argument("Unknown CPU engine");
 if(name=="jit"&&!jit)throw std::invalid_argument("This build does not include the JIT CPU engine");
}
inline std::string Keyboard(int key){return Common::ParamPackage{{"engine","keyboard"},{"code",std::to_string(key)}}.Serialize();}
inline std::string AnalogKeys(const std::array<std::string,4>& keys){return Common::ParamPackage{{"engine","analog_from_button"},{"up",keys[0]},{"down",keys[1]},{"left",keys[2]},{"right",keys[3]},{"modifier",Keyboard(SDL_SCANCODE_D)},{"modifier_scale","0.5"}}.Serialize();}
struct Button {std::string key,label,value;int default_key;};
struct Analog {std::string key,label,controller,keyboard_package;std::array<std::string,4> directions;bool use_controller=false;};
struct Model {
 std::string backend="fastinterp",original;
 std::vector<Button> buttons;
 std::array<Analog,2> analogs;
 explicit Model(const std::string& text={}):original(text){
  Ini ini(text);backend=ini.Get("Core","cpu_backend","fastinterp");
  const std::array<Button,15> defaults{{
   {"button_a","A","",SDL_SCANCODE_A},{"button_b","B","",SDL_SCANCODE_S},
   {"button_x","X","",SDL_SCANCODE_Z},{"button_y","Y","",SDL_SCANCODE_X},
   {"button_up","D-pad Up","",SDL_SCANCODE_UP},{"button_down","D-pad Down","",SDL_SCANCODE_DOWN},
   {"button_left","D-pad Left","",SDL_SCANCODE_LEFT},{"button_right","D-pad Right","",SDL_SCANCODE_RIGHT},
   {"button_l","L","",SDL_SCANCODE_Q},{"button_r","R","",SDL_SCANCODE_W},
   {"button_start","Start","",SDL_SCANCODE_M},{"button_select","Select","",SDL_SCANCODE_N},
   {"button_zl","ZL","",SDL_SCANCODE_1},{"button_zr","ZR","",SDL_SCANCODE_2},
   {"button_home","Home","",SDL_SCANCODE_B}}};
  for(auto b:defaults){b.value=ini.Get("Controls",b.key,Keyboard(b.default_key));if(b.value.empty())b.value=Keyboard(b.default_key);buttons.push_back(std::move(b));}
  constexpr int keys[2][4]={{SDL_SCANCODE_UP,SDL_SCANCODE_DOWN,SDL_SCANCODE_LEFT,SDL_SCANCODE_RIGHT},{SDL_SCANCODE_I,SDL_SCANCODE_K,SDL_SCANCODE_J,SDL_SCANCODE_L}};
  for(unsigned i=0;i<2;++i){auto& a=analogs[i];a.key=i?"c_stick":"circle_pad";a.label=i?"C-stick":"Circle Pad";
   for(unsigned d=0;d<4;++d)a.directions[d]=Keyboard(keys[i][d]);
   Common::ParamPackage p(ini.Get("Controls",a.key,AnalogKeys(a.directions)));
   a.use_controller=p.Get("engine","")=="sdl";a.controller=a.use_controller?p.Serialize():"";
   if(p.Get("engine","")=="analog_from_button"){a.keyboard_package=p.Serialize();const char* names[]={"up","down","left","right"};for(unsigned d=0;d<4;++d)a.directions[d]=p.Get(names[d],a.directions[d]);}
  }
 }
 std::string Serialize(bool jit)const{ValidateBackend(backend,jit);Ini ini(original);ini.Set("Core","cpu_backend",backend);
  for(const auto& b:buttons)ini.Set("Controls",b.key,b.value);
  for(const auto& a:analogs){
   if(a.use_controller){ini.Set("Controls",a.key,a.controller);continue;}
   Common::ParamPackage p(a.keyboard_package.empty()?AnalogKeys(a.directions):a.keyboard_package);
   const char* names[]={"up","down","left","right"};for(unsigned d=0;d<4;++d)p.Set(names[d],a.directions[d]);
   ini.Set("Controls",a.key,p.Serialize());
  }
  return ini.Text();
 }
};
}
