// SPDX-License-Identifier: GPL-2.0-or-later
#include "cara/guest_gpu_trace.h"
#include <cstdio>
#include <thread>
#include <iostream>
#include <sstream>
#include <stdexcept>
using namespace Cara::GuestGPUTrace;
static void Require(bool value) { if (!value) throw std::runtime_error("Guest GPU trace fixture mismatch"); }
struct FakeTiming {
    int clock_reads = 0;
    FakeTiming* GetTimer(unsigned) { return this; }
    std::int64_t GetTicks() { ++clock_reads; return 100; }
    std::int64_t GetGlobalTicks() { ++clock_reads; return 101; }
    unsigned GetCurrentTimerId() { return 1; }
};
int main() {
    Buffer<2> data;
    Record record{99, 4, 8, 100, 97, 102, static_cast<std::uint32_t>(Kind::GXBegin), 3, 5, 1, 6, 7, 8, 9};
    Require(data.Push(record));
    record.timer0_ticks = 90; // Preserve domains and host observation order, never sort by ticks.
    Require(data.Push(record));
    Require(!data.truncated && data.size == 2);
    Require(!data.Push(record) && data.truncated && data.size == 2);
    Require(data.records[0].sequence == 0 && data.records[1].sequence == 1);
    Require(data.records[0].current_ticks == 97 && data.records[1].timer0_ticks == 90);
    std::ostringstream out;
    WriteCSV(out, data, 4, true);
    Require(out.str().find("truncated=1,complete=1,records=2") != std::string::npos);
    Require(out.str().find("0,4,8,100,97,102,1,3,5,1,6,7,8,9\n") != std::string::npos);
    Require(out.str().find("1,4,8,90,97,102,1,3,5,1,6,7,8,9\n") != std::string::npos);
    data.size=0; data.truncated=false;
    Require(data.Push(record) && data.records[0].sequence==0);
    FakeTiming timing;
    unsetenv("CARA_GPU_TRACE");
    Arm("/private/tmp/cara-guest-gpu-trace-test.csv");
    Emit(timing, Kind::Run); VBlank(timing);
    Require(timing.clock_reads == 0 && !Enabled());
    setenv("CARA_GPU_TRACE", "1", 1);
    setenv("CARA_GPU_TRACE_START_FRAME", "2", 1);
    Arm("/private/tmp/cara-guest-gpu-trace-test.csv");
    VBlank(timing); VBlank(timing);
    Require(timing.clock_reads == 0 && !Enabled());
    VBlank(timing);
    Require(Enabled() && frame == 2 && buffer.size == 1);
    Emit(timing, Kind::Run);
    Require(buffer.records[1].frame == 2);
    const auto reads = timing.clock_reads;
    std::thread foreign([&] { Emit(timing, Kind::Run); }); foreign.join();
    Require(timing.clock_reads == reads && foreign_events == 1 && buffer.size == 2);
    for (unsigned i = 3; i < 18; ++i) VBlank(timing);
    Require(Enabled() && !complete && frame == 17);
    VBlank(timing);
    Require(!Enabled() && complete && buffer.size == 17);
    Require(Flush() && !Flush());
    std::remove("/private/tmp/cara-guest-gpu-trace-test.csv");
    setenv("CARA_GPU_TRACE_START_FRAME", "0", 1);
    setenv("CARA_GPU_TRACE_DURATION_VBLANKS", "4", 1);
    setenv("CARA_GPU_TRACE_DRAWS", "1", 1);
    Arm("/private/tmp/cara-guest-gpu-trace-test.csv");
    Require(duration_vblanks == 4 && vs_generation == 0 && gs_generation == 0);
    VBlank(timing);
    Require(DrawsEnabled() && ReserveDrawRecords());
    ShaderWordChanged(false); ShaderWordChanged(true);
    Require(vs_generation == 1 && gs_generation == 1);
    for (unsigned i = 1; i <= 4; ++i) VBlank(timing);
    Require(complete && !Enabled() && buffer.size == 4);
    Arm("/private/tmp/cara-guest-gpu-trace-test.csv"); VBlank(timing);
    buffer.size = buffer.records.size() - 2;
    Require(!ReserveDrawRecords() && buffer.truncated && complete && !Enabled());
    unsetenv("CARA_GPU_TRACE_DURATION_VBLANKS"); unsetenv("CARA_GPU_TRACE_DRAWS");
    Arm("/private/tmp/cara-no-such-directory/trace.csv");
    Require(!Flush(true) && export_attempted && TakeExportError() && !TakeExportError());
    Require(!Flush(true));
    unsetenv("CARA_GPU_TRACE"); unsetenv("CARA_GPU_TRACE_START_FRAME");
    std::cout << "Guest GPU trace: capacity, explicit truncation, order, separate timer domains, reset, CSV export, delayed activation, 16 intervals, foreign-thread rejection and one-shot export passed\n";
}
