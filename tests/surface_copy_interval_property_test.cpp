// SPDX-License-Identifier: GPL-2.0-or-later
// Actual private FindMatch implementation, not a parallel selection model.
// This ROM-free executable allocates metadata only; no GL object is created.
// Build/run the native cara-surface-copy-interval-self-test target. Including
// cache.cpp makes its private selection implementation testable unchanged;
// do not also compile that implementation as a direct source of this target.
#include "../emulator/src/video_core/renderer_opengl/gl_rasterizer_cache.cpp"
#include <iostream>
#include <stdexcept>

namespace {
using namespace OpenGL;
std::uint64_t checks=0, queries=0, hits=0, fill_hits=0, misses=0;
void Require(bool ok,const char* why) {++checks;if(!ok)throw std::runtime_error(why);}
void Same(const SurfaceParams& a,const SurfaceParams& b) {
    Require(a.ExactMatch(b) && a.size==b.size && a.end==b.end && a.res_scale==b.res_scale &&
        a.type==b.type,"FromInterval is not idempotent for normalized direct parameters");
}
Surface Source(const SurfaceParams& p,unsigned pattern,bool fill) {
    auto s=std::make_shared<CachedSurface>();
    static_cast<SurfaceParams&>(*s)=p;
    if(fill) {
        // Real fills have no pixel layout; only their byte span/pattern matters.
        s->type=SurfaceType::Fill;s->pixel_format=PixelFormat::Invalid;
        s->fill_size=pattern%3+2;s->fill_data={0x66,0x66,0x66,0x66};
    }
    const auto unit=p.BytesInPixels(p.is_tiled?64:1);
    const auto span=p.size;
    if(pattern==1) s->invalid_regions.add({p.addr,p.addr+std::min(unit,span)});
    if(pattern==2) s->invalid_regions.add({p.addr+span/3,p.addr+2*span/3});
    if(pattern==3) {
        s->invalid_regions.add({p.addr+span/4,p.addr+span/4+1});
        s->invalid_regions.add({p.addr+3*span/4,p.addr+3*span/4+1});
    }
    if(pattern==4) s->invalid_regions.add(p.GetInterval());
    return s;
}
void Run() {
    Common::FrameTiming::counters.enabled=false;
    // All actual format enum values, except the invalid gap. Linear 4bpp is
    // excluded: production pixel-alignment unit is zero there, not supported.
    for(unsigned format:{0u,1u,2u,3u,4u,5u,6u,7u,8u,9u,10u,11u,12u,13u,14u,16u,17u})
    for(bool tiled:{false,true}) {
        const auto fmt=static_cast<PixelFormat>(format);
        if(!tiled && SurfaceParams::GetFormatBpp(fmt)<8)continue;
        for(unsigned width:{8u,16u,24u})for(unsigned padding:{0u,8u})
        for(unsigned height:{8u,24u})for(unsigned scale:{1u,2u,3u}) {
            // Cached backings have width==stride (GetSurface asserts this;
            // GetSurfaceSubRect expands padded requests to gapless storage).
            SurfaceParams dst{};dst.addr=0x100000;dst.width=dst.stride=width+padding;
            dst.height=height;dst.is_tiled=tiled;dst.pixel_format=fmt;dst.res_scale=scale;dst.UpdateParams();
            auto requested=dst;requested.width=width;requested.UpdateParams();
            const auto unit=dst.BytesInPixels(tiled?64:1);
            const auto row=dst.BytesInPixels(dst.stride*(tiled?8:1));
            // Odd byte boundaries, exact units/row seams, padded last rows,
            // full extents and short spans all start from a legal outer surface.
            std::vector<unsigned> points{0,1,unit-1,unit,row-1,row,row+1,dst.size/2,requested.size,dst.size-1,dst.size};
            std::sort(points.begin(),points.end());points.erase(std::unique(points.begin(),points.end()),points.end());
            for(std::size_t j=0;j+1<points.size();++j) {
                if(points[j]>=dst.size)continue;
                const unsigned end=std::min(dst.size,points[j+1]);
                if(end<=points[j])continue;
                for(unsigned ending:{end,dst.size}) {
                    const SurfaceInterval invalid(dst.addr+points[j],dst.addr+ending);
                    const auto p=dst.FromInterval(invalid);
                    Same(p,p.FromInterval(invalid));
                    for(unsigned pattern=0;pattern<5;++pattern)for(bool fill:{false,true}) {
                        auto s=Source(dst,pattern,fill);
                        // Direct selected-vs-recomputed interval, including no-hit
                        // cases and valid-region fragmentation at arbitrary bytes.
                        const auto selected=p.FromInterval(invalid).GetCopyableInterval(s);
                        const auto recomputed=p.GetCopyableInterval(s);
                        Require(selected==recomputed,"direct intervals differ");
                        // Three candidates exercise higher-scale ranking and
                        // invalid-region length/tie policy in actual FindMatch.
                        auto higher=Source(dst,(pattern+1)%5,fill);higher->res_scale=scale+1;
                        auto alias=dst;alias.addr+=unit;alias.width=alias.stride=dst.width+8;alias.UpdateParams();
                        auto alternate=Source(alias,(pattern+2)%5,false);
                        SurfaceCache cache;
                        for(const auto& candidate:{s,higher,alternate})
                            cache.add({candidate->GetInterval(),SurfaceSet{candidate}});
                        const auto original=FindMatch<MatchFlags::Copy>(cache,p,ScaleMatch::Ignore,invalid);
                        SurfaceInterval returned(dst.addr+3,dst.addr+5);
                        const auto exported=FindMatch<MatchFlags::Copy>(cache,p,ScaleMatch::Ignore,
                                                                       invalid,nullptr,false,&returned);
                        queries+=2;
                        Require(exported==original,"outparam changed selected source identity");
                        if(!original) {
                            ++misses;
                            Require(boost::icl::length(returned)==0,"miss left stale output interval");
                            continue;
                        }
                        ++hits;fill_hits+=original->type==SurfaceType::Fill;
                        const auto winning=p.FromInterval(invalid).GetCopyableInterval(original);
                        const auto old_interval=p.GetCopyableInterval(original);
                        Require(winning==old_interval,"winning interval differs from legacy recomputation");
                        Require(returned==old_interval,"exported interval differs from legacy recomputation");
                        Require(boost::icl::length(winning&invalid)>0,"winner cannot make progress");
                        Require(original->IsRegionValid(winning),"winner covers invalid source bytes");
                        Require(original->CanCopy(p,winning),"winner lost source copyability");
                        const auto actual=dst.FromInterval(winning);
                        Require(actual.GetInterval()==winning,"copy interval is not destination rectangle");
                        const auto old_rect=dst.GetScaledSubRect(dst.FromInterval(old_interval));
                        const auto new_rect=dst.GetScaledSubRect(actual);
                        Require(old_rect==new_rect,"scaled destination rectangle changed");
                        if(original->type!=SurfaceType::Fill)
                            Require(original->GetScaledSubRect(actual)==original->GetScaledSubRect(dst.FromInterval(old_interval)),
                                    "scaled source rectangle changed");
                    }
                }
            }
        }
    }
    // Preserve the existing nonprogress fixture's discriminating case: an
    // arbitrary broad caller is NOT entitled to reuse FindMatch's narrow span.
    for(bool tiled:{false,true}) {
        SurfaceParams p{};p.addr=0x200000;p.width=p.stride=16;p.height=16;
        p.is_tiled=tiled;p.pixel_format=PixelFormat::RGBA8;p.res_scale=1;p.UpdateParams();
        auto s=Source(p,0,false);
        const unsigned unit=tiled?256:64;
        s->invalid_regions.add({p.addr+unit,p.addr+2*unit});
        const SurfaceInterval current(p.addr+unit/2,p.addr+2*unit);
        SurfaceCache cache;cache.add({s->GetInterval(),SurfaceSet{s}});
        Require(FindMatch<MatchFlags::Copy>(cache,p,ScaleMatch::Ignore,current)==s,
                "negative fixture did not select its overlapping source");
        const auto narrow=p.FromInterval(current).GetCopyableInterval(s);
        const auto broad=p.GetCopyableInterval(s);
        Require(boost::icl::length(narrow&current)>0,"negative fixture lacks overlapping selection");
        Require(boost::icl::length(broad&current)==0,"negative fixture lacks disjoint broad interval");
        Require(narrow!=broad,"negative fixture failed to discriminate general reuse");
    }
    Require(fill_hits>0 && misses>0,"fill winner or no-match path unexercised");
}
}
void FramebufferCandidates() {
    using namespace OpenGL;
    SurfaceParams q{};q.addr=0x2000;q.width=q.stride=16;q.height=16;
    q.is_tiled=true;q.pixel_format=PixelFormat::RGBA8;q.res_scale=1;q.UpdateParams();
    auto larger=q;larger.addr-=q.size;larger.height=32;larger.UpdateParams();
    auto parent=Source(larger,0,false);parent->registered=true;
    auto view=Source(q,4,false);view->registered=true;
    SurfaceCache cache;cache.add({parent->GetInterval(),SurfaceSet{parent}});cache.add({view->GetInterval(),SurfaceSet{view}});
    SurfaceSet pending;FramebufferCandidateVisits visits;
    Require(FindMatch<MatchFlags::SubRect|MatchFlags::Invalid>(cache,q,ScaleMatch::Exact)==parent,
            "legacy framebuffer must prefer valid parent");
    const auto before=view->invalid_regions;
    Require(FindTightFramebufferCandidate(cache,q,pending,visits)==view,
            "proposal must prefer fit before validity");
    Require(view->invalid_regions==before,"census changed invalid regions");
    Require(parent->registered && view->registered && pending.empty(),"census mutated lifetime state");
    for(unsigned guard=0;guard<6;++guard) {
        view->registered=true;view->custom_tex_info=nullptr;view->res_scale=1;
        view->is_tiled=true;view->type=SurfaceType::Color;pending.clear();
        if(guard==0)view->registered=false;
        if(guard==1)view->custom_tex_info=reinterpret_cast<const Core::CustomTexInfo*>(1);
        if(guard==2)view->res_scale=2;
        if(guard==3)view->is_tiled=false;
        if(guard==4)view->type=SurfaceType::Texture;
        if(guard==5)pending.insert(view);
        FramebufferCandidateVisits ignored;
        Require(FindTightFramebufferCandidate(cache,q,pending,ignored)==parent,"candidate guard failed");
    }
    view->registered=true;view->custom_tex_info=nullptr;view->res_scale=1;
    view->is_tiled=true;view->type=SurfaceType::Color;pending.clear();
    auto half=q;half.height=8;half.UpdateParams();auto narrow=Source(half,0,false);narrow->registered=true;
    SurfaceCache only_half;only_half.add({parent->GetInterval(),SurfaceSet{parent}});
    only_half.add({narrow->GetInterval(),SurfaceSet{narrow}});
    FramebufferCandidateVisits ignored;
    Require(FindTightFramebufferCandidate(only_half,q,pending,ignored)==parent,
            "viewport-only containment cannot satisfy full framebuffer request");
    // An ineligible half-size object splits the interval map: visits are not unique candidates.
    narrow->registered=false;cache.add({narrow->GetInterval(),SurfaceSet{narrow}});
    FramebufferCandidateVisits duplicated;
    Require(FindTightFramebufferCandidate(cache,q,pending,duplicated)==view,"split-map changed proposal");
    Require(duplicated.visited==5 && duplicated.compatible==4,"candidate visits were mistaken for unique objects");
    auto wrong=q;wrong.pixel_format=PixelFormat::RGB8;wrong.UpdateParams();FramebufferCandidateVisits no_scan;
    Require(!FindTightFramebufferCandidate(cache,wrong,pending,no_scan) && no_scan.visited==0,
            "non-RGBA8 query entered census");
}

