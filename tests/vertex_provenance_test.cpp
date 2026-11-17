// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_opengl/gl_vertex_provenance.h"
#include "video_core/renderer_opengl/gl_packed_vertex.h"
#include <iostream>
#include <sstream>
#include <stdexcept>
using namespace OpenGL::VertexProvenance;
static unsigned checks=0;
#define CHECK(x) do{++checks;if(!(x))throw std::runtime_error(#x);}while(false)
int main(){try{
    CHECK(Classify(OpenGL::PackedVertexDecoderGLSL)==Decoder::Scalar);
    CHECK(Classify(OpenGL::PackedVertexFetchReuseGLSL)==Decoder::Reused);
    CHECK(Classify("void main(){}") == Decoder::None);
    CHECK(Classify("uint cara_raw_word( uint cara_local_word(")==Decoder::Ambiguous);
    Ledger<3> ledger(17);unsigned inspections=0;bool first=false;
    const auto good=[&](Record& r){++inspections;r.current_program=r.program;r.actual_vs=r.managed_vs;
        r.linked=r.compiled=r.verified=true;r.decoder=Decoder::Reused;r.reason="ok";
        r.source_bytes=4096;r.actual_source_hash=123456789;};
    Record* r=ledger.Observe(10,20,30,2,good,first);
    CHECK(first&&r&&r->verified&&inspections==1&&r->draw_attempts==1);
    for(unsigned i=0;i<100;++i){r=ledger.Observe(10,20,30,2,good,first);CHECK(!first&&r&&inspections==1);}
    CHECK(r->draw_attempts==101&&r->input_modes[2]==101);
    auto* changed_shader=ledger.Observe(10,21,30,6,good,first);
    CHECK(first&&changed_shader&&inspections==2);
    auto* changed_source=ledger.Observe(10,20,31,99,good,first);
    CHECK(first&&changed_source&&inspections==3&&changed_source->input_modes[7]==1);
    CHECK(!ledger.Observe(11,22,33,2,good,first)&&!first&&inspections==3);
    CHECK(ledger.overflow_draw_attempts==1&&ledger.total_draw_attempts==104&&ledger.size==3);
    r=ledger.Observe(10,20,30,3,good,first);
    CHECK(r&&r->draw_attempts==102&&r->input_modes[3]==1&&!first&&inspections==3);
    Ledger<1> unknown(18);
    r=unknown.Observe(40,50,60,5,[&](Record& row){++inspections;row.reason="managed_vertex_shader_mismatch";row.actual_vs=99;},first);
    CHECK(first&&r&&!r->verified&&r->actual_source_hash==0&&r->decoder==Decoder::None);
    unknown.Observe(40,50,60,5,good,first);CHECK(!first&&inspections==4);
    // Cached observations never manufacture a successful inspection after an
    // unknown first use; key/source changes need a new bounded record.
    CHECK(unknown.records[0].draw_attempts==2&&!unknown.records[0].verified);
    std::ostringstream a,b;
    WriteRecord(a,"program_final",ledger.session,ledger.records[0]);
    WriteRecord(b,"program_final",unknown.session,unknown.records[0]);
    std::cout<<a.str()<<'\n'<<b.str()<<'\n';
    std::cerr<<"PASS checks="<<checks<<" inspect_calls="<<inspections<<" cached_followup_calls=101 overflow=1\n";
    return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
