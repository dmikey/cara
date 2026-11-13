// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <limits>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>

namespace Core::FastInterp::GuestPCCPU {
constexpr std::size_t MaxRanges = 8, MaxThreads = 16, MaxWindows = 64, MaxFrames = 1024;
constexpr std::size_t Other = 0, External = 1, Episode = 2, MaxBuckets = MaxRanges + 2;
struct Range { std::string name; std::uint64_t start = 0, end = 0; };
struct Config {
    bool enabled = false;
    bool episode_mode = false;
    std::uint32_t episode_start = 0, episode_end = 0;
    std::string error;
    std::array<Range, MaxRanges> ranges{};
    std::size_t range_count = 0;
    std::uint32_t start_frame = 300, window_frames = 4, period_frames = 120, windows = 4;
};
inline bool Number(std::string_view text, std::uint64_t& value) {
    int base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16; text.remove_prefix(2);
    }
    if (text.empty()) return false;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value, base);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}
inline Config Parse(std::string_view spec) {
    Config c;
    if (spec.empty()) return c;
    while (!spec.empty()) {
        const auto separator = spec.find(';');
        const auto item = spec.substr(0, separator);
        const auto eq = item.find('='), colon = item.find(':');
        if (c.range_count == MaxRanges || eq == std::string_view::npos ||
            colon == std::string_view::npos || eq == 0 || eq > 31 || colon <= eq + 1) {
            c.error = "invalid range specification"; return c;
        }
        const auto name = item.substr(0, eq);
        for (char ch : name)
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                  (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')) {
                c.error = "invalid range label"; return c;
            }
        if (name == "other_guest" || name == "svc_external" || name == "episode") {
            c.error = "reserved range label"; return c;
        }
        std::uint64_t start = 0, end = 0;
        if (!Number(item.substr(eq + 1, colon - eq - 1), start) ||
            !Number(item.substr(colon + 1), end) || start >= end || end > (1ULL << 32)) {
            c.error = "invalid range bounds"; return c;
        }
        for (std::size_t i = 0; i < c.range_count; ++i)
            if (c.ranges[i].name == name || (start < c.ranges[i].end && end > c.ranges[i].start)) {
                c.error = "duplicate label or overlapping ranges"; return c;
            }
        c.ranges[c.range_count++] = {std::string(name), start, end};
        if (separator == std::string_view::npos) break;
        spec.remove_prefix(separator + 1);
        if (spec.empty()) { c.error = "empty range entry"; return c; }
    }
    c.enabled = true;
    return c;
}
inline Config FromEnvironment() {
    const char* ranges = std::getenv("CARA_GUEST_PC_PROFILE_RANGES");
    Config c = Parse(ranges ? ranges : "");
    if (!c.enabled) return c;
    const auto read = [&](const char* name, std::uint32_t& target) {
        if (const char* text = std::getenv(name)) {
            std::uint64_t value = 0;
            if (!Number(text, value) || value > UINT32_MAX) return false;
            target = static_cast<std::uint32_t>(value);
        }
        return true;
    };
    if (!read("CARA_GUEST_PC_PROFILE_START_FRAME", c.start_frame) ||
        !read("CARA_GUEST_PC_PROFILE_WINDOW_FRAMES", c.window_frames) ||
        !read("CARA_GUEST_PC_PROFILE_PERIOD_FRAMES", c.period_frames) ||
        !read("CARA_GUEST_PC_PROFILE_WINDOWS", c.windows) || !c.window_frames ||
        c.window_frames > 64 || c.period_frames < c.window_frames || !c.windows ||
        c.windows > MaxWindows || std::uint64_t(c.window_frames) * c.windows > MaxFrames) {
        c.enabled = false; c.error = "invalid or excessive sampling window configuration";
    }
    const char* entry = std::getenv("CARA_GUEST_PC_PROFILE_EPISODE_START");
    const char* exit = std::getenv("CARA_GUEST_PC_PROFILE_EPISODE_END");
    if (entry || exit) {
        std::uint64_t a = 0, b = 0;
        bool allowed = false;
        if (entry && exit && Number(entry, a) && Number(exit, b) && a <= UINT32_MAX &&
            b <= UINT32_MAX && a != b) {
            for (std::size_t i = 0; i < c.range_count; ++i)
                allowed |= a >= c.ranges[i].start && a < c.ranges[i].end;
        }
        if (!allowed) { c.enabled = false; c.error = "invalid episode fences or entry outside allowlist"; }
        else { c.episode_mode = true; c.episode_start = a; c.episode_end = b; }
    }
    return c;
}
inline bool ReadThreadClock(void*, std::uint64_t& ns) {
#if defined(CLOCK_THREAD_CPUTIME_ID)
    timespec value{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) != 0 || value.tv_sec < 0 ||
        value.tv_nsec < 0 || value.tv_nsec >= 1000000000L) return false;
    const auto seconds = static_cast<std::uint64_t>(value.tv_sec);
    if (seconds > (UINT64_MAX - static_cast<std::uint64_t>(value.tv_nsec)) / 1000000000ULL) return false;
    ns = seconds * 1000000000ULL + static_cast<std::uint64_t>(value.tv_nsec);
    return true;
