// Copyright 2026 Cara contributors; GPL-2.0-or-later.
#pragma once
namespace Core { class System; }
namespace Cara::CPUJitTest {
struct Result { unsigned checks=0; bool adapter_state=false, mmio=false, monitor=false;
    bool full_system_archive=false; };
void RunFactorySystemArchiveTest(Core::System& system,Result& result);
Result RunActualAdapterTest(Core::System& system);
}
