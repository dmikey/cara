// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_metal/pica_metal_source_pool.h"
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
void Check(bool ok) { if (!ok) throw std::runtime_error("Source pool invariant failed"); }
int main() {
    Cara::PicaMetalSourcePool tiny(8);
    auto first=tiny.Intern({"a","bb","a","bb"});
    Check(first && first->added_bytes==3 && first->hits==2 && tiny.Bytes()==3 && tiny.Size()==2);
    const auto* original=first->sources[0].data();
    auto failed=tiny.Intern({"ccc","dddd","a","bb"});
    Check(!failed && tiny.Bytes()==3 && tiny.Size()==2 && first->sources[0]=="a");
    auto exact=tiny.Intern({"ccc","dd","a","bb"});
    Check(exact && tiny.Bytes()==8 && tiny.Size()==4);
    auto at_capacity=tiny.Intern({"dd","ccc","bb","a"});
    Check(at_capacity && at_capacity->added_bytes==0 && at_capacity->hits==4);
    Check(first->sources[0].data()==original && !tiny.Intern({"new","a","bb","dd"}));
    const std::string binary("x\0y",3);
    Cara::PicaMetalSourcePool bytes(32);
    auto full=bytes.Intern({binary,"x","y","x"});
    Check(full && full->sources[0].size()==3 && full->sources[0][1]==0 && bytes.Bytes()==5);
    Cara::PicaMetalSourcePool game(64*1024*1024);
    std::string vertex(75804,'v'),msl_vertex(150000,'m');
    std::vector<std::array<std::string_view,4>> pairs;
    std::size_t logical=0,expected=vertex.size()+msl_vertex.size();
    for (unsigned i=0;i<400;++i) {
        std::string fragment="fragment "+std::to_string(i),msl_fragment="metal fragment "+std::to_string(i);
        auto pair=game.Intern({vertex,fragment,msl_vertex,msl_fragment});
        Check(bool(pair)); pairs.push_back(pair->sources);
        logical+=vertex.size()+fragment.size()+msl_vertex.size()+msl_fragment.size();
        expected+=fragment.size()+msl_fragment.size();
    }
    Check(logical>64*1024*1024 && game.Bytes()==expected && game.Bytes()<1024*1024);
    vertex.clear(); vertex.shrink_to_fit(); msl_vertex.clear(); msl_vertex.shrink_to_fit();
    for (unsigned i=0;i<pairs.size();++i)
        Check(pairs[i][0].size()==75804 && pairs[i][0].front()=='v' &&
              pairs[i][2].size()==150000 && pairs[i][2].back()=='m' &&
              pairs[i][1]=="fragment "+std::to_string(i));
    std::cout << "PASS: exact byte interning, transactional capacity rejection, stable short/long views; "
              << logical << " logical bytes -> " << game.Bytes() << " owned UTF-8 bytes\n";
}
