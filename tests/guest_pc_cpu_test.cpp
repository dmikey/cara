// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/arm/fastinterp/guest_pc_cpu_profile.h"
#include <cassert>
#include <iostream>
#include <sstream>
using namespace Core::FastInterp::GuestPCCPU;
struct Fake {
    std::uint64_t now = 0, calls = 0;
    bool fail = false;
    static bool Read(void* p, std::uint64_t& out) {
        auto& f = *static_cast<Fake*>(p); ++f.calls;
        if (f.fail) return false;
        out = f.now; return true;
    }
};
static Config Configuration() {
    auto c = Parse("poll=0x1000:0x1100;convert=0x2000:0x2100");
    c.start_frame = 2; c.window_frames = 2; c.period_frames = 4; c.windows = 2;
    return c;
}
static std::string EpisodeFixtures() {
    auto c = Configuration(); c.episode_mode = true; c.episode_start = 0x1000; c.episode_end = 0x1100;
    c.windows = 1;
    Fake f; Tracker t(c, Fake::Read, &f); t.Calibrate();
    t.BeginRun(0, 1); t.Block(0x1000, 0x1004, 1); t.EndRun();
    assert(f.calls == 64 && t.Threads()[0].episode.active);
    t.BeginRun(2, 1); t.Block(0x2000, 0x2004, 1); f.now += 10;
    const auto reads = f.calls;
    t.Block(0x1000, 0x1004, 1); t.EnterExternal();
    t.SwitchThread(2); f.now += 20; // SVC remains caller-owned through LoadContext
    assert(f.calls == reads);
    t.LeaveExternal(2); f.now += 30;
    t.Block(0x2000, 0x2004, 2); f.now += 10;
    t.SwitchThread(1); t.Block(0x2000, 0x2004, 1); f.now += 40;
    t.EndRun();
    assert(t.Threads()[0].episode.current_cpu == 70);
    assert(t.Threads()[1].buckets[Episode].cpu_ns == 0);
    f.now += 10000; // outside Run is never charged
    t.BeginRun(3, 1); t.Block(0x1000, 0x1004, 1); f.now += 50;
    t.Block(0x1100, 0x1104, 1); f.now += 7; t.EndRun();
    t.BeginRun(4, 1); t.EndRun();
    const auto& e = t.Threads()[0].episode;
    assert(e.started == 1 && e.completed == 1 && e.reentries == 2 && !e.active);
    assert(e.partial_cpu == 120 && e.partial_sampled == 1 && e.completed_cpu == 0);
    std::ostringstream json; t.Write(json);
    assert(json.str().find("\"coverage_complete\":true") != std::string::npos);

    // A complete episode, repeated entry, helpers and SVC require only entry/exit clocks.
    c.start_frame = 0; c.window_frames = 1; c.period_frames = 2;
    Tracker complete(c, Fake::Read, &f); complete.BeginRun(0, 1);
    complete.Block(0x1000, 0x1004, 1); const auto before = f.calls;
    for (unsigned i = 0; i < 10; ++i) {
        complete.Block(0x2000, 0x2004, 1); complete.EnterExternal();
        complete.LeaveExternal(1); complete.Block(0x1000, 0x1004, 1); f.now += 1;
    }
    assert(f.calls == before);
    complete.Block(0x1100, 0x1104, 1); complete.EndRun(); complete.BeginRun(1, 1);
    assert(complete.Threads()[0].episode.completed_cpu == 10);

    // An unexpected PC aborts before attributing that block; sampled abort invalidates coverage.
    Tracker abort(c, Fake::Read, &f); abort.BeginRun(0, 1); abort.Block(0x1000, 0x1004, 1);
    f.now += 12; abort.Block(0x3000, 0x3004, 1); f.now += 5; abort.EndRun(); abort.BeginRun(1, 1);
    assert(abort.Threads()[0].episode.aborted_cpu == 12);
    assert(abort.Threads()[0].episode.sampled_aborted == 1);
    std::ostringstream bad; abort.Write(bad); assert(bad.str().find("\"coverage_complete\":false") != std::string::npos);

    Tracker zero_abort(c, Fake::Read, &f); zero_abort.BeginRun(0, 1);
    zero_abort.Block(0x1000, 0x1004, 1); zero_abort.Block(0x3000, 0x3004, 1); zero_abort.EndRun();
    assert(zero_abort.Threads()[0].episode.sampled_aborted == 1);
    // Closing after the sample window is a partial episode, even when it started sampled.
    Tracker cut(c, Fake::Read, &f); cut.BeginRun(0, 1); cut.Block(0x1000, 0x1004, 1);
    f.now += 9; cut.EndRun(); cut.BeginRun(1, 1); cut.Block(0x1100, 0x1104, 1);
    assert(cut.Threads()[0].episode.partial_cpu == 9 && cut.Threads()[0].episode.partial_sampled == 1);
    const auto no_reads = f.calls;
    cut.Block(0x1000, 0x1004, 1); cut.Block(0x3000, 0x3004, 1);
    assert(f.calls == no_reads && cut.Threads()[0].episode.aborted == 1 && cut.Threads()[0].episode.sampled_aborted == 0);
    return "{\"cores\":[{\"core\":0,\"guest_pc_cpu\":" + json.str() + "}]}\n";
}

