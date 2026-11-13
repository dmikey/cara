// SPDX-License-Identifier: GPL-2.0-or-later
#include <iostream>
#include <sstream>
#include <vector>
#include <boost/archive/binary_iarchive.hpp>
#include <boost/archive/binary_oarchive.hpp>
#include <boost/serialization/array.hpp>
#include <boost/serialization/shared_ptr.hpp>
#include "cara/p3d_completion.h"
#include "cara/gpu_completion_timeline.h"
#include "core/core_timing.h"

static Core::Timing* restored_timing = nullptr;
struct FixtureCallback {
    Cara::P3DCompletionQueue* owner;
    Core::Timing* timing;
    std::vector<unsigned>* delivered;
    void operator()(u64 token, int) const {
        const auto record = owner->Consume(token, Cara::GPUCompletionNow(*timing));
        if (record) delivered->push_back(record->recipient);
    }
};
namespace Core {
template <> Timing& Global<Timing>() { return *restored_timing; }
}
void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template <class Exception, class Action> void Reject(Action action) {
    bool failed = false;
    try { action(); } catch (const Exception&) { failed = true; }
    Require(failed, "Expected P3D diagnostic failure");
}
static void RunUntilEmpty(Core::Timing& timing, Cara::P3DCompletionQueue& queue) {
    for (unsigned i = 0; i < 1000; ++i) {
        if (std::none_of(queue.records.begin(), queue.records.end(), [](const auto& r) { return r.active; })) return;
        const auto timer = timing.GetTimer(0);
        timer->Advance(1000000);
        timer->AddTicks(timer->GetDowncount());
    }
    throw std::runtime_error("P3D CoreTiming events did not finish");
}
int main() {
    try {
        Cara::P3DWorkEstimate estimate;
        for (unsigned i = 0; i < 5; ++i) estimate.Command();
        estimate.Draw(9, 0); // 3 list triangles, not expanded GLES vertices.
        estimate.Draw(5, 1); // 3 strip triangles.
        estimate.Draw(5, 2); // 3 fan triangles.
        Require(estimate.TakeNanoseconds() == 300000 + 112 + 9 * 288, "Original PICA work estimate");
        Require(estimate.TakeNanoseconds() == 300000, "Work was not reset at completion");
        Require(Cara::P3DNanosecondsToTicks(0) == 1 &&
                Cara::P3DNanosecondsToTicks(1000000000) == 268111856, "Checked guest tick conversion");
        Require(Cara::P3DNanosecondsToTicks(UINT64_MAX) > 0, "Largest nanosecond duration wrapped");
        estimate.fifth_ns = UINT64_MAX;
        Reject<std::overflow_error>([&] { estimate.Command(); });
        Cara::P3DCompletionQueue queue;
        const auto first = queue.Enqueue(10, 100, 0);
        const auto second = queue.Enqueue(10, 1, 1);
        Require(first.deadline == 110 && second.deadline == 111, "Serial render completion order");
        Reject<std::runtime_error>([&] { queue.Consume(second.token, 200); });
        Reject<std::runtime_error>([&] { queue.Consume(first.token, 109); });
        Require(queue.Consume(first.token, 110)->recipient == 0, "Original waiter recipient lost");
        queue.InvalidateRecipient(1); // Old waiter disappears and the same thread slot is reused.
        Require(!queue.Consume(second.token, 111), "Stale event signaled new waiter");
        const auto replacement = queue.Enqueue(111, 1, 1);
        Require(queue.Consume(replacement.token, 112)->recipient == 1, "Replacement waiter not signaled");
        queue = {};
        for (unsigned i = 0; i < queue.Capacity; ++i) queue.Enqueue(0, 1, 0);
        Reject<std::runtime_error>([&] { queue.Enqueue(0, 1, 0); });
        queue = {};
        Reject<std::overflow_error>([&] { queue.Enqueue(INT64_MAX, 1, 0); });
        Require(Cara::PPFTransferNanoseconds(450 * 1024 * 1024, false, false) == 1000300000,
                "Measured RAM-to-RAM TEX rate");
        Require(Cara::PPFTransferNanoseconds(3100ULL * 1024 * 1024, false, true) == 1000300000 &&
                Cara::PPFTransferNanoseconds(5400ULL * 1024 * 1024, true, false) == 1000300000,
                "Measured VRAM TEX rates");
        Reject<std::overflow_error>([&] { Cara::PPFTransferNanoseconds(UINT64_MAX, false, false); });
        queue = {};
        const auto render = queue.Enqueue(0, 100, 0);
        const auto transfer = queue.Enqueue(0, 10, 0, true);
        const auto overlapping_render = queue.Enqueue(0, 1, 0);
        Require(transfer.deadline == 101 && overlapping_render.deadline == 101,
                "PPF dependency or independent later render deadline incorrect");
        queue.Validate();
        Reject<std::runtime_error>([&] { queue.Consume(transfer.token, 100); });
        queue.Consume(render.token, 100);
        queue.Consume(overlapping_render.token, 101);
        Require(queue.Consume(transfer.token, 110)->transfer, "PPF waiter woke before prior render");
        // A producer gated by PPF cannot accumulate unbounded earlier P3D work.
        queue = {};
        s64 producer_time = 0;
        for (unsigned i = 0; i < 10000; ++i) {
            const auto r = queue.Enqueue(producer_time, 100, 0);
            const auto t = queue.Enqueue(producer_time, 10, 0, true);
            Require(t.deadline > r.deadline, "Producer PPF dependency lost");
            queue.Consume(r.token, r.deadline);
            queue.Consume(t.token, t.deadline);
            producer_time = t.deadline;
        }
        // Ungated producer: 200 renders/s at 6ms estimated latency contributes
        // 1.2 modeled seconds per guest second. This is latency, not utilization:
        // adding it cumulatively used to exhaust 256 records after ~6 seconds.
        // PPFs arrive twice per render; every accepted IRQ must be delivered.
        queue = {};
        std::vector<Cara::PendingP3D> outstanding;
        unsigned accepted = 0, completed = 0;
        std::size_t high_water = 0;
        constexpr s64 submission_interval = 268111856 / 200;
        const auto render_latency = Cara::P3DNanosecondsToTicks(6000000);
        const auto transfer_latency = Cara::P3DNanosecondsToTicks(1000000);
        for (s64 i = 0; i <= 20000; ++i) {
            const auto now = i * submission_interval;
            while (!outstanding.empty() && outstanding.front().deadline <= now) {
                Require(queue.Consume(outstanding.front().token, now).has_value(),
                        "Sustained producer lost completion");
                outstanding.erase(outstanding.begin());
                ++completed;
            }
            const auto r = queue.Enqueue(now, render_latency, 0);
            outstanding.push_back(r);
            outstanding.push_back(queue.Enqueue(now, transfer_latency, 0, true));
            outstanding.push_back(queue.Enqueue(now, transfer_latency, 0, true));
            accepted += 3;
            high_water = std::max(high_water, outstanding.size());
            Require(r.deadline == now + render_latency, "Latency accumulated serial work debt");
            queue.Validate();
        }
        for (const auto& pending : outstanding) {
            Require(queue.Consume(pending.token, pending.deadline).has_value(),
                    "Final sustained completion lost");
            ++completed;
        }
        Require(high_water == 6 && accepted == completed,
                "Ungated producer backlog grew or completion was dropped");
        queue = {};
        const auto last = queue.Enqueue(0, INT64_MAX, 0);
        Reject<std::overflow_error>([&] { queue.Enqueue(0, 1, 0); });
        Reject<std::overflow_error>([&] { queue.Enqueue(0, 1, 0, true); });
        Require(queue.Peek(last.token).has_value(), "Overflow corrupted accepted record");
        // Kernel Advance may execute timer0 events while current_timer still
        // names the last CPU that ran. Submit across timers, then intentionally
        // leave that selector behind the deadlines while advancing only owner0.
        Core::Timing multicore;
        Cara::P3DCompletionQueue multi_pending;
        std::vector<unsigned> multi_delivered;
        auto* multi_event = multicore.RegisterEvent("MultiTimerGPUCompletion",
            FixtureCallback{&multi_pending, &multicore, &multi_delivered});
        multicore.SetCurrentTimer(1);
        multicore.GetTimer(1)->Advance();
        multicore.GetTimer(1)->AddTicks(5000);
        const auto multi_render = multi_pending.Enqueue(Cara::GPUSubmissionNow(multicore), 100, 0);
        Cara::ScheduleGPUCompletion(multicore, multi_render.deadline, multi_event, multi_render.token);
        multicore.SetCurrentTimer(2);
        multicore.GetTimer(2)->Advance();
        multicore.GetTimer(2)->AddTicks(10000);
        const auto multi_transfer = multi_pending.Enqueue(Cara::GPUSubmissionNow(multicore), 10, 1, true);
        Cara::ScheduleGPUCompletion(multicore, multi_transfer.deadline, multi_event, multi_transfer.token);
        Require(multi_render.deadline == 5100 && multi_transfer.deadline == 10010,
                "Absolute submission times lost across timers");
        multicore.SetCurrentTimer(1); // GetTicks()==5000; owner callbacks run after 10010.
        RunUntilEmpty(multicore, multi_pending);
        Require(multi_delivered == std::vector<unsigned>({0, 1}),
                "Different submit timers split dependent completion ordering");
        // Owner time can be ahead of a submitting CPU; never schedule in its past.
        Require(Cara::GPUSubmissionNow(multicore) >= Cara::GPUCompletionNow(multicore),
                "Submission moved behind owner timeline");
        // Off-mode checkpoint has no experimental events or records. Restore
        // with the new event type already bound, then submit from saved CPU2.
        Core::Timing off_timing;
        off_timing.SetCurrentTimer(2);
        off_timing.GetTimer(2)->Advance();
        off_timing.GetTimer(2)->AddTicks(10000);
        Cara::P3DCompletionQueue off_queue;
        std::stringstream off_state;
        { boost::archive::binary_oarchive out(off_state); out << off_timing << off_queue; }
        Core::Timing enabled_timing;
        restored_timing = &enabled_timing;
        Cara::P3DCompletionQueue enabled_queue;
        std::vector<unsigned> enabled_delivered;
        auto* enabled_event = enabled_timing.RegisterEvent("OffToOnGPUCompletion",
            FixtureCallback{&enabled_queue, &enabled_timing, &enabled_delivered});
        { boost::archive::binary_iarchive in(off_state); in >> enabled_timing >> enabled_queue; }
        enabled_timing.FinishStateLoad();
        enabled_queue.Validate();
        const auto restored_render = enabled_queue.Enqueue(Cara::GPUSubmissionNow(enabled_timing), 100, 0);
        Cara::ScheduleGPUCompletion(enabled_timing, restored_render.deadline, enabled_event, restored_render.token);
        const auto restored_ppf = enabled_queue.Enqueue(Cara::GPUSubmissionNow(enabled_timing), 10, 1, true);
        Cara::ScheduleGPUCompletion(enabled_timing, restored_ppf.deadline, enabled_event, restored_ppf.token);
        Require(restored_render.deadline == 10100 && restored_ppf.deadline == 10101,
                "Off-mode checkpoint lost selected CPU time");
        RunUntilEmpty(enabled_timing, enabled_queue);
        Require(enabled_delivered == std::vector<unsigned>({0, 1}), "Off-to-on completion restore failed");

        Core::Timing original;
        Cara::P3DCompletionQueue pending;
        Cara::P3DWorkEstimate partial_work;
        partial_work.Command(); partial_work.Draw(6, 0);
        const auto* event = original.RegisterEvent("CaraExperimentalP3DCompletion", [](u64, s64) {
            throw std::runtime_error("Old service callback survived restore");
        });
        for (unsigned recipient : {0u, 1u}) {
            const auto record = pending.Enqueue(0, 100, recipient, recipient == 1);
            original.ScheduleEvent(record.deadline, event, record.token);
        }
        const auto* marker = original.RegisterEvent("P3DFixtureEqualDeadline", [](u64, s64) {
            throw std::runtime_error("Old equal-deadline callback survived restore");
        });
        original.ScheduleEvent(100, marker);
        std::stringstream state;
        { boost::archive::binary_oarchive out(state); out << original << pending << partial_work; }
        Core::Timing restored;
        restored_timing = &restored; // Timer's actual archive constructor binds to this timeline.
        Cara::P3DCompletionQueue loaded;
        Cara::P3DWorkEstimate loaded_work;
        { boost::archive::binary_iarchive in(state); in >> restored >> loaded >> loaded_work; }
        loaded.Validate();
        Require(loaded.records[1].transfer && loaded.transfer_deadline == 101, "PPF kind/deadline lost across restore");
        Require(loaded_work.TakeNanoseconds() == 300000 + 22 + 2 * 288, "Partial work lost across state restore");
        std::vector<unsigned> delivered;
        auto* restored_event = restored.RegisterEvent("CaraExperimentalP3DCompletion",
            FixtureCallback{&loaded, &restored, &delivered});
        restored.RegisterEvent("P3DFixtureEqualDeadline", [&](u64, s64) { delivered.push_back(99); });
        // A replaced service can own identical token IDs. Its late destruction
        // must leave the new binding and restored events intact.
        const auto* binding = restored_event->callback.target<FixtureCallback>();
        if (binding && binding->owner == &pending)
            for (const auto& old : pending.records)
                if (old.active) restored.UnscheduleEvent(restored_event, old.token);
        restored.FinishStateLoad();
        RunUntilEmpty(restored, loaded);
        Require(delivered == std::vector<unsigned>({0, 99, 1}), "Active-pending save/load lost equal-deadline FIFO or callback rebind");
        Require(!loaded.Consume(1, restored.GetTicks()), "Consumed event duplicated");
        // Teardown cancels every owned pending token, without dropping another service's events.
        const auto record = loaded.Enqueue(restored.GetTicks(), 100, 0);
        restored.ScheduleEvent(record.deadline - restored.GetTicks(), restored_event, record.token);
        for (const auto& owned : loaded.records)
            if (owned.active) restored.UnscheduleEvent(restored_event, owned.token);
        restored_event->callback = [](u64, int) {};
        auto timer = restored.GetTimer(0);
        for (unsigned i = 0; i < 3; ++i) { timer->Advance(1000000); timer->AddTicks(timer->GetDowncount()); }
        Require(delivered.size() == 3, "Teardown left an owned callback queued");
        std::cout << "P3D approximate model, original counts, checked ticks, serial FIFO, recipient reuse, "
                     "PPF dependency/independent overlap/producer bounds, capacity failure and actual CoreTiming/Boost active-pending restore passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
