// Copyright 2026 Cara contributors. Licensed under GPL-2.0-or-later.
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <random>
#include <vector>
#include <iostream>
extern "C" int LLVMFuzzerTestOneInput(const unsigned char*,std::size_t);
int main() {
    std::vector<std::vector<unsigned char>> corpus;
    for(unsigned i=0;i<8;++i){std::ifstream f("corpus/seed"+std::to_string(i),std::ios::binary);if(!f)return 2;corpus.emplace_back(std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>());}
    std::mt19937 rng(0xb01ded);
    for(unsigned i=0;i<250000;++i) {
        auto b=corpus[rng()%corpus.size()];
        if(i%3==0) b.resize(rng()%4100);
        for(unsigned j=0,n=1+rng()%32;j<n&&!b.empty();++j)b[rng()%b.size()]=rng();
        LLVMFuzzerTestOneInput(b.data(),b.size());
    }
    std::cout<<"PASS 250000 standalone corpus mutations/resizes under ASan+UBSan\n";
}
