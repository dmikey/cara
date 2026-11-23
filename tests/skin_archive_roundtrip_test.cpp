// SPDX-License-Identifier: GPL-2.0-or-later
#include "ios/skin_archive.h"
#include <cassert>
#include <fstream>
#include <iterator>
int main(int argc, char** argv) {
    const std::map<std::string,std::vector<std::uint8_t>> files{
        {"info.json",{'{','}'}},{"transparent.png",{0,1,2,3,255}},{"empty",{}}};
    auto bytes=Cara::WriteSkinArchive(files);
    assert(Cara::ReadSkinArchive(bytes)==files);
    auto corrupted=bytes;
    const std::string marker="info.json";
    auto entry=std::search(corrupted.begin(),corrupted.end(),marker.begin(),marker.end());
    assert(entry!=corrupted.end()); *(entry+marker.size())^=1;
    bool rejected=false;try {Cara::ReadSkinArchive(corrupted);}catch(...){rejected=true;}
    assert(rejected);
    auto malicious=files; malicious["../bad"]={1};rejected=false;
    try {Cara::WriteSkinArchive(malicious);}catch(...){rejected=true;}assert(rejected);
    for(int i=1;i<argc;++i) {
        std::ifstream stream(argv[i],std::ios::binary); assert(stream);
        std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(stream)),{});
        auto archive=Cara::ReadSkinArchive(data);
        assert(archive.count("info.json") && archive.count("transparent.png"));
        assert(Cara::ReadSkinArchive(Cara::WriteSkinArchive(archive))==archive);
    }
}
