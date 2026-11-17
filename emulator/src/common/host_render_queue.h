// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

namespace Common {

// One persistent backend owner. A successful Submit returns after admission,
// without waiting for execution. Capacity includes queued AND executing jobs.
// bytes is the caller-declared size of all owned payload allocations; closures
// must not borrow mutable producer/guest storage. The job cap also bounds empty
// commands. This primitive neither reads guest memory nor changes guest clocks.
class HostRenderQueue {
public:
    struct Stats {
        std::uint64_t epoch = 1;
        std::uint64_t submissions = 0, started = 0, completions = 0;
        std::uint64_t cancelled = 0, errors = 0, blocked_submissions = 0;
        std::uint64_t last_submitted_sequence = 0, completed_sequence = 0;
        std::uint64_t retired_sequence = 0, executing_sequence = 0, executing_epoch = 0;
        std::size_t live_jobs = 0, live_bytes = 0;
        std::size_t high_water_jobs = 0, high_water_bytes = 0;
        bool accepting = true, worker_running = false, poisoned = false, resetting = false;
    };

    HostRenderQueue(std::size_t max_jobs, std::size_t max_bytes,
                    std::function<void()> on_start = {}, std::function<void()> on_stop = {})
        : max_jobs_(max_jobs), max_bytes_(max_bytes), on_start_(std::move(on_start)),
          on_stop_(std::move(on_stop)) {
        if (max_jobs == 0)
            throw std::invalid_argument("HostRenderQueue requires a positive job limit");
        worker_ = std::thread([this] { WorkerMain(); });
        std::unique_lock<std::mutex> lock(mutex_);
        startup_.wait(lock, [this] { return startup_finished_; });
        const auto failure = failure_;
        lock.unlock();
        if (failure) {
            worker_.join();
            std::rethrow_exception(failure);
        }
    }

    HostRenderQueue(const HostRenderQueue&) = delete;
    HostRenderQueue& operator=(const HostRenderQueue&) = delete;

    // Lifetime belongs to a non-worker owner. Explicit Stop reports errors;
    // destruction always joins but cannot throw during stack unwinding.
    ~HostRenderQueue() noexcept {
        if (IsWorkerThread())
            std::terminate();
        try { Stop(); } catch (...) {}
    }

    std::uint64_t Submit(std::function<void()> job, std::size_t bytes = 0) {
        if (!job)
            throw std::invalid_argument("HostRenderQueue cannot submit an empty job");
        return Enqueue({std::move(job), {}, bytes, 0, 0});
    }

    template <class F>
    auto Call(F&& function, std::size_t bytes = 0)
        -> std::invoke_result_t<std::decay_t<F>&> {
        using Fn = std::decay_t<F>;
        using R = std::invoke_result_t<Fn&>;
        // A backend command may call a backend utility. Execute that nested
        // operation in its current FIFO job; never enqueue-and-wait on itself.
        // It has no independent sequence or capacity charge.
        if (IsWorkerThread())
            return std::invoke(function);
        auto promise = std::make_shared<std::promise<R>>();
        auto future = promise->get_future();
        auto fn = std::make_shared<Fn>(std::forward<F>(function));
        Job job;
        job.bytes = bytes;
        job.run = [promise, fn] {
            try {
                if constexpr (std::is_void_v<R>) {
                    std::invoke(*fn);
                    promise->set_value();
                } else {
                    promise->set_value(std::invoke(*fn));
                }
            } catch (...) {
                const auto error = std::current_exception();
                promise->set_exception(error);
                throw;
            }
        };
        job.cancel = [promise](std::exception_ptr error) { promise->set_exception(error); };
        Enqueue(std::move(job));
        fn.reset();
        return future.get();
    }

    void Drain() {
        AssertNotWorker("Drain");
        std::unique_lock<std::mutex> lock(mutex_);
        retired_.wait(lock, [this] { return stats_.live_jobs == 0 || failure_; });
        RethrowFailureLocked();
    }

    // Atomically closes admission while draining, then advances the epoch.
    // Sequence numbers never reset. Poison is terminal; it cannot be hidden by
    // a new epoch. Resource/context owners must separately perform reset work.
    std::uint64_t EpochReset() {
        AssertNotWorker("EpochReset");
        std::unique_lock<std::mutex> lock(mutex_);
        space_.wait(lock, [this] { return !resetting_ || failure_ || !stats_.accepting; });
        CheckAcceptingLocked();
        resetting_ = true;
        retired_.wait(lock, [this] { return stats_.live_jobs == 0 || failure_; });
        resetting_ = false;
        space_.notify_all();
        CheckAcceptingLocked();
        if (stats_.epoch == UINT64_MAX)
            throw std::overflow_error("HostRenderQueue epoch exhausted");
        return ++stats_.epoch;
    }

    // No job is dropped on normal shutdown. Concurrent Stop callers serialize
    // the join. On failure, pending jobs cancel and all waiting Calls/producers
    // receive the first original exception. on_stop is attempted exactly once,
    // on the worker, even when on_start threw (it must tolerate partial setup).
    void Stop() {
        AssertNotWorker("Stop");
        std::lock_guard<std::mutex> join_lock(join_mutex_);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stats_.accepting = false;
            stop_requested_ = true;
        }
        work_.notify_all();
        space_.notify_all();
        if (worker_.joinable())
            worker_.join();
        std::lock_guard<std::mutex> lock(mutex_);
        RethrowFailureLocked();
    }

    bool IsWorkerThread() const noexcept { return std::this_thread::get_id() == worker_id_; }

    void AssertWorkerThread() const {
        if (!IsWorkerThread())
            throw std::logic_error("HostRenderQueue backend operation needs its worker owner");
    }

