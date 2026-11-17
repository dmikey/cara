// Copyright 2026 Cara contributors. GPLv2 or later; see license.txt.
#pragma once
#include <cstdlib>
#include "gl_packed_snapshot_compare.h"
#include "gl_vertex_upload_bank.h"
#include "gl_vertex_upload_order_reuse.h"
namespace OpenGL::PackedPoolObserver {
inline bool Enabled() {
    static const bool value=[] { const auto* v=std::getenv("CARA_PACKED_POOL_OWNER_TRACE"); return v && std::strcmp(v,"1")==0; }();
    return value;
}
enum class State { Inactive, PolicyOff, ColdIndex, Shape, Storage, Sources, Pending,
                   Concordant, OwnerChanged, DecisionChanged, Mode, Bypassed, Count };
// Correlation only: numeric GL names/slots are NOT a lifetime lease.
struct Token {
    State state=State::Inactive;
    unsigned frame=0, rows=0;
    std::size_t size_class=0, ordinal=0, upload_ordinal=0;
    IOSVertexUploadOrderReuse::Result match;
    std::uint32_t texture=0, indices=0;
    std::uint64_t logical=0, merged=0, payload_requested=0, padding_requested=0, index_requested=0;
};
inline Token Virtual(unsigned frame, unsigned previous_frame, std::size_t upload_ordinal,
                     std::size_t used, unsigned input_rows, std::size_t index_size,
                     unsigned width, std::size_t max_draws, unsigned max_rows) {
    Token t; t.frame=frame; t.state=State::Shape;
    if (!width || !input_rows || !index_size || index_size%width) return t;
    const auto needed=std::max<std::size_t>(input_rows,index_size/width);
    std::size_t height=0; t.rows=1;
    while (t.rows<needed && height<IOSVertexUploadBank::HeightClasses) {t.rows*=2; ++height;}
    if (height>=IOSVertexUploadBank::HeightClasses || t.rows>max_rows) return t;
    t.size_class=IOSVertexUploadBank::ClassIndex(2,height);
    t.ordinal=frame==previous_frame?used:0;
    t.upload_ordinal=frame==previous_frame?upload_ordinal:0;
    t.state=t.upload_ordinal<max_draws?State::Pending:State::Storage;
    return t;
}
template<class Entry>
void Probe(Token& t, const std::vector<Entry>& pool, const PackedSnapshotCompare::Segment* spans,
           std::size_t count, const std::uint16_t* index, std::size_t index_count) {
    if (t.state!=State::Pending) return;
    if (t.ordinal>=pool.size()) {t.state=State::Storage; return;} // no early allocation/reclaim
    t.match.selected=t.ordinal;
    const auto stop=t.ordinal+std::min<std::size_t>(32,pool.size()-t.ordinal-1);
    for (auto pos=t.ordinal;pos<=stop;++pos) {
        const auto& e=pool[pos];
        if (e.rows!=t.rows || !e.texture.handle || !e.indices.handle) continue;
        ++t.match.candidates;
        const auto a=PackedSnapshotCompare::Compare(spans,count,t.logical,
            PackedSnapshotCompare::Padding::Zero,e.input_shadow.data(),e.input_shadow.size());
        t.payload_requested+=a.payload_bytes_requested; t.padding_requested+=a.padding_bytes_requested;
        const bool input=a.decision==PackedSnapshotCompare::Decision::Equal;
        bool indices=false;
        if ((pos==t.ordinal || input) && index_count && e.index_shadow.size()==index_count*2) {
            t.index_requested+=index_count*2;
            indices=std::memcmp(e.index_shadow.data(),index,index_count*2)==0;
        }
        if (pos==t.ordinal) {t.match.input_equal=input; t.match.index_equal=indices;}
        if (input && indices) {t.match.selected=pos; t.match.input_equal=true; t.match.index_equal=true; break;}
    }
    t.texture=pool[t.match.selected].texture.handle; t.indices=pool[t.match.selected].indices.handle;
}
inline State Actual(const Token& t, unsigned frame, std::size_t size_class,
                    std::size_t ordinal, std::size_t upload_ordinal,
                    const IOSVertexUploadOrderReuse::Result& actual,
                    std::uint32_t texture, std::uint32_t indices) {
    if (frame!=t.frame || size_class!=t.size_class || ordinal!=t.ordinal ||
        upload_ordinal!=t.upload_ordinal || actual.selected!=t.match.selected ||
        texture!=t.texture || indices!=t.indices) return State::OwnerChanged;
    return actual.input_equal==t.match.input_equal && actual.index_equal==t.match.index_equal ?
        State::Concordant:State::DecisionChanged;
}
} // namespace OpenGL::PackedPoolObserver
