#include <hwcpipe/gpu.hpp>
#include <hwcpipe/sampler.hpp>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <time.h>
#include <unistd.h>

namespace {

struct Counter {
    const char* name;
    hwcpipe_counter id;
};

#define COUNTER(name) {#name, name}
constexpr Counter requested[] = {
    COUNTER(MaliGPUActiveCy),
    COUNTER(MaliFragQueueActiveCy),
    COUNTER(MaliNonFragQueueActiveCy),
    COUNTER(MaliTilerActiveCy),
    COUNTER(MaliTilerPosShadWaitCy),
    COUNTER(MaliTilerPosShadFIFOFullCy),
    COUNTER(MaliTilerPosShadStallCy),
    COUNTER(MaliTilerVarShadStallCy),
    COUNTER(MaliTilerPosShadTask),
    COUNTER(MaliTilerVarShadTask),
    COUNTER(MaliTilerPosCacheHit),
    COUNTER(MaliTilerPosCacheMiss),
    COUNTER(MaliTilerVarCacheHit),
    COUNTER(MaliTilerVarCacheMiss),
    COUNTER(MaliGeomTotalPrim),
    COUNTER(MaliGeomVisiblePrim),
    COUNTER(MaliTilerRdBy),
    COUNTER(MaliTilerWrBy),
    COUNTER(MaliCoreActiveCy),
    COUNTER(MaliFragActiveCy),
    COUNTER(MaliNonFragActiveCy),
    COUNTER(MaliCoreUtil),
    COUNTER(MaliFragUtil),
    COUNTER(MaliNonFragUtil),
    COUNTER(MaliTilerUtil),
    COUNTER(MaliFragQueueWaitDepCy),
    COUNTER(MaliNonFragQueueWaitDepCy),
    COUNTER(MaliFragQueueWaitFlushCy),
    COUNTER(MaliNonFragQueueWaitFlushCy),
    COUNTER(MaliFragQueueWaitIssueCy),
    COUNTER(MaliNonFragQueueWaitIssueCy),
    COUNTER(MaliExtBusRdBy),
    COUNTER(MaliExtBusWrBy),
    COUNTER(MaliExtBusRdStallRate),
    COUNTER(MaliExtBusWrStallRate),
    COUNTER(MaliL2CacheRdMissRate),
    COUNTER(MaliSCBusTileWrBy),
    COUNTER(MaliALUUtil),
    COUNTER(MaliLSUtil),
    COUNTER(MaliTexUtil),
    COUNTER(MaliTexDataFetchStallCy),
    COUNTER(MaliFragShadedQd),
    COUNTER(MaliFragOverdraw),
    COUNTER(MaliFragEZSKillRate),
    COUNTER(MaliFragLZSKillRate),
};
#undef COUNTER

long parse(const char* input, long low, long high) {
    char* end = nullptr;
    errno = 0;
    const long value = std::strtol(input, &end, 10);
    return errno || end == input || *end != '\0' || value < low || value > high ? 0 : value;
}

std::uint64_t nanos(clockid_t clock) {
    timespec now{};
    if (clock_gettime(clock, &now) == -1) {
        std::cerr << "clock_gettime: " << std::strerror(errno) << '\n';
        std::exit(1);
    }
    return static_cast<std::uint64_t>(now.tv_sec) * 1000000000 + now.tv_nsec;
}

} // namespace

int main(int argc, char** argv) {
    const long seconds = argc == 3 ? parse(argv[1], 1, 300) : 0;
    const long interval_ms = argc == 3 ? parse(argv[2], 20, 1000) : 0;
    if (seconds == 0 || interval_ms == 0) {
        std::cerr << "usage: cara-mali-sample SECONDS[1..300] INTERVAL_MS[20..1000]\n";
        return 2;
    }

    const int fd = open("/dev/mali0", O_RDWR | O_CLOEXEC);
    if (fd == -1) {
        std::cerr << "open /dev/mali0: " << std::strerror(errno) << '\n';
        return 1;
    }
    close(fd);

    hwcpipe::gpu gpu(0);
    if (!gpu) {
        std::cerr << "/dev/mali0 opened, but Arm libGPUCounters could not detect its backend\n";
        return 1;
    }
    std::cerr << "shader_cores=" << gpu.num_shader_cores() << '\n';

    hwcpipe::sampler_config config(gpu);
    std::vector<Counter> counters;
    for (const Counter& counter : requested) {
        if (auto result = config.add_counter(counter.id)) {
            std::cerr << counter.name << ": " << result.message() << '\n';
        } else {
            counters.push_back(counter);
        }
    }
    if (counters.empty() || counters.front().id != MaliGPUActiveCy) {
        std::cerr << "required GPU activity counter is unavailable\n";
        return 1;
    }

    auto sampler = hwcpipe::sampler<>(config);
    if (auto result = sampler.start_sampling()) {
        std::cerr << "start_sampling: " << result.message() << '\n';
        return 1;
    }

    std::cout << std::setprecision(17)
              << "monotonic_ns,boottime_ns,driver_interval_begin_ns,monotonic_sample_request_ns";
    for (const auto& counter : counters) {
        std::cout << ',' << counter.name;
    }
    std::cout << '\n';

    using Clock = std::chrono::steady_clock;
    const auto end = Clock::now() + std::chrono::seconds(seconds);
    const auto interval = std::chrono::milliseconds(interval_ms);
    auto next = Clock::now() + interval;
    do {
        std::this_thread::sleep_until(next);
        const auto before = nanos(CLOCK_MONOTONIC);
        if (auto result = sampler.sample_now()) {
            std::cerr << "sample_now: " << result.message() << '\n';
            return 1;
        }
        const auto after = nanos(CLOCK_MONOTONIC);
        const auto boot = nanos(CLOCK_BOOTTIME);

        hwcpipe::counter_sample activity;
        if (auto result = sampler.get_counter_value(MaliGPUActiveCy, activity)) {
            std::cerr << "read MaliGPUActiveCy: " << result.message() << '\n';
            return 1;
        }
        std::cout << after << ',' << boot << ',' << activity.timestamp << ',' << before;
        for (const auto& counter : counters) {
            std::cout << ',';
            hwcpipe::counter_sample sample;
            if (auto result = sampler.get_counter_value(counter.id, sample)) {
                std::cerr << "read " << counter.name << ": " << result.message() << '\n';
                continue;
            }
            if (sample.type == hwcpipe::counter_sample::type::uint64) {
                std::cout << sample.value.uint64;
            } else {
                std::cout << sample.value.float64;
            }
        }
        std::cout << '\n';
        next += interval;
        if (next <= Clock::now()) {
            next = Clock::now() + interval;
        }
    } while (next <= end);

    if (auto result = sampler.stop_sampling()) {
        std::cerr << "stop_sampling: " << result.message() << '\n';
        return 1;
    }
    return std::cout.good() ? 0 : 1;
}