    Stats GetStats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto result = stats_;
        result.resetting = resetting_;
        return result;
    }

    std::exception_ptr GetFailure() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return failure_;
    }

private:
    struct Job {
        std::function<void()> run;
        std::function<void(std::exception_ptr)> cancel;
        std::size_t bytes = 0;
        std::uint64_t sequence = 0, epoch = 0;
    };

    void AssertNotWorker(const char* operation) const {
        if (IsWorkerThread())
            throw std::logic_error(std::string("HostRenderQueue worker cannot ") + operation);
    }

    void RethrowFailureLocked() const {
        if (failure_)
            std::rethrow_exception(failure_);
    }

    void CheckAcceptingLocked() const {
        RethrowFailureLocked();
        if (!stats_.accepting)
            throw std::runtime_error("HostRenderQueue is stopped");
    }

    std::uint64_t Enqueue(Job job) {
        AssertNotWorker("Submit");
        if (job.bytes > max_bytes_)
            throw std::length_error("HostRenderQueue job exceeds byte limit");
        std::unique_lock<std::mutex> lock(mutex_);
        CheckAcceptingLocked();
        const auto fits = [this, &job] {
            return !resetting_ && stats_.live_jobs < max_jobs_ &&
                   job.bytes <= max_bytes_ - stats_.live_bytes;
        };
        if (!fits()) {
            ++stats_.blocked_submissions;
            space_.wait(lock, [&] { return failure_ || !stats_.accepting || fits(); });
            CheckAcceptingLocked();
        }
        if (stats_.last_submitted_sequence == UINT64_MAX)
            throw std::overflow_error("HostRenderQueue sequence exhausted");
        job.sequence = stats_.last_submitted_sequence + 1;
        job.epoch = stats_.epoch;
        const auto sequence = job.sequence;
        // deque allocation may throw. Charge capacity only after insertion.
        jobs_.push_back(std::move(job));
        stats_.last_submitted_sequence = sequence;
        ++stats_.submissions;
        ++stats_.live_jobs;
        stats_.live_bytes += jobs_.back().bytes;
        stats_.high_water_jobs = std::max(stats_.high_water_jobs, stats_.live_jobs);
        stats_.high_water_bytes = std::max(stats_.high_water_bytes, stats_.live_bytes);
        lock.unlock();
        work_.notify_one();
        return sequence;
    }

    // Called under mutex. Wake/cancel every admitted operation on poison.
    void PoisonLocked(std::exception_ptr error, std::deque<Job>& cancelled) {
        ++stats_.errors;
        if (!failure_)
            failure_ = error;
        stats_.poisoned = true;
        stats_.accepting = false;
        stop_requested_ = true;
        cancelled.swap(jobs_);
        for (const auto& job : cancelled) {
            --stats_.live_jobs;
            stats_.live_bytes -= job.bytes;
            ++stats_.cancelled;
            stats_.retired_sequence = std::max(stats_.retired_sequence, job.sequence);
        }
    }

    void PublishFailure(std::exception_ptr error) {
        std::deque<Job> cancelled;
        std::exception_ptr first;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            PoisonLocked(error, cancelled);
            first = failure_;
        }
        for (auto& job : cancelled)
            if (job.cancel) job.cancel(first);
        cancelled.clear();
        work_.notify_all();
        space_.notify_all();
        retired_.notify_all();
    }

    void WorkerMain() noexcept {
        worker_id_ = std::this_thread::get_id();
        bool started = false;
        try {
            if (on_start_) on_start_();
            started = true;
        } catch (...) {
            PublishFailure(std::current_exception());
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stats_.worker_running = started;
            startup_finished_ = true;
        }
        startup_.notify_all();
        if (started) {
            for (;;) {
                Job job;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    work_.wait(lock, [this] { return stop_requested_ || !jobs_.empty(); });
                    if (jobs_.empty() && stop_requested_) break;
                    job = std::move(jobs_.front());
                    jobs_.pop_front();
                    ++stats_.started;
                    stats_.executing_sequence = job.sequence;
                    stats_.executing_epoch = job.epoch;
                }
                std::exception_ptr error;
                try { job.run(); } catch (...) { error = std::current_exception(); }
                // Destroy owned input captures before releasing their byte charge.
                job.run = {};
                job.cancel = {};
                std::deque<Job> cancelled;
                std::exception_ptr first;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    --stats_.live_jobs;
                    stats_.live_bytes -= job.bytes;
                    stats_.executing_sequence = stats_.executing_epoch = 0;
                    stats_.retired_sequence = job.sequence;
                    if (!error) {
                        ++stats_.completions;
                        stats_.completed_sequence = job.sequence;
                    } else {
                        // Publish poison in the same critical section as
                        // retirement: Drain cannot observe zero live jobs and
                        // incorrectly return success before this failure.
                        PoisonLocked(error, cancelled);
                        first = failure_;
                    }
                }
                for (auto& pending : cancelled)
                    if (pending.cancel) pending.cancel(first);
                cancelled.clear();
                space_.notify_all();
                retired_.notify_all();
            }
        }
        try { if (on_stop_) on_stop_(); }
        catch (...) { PublishFailure(std::current_exception()); }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stats_.worker_running = false;
        }
        space_.notify_all();
        retired_.notify_all();
    }

    const std::size_t max_jobs_, max_bytes_;
    std::function<void()> on_start_, on_stop_;
    mutable std::mutex mutex_;
    std::mutex join_mutex_;
    std::condition_variable startup_, work_, space_, retired_;
    std::deque<Job> jobs_;
    Stats stats_;
    std::exception_ptr failure_;
    bool startup_finished_ = false, stop_requested_ = false, resetting_ = false;
    std::thread::id worker_id_;
    std::thread worker_;
};
} // namespace Common
