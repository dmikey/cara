// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise the actual private source-cache matcher and allocator. No Metal
// device, pipeline compilation, emulation, ROM or guest work is involved.
#include "video_core/renderer_metal/pica_metal_native_bindings.mm"
#include "video_core/shader/shader.cpp"
#include <nihstro/inline_assembly.h>
#include <boost/archive/binary_iarchive.hpp>
#include <boost/archive/binary_oarchive.hpp>
#include <sstream>
#include <iostream>

namespace {
void RequireIdentity(bool value, const char* reason) {
    if (!value) {
        throw std::runtime_error(reason);
    }
}
Pica::Shader::ShaderSetup IdentityProgram() {
    using A = nihstro::InlineAsm;
    using O = nihstro::OpCode;
    using D = nihstro::DestRegister;
    using S = nihstro::SourceRegister;
    const auto binary = A::CompileToRawBinary({
        {O::Id::MOV, D::MakeOutput(0), S::MakeInput(0)}, {O::Id::END}});
    Pica::Shader::ShaderSetup setup{};
    std::transform(binary.program.begin(), binary.program.end(), setup.program_code.begin(),
                   [](auto word) { return word.hex; });
    setup.MarkProgramCodeDirty();
    std::transform(binary.swizzle_table.begin(), binary.swizzle_table.end(), setup.swizzle_data.begin(),
                   [](auto word) { return word.hex; });
    setup.MarkSwizzleDataDirty();
    return setup;
}
std::string SaveIdentitySetup(const Pica::Shader::ShaderSetup& setup) {
    std::ostringstream stream(std::ios::binary);
    { boost::archive::binary_oarchive archive(stream); archive << setup; }
    return stream.str();
}
void LoadIdentitySetup(Pica::Shader::ShaderSetup& setup, const std::string& bytes) {
    std::istringstream stream(bytes, std::ios::binary);
    boost::archive::binary_iarchive archive(stream);
    archive >> setup;
}
u64 IdentityCount(Cara::NativeMetalCost::Phase phase) {
    return Cara::NativeMetalCost::spans[phase].calls;
}
} // namespace

