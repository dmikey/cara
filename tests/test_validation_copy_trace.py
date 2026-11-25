"""Exercise the actual collector with synthetic surfaces; no GL context or game data."""
import copy
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'emulator/src/video_core/renderer_opengl/gl_rasterizer_cache.cpp'


@unittest.skipUnless(shutil.which('clang++'), 'clang++ is required for the collector fixture')
class ValidationCopyCollectorTests(unittest.TestCase):
    def test_actual_collector_lifetime_caps_clocks_and_json(self):
        source = SOURCE.read_text()
        begin = source.index('struct RasterizerCacheOpenGL::ValidationCopyTrace {')
        end = source.index('\nRasterizerCacheOpenGL::RasterizerCacheOpenGL()', begin)
        collector = source[begin:end]
        params_source = (ROOT / 'emulator/src/video_core/renderer_opengl/gl_surface_params.cpp').read_text()
        interval_begin = params_source.index('SurfaceInterval SurfaceParams::GetSubRectInterval(')
        interval_end = params_source.index('\nSurfaceInterval SurfaceParams::GetCopyableInterval(', interval_begin)
        interval_method = params_source[interval_begin:interval_end]
        wrapper_begin = source.index('GLuint RasterizerCacheOpenGL::GetTextureCopyHandle(')
        wrapper_end = source.index('\nvoid RasterizerCacheOpenGL::RecordFlush(', wrapper_begin)
        wrapper = source[wrapper_begin:wrapper_end]
        # Only dependencies are substituted: the production collector body is
        # compiled verbatim. Actual weak_ptr ownership and clocks remain real.
        prefix = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <chrono>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <boost/icl/interval.hpp>
#include "common/alignment.h"
#include "common/math_util.h"
#include "common/file_util.h"
#include "common/frame_timing.h"
#include "core/settings.h"
#include "cara/quad_input_capture_trace.h"
#include "video_core/renderer_opengl/gl_surface_invalidation_context.h"
#include "video_core/renderer_opengl/gl_validation_copy_profile.h"
using s32 = std::int32_t; using u32 = std::uint32_t; using u64 = std::uint64_t; using PAddr = u32; using GLuint=u32;

