"""ROM-free checks of the actual native developer route parser and boundary policy."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class NativeMacControlsTests(unittest.TestCase):
    def test_actual_cpp_parser(self):
        compiler = shutil.which("clang++") or shutil.which("c++")
        if not compiler:
            self.skipTest("C++17 compiler unavailable")
        source = r'''
#include <cassert>
#include <string>
#include "cara/native_mac/benchmark_controls.h"
int main() {
    using namespace NativeMac;
    assert(ParseCircleSegments("").empty());
    const auto route = ParseCircleSegments("10:20:-1:0;20:25:1:1;30:40:0:-1");
    assert(CircleKeys(route, 9) == (std::array<bool,4>{false,false,false,false}));
    assert(CircleKeys(route, 10) == (std::array<bool,4>{true,false,false,false}));
    assert(CircleKeys(route, 19)[0]);
    assert(CircleKeys(route, 20) == (std::array<bool,4>{false,true,true,false}));
    assert(CircleKeys(route, 25) == (std::array<bool,4>{false,false,false,false}));
    assert(CircleKeys(route, 39)[3]);
    assert(!CircleKeys(route, 40)[3]);
    for (const auto* bad : {"1", "1:2:0", "1:2:0:0:0", "1:2:0:0;", ";",
                           "-1:2:0:0", "2:2:0:0", "3:2:0:0", "0:100001:0:0",
                           "0:1:2:0", "0:1:0:-2", "0:5:0:0;4:6:0:0",
                           "0:1:0:0;0:1:0:0", "x:2:0:0", " 0:1:0:0",
                           "999999999999999:2:0:0"}) {
        bool rejected = false;
        try { ParseCircleSegments(bad); } catch (const std::runtime_error&) { rejected = true; }
        assert(rejected);
    }
    std::string full;
    for (unsigned i = 0; i < 16; ++i) {
        if (i) full += ';';
        full += std::to_string(i) + ':' + std::to_string(i + 1) + ":0:0";
    }
    assert(ParseCircleSegments(full).size() == 16);
    bool rejected = false;
    try { ParseCircleSegments(full + ";16:17:0:0"); }
    catch (const std::runtime_error&) { rejected = true; }
    assert(rejected);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory)
            (path / "test.cpp").write_text(source)
            subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-I", str(ROOT / "emulator/src"), str(path / "test.cpp"),
                            "-o", str(path / "test")], check=True, capture_output=True)
            subprocess.run([str(path / "test")], check=True, capture_output=True)


if __name__ == "__main__":
    unittest.main()
