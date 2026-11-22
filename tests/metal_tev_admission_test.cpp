// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_metal/pica_metal_tev_admission.h"

int main() {
    using Mode = Cara::RuntimeTevMode;
    if (Cara::ParseRuntimeTevMode(nullptr) != Mode::Adaptive ||
        Cara::ParseRuntimeTevMode("adaptive") != Mode::Adaptive ||
        Cara::ParseRuntimeTevMode("burst") != Mode::Burst ||
        Cara::ParseRuntimeTevMode("0") != Mode::Off ||
        Cara::ParseRuntimeTevMode("1") != Mode::Always)
        return 5;
    Cara::PicaMetalTevAdmission policy;
    char vs[2]{}, fs[8]{};
    for (unsigned i = 0; i < 5; ++i) {
        for (unsigned repeated = 0; repeated < 8; ++repeated)
            if (policy.Observe(vs, fs + i, false, 1))
                return 1;
        if (policy.Observe(vs+1, fs + i, false, 1) || policy.Observe(vs, fs + i, true, 1))
            return 2;
    }
    if (!policy.Observe(vs, fs + 5, false, 2) || !policy.Observe(vs, fs, false, 30))
        return 3;
    if (policy.Observe(vs, fs + 6, false, 63) || policy.Observe(vs+1, fs + 5, false, 34))
        return 4;
    if (policy.ObserveBurst(fs, false, 1) || policy.ObserveBurst(fs, false, 1) ||
        policy.ObserveBurst(fs + 1, false, 1) || policy.ObserveBurst(fs + 2, true, 1) ||
        !policy.ObserveBurst(fs + 2, false, 1) || !policy.ObserveBurst(fs, false, 1) ||
        policy.ObserveBurst(fs + 3, false, 2))
        return 6;
    return 0;
}
