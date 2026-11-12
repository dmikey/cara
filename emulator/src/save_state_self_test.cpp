// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include "video_core/pica_state.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <cryptopp/sha.h>
#include <zstd.h>
#include "common/detached_tasks.h"
#include "common/file_util.h"
#include "common/logging/backend.h"
#include "common/logging/filter.h"
#include "core/arm/arm_interface.h"
#include "core/core.h"
#include "core/savestate.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/process.h"
#include "core/frontend/applets/default_applets.h"
#include "core/frontend/emu_window.h"
#include "core/settings.h"

class Window final : public Frontend::EmuWindow {
public:
    Window() { UpdateFramebufferLayout(400, 480); }
    void PollEvents() override {}
    void MakeCurrent() override {}
    void DoneCurrent() override {}
    void SwapBuffers() override {}
};
struct Snapshot {
    std::array<u32, 16> registers;
    u32 cpsr, counter, vertex_word, vram_word;
    s64 ticks;
    bool operator==(const Snapshot&) const = default;
};
static Snapshot Inspect(Core::System& system) {
    Snapshot s{};
    for (int i = 0; i < 16; ++i) s.registers[i] = system.GetCore(0).GetReg(i);
    s.vertex_word = Pica::g_state.vs.program_code[17];
    std::memcpy(&s.vram_word, system.Memory().GetPhysicalPointer(Memory::VRAM_PADDR), sizeof(u32));
    s.cpsr = system.GetCore(0).GetCPSR();
    s.counter = system.Memory().Read32(0x101000);
    s.ticks = system.CoreTiming().GetGlobalTicks();
    return s;
}
static void CheckPhysicalPointers(Memory::MemorySystem& memory) {
    const std::array<std::pair<u32,u32>,4> regions{{
        {Memory::VRAM_PADDR,Memory::VRAM_SIZE},
        {Memory::DSP_RAM_PADDR,Memory::DSP_RAM_SIZE},
        {Memory::FCRAM_PADDR,Memory::FCRAM_N3DS_SIZE},
        {Memory::N3DS_EXTRA_RAM_PADDR,Memory::N3DS_EXTRA_RAM_SIZE},
    }};
    // Interleave regions and revisit boundaries to exercise cached hits/misses.
    for (u32 trial=0;trial<32;++trial) {
        for (const auto& [base,size]:regions) {
            const u32 offset=trial==0?0:trial==1?size-1:(trial*7919)%size;
            auto owner=memory.GetPhysicalRef(base+offset);
            u8* raw=memory.GetPhysicalPointer(base+offset);
            if (!raw || raw!=owner.GetPtr() || owner.GetSize()!=size-offset)
                throw std::runtime_error("Physical raw/owned pointer mismatch");
            const u8 saved=*raw; *raw^=0x5A;
            if (*owner.GetPtr()!=u8(saved^0x5A)) throw std::runtime_error("Physical write alias mismatch");
            *raw=saved;
            if (memory.GetPhysicalPointer(base+size) || memory.GetPhysicalRef(base+size))
                throw std::runtime_error("Physical end address must remain invalid");
            if (memory.GetPhysicalPointer(base+offset)!=raw)
                throw std::runtime_error("Physical cache miss corrupted next lookup");
        }
    }
}
static void Run(Core::System& system, unsigned slices) {
    while (slices--) if (system.RunLoop() != Core::System::ResultStatus::Success) throw std::runtime_error("Guest execution failed");
}
static std::vector<u8> Read(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
static void Write(const std::string& path, const std::vector<u8>& bytes) {
    std::ofstream file(path, std::ios::binary); file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (!file) throw std::runtime_error("Fixture write failed");
}
static void U64(std::vector<u8>& bytes, unsigned offset, u64 value) {
    for (unsigned i = 0; i < 8; ++i) bytes[offset+i] = (value >> (8*i)) & 0xFF;
}
static u64 Read64(const std::vector<u8>& bytes, unsigned offset) {
    u64 value = 0; for (unsigned i=0;i<8;++i) value |= u64(bytes[offset+i]) << (8*i); return value;
}
int main(int argc, char** argv) {
    if (argc != 3) { std::cerr << "Usage: cara-save-state-self-test COUNTER_ELF OUTPUT_DIRECTORY\n"; return 2; }
    try {
        Common::DetachedTasks tasks;
        const auto output = std::filesystem::absolute(argv[2]).string();
        FileUtil::CreateFullPath(output + "/"); FileUtil::SetUserPath(output + "/userdata/");
        Log::SetGlobalFilter(Log::Filter(Log::Level::Warning));
        Log::AddBackend(std::make_unique<Log::FileBackend>(output + "/test.log"));
        auto& settings = Settings::values;
        settings.is_new_3ds = std::getenv("CARA_TEST_NEW_3DS") != nullptr;
        settings.use_cpu_jit = false; settings.use_shader_jit = false;
        settings.use_hw_renderer = false; settings.use_hw_shader = false;
        settings.use_virtual_sd = true; settings.sink_id = "null";
        settings.region_value = Settings::REGION_VALUE_AUTO_SELECT;
        settings.init_clock = Settings::InitClock::FixedTime; settings.init_time = 946684801;
        settings.resolution_factor = 1; settings.frame_limit = 0;
        settings.shared_font_type = Settings::SharedFontType::Auto;
        Settings::Apply(); Frontend::RegisterDefaultApplets();
        Window window; auto& system = Core::System::GetInstance();
        const auto status = system.Load(window, std::filesystem::absolute(argv[1]).string());
        if (status != Core::System::ResultStatus::Success) throw std::runtime_error("Counter ELF load failed: " + std::to_string(unsigned(status)));
        if (const char* clock=std::getenv("CARA_TEST_CPU_PERCENT"))
            system.CoreTiming().UpdateClockSpeed(unsigned(std::clamp(std::strtol(clock,nullptr,10),25L,100L)));
        CheckPhysicalPointers(system.Memory());
        Run(system, 500);
        auto table=system.Memory().GetCurrentPageTable();
        system.Memory().MapMemoryRegion(*table, Memory::VRAM_VADDR, Memory::PAGE_SIZE,
                                        system.Memory().GetPhysicalRef(Memory::VRAM_PADDR));
        // A host renderer may retain page-cache markers even after a flush. The
        // software fixture deliberately leaves one to exercise cold GPU recovery.
        system.Memory().RasterizerMarkRegionCached(Memory::VRAM_PADDR, Memory::PAGE_SIZE, true);
        table.reset();
        const auto saved = Inspect(system);
        if (!saved.counter) throw std::runtime_error("Counter fixture did not run");
        const auto path = output + "/checkpoint.carastate";
        std::cout << "Saving CPU, kernel, RAM, GPU and HLE DSP state" << std::endl;
        system.SaveStateToPath(path);
        Run(system, 500); const auto future = Inspect(system);
        if (saved == future) throw std::runtime_error("Guest did not advance after save");
        Pica::g_state.vs.program_code[17] ^= 0x12345678;
        system.Memory().GetPhysicalPointer(Memory::VRAM_PADDR)[0] ^= 0x80;
        std::weak_ptr<Kernel::Process> previous_process=system.Kernel().GetCurrentProcess();
        system.LoadStateFromPath(path);
        CheckPhysicalPointers(system.Memory());
        if (!previous_process.expired()) throw std::runtime_error("Previous process leaked after state load");
        if (!(Inspect(system) == saved)) throw std::runtime_error("Restore did not reproduce CPU/RAM/timing state");
        if (system.Memory().GetCurrentPageTable()->attributes[Memory::VRAM_VADDR >> Memory::PAGE_BITS]!=Memory::PageType::Memory)
            throw std::runtime_error("Restored page retained a stale host GPU marker");
        system.Memory().RasterizerMarkRegionCached(Memory::VRAM_PADDR, Memory::PAGE_SIZE, true);
        system.Memory().RasterizerMarkRegionCached(Memory::VRAM_PADDR, Memory::PAGE_SIZE, false);
        Run(system, 500);
        if (!(Inspect(system) == future)) {
            const auto actual=Inspect(system);
            std::cerr << "Expected ticks=" << future.ticks << " counter=" << future.counter << " cpsr=" << future.cpsr << "; actual ticks=" << actual.ticks << " counter=" << actual.counter << " cpsr=" << actual.cpsr << '\n';
            for(unsigned reg=0;reg<16;++reg) if(actual.registers[reg]!=future.registers[reg]) std::cerr << "r" << reg << ": expected=" << future.registers[reg] << " actual=" << actual.registers[reg] << '\n';
            throw std::runtime_error("Execution diverged after state restore");
        }
        std::cout << "State round trip and deterministic continuation passed" << std::endl;
        for (unsigned repetition=0; repetition<12; ++repetition) {
            previous_process=system.Kernel().GetCurrentProcess();
            system.LoadStateFromPath(path);
            if (!previous_process.expired()) throw std::runtime_error("Repeated load leaked a process");
            if (!(Inspect(system) == saved)) throw std::runtime_error("Repeated load changed the restored state");
            Run(system, 500);
            if (!(Inspect(system) == future)) throw std::runtime_error("Repeated load changed continuation");
        }
        std::cout << "12 repeated loads with matching continuation passed" << std::endl;
        const auto slot_checkpoint=Inspect(system);
        system.SaveState(Core::SaveStateSlotCount);
        if (Core::ListSaveStates(system.GetTitleID()).empty()) throw std::runtime_error("Saved slot was not listed");
        Run(system, 50);
        system.LoadState(Core::SaveStateSlotCount);
        if (!(Inspect(system)==slot_checkpoint)) throw std::runtime_error("Numbered slot round trip failed");
        for (const u32 invalid_slot : {0u, Core::SaveStateSlotCount+1}) {
            bool rejected=false;
            try { system.SaveState(invalid_slot); } catch (const std::exception&) { rejected=true; }
            if (!rejected) throw std::runtime_error("Invalid slot was accepted");
        }
        std::cout << "Numbered slots and slot bounds passed" << std::endl;
        const auto original = Read(path);
        auto reject = [&](std::vector<u8> bytes, const char* label) {
            const auto before = Inspect(system); const auto invalid = output + "/invalid.carastate"; Write(invalid, bytes);
            bool rejected = false; try { system.LoadStateFromPath(invalid); } catch (const std::exception&) { rejected = true; }
            if (!rejected || !(Inspect(system) == before)) throw std::runtime_error(std::string("Invalid state changed the live session: ") + label);
            std::cout << label << " rejected without changing the live session" << std::endl;
        };
        auto bytes = original; bytes.resize(12); reject(bytes, "Truncated header");
        bytes = original; bytes[8] ^= 0x40; reject(bytes, "Unsupported format");
        bytes = original; bytes[16] ^= 1; reject(bytes, "Wrong game");
        bytes = original; bytes[48] ^= 1; reject(bytes, "Wrong engine");
        bytes = original; bytes[144] ^= 1; reject(bytes, "Wrong system mode");
        bytes = original; bytes[148] ^= 1; reject(bytes, "Wrong game revision");
        bytes = original; bytes[180] ^= 1; reject(bytes, "Wrong CPU backend");
        bytes = original; bytes.back() ^= 0x80; reject(bytes, "Corrupt payload");
        // A valid checksum/compression envelope containing a late archive truncation
        // exercises rollback after the engine has already begun deserialization.
        std::vector<u8> archive(Read64(original, 40));
        const auto decoded = ZSTD_decompress(archive.data(), archive.size(), original.data()+256, original.size()-256);
        if (ZSTD_isError(decoded) || decoded < 256) throw std::runtime_error("Unable to prepare rollback fixture");
        archive.resize(decoded - 128);
        std::vector<u8> compressed(ZSTD_compressBound(archive.size()));
        const auto packed = ZSTD_compress(compressed.data(), compressed.size(), archive.data(), archive.size(), 1);
        if (ZSTD_isError(packed)) throw std::runtime_error("Unable to compress rollback fixture");
        compressed.resize(packed); bytes.assign(original.begin(), original.begin()+256);
        U64(bytes, 32, packed); U64(bytes, 40, archive.size());
        CryptoPP::SHA256().CalculateDigest(bytes.data()+112, compressed.data(), compressed.size());
        bytes.insert(bytes.end(), compressed.begin(), compressed.end());
        reject(bytes, "Late deserialization failure");
        system.Shutdown();
        std::cout << "Save-state regressions passed using original ARM test code; no commercial ROM required" << std::endl;
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; if (Core::System::GetInstance().IsPoweredOn()) Core::System::GetInstance().Shutdown(); return 1; }
}
