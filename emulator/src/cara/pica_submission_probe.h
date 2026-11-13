// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

namespace Cara {
// A logical submission spans command-buffer jumps. Observe executed writes only,
// never scan unexecuted memory. IRQ means the existing engine's trigger path;
// this probe does not infer hardware byte matching or AUTOSTOP state.
struct PicaSubmissionProbe {
    bool saw_irq = false;
    std::uint64_t writes = 0, draws = 0, jumps = 0, first_irq_remaining_words = 0;
    void Write(bool irq, bool draw, bool jump, std::uint64_t remaining_words = 0) {
        if (saw_irq) {
            ++writes;
            draws += draw;
            jumps += jump;
        }
        if (irq && !saw_irq) {
            saw_irq = true;
            first_irq_remaining_words = remaining_words;
        }
    }
};
} // namespace Cara