int main(int argc, char** argv) {
    const auto episode_json = EpisodeFixtures();
    if (argc > 1 && std::string(argv[1]) == "--episode-json") { std::cout << episode_json; return 0; }
    assert(!Parse("").enabled);
    for (const auto* text : {"x=1:1", "x=0:2;y=1:3", "x=0:2;x=4:8", "x=0:2;", "bad label=1:2", "svc_external=1:2"})
        assert(!Parse(text).error.empty());
    Fake f;
    Tracker disabled({}, Fake::Read, &f);
    disabled.BeginRun(0, 1); disabled.Block(0x1000, 0x1004, 1); disabled.EnterExternal(); disabled.EndRun();
    assert(f.calls == 0);
    auto c = Configuration();
    Tracker t(c, Fake::Read, &f);
    t.Calibrate(); assert(f.calls == 64);
    t.BeginRun(10, 1); assert(!t.Active());
    t.BeginRun(11, 1); assert(!t.Active());
    f.now = 100; t.BeginRun(12, 1);
    f.now = 110; t.Block(0x1000, 0x1004, 1);
    const auto before = f.calls;
    f.now = 120; t.Block(0x1004, 0x1008, 1); // warm/chained same-category block: no clock
    assert(f.calls == before);
    f.now = 130; t.Block(0x2000, 0x2004, 1);
    f.now = 150; t.EnterExternal();
    f.now = 250; t.LeaveExternal(2); // an SVC switched guest threads
    f.now = 260; t.Block(0x1000, 0x1004, 2);
    f.now = 280; t.EndRun();
    assert(t.CPUTime() == 180);
    assert(t.Threads()[0].buckets[2].cpu_ns == 20);
    assert(t.Threads()[0].buckets[3].cpu_ns == 20);
    assert(t.Threads()[0].buckets[External].cpu_ns == 100);
    assert(t.Threads()[1].buckets[2].cpu_ns == 20);
    f.now = 10000; // outside-Run/HLE callbacks are not charged
    t.BeginRun(13, 2); f.now += 10; t.EndRun();
    t.BeginRun(14, 2); assert(!t.Active());
    std::ostringstream partial; t.Write(partial);
    assert(partial.str().find("\"coverage_complete\":false") != std::string::npos);
    for (unsigned frame : {16u, 17u}) { t.BeginRun(frame, 1); f.now += 10; t.EndRun(); }
    t.BeginRun(18, 1); // closes the second window without sampling the gap
    std::ostringstream out; t.Write(out);
    assert(out.str().find("\"coverage_complete\":true") != std::string::npos);
    assert(out.str().find("\"sampled_frame_ids\":[12,13,16,17]") != std::string::npos);
    std::uint64_t total = 0;
    for (const auto& thread : t.Threads()) for (const auto& bucket : thread.buckets) total += bucket.cpu_ns;
    assert(total == t.CPUTime() && total == 210);
    if (argc > 1) { std::cout << "{\"cores\":[{\"core\":0,\"guest_pc_cpu\":" << out.str() << "}]}\n"; return 0; }
    // Same-category helper nesting is disjoint address attribution, not an inferred call tree.
    Tracker nested(c, Fake::Read, &f);
    nested.BeginRun(0, 1); nested.BeginRun(2, 1);
    nested.Block(0x1000, 0x1004, 1); f.now += 7;
    nested.Block(0x2000, 0x2004, 1); f.now += 11;
    nested.Block(0x2004, 0x2008, 1); f.now += 13;
    nested.Block(0x1004, 0x1008, 1); f.now += 17; nested.EndRun();
    assert(nested.Threads()[0].buckets[2].cpu_ns == 24);
    assert(nested.Threads()[0].buckets[3].cpu_ns == 24);
    nested.BeginRun(1, 1); nested.EndRun(); // backward frame reset closes and resets session safely
    std::ostringstream reset; nested.Write(reset);
    assert(reset.str().find("\"frame_resets\":1") != std::string::npos);
    Tracker failure(c, Fake::Read, &f); failure.BeginRun(0, 1); failure.BeginRun(2, 1);
    f.fail = true; failure.Block(0x1000, 0x1004, 1); f.fail = false; failure.EndRun();
    std::ostringstream failed; failure.Write(failed);
    assert(failed.str().find("\"coverage_complete\":false") != std::string::npos);
    Tracker boundary(c, Fake::Read, &f); boundary.BeginRun(0, 1); boundary.BeginRun(2, 1);
    boundary.Block(0x10fc, 0x1104, 1); boundary.EndRun();
    std::ostringstream straddle; boundary.Write(straddle);
    assert(straddle.str().find("\"straddling_blocks\":1") != std::string::npos);
    Tracker overflow(c, Fake::Read, &f); overflow.BeginRun(0, 0); overflow.BeginRun(2, 0);
    for (unsigned id = 1; id <= 20; ++id) overflow.SwitchThread(id);
    overflow.EndRun();
    std::ostringstream over; overflow.Write(over);
    assert(over.str().find("\"thread_table_overflow_entries\":5") != std::string::npos);
    auto malformed = c; malformed.windows = 10000; Tracker bad(malformed, Fake::Read, &f);
    assert(!bad.Enabled());
    std::cout << "Guest PC thread CPU profiler fixtures passed\n";
}
