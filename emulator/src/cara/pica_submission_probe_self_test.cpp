// SPDX-License-Identifier: GPL-2.0-or-later
#include <iostream>
#include <stdexcept>
#include "cara/pica_submission_probe.h"
void Require(bool value) {
    if (!value) throw std::runtime_error("PICA logical-submission probe mismatch");
}
int main() {
    Cara::PicaSubmissionProbe submission;
    submission.Write(false, true, false); // A legitimate draw before completion.
    submission.Write(true, false, false, 8);
    submission.Write(false, true, false); // Extra data in the IRQ packet can draw.
    submission.Write(false, false, true); // Jump does not create a new submission.
    submission.Write(false, true, false); // Draw in the jump destination.
    submission.Write(true, false, false, 2); // A later IRQ cannot replace first offset.
    Require(submission.saw_irq && submission.writes == 4 && submission.draws == 2 &&
            submission.jumps == 1 && submission.first_irq_remaining_words == 8);
    submission = {}; // New logical submission must not inherit earlier IRQ state.
    for (unsigned i = 0; i < 100000; ++i) submission.Write(false, true, false);
    Require(!submission.saw_irq && submission.writes == 0 && submission.draws == 0);
    submission.Write(true, false, false, 0);
    Require(submission.saw_irq && submission.writes == 0 && submission.first_irq_remaining_words == 0);
    std::cout << "PICA probe: pre/post IRQ, extra writes, jumps, repeated IRQ, empty tail and reset passed\n";
}
