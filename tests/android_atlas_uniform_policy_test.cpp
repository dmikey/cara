// SPDX-License-Identifier: GPL-2.0-or-later
#include "android/jni/atlas_uniform_policy.h"

#include <optional>
#include <iostream>

using Cara::AndroidAtlas::DefaultPersistentUniforms;
using Cara::AndroidAtlas::ResolvePersistentUniformsPreference;

int main() {
    static_assert(DefaultPersistentUniforms(true, false, "RG Slide", "ums9620_2h10_native", "", "ARM", "Mali-G57", true));
    static_assert(DefaultPersistentUniforms(true, false, "RG Slide", "", "ums9620_2h10_native", "ARM", "Mali-G57", true));
    static_assert(!DefaultPersistentUniforms(false, false, "RG Slide", "ums9620_2h10_native", "", "ARM", "Mali-G57", true));
    static_assert(!DefaultPersistentUniforms(true, true, "RG Slide", "ums9620_2h10_native", "", "ARM", "Mali-G57", true));
    static_assert(!DefaultPersistentUniforms(true, false, "RG Slide", "ums9620_2h10_native", "", "ARM", "Mali-G57", false));
    static_assert(!DefaultPersistentUniforms(true, false, "Other", "ums9620_2h10_native", "", "ARM", "Mali-G57", true));
    static_assert(!DefaultPersistentUniforms(true, false, "RG Slide", "other", "other", "ARM", "Mali-G57", true));
    static_assert(!DefaultPersistentUniforms(true, false, "RG Slide", "ums9620_2h10_native", "", "Qualcomm", "Mali-G57", true));
    static_assert(!DefaultPersistentUniforms(true, false, "RG Slide", "ums9620_2h10_native", "", "ARM", "Mali-G52", true));
    static_assert(!DefaultPersistentUniforms(true, false, "RG Slide", "ums9620_2h10_native", "", "", "", true));
    static_assert(!DefaultPersistentUniforms(true, false, "", "", "", "", "", true));

    static_assert(ResolvePersistentUniformsPreference(std::nullopt, false, true));
    static_assert(!ResolvePersistentUniformsPreference(std::nullopt, false, false));
    static_assert(!ResolvePersistentUniformsPreference(std::nullopt, true, true)); // malformed
    static_assert(!ResolvePersistentUniformsPreference(false, true, true)); // explicit opt-out
    static_assert(ResolvePersistentUniformsPreference(true, true, false)); // prior manual opt-in
    std::cout << "PASS: narrow RG Slide Atlas auto-default, unknown/missing capability, worker, accuracy and explicit config guards\n";
}