int main() { @autoreleasepool { try {
    setenv("CARA_METAL_FRAME_COST", "1", 1);
    setenv("CARA_METAL_REUSE_VERTEX_SOURCE_LOOKUP", "1", 1);
    setenv("CARA_METAL_CACHE_SHADER_POLICY", "1", 1);
    const auto option = [](const char* name) {
        const auto* value = std::getenv(name);
        return value && std::string_view(value) == "1";
    };
    const bool reuse = option("CARA_METAL_REUSE_SHADER_IDENTITY");
    const bool validate = option("CARA_METAL_VALIDATE_SHADER_IDENTITY");
    using namespace Cara::NativeMetalCost;
    Pica::Regs regs{};
    regs.vs.output_mask.Assign(1);
    auto setup = IdentityProgram();
    const auto source = [&](Pica::Shader::ShaderSetup& current, unsigned mask = 1, bool clip = false) {
        const OpenGL::PicaVSConfig config(regs, current);
        const OpenGL::PicaFixedGSConfig output(regs);
        return Cara::NativeVertexSource(current, config, output, mask, 2, clip);
    };
    const auto first = source(setup);
    RequireIdentity(!first.bytes.empty(), "Real PICA generator returned empty source");
    RequireIdentity(source(setup).revision == first.revision, "Cold exact program did not bind original source");
    const auto checked = IdentityCount(ShaderIdentityFullChecks);
    const auto hits = IdentityCount(ShaderIdentityFastHits);
    RequireIdentity(source(setup).revision == first.revision, "Stable shader identity changed source");
    RequireIdentity(IdentityCount(ShaderIdentityFastHits) == hits + unsigned(reuse), "Stable identity hit count differs");
    RequireIdentity(IdentityCount(ShaderIdentityFullChecks) == checked + unsigned(!reuse || validate),
                    "Stable source full-validation count differs");
    auto copied = setup;
    RequireIdentity(source(copied).revision == first.revision, "Whole exact setup copy lost source identity");
    const auto rewritten = copied.program_code[0];
    copied.program_code[0] = rewritten;
    copied.MarkProgramCodeDirty();
    RequireIdentity(source(copied).revision == first.revision, "Identical rewrite changed exact source binding");
    // Unused tails still belong to the complete cache key. These mutations
    // require new source revisions even when generated GLSL is text-identical.
    copied.program_code[4000] ^= 0x12345678;
    copied.MarkProgramCodeDirty();
    const auto code_changed = source(copied);
    RequireIdentity(code_changed.revision != first.revision, "Code-only mutation reused stale source entry");
    copied.swizzle_data[4000] ^= 0x87654321;
    copied.MarkSwizzleDataDirty();
    RequireIdentity(source(copied).revision != code_changed.revision, "Swizzle-only mutation reused stale source entry");
    const auto original = source(setup);
    const auto changed_mask = source(setup, 3);
    RequireIdentity(changed_mask.revision != original.revision, "Input-mask change bypassed small configuration key");
    RequireIdentity(source(setup).revision == original.revision, "Return to original mask lost exact source");
    RequireIdentity(source(setup, 1, true).revision != original.revision, "Clip-policy change bypassed source key");
    const auto archive = SaveIdentitySetup(setup);
    copied = setup;
    copied.program_code[4000] ^= 7;
    copied.MarkProgramCodeDirty();
    (void)source(copied);
    LoadIdentitySetup(copied, archive);
    RequireIdentity(source(copied).revision == original.revision, "State load reused pre-load source identity");
    try { LoadIdentitySetup(copied, archive.substr(0, archive.size() / 2)); } catch (...) {}
    // Restore valid bytes before source generation after the intentionally
    // truncated load. The second load must also mint independent lazy IDs.
    LoadIdentitySetup(copied, archive);
    RequireIdentity(source(copied).revision == original.revision, "Truncated-load recovery kept stale identity");
    // Same-owner nested calls have no callbacks or global raw-pointer map.
    const auto nested = [&] { return source(setup).revision; };
    RequireIdentity(nested() == source(setup).revision, "Same-owner nested query changed source");
    // The optional diagnostic deliberately catches a violated direct-writer
    // contract. The normal opt-in never promises to detect unmarked raw writes.
    if (!reuse || validate) {
        (void)source(setup);
        (void)source(setup); // Ensure the unchanged pair was fully validated.
        setup.program_code[3999] ^= 9; // Deliberately omit MarkDirty.
        bool rejected = false;
        try {
            const auto unmarked = source(setup);
            RequireIdentity(!reuse && unmarked.revision != original.revision,
                            "Unmarked bytes bypassed the complete control comparison");
        } catch (const std::runtime_error& error) {
            rejected = std::string_view(error.what()) == "Shader arrays changed without MarkDirty";
            if (!rejected) throw;
        }
        RequireIdentity(rejected == validate, "Unmarked-writer diagnostic did not fail closed");
        setup.MarkProgramCodeDirty();
    }
    // Exercise the production allocator's permanent saturation LAST. Existing
    // tickets can never authorize a hit once the global pool is unavailable.
    (void)source(setup);
    (void)source(setup);
    const auto saturation_hits = IdentityCount(ShaderIdentityFastHits);
    const auto saturation_checks = IdentityCount(ShaderIdentityFullChecks);
    Pica::Shader::next_shader_identity.store(std::numeric_limits<u64>::max(), std::memory_order_relaxed);
    (void)source(setup);
    RequireIdentity(IdentityCount(ShaderIdentityFastHits) == saturation_hits &&
                    IdentityCount(ShaderIdentityFullChecks) == saturation_checks + 1,
                    "Saturated allocator still admitted identity fast path");
    Pica::Shader::ShaderSetup fresh{};
    RequireIdentity(fresh.GetProgramCodeIdentity() == 0 && fresh.GetSwizzleDataIdentity() == 0,
                    "Exhausted production allocator recycled tickets");
    std::cout << "PASS: reuse=" << reuse << ", validate=" << validate
              << "; actual source matcher, code/swizzle/copy/config/restore/nested queries, writer diagnostic and saturation\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
} } }
