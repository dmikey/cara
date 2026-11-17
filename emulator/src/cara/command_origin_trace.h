// Diagnostic integration of CaraRecordSpan. No guest memory access or clock reads.
#pragma once
#include "cara/command_origin.h"
namespace Cara::GuestGPUTrace {
inline bool command_origins_enabled = false;
inline CaraRecordSpan::Tracker<8> command_origin_tracker;
struct CommandOriginFrame {
    CaraRecordSpan::ScopeToken token{};
    CaraRecordSpan::PacketToken packet{};
    CaraRecordSpan::Origin current{};
    std::uintptr_t observed_head=0;
    std::uint64_t observed_words=0;
};
inline std::array<CommandOriginFrame,8> command_origin_frames{};
inline std::size_t command_origin_depth=0;
inline void ResetCommandOrigins() {
    command_origin_frames={}; command_origin_depth=0; command_origin_tracker.Reset();
}
inline CommandOriginFrame* BeginCommandOrigin(std::uint64_t base,std::uint64_t words,const void* head) {
    const auto token=command_origin_tracker.Begin(base,words);
    if (!token.id) return nullptr;
    auto& frame=command_origin_frames[command_origin_depth++];
    frame={};frame.token=token;
    frame.observed_head=reinterpret_cast<std::uintptr_t>(head);frame.observed_words=words;
    if (!head)command_origin_tracker.Invalidate(CaraRecordSpan::Reason::UnknownAddressMapping);
    return &frame;
}
inline void EndCommandOrigin(CommandOriginFrame* frame) noexcept {
    if (!command_origin_depth || frame!=&command_origin_frames[command_origin_depth-1]) {
        command_origin_tracker.Invalidate(CaraRecordSpan::Reason::ScopeMismatch);return;
    }
    command_origin_tracker.End(frame->token);
    *frame={};--command_origin_depth;
}
inline bool CommandOriginIndex(CommandOriginFrame* frame,const void* head,const void* cursor,
                               std::uint64_t words,std::uint64_t consumed,std::uint64_t& index) {
    if (!frame || !command_origin_depth || frame!=&command_origin_frames[command_origin_depth-1]) {
        command_origin_tracker.Invalidate(CaraRecordSpan::Reason::ScopeMismatch);return false;
    }
    const auto begin=reinterpret_cast<std::uintptr_t>(head);
    const auto current=reinterpret_cast<std::uintptr_t>(cursor);
    const auto maximum=std::numeric_limits<std::uintptr_t>::max();
    if (!begin || begin!=frame->observed_head || words!=frame->observed_words ||
        words>maximum/4 || begin>maximum-words*4 || current<begin ||
        current>begin+words*4 || (current-begin)%4 || (current-begin)/4<consumed) {
        command_origin_tracker.Invalidate(CaraRecordSpan::Reason::UnknownAddressMapping);return false;
    }
    index=(current-begin)/4-consumed;return true;
}
inline void ObserveCommandHeader(CommandOriginFrame* frame,const void* head,const void* cursor,
                                 std::uint64_t words,std::uint32_t extra,std::uint32_t port,bool grouped) {
    std::uint64_t first=0,header=0;
    frame->current={};
    if (!CommandOriginIndex(frame,head,cursor,words,2,first) ||
        !CommandOriginIndex(frame,head,cursor,words,1,header)) return;
    frame->packet=command_origin_tracker.Header(header,first,extra,port,grouped);
}
inline void ObserveCommandParameter(CommandOriginFrame* frame,const void* head,const void* cursor,
                                    std::uint64_t words,std::uint32_t slot,std::uint32_t port,bool first) {
    std::uint64_t index=0;
    frame->current={};
    if (!CommandOriginIndex(frame,head,cursor,words,first?2:1,index)) return;
    frame->current=command_origin_tracker.Parameter(frame->packet,slot,index,port);
}
inline void RedirectCommandOrigin(std::uint64_t base,std::uint64_t words,const void* head) {
    if (!command_origin_depth) return; // Capture may have armed outside this list's entry.
    auto& frame=command_origin_frames[command_origin_depth-1];
    command_origin_tracker.Redirect(base,words);
    frame.observed_head=reinterpret_cast<std::uintptr_t>(head);frame.observed_words=words;
    if (!head)command_origin_tracker.Invalidate(CaraRecordSpan::Reason::UnknownAddressMapping);
}
inline CaraRecordSpan::Origin CurrentCommandOrigin() {
    if (command_origin_tracker.Failure()!=CaraRecordSpan::Reason::None) {
        CaraRecordSpan::Origin unknown;unknown.reason=command_origin_tracker.Failure();return unknown;
    }
    if (!command_origin_depth) return {};
    return command_origin_frames[command_origin_depth-1].current;
}
} // namespace Cara::GuestGPUTrace
