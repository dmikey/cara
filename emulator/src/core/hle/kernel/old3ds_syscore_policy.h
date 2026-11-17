// Copyright 2026 Cara contributors
// Licensed under GPLv2 or any later version; see license.txt.
// Original small policy, not a port of Azahar/Luma code. Interface references:
// LumaTeam/Luma3DS sysmodules/pm/source/reslimit.c (descriptor/mode);
// docs.mikage.app/KResourceLimit/ (type0 allowance). Hardware edge behavior unverified.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace Kernel::Old3dsSyscore {
inline bool Requested() {
    static const bool requested = [] {
        const char* value = std::getenv("CARA_OLD3DS_SYSCORE");
        return value && std::strcmp(value, "1") == 0;
    }();
    return requested;
}
struct Policy {
    // Same base-clock conversion as Core::nsToCycles(2'000'000), integer only.
    static constexpr std::int64_t period = 268111856LL * 2 / 1000;
    std::uint16_t descriptor = 0;
    std::uint32_t percent = 0;
    std::int64_t epoch = 0;
    std::int64_t deadline = 0;
    bool app_phase = false;
    bool pending = false;

    bool Supported() const {
        // Unknown upper descriptor bits and single mode explicitly retain legacy behavior.
        return (descriptor & 0xff80) == 0x0080 && MaxPercent() >= 5 && MaxPercent() <= 89;
    }
    std::uint32_t MaxPercent() const { return descriptor & 0x7f; }
    void Configure(std::uint16_t value) {
        descriptor = value;
        percent = 0;
        epoch = deadline = 0;
        app_phase = pending = false;
    }
    bool SetLimit(std::uint32_t value, std::int64_t now) {
        if (!Supported() || (value && (value < 5 || value > MaxPercent()))) return false;
        percent = value;
        epoch = now;
        // A limit change starts a SYS reservation, matching the supported multi reference.
        Refresh(now);
        return true;
    }
    void Refresh(std::int64_t now) {
        pending = Supported() && percent != 0;
        if (!pending) { app_phase = false; deadline = 0; return; }
        const auto app_ticks = period * percent / 100;
        const auto sys_ticks = period - app_ticks;
        // APT executes on either core; the other timer can still lag the change timestamp.
        // Keep it ineligible until it reaches the new reservation, without rewinding time.
        if (now < epoch) { app_phase = false; deadline = epoch + sys_ticks; return; }
        const auto elapsed = now - epoch;
        const auto offset = elapsed % period;
        const auto cycle_start = now - offset;
        app_phase = offset >= sys_ticks;
        deadline = cycle_start + (app_phase ? period : sys_ticks);
    }
    bool AppEligible() const { return pending && app_phase; }
    void Validate() const {
        if (percent && (!Supported() || percent < 5 || percent > MaxPercent()))
            throw std::runtime_error("Invalid saved syscore allowance");
        if (pending != (Supported() && percent != 0) || (pending && deadline <= epoch))
            throw std::runtime_error("Invalid saved syscore phase");
    }
    template<class Archive> void Serialize(Archive& ar) {
        ar & descriptor; ar & percent; ar & epoch; ar & deadline;
        ar & app_phase; ar & pending;
    }
};
} // namespace Kernel::Old3dsSyscore
