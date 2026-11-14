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
int main(){try{Run();std::cout<<"PASS checks="<<checks<<" queries="<<queries<<" hits="<<hits
    <<" fill_hits="<<fill_hits<<" misses="<<misses<<"\n";}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