void TightFramebufferGuards() {
    using namespace OpenGL;
    SurfaceParams q{};q.addr=0x8000;q.width=q.stride=16;q.height=16;
    q.is_tiled=true;q.res_scale=1;q.pixel_format=PixelFormat::RGBA8;q.UpdateParams();
    auto p=q;p.addr-=q.size;p.height*=2;p.UpdateParams();
    auto parent=Source(p,0,false), view=Source(q,4,false);
    parent->registered=view->registered=true;
    SurfaceCache cache;cache.add({parent->GetInterval(),SurfaceSet{parent}});
    cache.add({view->GetInterval(),SurfaceSet{view}});SurfaceSet pending;
    auto rect=parent->GetScaledSubRect(q);auto shifted=rect;shifted.bottom+=1;
    Require(view->GetScaledSubRect(q)==rect,"fixture lacks equal attachment coordinates");
    Pica::TexturingRegs regs{};
    const auto choose=[&](bool shadow=false,const Surface& depth=nullptr) {
        return SelectTightFramebufferBacking(cache,q,pending,parent,depth,rect,rect,shadow,regs.GetTextures());
    };
    const auto invalid=view->invalid_regions;
    Require(choose()==view,"invalid tight view not selected before normal validation");
    Require(view->invalid_regions==invalid,"selector must not validate or mutate the view");
    Require(choose(true)==parent,"shadow framebuffer entered experiment");
    Require(choose(false,parent)==view,"matching depth rectangle rejected");
    Require(SelectTightFramebufferBacking(cache,q,pending,parent,parent,rect,shifted,false,regs.GetTextures())==parent,
            "different depth coordinates accepted");
    Require(SelectTightFramebufferBacking(cache,q,pending,parent,nullptr,shifted,shifted,false,regs.GetTextures())==parent,
            "different color coordinates accepted");
    pending.insert(view);Require(choose()==parent,"pending removal accepted");pending.clear();
    view->max_level=1;Require(choose()==parent,"mipped candidate accepted");view->max_level=0;
    parent->registered=false;Require(choose()==parent,"unregistered legacy replaced");parent->registered=true;
    auto overlapping=p;overlapping.height=24;overlapping.addr=q.addr-q.size/2;overlapping.UpdateParams();
    auto overlap_parent=Source(overlapping,0,false);overlap_parent->registered=true;
    SurfaceCache overlap_cache;overlap_cache.add({overlap_parent->GetInterval(),SurfaceSet{overlap_parent}});
    overlap_cache.add({view->GetInterval(),SurfaceSet{view}});
    auto overlap_rect=overlap_parent->GetScaledSubRect(q);
    Require(overlap_rect==view->GetScaledSubRect(q),"overlap witness lacks matching coordinates");
    Require(SelectTightFramebufferBacking(overlap_cache,q,pending,overlap_parent,nullptr,
        overlap_rect,overlap_rect,false,regs.GetTextures())==overlap_parent,
        "partially overlapping padded views entered partitioned experiment");
    for(unsigned unit=0;unit<3;++unit) {
        for(unsigned guard=0;guard<9;++guard) {
            regs={};
            if(unit==0)regs.main_config.texture0_enable.Assign(1);
            if(unit==1)regs.main_config.texture1_enable.Assign(1);
            if(unit==2)regs.main_config.texture2_enable.Assign(1);
            auto& t=unit==0?regs.texture0:unit==1?regs.texture1:regs.texture2;
            t.width.Assign(16);t.height.Assign(16);t.address.Assign((q.addr+0x10000)/8);
            if(guard==0)t.address.Assign(q.addr/8);
            if(guard==1)t.lod.max_level.Assign(1);
            if(guard==2)t.width.Assign(0);
            if(guard==3)t.height.Assign(7);
            if(guard==4)t.address.Assign(0);
            if(guard==5) {
                if(unit==0)regs.texture0_format.Assign(static_cast<Pica::TexturingRegs::TextureFormat>(14));
                if(unit==1)regs.texture1_format.Assign(static_cast<Pica::TexturingRegs::TextureFormat>(14));
                if(unit==2)regs.texture2_format.Assign(static_cast<Pica::TexturingRegs::TextureFormat>(14));
            }
            if(guard==6)t.type.Assign(Pica::TexturingRegs::TextureConfig::Shadow2D);
            if(guard==7)t.type.Assign(Pica::TexturingRegs::TextureConfig::TextureCube);
            const bool accepted=guard==8 || (unit!=0 && (guard==6 || guard==7));
            Require(choose()==(accepted?view:parent),"ordinary/special/feedback sampler guard disagreed");
        }
    }
}

int main(){try{Run();FramebufferCandidates();TightFramebufferGuards();std::cout<<"PASS checks="<<checks<<" queries="<<queries<<" hits="<<hits
    <<" fill_hits="<<fill_hits<<" misses="<<misses<<"\n";}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
