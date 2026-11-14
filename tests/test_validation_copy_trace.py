"""Exercise the actual collector with synthetic surfaces; no GL context or game data."""
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
        # Only dependencies are substituted: the production collector body is
        # compiled verbatim. Actual weak_ptr ownership and clocks remain real.
        prefix = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include "video_core/renderer_opengl/gl_validation_copy_profile.h"
using u32 = std::uint32_t; using u64 = std::uint64_t; using PAddr = u32;
namespace Common { template<class T> struct Rectangle { T left{}, top{}, right{}, bottom{}; }; }
namespace VideoCore { static u32 frame = 0; u32 GetCurrentFrame() { return frame; } }
namespace FileUtil {
static std::string directory;
enum class UserPath { UserDir };
std::string GetUserPath(UserPath) { return directory; }
}
namespace OpenGL {
struct SurfaceParams {
 u32 pixel_format=0,type=0,width=16,height=16,stride=16,res_scale=1,addr=4096,end=5120;
 bool is_tiled=true;
};
struct CachedSurface : SurfaceParams {
 struct { u32 handle=7; } texture;
 bool registered=true; const void* custom_tex_info=nullptr;
};
using Surface=std::shared_ptr<CachedSurface>;
struct RasterizerCacheOpenGL { struct ValidationCopyTrace; };
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
 trace->invalidations=9;
 trace->Invalidated(b,4100,4112,true);
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
 trace->invalidations=10; trace->Invalidated(b,4200,4208,false);
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
 return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='validation-copy-') as directory:
            directory = Path(directory)
            cpp, exe = directory / 'collector.cpp', directory / 'collector'
            cpp.write_text(prefix + collector + suffix)
            subprocess.run(['clang++', '-std=c++17', '-fsanitize=address,undefined',
                            '-I' + str(ROOT / 'emulator/src'), str(cpp), '-o', str(exe)],
                           check=True, capture_output=True, text=True)
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
            failed = json.loads((directory / 'surface-validation-copies.json').read_text())
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
            for name in ('overflow.json', 'empty.json', 'surface-validation-copies.json'):
                parsed = summary.summarize(json.loads((directory / name).read_text()))
                self.assertNotIn('per_frame', parsed)
            if os.environ.get('CARA_VALIDATION_COPY_FIXTURE_OUTPUT'):
                shutil.copytree(directory, os.environ['CARA_VALIDATION_COPY_FIXTURE_OUTPUT'],
                                dirs_exist_ok=True)


if __name__ == '__main__':
    unittest.main()
