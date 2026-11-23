// SPDX-License-Identifier: GPL-2.0-or-later
// Compile the actual production lane implementation, intercepting only its
// final LCD call. This observes whether production Present reaches the external
// visibility boundary without requiring a window or showing unchecked pixels.
#include "video_core/renderer_metal/pica_metal_native_draw_lane.h"
namespace Cara::MetalLCD {
Receipt ProbePresentNative(id<MTLCommandQueue>, CAMetalLayer*, const Layout::FramebufferLayout&,
    const Pipelines&, Filter, EdgeMode, const ResolveNativeScreen&, bool,
    std::array<std::optional<Screen>, 2>*, unsigned, LosslessBuffers*, bool);
}
#define PresentNative ProbePresentNative
#include "video_core/renderer_metal/pica_metal_native_draw_lane.mm"
#undef PresentNative
#define CARA_ASYNC_DEPTH_FIXTURE_ONLY
#include "metal_async_depth_validation_test.mm"
#undef CARA_ASYNC_DEPTH_FIXTURE_ONLY

namespace {
unsigned lcd_calls = 0;
bool last_capture = false, last_present = false;
constexpr std::string_view visibility_failure = "Native Metal presentation depends on failed validation";
}
namespace Cara::MetalLCD {
Receipt ProbePresentNative(id<MTLCommandQueue>, CAMetalLayer*, const Layout::FramebufferLayout&,
    const Pipelines&, Filter, EdgeMode, const ResolveNativeScreen&, bool capture,
    std::array<std::optional<Screen>, 2>*, unsigned, LosslessBuffers*, bool present) {
    ++lcd_calls;
    last_capture = capture;
    last_present = present;
    return {Status::DrawableUnavailable, nil};
}
}