namespace VideoCore { static u32 frame = 0; u32 GetCurrentFrame() { return frame; } }
namespace FileUtil {
static std::string directory;
const std::string& GetUserPath(UserPath) { return directory; }
}
namespace OpenGL {
using SurfaceInterval=boost::icl::right_open_interval<PAddr>;
struct SurfaceParams {
 u32 pixel_format=0,type=0,width=16,height=16,stride=16,res_scale=1,addr=4096,end=5120,size=1024;
 bool is_tiled=true;
 u32 BytesInPixels(u32 pixels) const {return pixels*4;}
 SurfaceInterval GetSubRectInterval(Common::Rectangle<u32> rect) const;
 SurfaceInterval GetInterval() const {return {addr,end};}
 bool CanSubRect(const SurfaceParams& other) const {return addr<=other.addr && end>=other.end;}
 Common::Rectangle<u32> GetScaledSubRect(const SurfaceParams& other) const {return {0,other.height,other.width,0};}
};
struct CachedSurface : SurfaceParams {
 struct { u32 handle=7; } texture;
 struct { u32 handle=0; } texture_copy;
 u32 max_level=0, feedback_calls=0;
 u32 GetScaledWidth() const {return width*res_scale;}
 u32 GetScaledHeight() const {return height*res_scale;}
 static u32 GetGLBytesPerPixel(u32) {return 4;}
 u32 GetTextureCopyHandle() {++feedback_calls;if(!texture_copy.handle)texture_copy.handle=101;return texture_copy.handle;}
 bool IsRegionValid(SurfaceInterval interval) const {return interval.lower()>=addr && interval.upper()<=end;}
 bool registered=true; const void* custom_tex_info=nullptr;
};
using Surface=std::shared_ptr<CachedSurface>;
struct RasterizerCacheOpenGL { struct ValidationCopyTrace; std::unique_ptr<ValidationCopyTrace> validation_copy_trace; GLuint GetTextureCopyHandle(const Surface&); void ClearValidationCopyDrawAttachments(); void RecordValidationCopyDrawAttachments(const Surface&, const Surface&); };
'''
        suffix = r'''
} // namespace OpenGL
int main(int argc, char** argv) {
 using Trace = OpenGL::RasterizerCacheOpenGL::ValidationCopyTrace;
 assert(argc == 2); FileUtil::directory=std::string(argv[1])+"/";
 setenv("CARA_SURFACE_VALIDATION_COPY_TRACE_START_FRAME","300",1);
 setenv("CARA_SURFACE_VALIDATION_COPY_TRACE_END_FRAME","1200",1);
 // Heap allocation mirrors production and avoids a large fixture stack object.
 auto trace=std::make_unique<Trace>();
 assert(!trace->InWindow());
 auto a=std::make_shared<OpenGL::CachedSurface>();
 trace->Created(a); auto first=trace->Snapshot(a);
 assert(first.identity.allocation==1 && first.identity.surface.expired());
 assert(a.use_count()==1); // Table and snapshots never hold a strong reference.
 std::weak_ptr<OpenGL::CachedSurface> old=a;
 a.reset(); assert(old.expired());
 auto b=std::make_shared<OpenGL::CachedSurface>(); // Same guest address, new lifetime.
 trace->Created(b); auto replacement=trace->Snapshot(b);
 assert(replacement.identity.allocation==2 && replacement.params.addr==first.params.addr);
 trace->invalidations=8;
 auto observed=trace->BeginInvalidation(b,4100,4112,OpenGL::SurfaceInvalidationOrigin::DrawColor,nullptr);
 assert(trace->Snapshot(b).identity.own_write.serial==9);
 assert(trace->Snapshot(b).identity.invalidation==0); // Own write is not alias invalidation.
 assert(trace->Snapshot(b).identity.own_write.owner_id==2);
 trace->Invalidated(b,4100,4112,true,observed);
 auto invalidated=trace->Snapshot(b);
 assert(invalidated.identity.invalidation==9 && invalidated.identity.reason==2);
 assert(invalidated.identity.invalid_start==4100 && invalidated.identity.invalid_end==4112);
 auto c=std::make_shared<OpenGL::CachedSurface>();
 auto unknown=trace->Snapshot(c); assert(unknown.identity.allocation==0);
 // 256 live identities exhaust the table; the expired first slot was recycled.
 std::array<OpenGL::Surface,255> live{};
 for(auto& s:live) { s=std::make_shared<OpenGL::CachedSurface>(); trace->Created(s); }
 trace->Created(c);
 assert(trace->untracked_allocations==1 && trace->Snapshot(c).identity.allocation==0);
 VideoCore::frame=300; assert(trace->InWindow());
 for(u32 n=0;n<1000;++n) {
   Trace::Entry e; e.invocation=n+1; e.fragment=1; e.frame=300+n%900;
   e.src=trace->Snapshot(b); e.dst=e.src;
   // More than64 distinct bucket keys; first key recurs after detail exhaustion.
   e.dst.params.stride=n<70 ? n+1 : 1;
   e.requested_start=4096; e.requested_end=5120;
   e.invalid_start=4100; e.invalid_end=4200;
   e.actual_start=4100; e.actual_end=4164; e.bytes=64;
   e.wall={true,n}; e.cpu={n%2==0,n/2};
   trace->Record(e);
 }
 assert(b.use_count()==1 && trace->totals.calls==1000);
 assert(trace->detail_slots.used==512 && trace->detail_overflow.calls==488);
 assert(trace->bucket_count==64 && trace->bucket_overflow.calls==6);
 VideoCore::frame=1200; assert(!trace->InWindow());
 trace->Created(c); assert(trace->post_window_allocations==1);
 trace->invalidations=10; trace->Invalidated(b,4200,4208,false,observed);
 assert(trace->Snapshot(b).identity.invalidation==9); // No post-window weak scan/update.
 trace->Write();
 std::rename((FileUtil::directory+"surface-validation-copies.json").c_str(),
             (FileUtil::directory+"overflow.json").c_str());
 auto empty=std::make_unique<Trace>(); empty->Write();
 std::rename((FileUtil::directory+"surface-validation-copies.json").c_str(),
             (FileUtil::directory+"empty.json").c_str());
 auto failed=std::make_unique<Trace>(); Trace::Entry e;
 e.invocation=1; e.fragment=1; e.frame=300; e.bytes=1;
 e.requested_start=e.invalid_start=e.actual_start=4096;
 e.requested_end=e.invalid_end=e.actual_end=4097;
 e.src=trace->Snapshot(b); e.dst=trace->Snapshot(c); e.source_valid=true;
 e.wall={false,0}; e.cpu={false,0}; failed->Record(e); failed->Write();
 std::rename((FileUtil::directory+"surface-validation-copies.json").c_str(),
             (FileUtil::directory+"failed-clocks.json").c_str());
 auto damage=std::make_unique<Trace>(); VideoCore::frame=300;
 auto owner=std::make_shared<OpenGL::CachedSurface>(); damage->Created(owner);
 OpenGL::SurfaceInvalidationContext ctx; ctx.valid=true;ctx.mode=3;ctx.scale=1;
 ctx.viewport=ctx.actual_damage={0,0,16,16};ctx.shader_scissor={0,0,8,8};
 ctx.color_write=true;ctx.color_mask=15;
 auto write=damage->BeginInvalidation(owner,4096,5120,OpenGL::SurfaceInvalidationOrigin::DrawColor,&ctx);
 assert(write.serial==1 && write.owner_id==1 && owner.use_count()==1);
 assert(damage->Snapshot(owner).identity.own_write.serial==1);
 assert(damage->Snapshot(owner).identity.invalidation==0);
 assert(damage->damage_totals.calls==1 && damage->damage_totals.actual_interval_bytes==1024);
 assert(damage->damage_totals.hypothetical_interval_bytes==256);
 assert(damage->damage_totals.hypothetical_reduction_bytes==768);
 // Two duplicate interval-map visits must not add top-level ledger events.
 damage->Invalidated(c,4096,4160,true,write);damage->Invalidated(c,4160,4224,true,write);
 assert(damage->damage_totals.calls==1);
 Trace::Entry copied;copied.invocation=1;copied.fragment=1;copied.frame=300;
 copied.src=damage->Snapshot(owner);copied.dst=damage->Snapshot(c);
 copied.requested_start=copied.invalid_start=copied.actual_start=4096;
 copied.requested_end=copied.invalid_end=copied.actual_end=4160;copied.bytes=64;
 copied.source_valid=true;copied.wall={true,10};copied.cpu={true,8};damage->Record(copied);
 auto cpu=damage->BeginInvalidation({},4096,4104,OpenGL::SurfaceInvalidationOrigin::Unknown,nullptr);
 assert(!cpu.gpu_owner && cpu.origin==OpenGL::SurfaceInvalidationOrigin::CPU);
 auto unknown_owner=std::make_shared<OpenGL::CachedSurface>();
 damage->BeginInvalidation(owner,4096,5120,OpenGL::SurfaceInvalidationOrigin::DisplayTransfer,nullptr);
 damage->BeginInvalidation(owner,4096,5120,OpenGL::SurfaceInvalidationOrigin::TextureCopy,nullptr);
 auto unknown_write=damage->BeginInvalidation(unknown_owner,4096,5120,OpenGL::SurfaceInvalidationOrigin::Fill,nullptr);
 assert(!unknown_write.owner_id && damage->unknown_owner_events==1);
 // Guarded stencil/unknown modes and interval mismatch cannot claim reduction.
 ctx.has_stencil=ctx.stencil_test=true;ctx.stencil_mask=255;
 damage->BeginInvalidation(owner,4096,5120,OpenGL::SurfaceInvalidationOrigin::DrawDepth,&ctx);
 ctx.has_stencil=ctx.stencil_test=false;ctx.stencil_mask=0;
 ctx.damage_narrowing_active=true;
 damage->BeginInvalidation(owner,4096,5120,OpenGL::SurfaceInvalidationOrigin::DrawColor,&ctx);
 ctx.damage_narrowing_active=false;
 damage->BeginInvalidation(owner,4096,5000,OpenGL::SurfaceInvalidationOrigin::DrawColor,&ctx);
 for(unsigned n=0;n<70;++n) {
  owner->stride=owner->width=16+n;
  damage->BeginInvalidation(owner,4096,5120,OpenGL::SurfaceInvalidationOrigin::Unknown,nullptr);
 }
 assert(damage->damage_bucket_count==64 && damage->damage_overflow.calls>0);
 damage->Write();
 std::rename((FileUtil::directory+"surface-validation-copies.json").c_str(),
             (FileUtil::directory+"damage.json").c_str());
 auto census=std::make_unique<Trace>();
 auto fb_parent=std::make_shared<OpenGL::CachedSurface>();fb_parent->height=32;fb_parent->end=6144;fb_parent->size=2048;
 auto fb_view=std::make_shared<OpenGL::CachedSurface>();fb_view->addr=5120;fb_view->end=6144;fb_view->texture.handle=8;
 census->Created(fb_parent);census->Created(fb_view);
 for(unsigned n=0;n<200;++n) {
  Trace::FramebufferEvent f;f.frame=300+n;f.proposed_present=true;f.legacy_present=true;
  f.original=f.effective=static_cast<const OpenGL::SurfaceParams&>(*fb_view);
  f.changed=n%2==0;f.same_rect=true;f.depth_compatible=true;
  f.legacy=census->Compact(fb_parent);f.proposed=census->Compact(f.changed?fb_view:fb_parent);
  f.legacy_rect=f.legacy_query_rect=f.proposed_rect={0,16,16,0};
  f.feedback_risk=n%5==0;f.mip_or_special_risk=n%7==0;
  if(f.mip_or_special_risk)f.proposed.max_level=1;
  if(f.feedback_risk) {
   f.samplers[0].enabled=f.samplers[0].prediction_available=f.samplers[0].exact_proposal_layout=true;
   f.samplers[0].params=f.proposed.params;
   f.samplers[0].address=f.proposed.params.addr;f.samplers[0].width=f.proposed.params.width;
   f.samplers[0].height=f.proposed.params.height;f.samplers[0].format=f.proposed.params.pixel_format;
  }
  f.legacy_full_valid=true;f.legacy_viewport_valid=true;
  f.proposed_full_valid=f.proposed_viewport_valid=!f.changed;f.visits=3;f.compatible_visits=f.changed?2:1;
  census->RecordFramebuffer(f);
 }
 assert(census->census_totals.calls==200 && census->census_count==128 && census->census_overflow.calls==72);
 census->Write();std::rename((FileUtil::directory+"surface-validation-copies.json").c_str(),(FileUtil::directory+"census.json").c_str());
 auto feedback=std::make_unique<Trace>();
 auto image=std::make_shared<OpenGL::CachedSurface>();image->width=image->height=image->stride=8;image->end=4352;image->size=256;
 feedback->Created(image);
 for(unsigned n=0;n<200;++n) {
  Trace::FeedbackEvent f;f.frame=300+n;f.source=feedback->Compact(image);f.copy_handle=101;
  f.texels=64;f.bytes=256;f.footprint_valid=n!=199;f.cold=n==0;f.wall={true,n};f.cpu={n%2==0,n/2};
  feedback->RecordFeedback(f);
 }
 assert(image.use_count()==1 && feedback->feedback_totals.timing.calls==200);
 assert(feedback->feedback_count==128 && feedback->feedback_overflow.timing.calls==72);
 assert(feedback->feedback_totals.footprint_failures==1 && feedback->feedback_totals.cold_allocations==1);
 feedback->Write();std::rename((FileUtil::directory+"surface-validation-copies.json").c_str(),(FileUtil::directory+"feedback.json").c_str());
 // Compile and execute the actual wrapper: disabled/default and post-window paths
 // call the original once, and active observation does not add another snapshot.
 OpenGL::RasterizerCacheOpenGL cache;
 const auto baseline_calls=image->feedback_calls;
 assert(cache.GetTextureCopyHandle(image)==101 && image->feedback_calls==baseline_calls+1);
 assert(!cache.validation_copy_trace); // No implicit diagnostic enablement.
 cache.validation_copy_trace=std::make_unique<Trace>();VideoCore::frame=300;
 cache.validation_copy_trace->Created(image);
 assert(cache.GetTextureCopyHandle(image)==101 && image->feedback_calls==baseline_calls+2);
 assert(cache.validation_copy_trace->feedback_totals.timing.calls==1);
 VideoCore::frame=1200;
 assert(cache.GetTextureCopyHandle(image)==101 && image->feedback_calls==baseline_calls+3);
 assert(cache.validation_copy_trace->feedback_totals.timing.calls==1);
 // The real timing wrapper now references shared FrameTiming; test both its
 // default disabled path above and enabled feedback accounting independently
 // of the validation-copy collector's window.
 auto& timing=Common::FrameTiming::counters;
 assert(timing.texture_feedback_copies==0 && timing.texture_feedback_copy_bytes==0);
 setenv("CARA_RENDER_BOOKKEEPING_PROFILE","1",1);
 timing.enabled=true;
 assert(cache.GetTextureCopyHandle(image)==101 && image->feedback_calls==baseline_calls+4);
 assert(timing.texture_feedback_copies==1 && timing.texture_feedback_copy_bytes==256);
 assert(cache.validation_copy_trace->feedback_totals.timing.calls==1);
 timing.enabled=false;
 return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='validation-copy-') as directory:
            directory = Path(directory)
            cpp, exe = directory / 'collector.cpp', directory / 'collector'
            cpp.write_text(prefix + interval_method + collector + wrapper + suffix)
            compile_result = subprocess.run(['clang++', '-std=c++17', '-Wno-invalid-specialization', '-fsanitize=address,undefined',
                            '-I' + str(ROOT / 'emulator/externals/fmt/include'), '-I' + str(ROOT / 'emulator/externals/nihstro/include'), '-I' + str(ROOT / 'emulator/src'), '-I' + str(ROOT / 'emulator/externals/boost'), str(cpp), '-o', str(exe)],
                           capture_output=True, text=True)
            self.assertEqual(compile_result.returncode, 0, compile_result.stderr)
            subprocess.run([str(exe), str(directory)], check=True, capture_output=True, text=True)
            report = json.loads((directory / 'overflow.json').read_text())
            self.assertFalse(report['common_counters_required'])
            self.assertEqual(report['totals']['calls'], 1000)
            self.assertEqual(report['totals']['guest_bytes'], 64000)
            self.assertEqual(len(report['details']), 512)
            self.assertEqual(len(report['buckets']), 64)
            for field in ('calls', 'guest_bytes', 'wall_ns', 'thread_cpu_ns',
                          'wall_samples', 'cpu_samples', 'wall_failures', 'cpu_failures'):
                self.assertEqual(report['totals'][field],
                                 sum(b['totals'][field] for b in report['buckets']) +
                                 report['bucket_overflow'][field], field)
            self.assertEqual(report['totals']['calls'],
                             len(report['details']) + report['detail_overflow']['calls'])
            for field, detail in [('guest_bytes', 'guest_bytes'), ('wall_ns', 'wall_ns'),
                                  ('thread_cpu_ns', 'thread_cpu_ns')]:
                self.assertEqual(report['totals'][field],
                                 sum(e[detail] or 0 for e in report['details']) +
                                 report['detail_overflow'][field])
            self.assertEqual(report['totals']['cpu_failures'], 500)
            empty = json.loads((directory / 'empty.json').read_text())
            self.assertEqual(empty['totals']['calls'], 0)
            self.assertIsNone(empty['actual_min_frame'])
            self.assertIsNone(empty['actual_max_frame'])
            failed = json.loads((directory / 'failed-clocks.json').read_text())
            self.assertIsNone(failed['details'][0]['wall_ns'])
            self.assertIsNone(failed['details'][0]['thread_cpu_ns'])
            self.assertEqual(failed['totals']['wall_failures'], 1)
            self.assertEqual(failed['totals']['cpu_failures'], 1)
            # Cross-check real emitted reports with the offline schema validator,
            # including bounded overflow and failed clocks (not only a model).
            import importlib.util
            spec = importlib.util.spec_from_file_location('validation_copy_summary',
                ROOT / 'tools/summarize_validation_copies.py')
            summary = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(summary)
            for name in ('overflow.json', 'empty.json', 'failed-clocks.json', 'damage.json', 'census.json', 'feedback.json'):
                parsed = summary.summarize(json.loads((directory / name).read_text()))
                self.assertNotIn('per_frame', parsed)
            # Malformed nested reports must fail before analysis can suggest a
            # selection benefit. Mutate actual emitted reports, not hand-built
            # copies of the collector's arithmetic.
            census = json.loads((directory / 'census.json').read_text())
            feedback = json.loads((directory / 'feedback.json').read_text())
            for kind, base, mutations in (
                ('census', census, (
                    lambda c: c['totals'].__setitem__('calls', 199),
                    lambda c: c['classes'].__setitem__(0, c['classes'][0] + 1),
                    lambda c: c.__setitem__('actual_max_frame', 1200),
                    lambda c: c.__setitem__('class_saturations', 1),
                    lambda c: c['details'][0].__setitem__('frame', 299),
                    lambda c: c['details'][0]['proposal'].__setitem__('registered', False),
                    lambda c: c['details'][0]['proposal'].__setitem__('custom', True),
                    lambda c: c['details'][0]['effective'].__setitem__('size', 1),
                    lambda c: c['details'][0]['samplers'][0].__setitem__('enabled', False),
                    lambda c: c['details'][0].__setitem__('same_rect', False),
                    lambda c: c['details'][0].__setitem__('candidate_visits', 0),
                )),
                ('feedback', feedback, (
                    lambda c: c['totals'].__setitem__('calls', 199),
                    lambda c: c['totals'].__setitem__('cold_allocations', 201),
                    lambda c: c['detail_overflow'].__setitem__('footprint_failures', 999),
                    lambda c: c['totals'].__setitem__('timing_integer_saturations', 1),
                    lambda c: c['details'][0].__setitem__('frame', 299),
                    lambda c: c['details'][0].__setitem__('wall_ns', -1),
                    lambda c: c['details'][0].__setitem__('source', None),
                    lambda c: c['details'][0].__setitem__('allocation_attempt', 1),
                )),
            ):
                key = 'framebuffer_census' if kind == 'census' else 'feedback'
                for index, mutate in enumerate(mutations):
                    malformed = copy.deepcopy(base)
                    mutate(malformed[key])
                    with self.subTest(nested=kind, mutation=index), self.assertRaises(ValueError):
                        summary.summarize(malformed)
            damage = json.loads((directory / 'damage.json').read_text())
            self.assertEqual(damage['schema_version'], 2)
            ledger = damage['hypothetical_damage']
            for key in ('calls', 'actual_interval_bytes', 'hypothetical_interval_bytes',
                        'hypothetical_reduction_bytes', 'integer_saturations'):
                self.assertEqual(ledger['totals'][key], sum(b['totals'][key] for b in ledger['buckets']) + ledger['overflow'][key])
            event = damage['details'][0]['src']['last_owner_write']
            self.assertEqual(event['owner_allocation_id'], 1)
            self.assertEqual(event['serial'], 1)
            self.assertEqual(event['draw']['shader_scissor'], [0, 0, 8, 8])
            self.assertEqual(damage['unknown_gpu_owner_events'], 1)
            self.assertGreater(ledger['overflow']['calls'], 0)
            self.assertEqual({b['origin'] for b in ledger['buckets']}, set(range(7)))
            self.assertEqual(ledger['totals']['calls'], 78)
            self.assertTrue(any(b['eligibility'] == 7 and b['totals']['hypothetical_reduction_bytes'] == 0 for b in ledger['buckets']))
            if os.environ.get('CARA_VALIDATION_COPY_FIXTURE_OUTPUT'):
                shutil.copytree(directory, os.environ['CARA_VALIDATION_COPY_FIXTURE_OUTPUT'],
                                dirs_exist_ok=True)


if __name__ == '__main__':
    unittest.main()
