// SPDX-License-Identifier: GPL-2.0-or-later
// Native owner lifecycle contract; no GPU or hardware timing model.
#include "../emulator/src/cara/texture_copy_trace.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
int main(int argc, char** argv) {
    assert(argc == 2);
    using namespace Cara::TextureCopyTrace;
    const std::string path = argv[1];
    std::remove(path.c_str());
    Configure(true, path, 60, 4);
    for (unsigned i = 0; i <= 63; ++i) {
        VBlank();
        Request request({});
        if (current) assert(!Flush(true)); // Active requests cannot export from a GPU hook.
    }
    assert(capture.requests == 4 && !complete);
    for (unsigned i = 0; i != 4; ++i) assert(capture.records[i].frame == 60 + i);
    assert(!Flush()); // Incomplete natural window.
    assert(Flush(true) && !TakeExportError());
    std::ifstream input(path); std::ostringstream bytes; bytes << input.rdbuf();
    assert(bytes.str().find("\"complete\":0") != std::string::npos);
    assert(!Flush(true)); // Exactly one export attempt.
    Configure(true, path, 0, 4); // Quiescent restore/rearm resets previous reasons and totals.
    VBlank(); { Request request({}); SetReason(Reason::SourceMiss); }
    for (unsigned i = 0; i != 4; ++i) VBlank();
    assert(complete && capture.requests == 1 && !capture.records[0].source_surface.valid);
    assert(Flush() && !TakeExportError());
    Configure(true, path + "/missing/output.json", 0, 4);
    assert(!Flush(true) && TakeExportError() && !TakeExportError());
    assert(!Flush(true)); // Error is reported once; no silent retry.
    Configure(false, "", 0, 4);
    assert(!Enabled() && !Flush(true));
}