// Status-only command for a precise no-wait/mismatched-receipt unit check of
// the SAME private helper used by production Present. No driver fault injected.
@interface CompletedPresentationCommand : NSObject {
@public
    id<MTLDevice> owner_device;
    unsigned waits;
}
@end
@implementation CompletedPresentationCommand
- (MTLCommandBufferStatus)status { return MTLCommandBufferStatusCompleted; }
- (NSError*)error { return nil; }
- (id<MTLDevice>)device { return owner_device; }
- (void)waitUntilCompleted { ++waits; }
@end
namespace {
Cara::MetalLCD::Receipt RequestPresentation(DepthFixture& fixture, bool present, bool capture) {
    return fixture.f.lane.Present(nil, {}, {}, Cara::MetalLCD::Filter::Nearest,
        Cara::MetalLCD::EdgeMode::ClampToEdge, fixture.f.Reader(), capture, 1, present);
}
void SetupDepth(DepthFixture& fixture, bool invalid) {
    const auto original = fixture.Acquire(DepthFixture::a);
    if (invalid)
        fixture.SeedInvalid(original);
    fixture.Publish(original);
    fixture.f.lane.Drain();
    (void)fixture.Acquire(DepthFixture::b);
}
void CheckTerminalAndMismatched(id<MTLDevice> device) {
    for (unsigned bad = 0; bad != 3; ++bad) {
        auto command = [CompletedPresentationCommand new];
        command->owner_device = device;
        auto other = [CompletedPresentationCommand new];
        other->owner_device = device;
        std::array<id<MTLBuffer>,2> errors;
        for (unsigned i = 0; i < errors.size(); ++i) {
            errors[i] = [device newBufferWithLength:sizeof(u32) options:MTLResourceStorageModeShared];
            *static_cast<u32*>(errors[i].contents) = bad == i + 1 ? 1 : 0;
        }
        const auto receipt = std::make_shared<PicaMetalValidationReceipt>(
            (id<MTLCommandBuffer>)command, errors);
        MetalProducerPredecessors::Commands commands{};
        MetalProducerPredecessors::Validations validations{};
        commands[0] = (id<MTLCommandBuffer>)command;
        validations[0] = receipt;
        const MetalProducerPredecessors original(commands, validations, 1);
        Require(Cara::ValidatePresentationReceipts(original) == (bad == 0),
                "Completed presentation receipt check differs");
        Require(command->waits == 0, "Already-completed validation introduced a GPU wait");
        if (!bad) {
            commands[0] = (id<MTLCommandBuffer>)other;
            const MetalProducerPredecessors mismatched(commands, validations, 1);
            Require(!Cara::ValidatePresentationReceipts(mismatched),
                    "Mismatched presentation receipt was admitted");
            const MetalProducerPredecessors malformed(commands, validations, commands.size() + 1);
            Require(!Cara::ValidatePresentationReceipts(malformed),
                    "Malformed presentation dependency bounds were admitted");
        }
    }
    Require(lcd_calls == 0, "Private helper tests unexpectedly invoked LCD");
}
}
int main(int argc, char** argv) { @autoreleasepool { try {
    Require(argc == 2, "Pass production Metal library");
    setenv("CARA_METAL_FRAME_COST", "1", 1);
    setenv("CARA_METAL_GROUPED_PASS", "1", 1);
    setenv("CARA_METAL_JOIN_CHECKED_DEPTH_IMPORT", "1", 1);
    setenv("CARA_METAL_ASYNC_CHECKED_DEPTH_IMPORT", "1", 1);
    setenv("CARA_METAL_FUSE_DEPTH_CLEAR", "0", 1);
    setenv("CARA_METAL_EXACT_DEPTH_CLEAR", "0", 1);
    setenv("CARA_METAL_QUEUE_COLOR_EXPORT", "1", 1);
    const auto device = MTLCreateSystemDefaultDevice();
    Require(device != nil, "No Metal device");
    const auto library = [device newLibraryWithURL:
        [NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]] error:nullptr];
    Require(library != nil, "No production Metal library");
    MetalTransfer::Pipelines transfers{};
    CheckTerminalAndMismatched(device);
    unsigned cases = 0;
    {
        DepthFixture fixture(device, library, transfers);
        SetupDepth(fixture, false);
        QueueGate validation_gate(fixture.f.queue);
        const auto restored = fixture.Acquire(DepthFixture::a);
        fixture.Publish(restored);
        QueueGate unrelated_gate(fixture.f.queue);
        const auto unrelated = [fixture.f.queue commandBuffer];
        fixture.f.lane.SubmitSampledTextureBuild(unrelated);
        const auto calls = lcd_calls;
        Require(Watchdog(validation_gate, [&] {
            (void)RequestPresentation(fixture, true, false);
        }), "Visible presentation bypassed pending validation");
        Require(lcd_calls == calls + 1 && last_present && !last_capture,
                "Valid pending receipt failed to reach actual Present's LCD boundary");
        Require(unrelated_gate.event.signaledValue == 0 &&
                unrelated.status != MTLCommandBufferStatusCompleted,
                "Presentation drained unrelated later GPU commands");
        unrelated_gate.Release();
        fixture.f.lane.Drain();
        const auto checks = Count(NativeMetalCost::AsyncDepthChecksPassed);
        (void)RequestPresentation(fixture, true, false);
        Require(lcd_calls == calls + 2 && Count(NativeMetalCost::AsyncDepthChecksPassed) == checks,
                "Already-completed presentation rechecked retired validations");
        ++cases;
    }
    for (bool capture_only : {false, true}) {
        DepthFixture fixture(device, library, transfers);
        SetupDepth(fixture, true);
        QueueGate validation_gate(fixture.f.queue);
        const auto restored = fixture.Acquire(DepthFixture::a);
        fixture.Publish(restored);
        const auto calls = lcd_calls;
        bool rejected = false;
        try {
            (void)Watchdog(validation_gate, [&] {
                (void)RequestPresentation(fixture, !capture_only, capture_only);
            });
        } catch (const std::runtime_error& error) {
            rejected = std::string_view(error.what()) == visibility_failure;
            if (!rejected) throw;
        }
        Require(rejected && validation_gate.event.signaledValue == 1 && lcd_calls == calls,
                "Unchecked invalid depth reached drawable/capture preparation");
        Rejected([&] { (void)RequestPresentation(fixture, true, false); });
        Rejected([&] { (void)fixture.f.Read(source, 4); });
        Require(lcd_calls == calls, "Faulted presentation lane resumed LCD work");
        ++cases;
    }
    {
        DepthFixture fixture(device, library, transfers);
        SetupDepth(fixture, true);
        QueueGate validation_gate(fixture.f.queue);
        const auto restored = fixture.Acquire(DepthFixture::a);
        fixture.Publish(restored);
        const auto calls = lcd_calls;
        Require(!Watchdog(validation_gate, [&] {
            (void)RequestPresentation(fixture, false, false);
        }), "Internal coalesced refresh drained pending validation");
        Require(validation_gate.event.signaledValue == 0 && lcd_calls == calls + 1 &&
                !last_present && !last_capture,
                "Coalesced-only path incorrectly requested external visibility");
        validation_gate.Release();
        fixture.f.lane.Abort();
        ++cases;
    }
    std::cout << "PASS: " << cases << " actual production Present boundary cases; pending valid/invalid/capture, unrelated work, completed success and coalescing; exact helper mismatch/bounds checks\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
} } }
