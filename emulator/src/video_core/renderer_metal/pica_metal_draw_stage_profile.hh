// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#import <Metal/Metal.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <ostream>
#include <string_view>

namespace Cara::MetalDrawStageProfile {

struct Total {
    std::uint64_t calls{}, value{};
    void Add(std::uint64_t n) { ++calls; value += n; }
};

struct Slot {
    id<MTLCommandBuffer> command = nil;
    id<MTLCounterSampleBuffer> buffer = nil;
    MTLTimestamp cpu_start{}, gpu_start{};
};

struct Resources {
    std::array<Slot,8> slots{};
    id<MTLCounterSet> counters = nil;
    bool initialized = false;
};
inline thread_local std::unique_ptr<Resources> resources;
inline thread_local std::uint64_t attempted{}, armed{}, unsupported{}, failed{};
inline thread_local std::array<Total, 2> vertex{}, fragment{}, command_span{};
inline thread_local std::array<Total, 5> draw_sizes{}, draw_waits{};
inline thread_local Total missing_draw_spans{};
inline thread_local std::uint64_t allocation_failure{}, invalid_data{}, invalid_clock{},
    invalid_stamp{}, outside_command{};

inline bool Enabled() {
    static const bool enabled = [] {
        const auto* flag = std::getenv("CARA_METAL_DRAW_STAGE_PROFILE");
        return flag && std::string_view(flag) == "1";
    }();
    return enabled;
}

inline void Attach(MTLRenderPassDescriptor* pass, id<MTLCommandBuffer> command) {
    if (!Enabled())
        return;
    const auto seq=attempted++;
    // Rotate the sample phase per block so a periodic scene cannot
    // permanently exclude the same positions in each block.
    if ((seq & 31) != ((seq >> 5) & 31))
        return;
    if (!resources)
        resources=std::make_unique<Resources>();
    auto& [slots,counters,initialized]=*resources;
    id<MTLDevice> device=command.commandQueue.device;
    if (!initialized) {
        initialized=true;
        if ([device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]) {
            for (id<MTLCounterSet> set in device.counterSets) {
                if ([set.name isEqualToString:MTLCommonCounterSetTimestamp]) {
                    counters=set;
                    break;
                }
            }
        }
    }
    if (!counters) {
        ++unsupported;
        return;
    }
    // Sample at most one render pass inside each command buffer.
    for (const auto& slot : slots) {
        if (slot.command == command)
            return;
    }
    for (auto& slot : slots) {
        if (slot.command)
            continue;
        if (!slot.buffer) {
            auto* config=[MTLCounterSampleBufferDescriptor new];
            config.counterSet=counters;
            config.sampleCount=4;
            config.storageMode=MTLStorageModeShared;
            NSError* error=nil;
            slot.buffer=[device newCounterSampleBufferWithDescriptor:config error:&error];
        }
        if (!slot.buffer) {
            ++allocation_failure;
            ++failed;
            return;
        }
        auto* attachment=pass.sampleBufferAttachments[0];
        attachment.sampleBuffer=slot.buffer;
        attachment.startOfVertexSampleIndex=0;
        attachment.endOfVertexSampleIndex=1;
        attachment.startOfFragmentSampleIndex=2;
        attachment.endOfFragmentSampleIndex=3;
        [device sampleTimestamps:&slot.cpu_start gpuTimestamp:&slot.gpu_start];
        slot.command=command;
        ++armed;
        return;
    }
    ++failed;
}

inline void Cancel(id<MTLCommandBuffer> command) {
    if (!Enabled() || !resources)
        return;
    for (auto& slot : resources->slots) {
        if (slot.command == command) {
            slot.command=nil;
            ++failed;
            return;
        }
    }
}

inline void Retire(id<MTLCommandBuffer> command, std::size_t draws, std::uint64_t wait,
                   bool valid_command_span, std::uint64_t gpu_ns) {
    if (!Enabled() || !draws)
        return;
    const auto bin=draws==1 ? 0 : draws<=4 ? 1 : draws<=8 ? 2 : draws<=16 ? 3 : 4;
    if (valid_command_span)
        draw_sizes[bin].Add(gpu_ns);
    else
        missing_draw_spans.Add(0);
    if (wait)
        draw_waits[bin].Add(wait);
    if (!resources)
        return;
    for (auto& slot : resources->slots) {
        if (slot.command != command)
            continue;
        MTLTimestamp cpu_end{}, gpu_end{};
        [command.commandQueue.device sampleTimestamps:&cpu_end gpuTimestamp:&gpu_end];
        auto* data=[slot.buffer resolveCounterRange:NSMakeRange(0,4)];
        slot.command=nil;
        std::array<MTLCounterResultTimestamp,4> stamps{};
        if (data.length != sizeof(stamps) || !valid_command_span) {
            ++invalid_data;
            ++failed;
            return;
        }
        std::memcpy(stamps.data(),data.bytes,sizeof(stamps));
        for (const auto& stamp : stamps) {
            if (!stamp.timestamp || stamp.timestamp==UINT64_MAX ||
                stamp.timestamp < slot.gpu_start || stamp.timestamp > gpu_end) {
                ++invalid_stamp;
                ++failed;
                return;
            }
        }
        if (stamps[1].timestamp < stamps[0].timestamp ||
            stamps[3].timestamp < stamps[2].timestamp) {
            ++failed;
            return;
        }
        if (cpu_end <= slot.cpu_start || gpu_end <= slot.gpu_start) {
            ++invalid_clock;
            ++failed;
            return;
        }
        // Metal's CPU samples are already in nanoseconds. Correlate the
        // separately sampled GPU clock using the same command's endpoints.
        const double ratio = double(cpu_end - slot.cpu_start) / double(gpu_end - slot.gpu_start);
        const auto ns = [ratio](std::uint64_t ticks) { return double(ticks) * ratio; };
        const double vs=ns(stamps[1].timestamp-stamps[0].timestamp);
        const double fs=ns(stamps[3].timestamp-stamps[2].timestamp);
        // The two stage intervals can overlap. Each should lie within
        // its sampled command interval up to timestamp rounding.
        if (vs>gpu_ns+1000.0 || fs>gpu_ns+1000.0) {
            ++outside_command;
            ++failed;
            return;
        }
        const auto blocked=wait>0 ? 1 : 0;
        vertex[blocked].Add(static_cast<std::uint64_t>(vs));
        fragment[blocked].Add(static_cast<std::uint64_t>(fs));
        command_span[blocked].Add(gpu_ns);
        return;
    }
}

inline void Append(std::ostream& out) {
    if (!Enabled())
        return;
    const auto row=[&](const char* name,Total total) {
        out << "draw_stage_" << name << ',' << total.calls << ',' << total.value << '\n';
    };
    row("attempted",{attempted,0});
    row("armed",{armed,0});
    row("unsupported",{unsupported,0});
    row("failed",{failed,0});
    row("allocation_failure",{allocation_failure,0});
    row("invalid_data",{invalid_data,0});
    row("invalid_clock",{invalid_clock,0});
    row("invalid_stamp",{invalid_stamp,0});
    row("outside_command",{outside_command,0});
    row("missing_draw_spans",missing_draw_spans);
    constexpr const char* buckets[]={"one","two_to_four","five_to_eight","nine_to_sixteen","seventeen_to_32"};
    for (std::size_t i=0;i<draw_sizes.size();++i) {
        out << "draw_stage_size_" << buckets[i] << ',' << draw_sizes[i].calls << ',' << draw_sizes[i].value << '\n';
        out << "draw_stage_wait_" << buckets[i] << ',' << draw_waits[i].calls << ',' << draw_waits[i].value << '\n';
    }
    row("nonblocking_vertex",vertex[0]); row("blocking_vertex",vertex[1]);
    row("nonblocking_fragment",fragment[0]); row("blocking_fragment",fragment[1]);
    row("nonblocking_command",command_span[0]); row("blocking_command",command_span[1]);
}

inline void AppendWindow(std::ostream& out) {
    if (!Enabled())
        return;
    out << ",\"metal_stage_samples\":["
        << attempted << ',' << armed << ',' << unsupported << ',' << failed;
    for (std::size_t blocked = 0; blocked < vertex.size(); ++blocked) {
        out << ',' << vertex[blocked].calls << ',' << vertex[blocked].value
            << ',' << fragment[blocked].value << ',' << command_span[blocked].value;
    }
    out << ']';
}

} // namespace Cara::MetalDrawStageProfile
