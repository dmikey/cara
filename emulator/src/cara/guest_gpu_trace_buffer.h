// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <ostream>
namespace Cara::GuestGPUTrace {
enum class Kind : std::uint32_t { VBlank, GXBegin, GXEnd, IRQRequest, Relay, EventSignal,
    EventAcquire, EventClear, WaitReady, WaitPoll, WaitBlock, WaitObject, Wake, Run, Swap, Binding, PicaIRQ, Draw, DrawState, DrawInput, HIDUpdate, SleepRequest };
struct Record {
    std::uint64_t sequence, frame, submission;
    std::int64_t timer0_ticks, current_ticks, global_ticks;
    std::uint32_t kind, thread, object, core;
    std::uint64_t a, b, c, d;
};
template<std::size_t Capacity> struct Buffer {
    std::array<Record, Capacity> records{};
    std::size_t size = 0;
    bool truncated = false;
    bool Push(Record record) {
        if (size == Capacity) { truncated = true; return false; }
        record.sequence = size;
        records[size++] = record;
        return true;
    }
};
template<std::size_t Capacity>
inline void WriteCSV(std::ostream& out, const Buffer<Capacity>& source,
                     std::uint64_t start_frame, bool complete, std::uint64_t duration = 16, bool draws_enabled = false) {
    out << "# cara_guest_gpu_trace_v1,start_frame=" << start_frame
        << ",duration_vblanks=" << duration << ",truncated=" << source.truncated
        << ",complete=" << complete << ",records=" << source.size
        << ",draws_enabled=" << draws_enabled << '\n';
    out << "# kinds=0:VBlank,1:GXBegin,2:GXEnd,3:IRQRequest,4:Relay,5:EventSignal,6:EventAcquire,7:EventClear,8:WaitReady,9:WaitPoll,10:WaitBlock,11:WaitObject,12:Wake,13:Run,14:Swap,15:Binding,16:PicaIRQ,17:Draw,18:DrawState,19:DrawInput,20:HIDUpdate,21:SleepRequest\n";
    out << "sequence,frame,submission,timer0_ticks,current_ticks,global_ticks,kind,thread,object,core,a,b,c,d\n";
    for (std::size_t i = 0; i < source.size; ++i) {
        const auto& r = source.records[i];
        out << r.sequence << ',' << r.frame << ',' << r.submission << ',' << r.timer0_ticks << ','
            << r.current_ticks << ',' << r.global_ticks << ',' << r.kind << ',' << r.thread << ','
            << r.object << ',' << r.core << ',' << r.a << ',' << r.b << ',' << r.c << ',' << r.d << '\n';
    }
}

}