#else
    return false;
#endif
}
struct Bucket { std::uint64_t cpu_ns = 0, blocks = 0, transitions = 0; };
struct EpisodeStats {
    bool active = false, partial = false, sampled_seen = false;
    std::uint64_t started = 0, completed = 0, aborted = 0, reentries = 0;
    std::uint64_t completed_cpu = 0, partial_cpu = 0, aborted_cpu = 0, current_cpu = 0;
    std::uint64_t partial_sampled = 0, sampled_aborted = 0;
};
struct Thread {
    std::uint32_t id = 0;
    bool used = false;
    EpisodeStats episode;
    std::array<Bucket, MaxBuckets> buckets{};
};
struct Window {
    bool started = false, closed = false;
    std::uint64_t start = 0, end = 0, observed_mask = 0;
};

// Raw host thread CPU deltas, partitioned by decoded block start. No guest
// timer writes, memory accesses, call-stack inference or per-instruction hooks.
// Episode mode tracks externally configured fences even outside duty windows.
// Only validated blocks and caller-owned SVC spans enter its inclusive bucket;
// initial Run lookup/setup remains other_guest until the first block is validated.
class Tracker {
public:
    using Clock = bool (*)(void*, std::uint64_t&);
    explicit Tracker(Config config = {}, Clock clock = ReadThreadClock, void* context = nullptr)
        : config_(std::move(config)), clock_(clock), clock_context_(context) {
        if (config_.enabled && (config_.range_count > MaxRanges || !config_.window_frames ||
            config_.window_frames > 64 || config_.period_frames < config_.window_frames ||
            !config_.windows || config_.windows > MaxWindows ||
            std::uint64_t(config_.window_frames) * config_.windows > MaxFrames)) {
            config_.enabled = false; config_.error = "invalid tracker configuration";
            config_.range_count = 0; config_.windows = 0;
        }
    }
    bool Enabled() const { return config_.enabled; }
    bool Active() const { return in_run_; }
    bool ObservesBlocks() const { return in_run_ || (Enabled() && config_.episode_mode); }
    void Reset() {
        EndRun();
        threads_ = {}; windows_ = {}; frames_ = {};
        thread_count_ = frame_count_ = 0; have_base_ = false;
        cpu_ns_ = reads_ = failures_ = unassigned_ = overflow_ = straddles_ = frame_resets_ = 0;
        last_valid_ = false; capacity_exhausted_ = false;
    }
    void Calibrate() {
        if (!Enabled() || calibration_reads_) return;
        std::uint64_t first = 0, last = 0, value = 0;
        for (unsigned i = 0; i < 64; ++i) {
            ++calibration_reads_;
            if (!clock_(clock_context_, value) || (i && value < last)) {
                ++calibration_failures_; continue;
            }
            if (!i) first = value;
            last = value;
        }
        if (!calibration_failures_) clock_ns_per_read_ = double(last - first) / 63.0;
    }
    void BeginRun(std::uint32_t frame, std::uint32_t thread) {
        if (!Enabled()) return;
        EndRun();
        if (have_base_ && frame < last_frame_) {
            const auto resets = frame_resets_ + 1;
            Reset(); frame_resets_ = resets;
        }
        if (!have_base_) { base_frame_ = frame; have_base_ = true; }
        last_frame_ = frame;
        for (auto& window : windows_)
            if (window.started && frame >= window.end) window.closed = true;
        const std::uint64_t relative = std::uint64_t(frame) - base_frame_;
        if (relative < config_.start_frame) { MarkEpisodeWindowCut(); return; }
        const auto offset = relative - config_.start_frame;
        const auto index = offset / config_.period_frames;
        if (index >= config_.windows || offset % config_.period_frames >= config_.window_frames) { MarkEpisodeWindowCut(); return; }
        auto& window = windows_[index];
        window.started = true;
        window.start = std::uint64_t(base_frame_) + config_.start_frame + index * config_.period_frames;
        window.end = window.start + config_.window_frames;
        window.observed_mask |= 1ULL << (std::uint64_t(frame) - window.start);
        if (!frame_count_ || frames_[frame_count_ - 1] != frame) {
            if (frame_count_ == frames_.size()) { capacity_exhausted_ = true; return; }
            frames_[frame_count_++] = frame;
        }
        in_run_ = true;
        current_thread_ = ThreadSlot(thread);
        current_bucket_ = Other;
        ++threads_[current_thread_].buckets[Other].transitions;
        last_valid_ = Read(last_ns_);
    }
    void EndRun() {
        if (!in_run_) return;
        Charge(); in_run_ = false; last_valid_ = false; external_open_ = false;
    }
    void SwitchThread(std::uint32_t thread) {
        if (!in_run_ || external_open_) return;
        const auto slot = ThreadSlot(thread);
        Transition(config_.episode_mode ? (threads_[slot].episode.active ? Episode : Other) : current_bucket_, slot);
    }
    void PrepareBlock(std::uint32_t start, std::uint32_t thread) {
        if (!in_run_ || config_.episode_mode) return;
        std::size_t category = Other;
        for (std::size_t i = 0; i < config_.range_count; ++i)
            if (start >= config_.ranges[i].start && start < config_.ranges[i].end) category = i + 2;
        Transition(category, ThreadSlot(thread));
    }
    void Block(std::uint32_t start, std::uint32_t end, std::uint32_t thread) {
        if (Enabled() && config_.episode_mode) { EpisodeBlock(start, end, thread); return; }
        if (!in_run_) return;
        std::size_t category = Other;
        for (std::size_t i = 0; i < config_.range_count; ++i) {
            const auto& range = config_.ranges[i];
            if (start >= range.start && start < range.end) category = i + 2;
            if ((range.start > start && range.start < end) || (range.end > start && range.end < end))
                ++straddles_;
        }
        const auto slot = ThreadSlot(thread);
        Transition(category, slot);
        ++threads_[slot].buckets[category].blocks;
    }
    void EnterExternal() {
        if (!in_run_) return;
        external_open_ = true;
        Transition(config_.episode_mode && threads_[current_thread_].episode.active ? Episode : External, current_thread_);
    }
    void LeaveExternal(std::uint32_t thread) {
        if (!in_run_) return;
        external_open_ = false;
        const auto slot = ThreadSlot(thread);
        Transition(config_.episode_mode && threads_[slot].episode.active ? Episode : Other, slot);
    }
    std::uint64_t CPUTime() const { return cpu_ns_; }
    std::uint64_t ClockReads() const { return reads_; }
    const std::array<Thread, MaxThreads + 1>& Threads() const { return threads_; }
    void Write(std::ostream& out) const {
        bool complete = Enabled() && frame_count_ == std::uint64_t(config_.windows) * config_.window_frames &&
                        !failures_ && !calibration_failures_ && !unassigned_ && !overflow_ &&
                        !straddles_ && !in_run_ && !frame_resets_ && !capacity_exhausted_;
        for (std::size_t i = 0; i < config_.windows; ++i)
            if (!windows_[i].started || !windows_[i].closed || !Full(windows_[i])) complete = false;
        for (const auto& thread : threads_) if (thread.episode.sampled_aborted) complete = false;
        out << "{\"enabled\":" << (Enabled() ? "true" : "false")
            << ",\"outside_run_cpu_ns\":null"
            << ",\"config_error\":\"" << config_.error << "\",\"kind\":\"thread_cpu_by_block_start_disjoint_ranges\""
            << ",\"mode\":\"" << (config_.episode_mode ? "episode" : "disjoint") << "\""
            << ",\"episode_start_pc\":";
        if (config_.episode_mode) out << config_.episode_start; else out << "null";
        out << ",\"episode_end_pc\":";
        if (config_.episode_mode) out << config_.episode_end; else out << "null";
        out << ",\"base_frame\":";
        if (have_base_) out << base_frame_; else out << "null";
        out << ",\"frame_resets\":" << frame_resets_ << ",\"start_frame\":" << config_.start_frame
            << ",\"window_frames\":" << config_.window_frames << ",\"period_frames\":" << config_.period_frames
            << ",\"window_count\":" << config_.windows << ",\"sampled_refreshes\":" << frame_count_
            << ",\"cpu_ns\":" << cpu_ns_ << ",\"clock_reads\":" << reads_
            << ",\"clock_failures\":" << failures_ << ",\"unassigned_intervals\":" << unassigned_
            << ",\"coverage_complete\":" << (complete ? "true" : "false")
            << ",\"calibration_reads\":" << calibration_reads_ << ",\"calibration_failures\":" << calibration_failures_
            << ",\"clock_ns_per_read\":" << clock_ns_per_read_
            << ",\"sample_capacity_exhausted\":" << (capacity_exhausted_ ? "true" : "false")
            << ",\"thread_table_overflow_entries\":" << overflow_ << ",\"straddling_blocks\":" << straddles_
            << ",\"ranges\":[";
        for (std::size_t i = 0; i < config_.range_count; ++i) {
            const auto& r = config_.ranges[i];
            out << (i ? "," : "") << "{\"name\":\"" << r.name << "\",\"start\":" << r.start << ",\"end\":" << r.end << '}';
        }
        out << "],\"sampled_frame_ids\":[";
        for (std::size_t i = 0; i < frame_count_; ++i) out << (i ? "," : "") << frames_[i];
        out << "],\"windows\":[";
        bool first = true;
        for (std::size_t i = 0; i < config_.windows; ++i) {
            const auto& w = windows_[i]; if (!w.started) continue;
            out << (first ? "" : ",") << "{\"index\":" << i << ",\"start_frame\":" << w.start
                << ",\"end_frame\":" << w.end << ",\"observed_frames\":" << CountBits(w.observed_mask)
                << ",\"closed\":" << (w.closed ? "true" : "false") << ",\"complete\":" << (Full(w) ? "true" : "false") << '}';
            first = false;
        }
        out << "],\"threads\":["; first = true;
        for (std::size_t i = 0; i < threads_.size(); ++i) {
            const auto& t = threads_[i]; if (!t.used) continue;
            std::uint64_t total = 0;
            for (const auto& b : t.buckets) total += b.cpu_ns;
            out << (first ? "" : ",") << "{\"thread_id\":";
            if (i == MaxThreads) out << "null"; else out << t.id;
            out << ",\"overflow\":" << (i == MaxThreads ? "true" : "false") << ",\"cpu_ns\":" << total << ",\"buckets\":[";
            for (std::size_t j = 0; j < (config_.episode_mode ? 3 : config_.range_count + 2); ++j) {
                const auto& b = t.buckets[j];
                const auto name = j == Other ? "other_guest" : j == External ? "svc_external" : config_.episode_mode ? "episode" : config_.ranges[j - 2].name.c_str();
                out << (j ? "," : "") << "{\"name\":\"" << name << "\",\"cpu_ns\":" << b.cpu_ns
                    << ",\"blocks\":" << b.blocks << ",\"transitions\":" << b.transitions << '}';
            }
            out << "]";
            if (config_.episode_mode) {
                const auto& e = t.episode;
                out << ",\"episode\":{\"started\":" << e.started << ",\"completed\":" << e.completed
                    << ",\"aborted\":" << e.aborted << ",\"reentries\":" << e.reentries
                    << ",\"sampled_completed_cpu_ns\":" << e.completed_cpu
                    << ",\"sampled_partial_cpu_ns\":" << e.partial_cpu
                    << ",\"sampled_aborted_cpu_ns\":" << e.aborted_cpu
                    << ",\"active_sampled_cpu_ns\":" << e.current_cpu
                    << ",\"partial_sampled_episodes\":" << e.partial_sampled + (e.active && e.partial && e.current_cpu ? 1 : 0)
                    << ",\"sampled_aborted_episodes\":" << e.sampled_aborted
                    << ",\"active\":" << (e.active ? "true" : "false")
                    << ",\"active_partial\":" << (e.active && e.partial ? "true" : "false") << '}';
            }
            out << '}'; first = false;
        }
        out << "]}";
    }
private:
    void MarkEpisodeWindowCut() {
        if (config_.episode_mode) for (auto& t : threads_) if (t.episode.active) t.episode.partial = true;
    }
    void FinishEpisode(std::size_t slot, bool aborted) {
        if (in_run_) Transition(Other, slot);
        auto& e = threads_[slot].episode;
        if (aborted) { ++e.aborted; e.aborted_cpu += e.current_cpu; if (e.sampled_seen) ++e.sampled_aborted; }
        else { ++e.completed; (e.partial ? e.partial_cpu : e.completed_cpu) += e.current_cpu; }
        if (e.partial && e.current_cpu) ++e.partial_sampled;
        e.active = false; e.partial = false; e.sampled_seen = false; e.current_cpu = 0;
    }
    void EpisodeBlock(std::uint32_t start, std::uint32_t end, std::uint32_t thread) {
        std::size_t slot = 0;
        while (slot < thread_count_ && threads_[slot].id != thread) ++slot;
        if (slot == thread_count_) {
            if (!in_run_ && start != config_.episode_start) return;
            slot = ThreadSlot(thread);
        }
        auto& e = threads_[slot].episode;
        if (start == config_.episode_end && e.active) FinishEpisode(slot, false);
        else {
            if (start == config_.episode_start) {
                if (e.active) ++e.reentries;
                else { e.active = true; e.partial = !in_run_; e.current_cpu = 0; ++e.started; }
            }
            if (e.active) {
                if (in_run_) e.sampled_seen = true;
                bool allowed = false;
                for (std::size_t i = 0; i < config_.range_count; ++i)
                    allowed |= start >= config_.ranges[i].start && start < config_.ranges[i].end &&
                               end > start && end <= config_.ranges[i].end;
                if (!allowed) FinishEpisode(slot, true);
            }
        }
        if (in_run_) {
            const auto bucket = e.active ? Episode : Other;
            Transition(bucket, slot); ++threads_[slot].buckets[bucket].blocks;
        }
    }
    bool Read(std::uint64_t& now) {
        ++reads_;
        if (!clock_(clock_context_, now)) { ++failures_; return false; }
        return true;
    }
    void Charge() {
        std::uint64_t now = 0;
        const bool valid = Read(now);
        if (valid && last_valid_ && now >= last_ns_) {
            const auto elapsed = now - last_ns_;
            threads_[current_thread_].buckets[current_bucket_].cpu_ns += elapsed;
            cpu_ns_ += elapsed;
            if (config_.episode_mode && current_bucket_ == Episode) threads_[current_thread_].episode.current_cpu += elapsed;
        } else {
            if (valid && last_valid_ && now < last_ns_) ++failures_;
            ++unassigned_;
        }
        const bool backward = valid && last_valid_ && now < last_ns_;
        last_ns_ = now; last_valid_ = valid && !backward;
    }
    void Transition(std::size_t bucket, std::size_t thread) {
        if (current_bucket_ == bucket && current_thread_ == thread) return;
        Charge(); current_bucket_ = bucket; current_thread_ = thread;
        ++threads_[thread].buckets[bucket].transitions;
    }
    std::size_t ThreadSlot(std::uint32_t id) {
        for (std::size_t i = 0; i < thread_count_; ++i) if (threads_[i].id == id) return i;
        if (thread_count_ < MaxThreads) {
            auto& t = threads_[thread_count_]; t.id = id; t.used = true;
            return thread_count_++;
        }
        ++overflow_; threads_[MaxThreads].used = true; return MaxThreads;
    }
    bool Full(const Window& w) const {
        return w.observed_mask == (config_.window_frames == 64 ? UINT64_MAX : (1ULL << config_.window_frames) - 1);
    }
    static unsigned CountBits(std::uint64_t bits) {
        unsigned n = 0; for (; bits; bits &= bits - 1) ++n; return n;
    }
    Config config_;
    Clock clock_; void* clock_context_;
    std::array<Thread, MaxThreads + 1> threads_{};
    std::array<Window, MaxWindows> windows_{};
    std::array<std::uint32_t, MaxFrames> frames_{};
    std::size_t thread_count_ = 0, frame_count_ = 0, current_thread_ = 0, current_bucket_ = Other;
    bool external_open_ = false;
    bool in_run_ = false, last_valid_ = false, have_base_ = false, capacity_exhausted_ = false;
    std::uint32_t base_frame_ = 0, last_frame_ = 0;
    std::uint64_t cpu_ns_ = 0, reads_ = 0, failures_ = 0, unassigned_ = 0, overflow_ = 0, straddles_ = 0;
    std::uint64_t last_ns_ = 0, frame_resets_ = 0, calibration_reads_ = 0, calibration_failures_ = 0;
    double clock_ns_per_read_ = 0;
};
} // namespace Core::FastInterp::GuestPCCPU
