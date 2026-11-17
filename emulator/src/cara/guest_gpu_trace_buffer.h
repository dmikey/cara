// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <ostream>
namespace Cara::GuestGPUTrace {
enum class Kind : std::uint32_t { VBlank, GXBegin, GXEnd, IRQRequest, Relay, EventSignal,
    EventAcquire, EventClear, WaitReady, WaitPoll, WaitBlock, WaitObject, Wake, Run, Swap, Binding, PicaIRQ, Draw, DrawState, DrawInput, HIDUpdate, SleepRequest, QueueState, GuestCallback, HostCPUStamp, GuestCallbackArgs, GuestExecutionContext, DrawOriginAddress, DrawOriginContext, RecordSnapshotRegisters, RecordSnapshotGroup, RecordSnapshotWord, WaitOriginArm, WaitOriginArmTiming, WaitOriginTimer, WaitOriginResume, WaitOriginCancel, WaitOriginArbRequest, WaitOriginArbWait, WaitOriginArbWake, WaitOriginArbTimeout, WaitOriginArbEnd };
enum class QueuePhase : std::uint32_t { BatchEntry, BeforeExecute, AfterExecute, BeforeClear, AfterClear };
constexpr std::uint32_t NoCommandSlot = 0xffffffffu;
struct Record {
    std::uint64_t sequence, frame, submission;
    std::int64_t timer0_ticks, current_ticks, global_ticks;
    std::uint32_t kind, thread, object, core;
    std::uint64_t a, b, c, d;
};
// Preserve the adjacent GX timestamp without any additional per-command clock read.
inline Record QueueStateRecord(const Record& timestamp, QueuePhase phase,
                               std::uint32_t raw_header, std::uint32_t slot,
                               std::uint32_t raw_command_word) {
    Record result = timestamp;
    result.kind = static_cast<std::uint32_t>(Kind::QueueState);
    result.a = static_cast<std::uint32_t>(phase);
    result.b = raw_header; result.c = slot; result.d = raw_command_word;
    return result;
}
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
                     std::uint64_t start_frame, bool complete, std::uint64_t duration = 16, bool draws_enabled = false, bool command_origins = false, bool record_snapshots = false, bool wait_origins = false, std::uint32_t wait_origin_error = 0) {
    out << (wait_origins ? "# cara_guest_gpu_trace_v5,start_frame=" : (record_snapshots ? "# cara_guest_gpu_trace_v4,start_frame=" : (command_origins ? "# cara_guest_gpu_trace_v3,start_frame=" : "# cara_guest_gpu_trace_v2,start_frame="))) << start_frame
        << ",duration_vblanks=" << duration << ",truncated=" << source.truncated
        << ",complete=" << complete << ",records=" << source.size
        << ",draws_enabled=" << draws_enabled << '\n';
    out << "# kinds=0:VBlank,1:GXBegin,2:GXEnd,3:IRQRequest,4:Relay,5:EventSignal,6:EventAcquire,7:EventClear,8:WaitReady,9:WaitPoll,10:WaitBlock,11:WaitObject,12:Wake,13:Run,14:Swap,15:Binding,16:PicaIRQ,17:Draw,18:DrawState,19:DrawInput,20:HIDUpdate,21:SleepRequest,22:QueueState,23:GuestCallback,24:HostCPUStamp,25:GuestCallbackArgs,26:GuestExecutionContext";
    if (command_origins) out << ",27:DrawOriginAddress,28:DrawOriginContext";
    if(record_snapshots) out << ",29:RecordSnapshotRegisters,30:RecordSnapshotGroup,31:RecordSnapshotWord";
    if(wait_origins)out << ",32:WaitOriginArm,33:WaitOriginArmTiming,34:WaitOriginTimer,35:WaitOriginResume,36:WaitOriginCancel,37:WaitOriginArbRequest,38:WaitOriginArbWait,39:WaitOriginArbWake,40:WaitOriginArbTimeout,41:WaitOriginArbEnd";
    out << '\n';
    if(wait_origins)out << "# wait_origin_schema=1,error=" << wait_origin_error << ",arm_sequence=trace_sequence,deadline=derived_guest_current_ticks_plus_original_delay,unknown_sequence=18446744073709551615\n";
    if (command_origins) {
        out << "# draw_origin_layout=27:a:header_pa,b:parameter_pa,c:header_epoch,d:parameter_epoch;"
               "28:a:helper_capture,b:scope,c:packet,d:reason[55:48]|valid[47]|slot[31:16]|port[15:0]\n";
        out << "# draw_origin_reason=0:None,1:NoScope,2:ContextOverflow,3:ScopeMismatch,4:InvalidRange,"
               "5:CounterOverflow,6:PacketMismatch,7:HeaderMismatch,8:ParameterMismatch,9:UnknownAddressMapping\n";
    }
    if(record_snapshots) {
        out << "# record_snapshot_schema=1,enabled=1,addresses=guestVA,ownership=unassigned\n";
        out << "# record_snapshot_layout=29:a:callback_sequence,b:r4,c:r5,d:r8;"
               "30:a:callback_sequence,b:group,c:reason[31:0]|known[32]|requested[47:40]|failed_index[55:48],d:baseVA;"
               "31:a:callback_sequence,b:group[63:32]|field[31:0],c:wordVA,d:raw_u32\n";
        out << "# record_snapshot_fields=0:Word0,1:Word4,2:RecorderDepth,3:RecorderBase,4:RecorderCount,"
               "5:RecorderCommitted,6:RecorderExtent,7:ListBase,8:ListExtent,9:ListFinalized,"
               "10:ListDescriptors,11:ListProduced,12:ListConsumed,13:DescriptorKind,"
               "14:DescriptorAddress,15:DescriptorBytes,16:DescriptorFlags\n";
        out << "# record_snapshot_groups=1:Recorder,2:Entry,3:Payload,4:Subtype,5:Descriptor,6:List,"
               "7:Globals,8:RecordingList,9:ExecutingList\n";
        out << "# record_snapshot_reason=0:None,1:Disabled,2:Unmatched,3:NoTable,4:ArrayMismatch,"
               "5:InvalidPlan,6:TooManyWords,7:InvalidField,8:AddressOverflow,9:Unaligned,10:CrossPage,"
               "11:PageIndexOverflow,12:Cached,13:MMIO,14:Unmapped,15:NullOrdinaryPointer,"
               "16:InvalidPageClass,256:DependencyUnknown,257:MetadataDomainRejected\n";
    }
    out << "sequence,frame,submission,timer0_ticks,current_ticks,global_ticks,kind,thread,object,core,a,b,c,d\n";
    for (std::size_t i = 0; i < source.size; ++i) {
        const auto& r = source.records[i];
        out << r.sequence << ',' << r.frame << ',' << r.submission << ',' << r.timer0_ticks << ','
            << r.current_ticks << ',' << r.global_ticks << ',' << r.kind << ',' << r.thread << ','
            << r.object << ',' << r.core << ',' << r.a << ',' << r.b << ',' << r.c << ',' << r.d << '\n';
    }
}

}
