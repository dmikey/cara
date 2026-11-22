// SPDX-License-Identifier: GPL-2.0-or-later
#include "pica_metal_pipelines.h"
#include "simd_vertex_source.h"
#include "video_core/renderer_metal/pica_metal_frame_cost.hh"
#include "pica_metal_render_pipeline_key.h"
#include "video_core/renderer_metal/pica_metal_raw_index_format.hh"
#include <TargetConditionals.h>
#include <CommonCrypto/CommonDigest.h>
#if TARGET_OS_OSX
#include <sys/sysctl.h>
#endif
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include "core/savestate.h"
#include "video_core/video_core.h"


#import <Foundation/Foundation.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>
#include "common/logging/log.h"

#include "batched_vertex_source.h"
#include <map>

namespace Cara {
namespace {

bool TraceColdPipelines() {
    static const bool enabled = [] {
        const char* value = std::getenv("CARA_METAL_TRACE_COLD_PSO");
        return value && std::string_view(value) == "1";
    }();
    return enabled;
}

struct Pair {
    std::string_view vertex, fragment, msl_vertex, msl_fragment;
    unsigned sampled_unit;
    std::array<NSUInteger,6> slots;
    unsigned secondary_unit = NoMaterialUnit;
    NSUInteger secondary_texture_slot = NSNotFound, secondary_sampler_slot = NSNotFound;
    bool precompiled = false;
    PicaMetalVertexMode vertex_mode = PicaMetalVertexMode::Packed;
    bool runtime = false;
    unsigned tertiary_unit = NoMaterialUnit;
    NSUInteger tertiary_texture_slot = NSNotFound, tertiary_sampler_slot = NSNotFound;
};
// Ordinary builds translate exact guest shader sources locally. Precompiled
// catalog data can be supplied separately for private compatibility tests.
constexpr std::array<Pair, 0> Catalog{};
constexpr std::array<Pair, 0> RawCorpus{};

#ifdef CARA_PRIVATE_GATE_METAL_CATALOG
constexpr Pair GatePairs[] = {
#include "pica_metal_gate_catalog_data.inc"
};
#else
constexpr std::array<Pair, 0> GatePairs{};
#endif

id<MTLLibrary> OpenGateLibrary(id<MTLDevice> device) {
#if defined(CARA_PRIVATE_GATE_METAL_CATALOG)
    const auto url = [NSBundle.mainBundle URLForResource:@"cara-gate" withExtension:@"metallib"];
    return url ? [device newLibraryWithURL:url error:nullptr] : nil;
#else
    return nil;
#endif
}

std::array<char, 64> SourceDigest(std::string_view source) {
    using Digest = std::array<char, 64>;
    if (source.empty() || source.size() > std::numeric_limits<CC_LONG>::max())
        return {};

    // Catalog lookup and pipeline lookup both hash these generated stages.
    // Reuse a digest only after checking the complete original source string.
    struct Entry {
        std::string source;
        Digest digest;
    };
    thread_local std::array<Entry, 64> entries{};
    thread_local std::size_t next = 0;
    const bool cacheable = source.size() <= 128 * 1024;
    if (cacheable) {
        for (const auto& entry : entries)
            if (entry.source == source)
                return entry.digest;
    }

    std::array<unsigned char, CC_SHA256_DIGEST_LENGTH> digest{};
    CC_SHA256(source.data(), static_cast<CC_LONG>(source.size()), digest.data());
    Digest text{};
    constexpr char hex[] = "0123456789abcdef";
    for (std::size_t i = 0; i < digest.size(); ++i) {
        text[i * 2] = hex[digest[i] >> 4];
        text[i * 2 + 1] = hex[digest[i] & 15u];
    }
    if (cacheable) {
        entries[next] = {std::string(source), text};
        next = (next + 1) % entries.size();
    }
    return text;
}

struct GatePairLookup {
    std::optional<unsigned> index;
    bool ambiguous = false;
};

GatePairLookup FindGatePair(std::string_view vertex, std::string_view fragment,
                            PicaMetalVertexMode mode) {
    if constexpr (std::size(GatePairs) == 0)
        return {};
    struct Entry {
        std::string vertex, fragment;
        PicaMetalVertexMode mode;
        GatePairLookup result;
    };
    thread_local std::array<std::optional<Entry>, 64> recent{};
    thread_local std::size_t next = 0, last = 0;
    const auto match = [&](std::size_t i) {
        return recent[i] && recent[i]->mode == mode &&
            recent[i]->vertex == vertex && recent[i]->fragment == fragment;
    };
    if (match(last)) {
        NativeMetalCost::Count(NativeMetalCost::PairHits, 1);
        return recent[last]->result;
    }
    for (std::size_t i = 0; i < recent.size(); ++i) {
        if (match(i)) {
            last = i;
            NativeMetalCost::Count(NativeMetalCost::PairHits, 1);
            return recent[i]->result;
        }
    }
    const auto vs = SourceDigest(vertex);
    const auto fs = SourceDigest(fragment);
    GatePairLookup result;
    for (unsigned i = 0; i < std::size(GatePairs); ++i) {
        const auto& pair = GatePairs[i];
        if (pair.vertex != std::string_view(vs.data(), vs.size()) ||
            pair.fragment != std::string_view(fs.data(), fs.size()) || pair.vertex_mode != mode)
            continue;
        if (result.index) {
            result.ambiguous = true;
            break;
        }
        result.index = i;
    }
    if (vertex.size() <= 128 * 1024 && fragment.size() <= 128 * 1024) {
        recent[next].emplace(Entry{std::string(vertex), std::string(fragment), mode, result});
        last = next;
        next = (next + 1) % recent.size();
    }
    return result;
}

bool ReuseRenderPipelines() {
    // The validated descriptor identity is shared by the Apple backends.
    // Remain explicit opt-in on every host; do not promote the Mac default.
    static const bool enabled = [] {
        const char* option = std::getenv("CARA_METAL_REUSE_RENDER_PIPELINES");
        return option && std::string_view(option) == "1";
    }();
    return enabled;
}

#if defined(CARA_PRIVATE_GATE_METAL_CATALOG) && TARGET_OS_IOS
enum class PipelineArchiveMode { Off, Record, Strict, EmptyStrict };

struct GatePipelineArchive {
    PipelineArchiveMode mode = PipelineArchiveMode::Off;
    NSURL* destination = nil;
    id<MTLDevice> device = nil;
    id<MTLBinaryArchive> archive = nil;
    NSMutableArray<MTLRenderPipelineDescriptor*>* descriptors = nil;
    unsigned hits = 0;
    unsigned misses = 0;

    bool IsStrict() const {
        return mode == PipelineArchiveMode::Strict || mode == PipelineArchiveMode::EmptyStrict;
    }

    bool Open() {
        if (archive)
            return true;
        if (!destination || !device)
            return false;
        MTLBinaryArchiveDescriptor* config = [MTLBinaryArchiveDescriptor new];
        if (mode == PipelineArchiveMode::Strict)
            config.url = destination;
        NSError* error = nil;
        {
            NativeMetalCost::Scope open(NativeMetalCost::RenderPipelineArchiveOpen);
            archive = [device newBinaryArchiveWithDescriptor:config error:&error];
        }
        if (!archive) {
            LOG_ERROR(Render_OpenGL, "Metal pipeline archive open: {}",
                      error.localizedDescription.UTF8String ?: "unknown Metal error");
            return false;
        }
        NativeMetalCost::Count(NativeMetalCost::RenderPipelineArchiveOpenSuccess, 1);
        return true;
    }

    bool Remember(MTLRenderPipelineDescriptor* descriptor) {
        // Only exact descriptors accepted by the driver and reflection validator
        // enter the archive. Keep recording work outside the timed replay.
        constexpr NSUInteger max_descriptors = 512;
        if (!descriptors || descriptors.count >= max_descriptors)
            return false;
        [descriptors addObject:[descriptor copy]];
        return true;
    }

    bool Write() {
        if (!descriptors.count || !destination ||
            [NSFileManager.defaultManager fileExistsAtPath:destination.path] || !Open())
            return false;
        for (MTLRenderPipelineDescriptor* descriptor in descriptors) {
            NSError* error = nil;
            bool added;
            {
                NativeMetalCost::Scope record(NativeMetalCost::RenderPipelineArchiveRecord);
                added = [archive addRenderPipelineFunctionsWithDescriptor:descriptor error:&error];
            }
            if (!added) {
                LOG_ERROR(Render_OpenGL, "Metal pipeline archive record: {}",
                          error.localizedDescription.UTF8String ?: "unknown Metal error");
                return false;
            }
            NativeMetalCost::Count(NativeMetalCost::RenderPipelineArchiveRecordSuccess, 1);
        }

        const auto directory = [destination URLByDeletingLastPathComponent];
        const auto name = [NSString stringWithFormat:@".cara-metal-gate-pipelines-%@.metallib",
                                                     NSUUID.UUID.UUIDString];
        const auto temporary = [directory URLByAppendingPathComponent:name isDirectory:NO];
        if ([NSFileManager.defaultManager fileExistsAtPath:temporary.path])
            return false;
        NSError* error = nil;
        bool published;
        {
            NativeMetalCost::Scope serialize(NativeMetalCost::RenderPipelineArchiveSerialize);
            published = [archive serializeToURL:temporary error:&error] &&
                [NSFileManager.defaultManager moveItemAtURL:temporary toURL:destination error:&error];
        }
        if (!published) {
            [NSFileManager.defaultManager removeItemAtURL:temporary error:nullptr];
            LOG_ERROR(Render_OpenGL, "Metal pipeline archive publish: {}",
                      error.localizedDescription.UTF8String ?: "unknown Metal error");
            return false;
        }
        NativeMetalCost::Count(NativeMetalCost::RenderPipelineArchiveSerializeSuccess, 1);
        return true;
    }
};

thread_local GatePipelineArchive pipeline_archive;
#endif

constexpr unsigned PairCount = std::size(RawCorpus) + std::size(Catalog);
Pair PairAt(unsigned index, const std::vector<PicaMetalDualAsset>& dual) {
    if (index < PairCount)
        return index < std::size(RawCorpus) ? RawCorpus[index] : Catalog[index - std::size(RawCorpus)];
    index -= PairCount;
    if (index >= dual.size())
        return GatePairs[index - dual.size()];
    const auto& a=dual[index];
    return {a.vertex,a.fragment,a.msl_vertex,a.msl_fragment,a.primary_unit,a.primary_slots,
            a.secondary_unit,a.secondary_texture_slot,a.secondary_sampler_slot};
}

bool MaterialSlotLayout(const Pair& p) {
    if (p.tertiary_unit != NoMaterialUnit)
        return p.runtime && p.sampled_unit == 0 && p.secondary_unit == 1 && p.tertiary_unit == 2;
    if (p.precompiled || p.runtime)
        return p.secondary_unit == NoMaterialUnit ||
               SupportedDualUnits(p.sampled_unit, p.secondary_unit);
    if (p.secondary_unit == NoMaterialUnit)
        return true;
    return SupportedDualUnits(p.sampled_unit,p.secondary_unit) &&
           p.slots[0]<16 && p.slots[1]<16 && p.slots[2]<16 && p.slots[3]<16 &&
           p.slots[4]<16 && (p.slots[5]==NSNotFound ||
                             (p.slots[5]<16 && p.slots[5]!=p.slots[4])) &&
           p.secondary_texture_slot<16 && p.secondary_sampler_slot<16 &&
           DistinctFragmentMaterialSlots(
               {unsigned(p.slots[0]),unsigned(p.slots[2]),unsigned(p.secondary_texture_slot)},
               {unsigned(p.slots[1]),unsigned(p.slots[3]),unsigned(p.secondary_sampler_slot)});
}

void Require(bool valid, const char* reason) {
    if (!valid)
        throw std::runtime_error(reason);
}

MTLCompareFunction Compare(unsigned value) {
    constexpr MTLCompareFunction functions[] = {MTLCompareFunctionNever,MTLCompareFunctionLess,
        MTLCompareFunctionEqual,MTLCompareFunctionLessEqual,MTLCompareFunctionGreater,
        MTLCompareFunctionNotEqual,MTLCompareFunctionGreaterEqual,MTLCompareFunctionAlways};
    Require(value>=0x200 && value<=0x207,"unsupported compare function");
    return functions[value-0x200];
}

MTLStencilOperation Stencil(unsigned op) {
    switch (op) {
    case 0: return MTLStencilOperationZero;
    case 0x1e00: return MTLStencilOperationKeep;
    case 0x1e01: return MTLStencilOperationReplace;
    case 0x1e02: return MTLStencilOperationIncrementClamp;
    case 0x1e03: return MTLStencilOperationDecrementClamp;
    case 0x150a: return MTLStencilOperationInvert;
    case 0x8507: return MTLStencilOperationIncrementWrap;
    case 0x8508: return MTLStencilOperationDecrementWrap;
    default: throw std::runtime_error("unsupported stencil operation");
    }
}

MTLBlendFactor Factor(unsigned value) {
    switch (value) {
    case 0: return MTLBlendFactorZero;
    case 1: return MTLBlendFactorOne;
    case 0x300: return MTLBlendFactorSourceColor;
    case 0x301: return MTLBlendFactorOneMinusSourceColor;
    case 0x302: return MTLBlendFactorSourceAlpha;
    case 0x303: return MTLBlendFactorOneMinusSourceAlpha;
    case 0x304: return MTLBlendFactorDestinationAlpha;
    case 0x305: return MTLBlendFactorOneMinusDestinationAlpha;
    case 0x306: return MTLBlendFactorDestinationColor;
    case 0x307: return MTLBlendFactorOneMinusDestinationColor;
    case 0x308: return MTLBlendFactorSourceAlphaSaturated;
    case 0x8001: return MTLBlendFactorBlendColor;
    case 0x8002: return MTLBlendFactorOneMinusBlendColor;
    case 0x8003: return MTLBlendFactorBlendAlpha;
    case 0x8004: return MTLBlendFactorOneMinusBlendAlpha;
    default: throw std::runtime_error("unsupported blend factor");
    }
}

MTLBlendOperation Blend(unsigned value) {
    switch (value) {
    case 0x8006: return MTLBlendOperationAdd;
    case 0x8007: return MTLBlendOperationMin;
    case 0x8008: return MTLBlendOperationMax;
    case 0x800a: return MTLBlendOperationSubtract;
    case 0x800b: return MTLBlendOperationReverseSubtract;
    default: throw std::runtime_error("unsupported blend operation");
    }
}

bool KeepPicaSubroutineCalls() {
    static const bool enabled = [] {
        const char* value = std::getenv("CARA_METAL_KEEP_PICA_SUBROUTINE_CALLS");
        return value && std::string_view(value) == "1";
    }();
    return enabled;
}

id<MTLFunction> Compile(id<MTLDevice> device, std::string_view source, NSString* entry) {
    NSString* body = [[NSString alloc] initWithBytes:source.data() length:source.size()
                                            encoding:NSUTF8StringEncoding];
    if (!body)
        return nil;
    MTLCompileOptions* options = [MTLCompileOptions new];
    // Give cold compiler A/B runs independent source and function identities.
    // Use separate Cara cache directories for each diagnostic as well.
    static NSString* const diagnostic_suffix = []() -> NSString* {
        const char* value = std::getenv("CARA_METAL_DIAGNOSTIC_SHADER_NAMESPACE");
        const std::string_view suffix = value ? std::string_view(value) : std::string_view{};
        if (suffix.empty() || suffix.size() > 32 ||
            suffix.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != suffix.npos)
            return nil;
        return [NSString stringWithUTF8String:value];
    }();
    if (diagnostic_suffix) {
        NSString* named_entry = [NSString stringWithFormat:@"%@_%@", entry, diagnostic_suffix];
        body = [body stringByReplacingOccurrencesOfString:entry withString:named_entry];
        entry = named_entry;
    }
    const bool simd_vertex = source.find(SimdVertexMslTag) != source.npos;
    if (simd_vertex)
        options.languageVersion = MTLLanguageVersion2_3;
    NSString* modified_body = nil;
    if (!simd_vertex && source.size() >= 32 * 1024 && KeepPicaSubroutineCalls() &&
        [entry hasPrefix:@"cara_pica_vert"]) {
        // SPIRV-Cross force-inlines each PICA subroutine by default. Keep the
        // ABI and body of these generated helpers, while letting Metal use GPU
        // calls instead of duplicating the full call tree in the vertex entry.
        modified_body = [body stringByReplacingOccurrencesOfString:
            @"static inline __attribute__((always_inline))\nbool sub_"
            withString:@"static __attribute__((noinline))\nbool sub_"];
    }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    options.fastMathEnabled = NO;
#pragma clang diagnostic pop
    NSError* error = nil;
    id<MTLLibrary> library = nil;
    if (modified_body && ![modified_body isEqualToString:body]) {
        library = [device newLibraryWithSource:modified_body options:options error:&error];
        if (!library)
            LOG_WARNING(Render_OpenGL, "Metal did not accept PICA subroutine calls: {}",
                        error.localizedDescription.UTF8String ?: "unknown Metal error");
    }
    if (!library)
        library = [device newLibraryWithSource:body options:options error:&error];
    if (!library) {
        LOG_ERROR(Render_OpenGL, "Metal stage {} compile failed: {}",
                  entry.UTF8String, error.localizedDescription.UTF8String ?: "unknown Metal error");
        return nil;
    }
    return [library newFunctionWithName:entry];
}

#ifdef CARA_RUNTIME_METAL_SHADERS
struct ComputeFunctions {
    id<MTLComputePipelineState> compute;
    id<MTLArgumentEncoder> arguments;
    id<MTLFunction> raster;
    std::uint8_t resources;
    bool gathers_unique_vertices;
};

std::optional<ComputeFunctions> PrepareComputeFunctions(id<MTLDevice> device,
                                                       std::string_view source,
                                                       std::string_view vertex,
                                                       std::string_view fragment) {
    static const bool enabled = [] {
        const char* value = std::getenv("CARA_METAL_BATCH_VERTEX_COMPUTE");
        return value && std::string_view(value) == "1";
    }();
    static const bool unique = [] {
        const char* value = std::getenv("CARA_METAL_UNIQUE_VERTEX_COMPUTE");
        return value && std::string_view(value) == "1";
    }();
    if ((!enabled && !unique) || device.argumentBuffersSupport != MTLArgumentBuffersTier2 ||
        (unique && !UseMetalRawIndexConstants()))
        return std::nullopt;

    struct Cache {
        id<MTLDevice> device = nil;
        std::map<std::string, std::optional<ComputeFunctions>, std::less<>> entries;
    };
    thread_local Cache cache;
    if (cache.device != device) {
        cache.entries.clear();
        cache.device = device;
    }
    if (const auto found = cache.entries.find(source); found != cache.entries.end())
        return found->second;
    auto& answer = cache.entries[std::string(source)];
    std::optional<RuntimeMetalShaderPair> unique_stages;
    if (unique) {
        const auto unique_vertex = MakeUniquePackedVertexSource(vertex);
        if (!unique_vertex)
            return std::nullopt;
        std::string error;
        unique_stages = TranslateRuntimeMetalShaders(*unique_vertex, fragment,
                                                     RuntimeMetalVertexMode::Packed,
                                                     false, false, error);
        if (!unique_stages) {
            LOG_ERROR(Render_OpenGL, "Unique PICA vertex translation failed: {}", error);
            return std::nullopt;
        }
        source = unique_stages->vertex;
    }
    const auto code = MakeBatchedVertexSource(source, unique);
    if (!code)
        return std::nullopt;
    const auto body = [[NSString alloc] initWithBytes:code->data() length:code->size()
                                            encoding:NSUTF8StringEncoding];
    auto options = [MTLCompileOptions new];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    options.fastMathEnabled = NO;
#pragma clang diagnostic pop
    NSError* error = nil;
    const auto library = [device newLibraryWithSource:body options:options error:&error];
    const auto fn = [library newFunctionWithName:@"cara_compute_batch"];
    const auto raster = [library newFunctionWithName:@"cara_compute_raster"];
    const auto pipeline = fn ? [device newComputePipelineStateWithFunction:fn error:&error] : nil;
    const auto arguments = [fn newArgumentEncoderWithBufferIndex:0];
    if (!pipeline || !raster || !arguments || !arguments.encodedLength) {
        LOG_ERROR(Render_OpenGL, "Batched PICA shader preparation failed: {}",
                  error.localizedDescription.UTF8String ?: "compute interface");
        return std::nullopt;
    }
    static constexpr std::array members{
        "layout [[id(0)]]", "uniforms [[id(1)]]", "common [[id(2)]]", "vertices [[id(3)]]",
        "indices [[id(4)]]", "vertex_sampler [[id(5)]]", "index_sampler [[id(6)]]",
    };
    std::uint8_t resources = 0;
    for (unsigned i = 0; i < members.size(); ++i)
        if (code->find(members[i]) != std::string::npos)
            resources |= static_cast<std::uint8_t>(1u << i);
    answer = ComputeFunctions{pipeline, arguments, raster, resources, unique};
    return answer;
}
#endif

using Binding = std::pair<MTLBindingType,NSUInteger>;

bool CoversBindings(NSArray<id<MTLBinding>>* bindings, const std::set<Binding>& expected,
                    bool allow_inactive = false) {
    if (!bindings)
        return false;
    std::set<Binding> actual;
    for (id<MTLBinding> binding in bindings) {
        if (!binding.isUsed)
            continue;
        if (!actual.emplace(binding.type,binding.index).second ||
            binding.access != MTLBindingAccessReadOnly)
            return false;
    }
    return allow_inactive ? std::includes(expected.begin(), expected.end(),
                                           actual.begin(), actual.end()) : actual == expected;
}

bool CheckReflection(MTLRenderPipelineReflection* reflection, const Pair& pair,
                     bool& primary_material_cube, bool simd_reference = false) {
    if (@available(macOS 13.0, iOS 16.0, *)) {
        if (!reflection)
            return false;
        constexpr auto buffer = MTLBindingTypeBuffer;
        constexpr auto texture = MTLBindingTypeTexture;
        constexpr auto sampler = MTLBindingTypeSampler;
        const auto& [lut, lut_sampler, image, image_sampler, common, light] = pair.slots;
        for (id<MTLBinding> value in reflection.fragmentBindings) {
            if (!value.isUsed || value.type != texture || image == NSNotFound ||
                value.index != image)
                continue;
            auto texture_binding = (id<MTLTextureBinding>)value;
            if (texture_binding.textureType == MTLTextureTypeCube) {
                if (!pair.runtime || pair.sampled_unit != 0 ||
                    ![value.name isEqualToString:@"tex_cube"])
                    return false;
                primary_material_cube = true;
            }
        }
        std::set<Binding> vertex = {{buffer, 0}};
        if (simd_reference) vertex.emplace(buffer, 3);
        if (pair.vertex_mode != PicaMetalVertexMode::StageIn)
            vertex.insert({{buffer, 1}, {texture, 0}, {texture, 1}});
        if (pair.vertex_mode == PicaMetalVertexMode::Packed ||
            pair.vertex_mode == PicaMetalVertexMode::PairIndexed)
            vertex.emplace(buffer, 2);
        std::set<Binding> fragment = {{buffer,common}};
        if (lut != NSNotFound)
            fragment.emplace(texture, lut);
        if (image != NSNotFound) {
            fragment.emplace(texture, image);
            fragment.emplace(sampler, image_sampler);
        }
        if (light != NSNotFound)
            fragment.emplace(buffer,light);
        if (pair.secondary_unit != NoMaterialUnit) {
            if (!MaterialSlotLayout(pair))
                return false;
            fragment.emplace(texture,pair.secondary_texture_slot);
            fragment.emplace(sampler,pair.secondary_sampler_slot);
            const auto named=[&](MTLBindingType kind, NSUInteger index, NSString* name) -> bool {
                for (id<MTLBinding> value in reflection.fragmentBindings)
                    if (value.isUsed && value.type==kind && value.index==index)
                        return [value.name isEqualToString:name];
                return false;
            };
            const auto primary = primary_material_cube ? @"tex_cube"
                : [NSString stringWithFormat:@"tex%u", pair.sampled_unit];
            const auto secondary=[NSString stringWithFormat:@"tex%u",pair.secondary_unit];
            if ((lut != NSNotFound && !named(texture,lut,@"texture_buffer_lut_lf")) ||
                !named(texture,image,primary) ||
                !named(sampler,image_sampler,[primary stringByAppendingString:@"Smplr"]) ||
                !named(texture,pair.secondary_texture_slot,secondary) ||
                !named(sampler,pair.secondary_sampler_slot,[secondary stringByAppendingString:@"Smplr"]))
                return false;
            if (pair.tertiary_unit != NoMaterialUnit) {
                const auto third = [NSString stringWithFormat:@"tex%u", pair.tertiary_unit];
                if (!fragment.emplace(texture, pair.tertiary_texture_slot).second ||
                    !fragment.emplace(sampler, pair.tertiary_sampler_slot).second ||
                    pair.tertiary_sampler_slot == lut_sampler ||
                    !named(texture, pair.tertiary_texture_slot, third) ||
                    !named(sampler, pair.tertiary_sampler_slot, [third stringByAppendingString:@"Smplr"]))
                    return false;
            }
        }
        bool vertex_matches = CoversBindings(reflection.vertexBindings, vertex, pair.runtime);
        if (pair.vertex_mode == PicaMetalVertexMode::StageIn && !vertex_matches) {
            // The vertex descriptor binds the attribute stream at slot 1.
            // Some drivers include it in the reflection beside the slot-0 UBO.
            vertex.emplace(buffer, 1);
            vertex_matches = CoversBindings(reflection.vertexBindings, vertex, pair.runtime);
        }
        const bool fragment_matches = CoversBindings(reflection.fragmentBindings, fragment, pair.runtime);
        if ((!vertex_matches || !fragment_matches) && (pair.precompiled || pair.runtime)) {
            for (NSArray<id<MTLBinding>>* bank in @[reflection.vertexBindings, reflection.fragmentBindings]) {
                for (id<MTLBinding> item in bank) {
                    LOG_ERROR(Render_OpenGL, "Gate Metal reflection mode={} name={} type={} index={} used={} access={}",
                              unsigned(pair.vertex_mode), item.name.UTF8String, unsigned(item.type),
                              item.index, bool(item.isUsed), unsigned(item.access));
                }
            }
        }
        return (lut_sampler == NSNotFound || lut_sampler != image_sampler) &&
               vertex_matches && fragment_matches;
    }
    return false;
}

bool SupportedSampler(unsigned unit, const PicaSampleInputs& s) {
    if (!s.rgba8 || !s.normalized || !s.identity_swizzle || s.base_level != 0 ||
        s.mag_filter != 0x2601 || s.wrap_s != 0x2901 || s.wrap_t != 0x2901 ||
        s.compare != 0 || s.lod_bias != 0 || s.anisotropy != 1 || s.min_lod != 0)
        return false;
    // Unit 1 is the physical GL/Metal zero-clamp differential. The other
    // tuple is the exact shape used by the original one-texture replay.
    if (unit == 1)
        return s.min_filter == 0x2702 && s.max_level == 0 && s.levels == 1 && s.max_lod == 0;
    return unit == 0 && s.min_filter == 0x2703 && s.max_level == 4 && s.levels == 5 &&
           s.max_lod == 4;
}

std::optional<DualSamplerRecipe> ParityRecipe(const PicaSampleInputs& s) {
    if (!s.rgba8 || !s.normalized || !s.identity_swizzle || s.compare || s.base_level ||
        s.max_level || s.levels!=1 || s.min_lod!=0 || s.max_lod!=0 || s.lod_bias!=0 ||
        s.anisotropy!=1 || s.opaque_black_border)
        return std::nullopt;
    return ResolveDualSampler(s.min_filter,s.mag_filter,s.wrap_s,s.wrap_t);
}

std::optional<MTLSamplerAddressMode> GateAddressMode(unsigned mode, bool opaque_black_border = false) {
    switch (mode) {
    case 0x2901: return MTLSamplerAddressModeRepeat;
    case 0x8370: return MTLSamplerAddressModeMirrorRepeat;
    case 0x812f: return MTLSamplerAddressModeClampToEdge;
    case 0x812d:
        return opaque_black_border ? MTLSamplerAddressModeClampToBorderColor
                                   : MTLSamplerAddressModeClampToZero;
    default: return std::nullopt;
    }
}

std::optional<DualSamplerRecipe> GateSamplerKey(const PicaSampleInputs& s) {
    if (!s.rgba8 || !s.normalized || !s.identity_swizzle || s.compare || s.base_level ||
        s.max_level > 7 || s.levels != s.max_level + 1 || s.lod_bias || s.anisotropy != 1 ||
        !std::isfinite(s.min_lod) || s.min_lod < 0 || s.min_lod > s.max_lod || s.max_lod != s.max_level ||
        s.min_lod != static_cast<unsigned>(s.min_lod) ||
        (s.min_filter < 0x2700 || s.min_filter > 0x2703) ||
        (s.mag_filter != 0x2600 && s.mag_filter != 0x2601) ||
        !GateAddressMode(s.wrap_s) || !GateAddressMode(s.wrap_t))
        return std::nullopt;
    return DualSamplerRecipe{s.min_filter | ((s.mag_filter & 1) << 16) |
        (static_cast<unsigned>(s.min_lod) << 17) | (s.max_level << 20) |
        (static_cast<unsigned>(s.opaque_black_border) << 23), s.wrap_s, s.wrap_t};
}

id<MTLSamplerState> BuildGateSampler(id<MTLDevice> device, const PicaSampleInputs& s) {
    if (!GateSamplerKey(s) ||
        (s.opaque_black_border && ![device supportsFamily:MTLGPUFamilyApple7]))
        return nil;
    auto* desc = [MTLSamplerDescriptor new];
    desc.normalizedCoordinates = YES;
    desc.minFilter = (s.min_filter & 1) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    desc.magFilter = (s.mag_filter & 1) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    desc.mipFilter = (s.min_filter & 2) ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
    desc.lodMinClamp = s.min_lod;
    desc.lodMaxClamp = s.max_lod;
    desc.sAddressMode = *GateAddressMode(s.wrap_s, s.opaque_black_border);
    desc.tAddressMode = *GateAddressMode(s.wrap_t, s.opaque_black_border);
    if (s.opaque_black_border)
        desc.borderColor = MTLSamplerBorderColorOpaqueBlack;
    return [device newSamplerStateWithDescriptor:desc];
}

id<MTLSamplerState> BuildDualSampler(id<MTLDevice> device,const DualSamplerRecipe& r) {
    auto* s=[MTLSamplerDescriptor new];
    s.normalizedCoordinates=YES;
    s.minFilter=r.color_filter==0x2600 ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
    s.magFilter=s.minFilter;
    s.mipFilter=MTLSamplerMipFilterNotMipmapped;
    s.lodMinClamp=0;
    s.lodMaxClamp=0;
    s.sAddressMode=r.wrap_s==0x2901 ? MTLSamplerAddressModeRepeat : MTLSamplerAddressModeClampToEdge;
    s.tAddressMode=r.wrap_t==0x2901 ? MTLSamplerAddressModeRepeat : MTLSamplerAddressModeClampToEdge;
    s.maxAnisotropy=1;
    return [device newSamplerStateWithDescriptor:s];
}

id<MTLSamplerState> BuildSampler(id<MTLDevice> device, bool material, unsigned unit) {
    MTLSamplerDescriptor* sampler = [MTLSamplerDescriptor new];
    sampler.normalizedCoordinates = YES;
    sampler.minFilter = material ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    sampler.magFilter = sampler.minFilter;
    if (material) {
        sampler.sAddressMode = MTLSamplerAddressModeRepeat;
        sampler.tAddressMode = MTLSamplerAddressModeRepeat;
        sampler.mipFilter = unit == 0 ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNotMipmapped;
        sampler.lodMinClamp = 0;
        sampler.lodMaxClamp = unit == 0 ? 4 : 0;
    }
    return [device newSamplerStateWithDescriptor:sampler];
}

bool VerifiedDitherHost(id<MTLDevice> device) {
#if TARGET_OS_OSX
    const char* opted_in = std::getenv("CARA_PRIVATE_METAL_M1_RGBA8_DITHER");
    if (!opted_in || std::strcmp(opted_in, "1") != 0 ||
        ![device.name isEqualToString:@"Apple M1"])
        return false;
    std::array<char, 32> build{};
    std::size_t size = build.size();
    // Exact GPU/OS scope from the physical blend, scissor and mask
    // differential. Unknown builds retain the normal rejection.
    constexpr char supported_build[] = "26B131";
    return sysctlbyname("kern.osversion", build.data(), &size, nullptr, 0) == 0 &&
           size == sizeof(supported_build) &&
           std::memcmp(build.data(), supported_build, sizeof(supported_build)) == 0;
#else
    (void)device;
    return false;
#endif
}

bool SupportedDitheredColorState(const PicaPipelineInputs& p) {
    constexpr unsigned gl_add = 0x8006;
    constexpr unsigned gl_dst_alpha = 0x304;
    return p.blend && p.rgb_op == gl_add && p.alpha_op == gl_add &&
           p.rgb_src == gl_dst_alpha && p.rgb_dst == 0 &&
           p.alpha_src == gl_dst_alpha && p.alpha_dst == 0 &&
           p.color_mask == 7 && p.dither_width == 256 && p.dither_height == 416 &&
           p.dither_scissor == std::array<unsigned, 4>{0, 0, 240, 400};
}

} // namespace

void SetNativeMetalPipelineArchiveDirectory(const std::string& user) {
#if defined(CARA_PRIVATE_GATE_METAL_CATALOG) && TARGET_OS_IOS
    pipeline_archive = {};
    const char* flag = std::getenv("CARA_METAL_PIPELINE_ARCHIVE");
    const std::string_view mode = flag ? flag : "";
    if (mode == "record") {
        pipeline_archive.mode = PipelineArchiveMode::Record;
        pipeline_archive.descriptors = [NSMutableArray array];
    } else if (mode == "strict") {
        pipeline_archive.mode = PipelineArchiveMode::Strict;
    } else if (mode == "empty-strict") {
        pipeline_archive.mode = PipelineArchiveMode::EmptyStrict;
    } else {
        return;
    }
    auto* directory = [[NSString alloc] initWithBytes:user.data() length:user.size()
                                             encoding:NSUTF8StringEncoding];
    if (directory.length) {
        const auto url = [NSURL fileURLWithPath:directory isDirectory:YES];
        pipeline_archive.destination = [url URLByAppendingPathComponent:
            @"cara-metal-gate-pipelines-1119-54.metallib" isDirectory:NO];
    }
#else
    (void)user;
#endif
}

bool FinishNativeMetalPipelineArchive() {
#if defined(CARA_PRIVATE_GATE_METAL_CATALOG) && TARGET_OS_IOS
    switch (pipeline_archive.mode) {
    case PipelineArchiveMode::Off:
        return true;
    case PipelineArchiveMode::Record:
        return pipeline_archive.Write();
    case PipelineArchiveMode::Strict:
        return pipeline_archive.hits > 0 && pipeline_archive.misses == 0;
    case PipelineArchiveMode::EmptyStrict:
        return false;
    }
    return false;
#else
    return true;
#endif
}

#include "pica_metal_startup_cache.inl"

namespace {
thread_local bool general_fragment_requested = false;
}

void ConfigureMetalGeneralFragment(bool enabled) {
    general_fragment_requested = enabled;
}

bool UseMetalGeneralFragment() {
    static const auto override = []() -> std::optional<bool> {
        if (const char* setting = std::getenv("CARA_METAL_GENERAL_FRAGMENT"))
            return std::string_view(setting) == "1";
        return std::nullopt;
    }();
    return override.value_or(general_fragment_requested);
}

bool UseMetalDeadCombinerElision() {
    static const auto override = []() -> std::optional<bool> {
        if (const char* setting = std::getenv("CARA_METAL_ELIDE_DEAD_COMBINER"))
            return std::string_view(setting) == "1";
        return std::nullopt;
    }();
    return override.value_or(UseMetalGeneralFragment());
}

PicaMetalPipelines::PicaMetalPipelines(id<MTLDevice> device_, std::vector<PicaMetalDualAsset> dual)
    : device(device_), dual_assets(std::move(dual)), gate_library(OpenGateLibrary(device_)),
      rgba8_dither_verified_host(VerifiedDitherHost(device_)) {}

std::optional<unsigned> PicaMetalPipelines::FindRuntime(
    std::string_view vertex, std::string_view fragment, PicaMetalVertexMode mode) const {
    for (unsigned index = 0; index < runtime_pairs.size(); ++index) {
        const auto& pair = runtime_pairs[index];
        if (pair.mode == mode && pair.vertex_source == vertex && pair.fragment_source == fragment)
            return index;
    }
    return std::nullopt;
}

std::optional<std::array<unsigned, 3>> PicaMetalPipelines::RuntimeMaterialUnits(
    std::string_view vertex, std::string_view fragment, PicaMetalVertexMode mode) const {
#ifdef CARA_RUNTIME_METAL_SHADERS
    if (const auto existing = FindRuntime(vertex, fragment, mode))
        return runtime_pairs[*existing].stages.units;
    if (warmup_mode == WarmupReplay && !warming) {
        ++warmup_translation_misses;
        LOG_ERROR(Render_OpenGL, "Metal warmup runtime translation miss");
        return std::nullopt;
    }
    constexpr std::size_t PairLimit = 4096;
    if (runtime_pairs.size() >= PairLimit) {
        LOG_ERROR(Render_OpenGL, "Runtime Metal shader cache budget exhausted");
        return std::nullopt;
    }
    const auto translate_mode = (mode == PicaMetalVertexMode::Packed || mode == PicaMetalVertexMode::PairIndexed)
        ? RuntimeMetalVertexMode::Packed
        : mode == PicaMetalVertexMode::DecodedIndexed ? RuntimeMetalVertexMode::DecodedIndexed
                                                     : RuntimeMetalVertexMode::StageIn;
    std::string error;
    const bool raw = mode == PicaMetalVertexMode::Packed && UseMetalRawIndices();
    if (TraceColdPipelines() && !warming && !retain_compiled_descriptor) {
        LOG_INFO(Render_OpenGL, "Cold pair first use refresh={} pair={} mode={}",
                 VideoCore::GetCurrentFrame() + 1,
                 PairCount + dual_assets.size() + std::size(GatePairs) + runtime_pairs.size(),
                 unsigned(mode));
    }
    auto stages = [&] {
        if (auto cached = ReadMslDisk(vertex, fragment, unsigned(mode)))
            return cached;
        NativeMetalCost::RecordFirstShaderTranslation();
        NativeMetalCost::Scope translate(NativeMetalCost::RuntimeShaderTranslate);
        return TranslateRuntimeMetalShaders(vertex, fragment, translate_mode, raw,
                                            raw && UseMetalRawIndexConstants(), error);
    }();
    if (!stages) {
        LOG_ERROR(Render_OpenGL, "Runtime Metal shader translation rejected mode={}: {}",
                  unsigned(mode), error);
        return std::nullopt;
    }
    const auto logical_bytes = vertex.size() + fragment.size() + stages->vertex.size() + stages->fragment.size();
    const auto owned = runtime_sources.Intern({vertex,fragment,stages->vertex,stages->fragment});
    if (!owned) {
        LOG_ERROR(Render_OpenGL, "Runtime Metal shader exceeds unique source budget pairs={} unique_bytes={} incoming_logical_bytes={}",
                  runtime_pairs.size(), runtime_sources.Bytes(), logical_bytes);
        return std::nullopt;
    }
    const auto units = stages->units;
    std::string{}.swap(stages->vertex);
    std::string{}.swap(stages->fragment);
    runtime_pairs.push_back({owned->sources[0],owned->sources[1],mode,std::move(*stages),
                             owned->sources[2],owned->sources[3]});
    NativeMetalCost::Count(NativeMetalCost::RuntimeSourceLogicalBytes, logical_bytes);
    NativeMetalCost::Count(NativeMetalCost::RuntimeSourceUniqueBytes, owned->added_bytes);
    NativeMetalCost::Count(NativeMetalCost::RuntimeSourceInternHits, owned->hits);
    return units;
#else
    return std::nullopt;
#endif
}

std::optional<std::array<unsigned, 3>> PicaMetalPipelines::GateMaterialUnits(
    std::string_view vertex, std::string_view fragment, PicaMetalVertexMode mode) const {
    if (!gate_library)
        return RuntimeMaterialUnits(vertex, fragment, mode);
    if (NativeMetalCost::ReuseDrawInputs()) {
        const auto lookup = FindGatePair(vertex, fragment, mode);
        if (lookup.ambiguous)
            return std::nullopt;
        if (!lookup.index)
            return RuntimeMaterialUnits(vertex, fragment, mode);
        const auto& entry = GatePairs[*lookup.index];
        return std::array{entry.sampled_unit, entry.secondary_unit, NoMaterialUnit};
    }
    const auto vs = SourceDigest(vertex);
    const auto fs = SourceDigest(fragment);
    std::optional<std::array<unsigned, 3>> result;
    for (const auto& pair : GatePairs) {
        if (pair.vertex != std::string_view(vs.data(), vs.size()) ||
            pair.fragment != std::string_view(fs.data(), fs.size()) || pair.vertex_mode != mode)
            continue;
        if (result)
            return std::nullopt;
        result = std::array{pair.sampled_unit, pair.secondary_unit, NoMaterialUnit};
    }
    return result ? result : RuntimeMaterialUnits(vertex, fragment, mode);
}

std::optional<PicaMetalPipelines::Key> PicaMetalPipelines::MakeKey(
    const PicaPipelineInputs& p, const PicaSampleInputs& sampler,
    const std::optional<PicaSampleInputs>& secondary,
    const std::optional<PicaSampleInputs>& tertiary) const {
    const bool is_dual = secondary.has_value();
    const bool is_triple = tertiary.has_value();
    const bool has_material = p.sampled_unit != NoMaterialUnit;
    auto first = has_material ? GateSamplerKey(sampler) : std::nullopt;
    auto second = is_dual ? GateSamplerKey(*secondary) : std::nullopt;
    const auto third = is_triple ? GateSamplerKey(*tertiary) : std::nullopt;
    if (is_dual ? !SupportedDualUnits(p.sampled_unit,p.secondary_unit)
                : p.secondary_unit != NoMaterialUnit)
        return std::nullopt;
    if (is_triple ? (!is_dual || p.sampled_unit != 0 || p.secondary_unit != 1 ||
                     p.tertiary_unit != 2 || !third)
                  : p.tertiary_unit != NoMaterialUnit)
        return std::nullopt;
    const bool rgba8 = p.color_format == MTLPixelFormatRGBA8Unorm;
    const bool packed_color = p.color_format == MTLPixelFormatB5G6R5Unorm ||
                              p.color_format == MTLPixelFormatABGR4Unorm ||
                              p.color_format == MTLPixelFormatA1BGR5Unorm;
    if (!rgba8 && (!packed_color || ![device supportsFamily:MTLGPUFamilyApple2]))
        return std::nullopt;
    const bool unsupported_dither =
        p.dither && (!rgba8 || !rgba8_dither_verified_host || !SupportedDitheredColorState(p));
    if (((p.vertex_mode == PicaMetalVertexMode::Packed || p.vertex_mode == PicaMetalVertexMode::PairIndexed) &&
         !p.packed_arrays) ||
        unsupported_dither || p.shadow || p.polygon_offset || p.discard ||
        p.sample_control || p.active_logic_op || !p.clip0 ||
        (p.clip1 != p.vertex.starts_with(RuntimeMetalUserClip)) ||
        (p.clip1 && p.vertex_mode == PicaMetalVertexMode::StageIn) ||
        p.depth_near != 0 || p.depth_far != 1 || p.color_mask > 15 ||
        (p.stencil && (p.stencil_read > 255 || p.stencil_write > 255 || p.stencil_reference > 255)) ||
        (p.cull && p.cull_face != 0x404 && p.cull_face != 0x405) ||
        (p.winding != 0x900 && p.winding != 0x901) ||
        !std::all_of(p.blend_color.begin(),p.blend_color.end(),[](float f) { return std::isfinite(f); }))
        return std::nullopt;
    // The vector and its stages are immutable during the renderer lifetime.
    if (dual_assets.size() > std::numeric_limits<unsigned>::max() - PairCount -
                             std::size(GatePairs))
        return std::nullopt;
    const auto gate_begin = PairCount + static_cast<unsigned>(dual_assets.size());
    std::optional<unsigned> match;
    if (!is_triple && gate_library && (!has_material || first) && (!is_dual || second)) {
        const bool reuse = NativeMetalCost::ReuseDrawInputs();
        const auto lookup = reuse ? FindGatePair(p.vertex, p.fragment, p.vertex_mode)
                                  : GatePairLookup{};
        if (reuse && !lookup.ambiguous) {
            if (lookup.index) {
                const auto& pair = GatePairs[*lookup.index];
                if (pair.sampled_unit == p.sampled_unit && pair.secondary_unit == p.secondary_unit) {
                    if (!MaterialSlotLayout(pair))
                        return std::nullopt;
                    match = gate_begin + *lookup.index;
                }
            }
        } else {
            const auto vs = SourceDigest(p.vertex);
            const auto fs = SourceDigest(p.fragment);
            const auto matches = [&](const Pair& pair) {
                return pair.vertex == std::string_view(vs.data(), vs.size()) &&
                       pair.fragment == std::string_view(fs.data(), fs.size()) &&
                       pair.sampled_unit == p.sampled_unit &&
                       pair.secondary_unit == p.secondary_unit && pair.vertex_mode == p.vertex_mode;
            };
            for (unsigned i = 0; i < std::size(GatePairs); ++i) {
                if (!matches(GatePairs[i]))
                    continue;
                if (match || !MaterialSlotLayout(GatePairs[i]))
                    return std::nullopt;
                match = gate_begin + i;
            }
        }
    }

    if (!match && (!has_material || first) && (!is_dual || second)) {
        if (const auto index = FindRuntime(p.vertex, p.fragment, p.vertex_mode)) {
            const auto& shader = runtime_pairs[*index].stages;
            if (shader.units == std::array{p.sampled_unit, p.secondary_unit, p.tertiary_unit})
                match = gate_begin + static_cast<unsigned>(std::size(GatePairs)) + *index;
        }
    }

    if (!match) {
        if (p.clip1 || is_triple)
            return std::nullopt;
        first = is_dual ? ParityRecipe(sampler) : std::nullopt;
        second = is_dual ? ParityRecipe(*secondary) : std::nullopt;
        if (is_dual && (!first || !second))
            return std::nullopt;
        if (p.vertex_mode != PicaMetalVertexMode::Packed)
            return std::nullopt;
#if TARGET_OS_IOS
        // Only captured iOS assets can be selected on the device.
        if (!is_dual)
            return std::nullopt;
#endif
        if (!is_dual && (!has_material || !SupportedSampler(p.sampled_unit, sampler)))
            return std::nullopt;
        const unsigned begin = is_dual ? PairCount : 0;
        const unsigned end = is_dual ? gate_begin : PairCount;
        for (unsigned index = begin; index < end; ++index) {
            if (is_dual) {
                const auto& asset = dual_assets[index - PairCount];
#if TARGET_OS_IOS
                if (!asset.ios_gles_verified)
                    continue;
#else
                if (asset.ios_gles_verified)
                    continue;
#endif
            }
            const auto pair = PairAt(index, dual_assets);
            if (pair.sampled_unit != p.sampled_unit || pair.secondary_unit != p.secondary_unit ||
                pair.vertex != p.vertex || pair.fragment != p.fragment)
                continue;
            if (match || pair.msl_vertex.empty() || pair.msl_fragment.empty() ||
                !MaterialSlotLayout(pair))
                return std::nullopt;
            match = index;
            if (!is_dual)
                break;
        }
    }
    if (match) {
        const unsigned index=*match;
            return Key{index,p.blend,p.color_mask,p.blend?p.rgb_op:0,p.blend?p.alpha_op:0,
                       p.blend?p.rgb_src:0,p.blend?p.rgb_dst:0,p.blend?p.alpha_src:0,
                       p.blend?p.alpha_dst:0,static_cast<unsigned>(p.color_format),
                       p.depth_test?p.depth_compare:0x207,
                       p.depth_test&&p.depth_write,p.stencil,p.stencil?p.stencil_compare:0,
                       p.stencil?p.stencil_read:0,p.stencil?p.stencil_write:0,p.stencil?p.stencil_fail:0,
                       p.stencil?p.stencil_depth_fail:0,p.stencil?p.stencil_pass:0,
                       first?first->color_filter:0,first?first->wrap_s:0,first?first->wrap_t:0,
                       second?second->color_filter:0,second?second->wrap_s:0,second?second->wrap_t:0,
                       third?third->color_filter:0,third?third->wrap_s:0,third?third->wrap_t:0};
        }
    return std::nullopt;
}

const PicaMetalPipelines::Entry* PicaMetalPipelines::Find(const Key& key) const {
    const auto stop = entries.end();
    const auto match = std::find_if(entries.begin(),stop,[&](const Entry& entry) { return entry.key == key; });
    return match == stop ? nullptr : &*match;
}

bool PicaMetalPipelines::Prepare(const PicaPipelineInputs& p, const PicaSampleInputs& sampler,
                                 const std::optional<PicaSampleInputs>& secondary,
                                 const std::optional<PicaSampleInputs>& tertiary) {
    const auto key = MakeKey(p, sampler, secondary, tertiary);
    if (!key || !device) {
        LOG_ERROR(Render_OpenGL, "Gate Metal pipeline key rejected mode={} unit={} clip1={} blend={} "
                  "src_rgb={} dst_rgb={} op_rgb={} depth_compare={} stencil={}",
                  unsigned(p.vertex_mode), p.sampled_unit, p.clip1, p.blend, p.rgb_src,
                  p.rgb_dst, p.rgb_op, p.depth_compare, p.stencil);
        return false;
    }
    if (Find(*key))
        return true;
    if (warmup_mode == WarmupLearn && !warming)
        ++warmup_draw_misses;
    if (warmup_mode == WarmupReplay && !warming) {
        ++warmup_draw_misses;
        LOG_ERROR(Render_OpenGL, "Metal warmup draw-time pipeline miss id={}", (*key)[0]);
        return false;
    }
    if (@available(macOS 13.0, iOS 16.0, *)) {
        try {
            const auto id = (*key)[0];
            const auto runtime_begin = PairCount + dual_assets.size() + std::size(GatePairs);
            Pair pair;
            if (id < runtime_begin) {
                pair = PairAt(id, dual_assets);
            } else {
                const auto& runtime = runtime_pairs[id - runtime_begin];
                const auto& shader = runtime.stages;
                const auto metal_slot = [](unsigned slot) -> NSUInteger {
                    return slot == RuntimeMetalUnusedSlot ? NSNotFound : slot;
                };
                std::array<NSUInteger, 6> slots{};
                std::transform(shader.slots.begin(), shader.slots.end(), slots.begin(), metal_slot);
                pair = {runtime.vertex_source, runtime.fragment_source, runtime.msl_vertex, runtime.msl_fragment,
                        shader.units[0], slots, shader.units[1], metal_slot(shader.secondary_texture),
                        metal_slot(shader.secondary_sampler), false, runtime.mode, true,
                        shader.units[2], metal_slot(shader.tertiary_texture),
                        metal_slot(shader.tertiary_sampler)};
            }
            if (!retain_compiled_descriptor && pair.runtime &&
                std::getenv("CARA_METAL_VERTEX_FALLBACK_TRACE")) {
                LOG_INFO(Render_OpenGL,
                         "Live Metal prepare id={} active={} same_key={} same_render={} same_vertex={} vertex_cached={}",
                         id, specializer ? int(specializer->key[0]) : -1,
                         specializer && specializer->key == *key,
                         specializer && SamePicaMetalRenderPipeline(*key, specializer->key),
                         specializer && runtime_pairs[specializer->runtime_index].msl_vertex == pair.msl_vertex,
                         vertex_functions.contains(pair.msl_vertex));
            }
            MTLRenderPipelineDescriptor* learned_descriptor = nil;
            MTLDepthStencilDescriptor* depth = [MTLDepthStencilDescriptor new];
            depth.depthCompareFunction = p.depth_test ? Compare(p.depth_compare) : MTLCompareFunctionAlways;
            depth.depthWriteEnabled = p.depth_test && p.depth_write;
            if (p.stencil) {
                MTLStencilDescriptor* face = [MTLStencilDescriptor new];
                face.stencilCompareFunction = Compare(p.stencil_compare);
                face.readMask = p.stencil_read;
                face.writeMask = p.stencil_write;
                face.stencilFailureOperation = Stencil(p.stencil_fail);
                face.depthFailureOperation = Stencil(p.stencil_depth_fail);
                face.depthStencilPassOperation = Stencil(p.stencil_pass);
                depth.frontFaceStencil = face;
                depth.backFaceStencil = face;
            }
            PicaMetalPipelineSelection selection{};
            const auto render = ReuseRenderPipelines() && (pair.precompiled || pair.runtime) ?
                std::find_if(entries.cbegin(), entries.cend(), [&](const Entry& entry) {
                    return SamePicaMetalRenderPipeline(*key, entry.key);
                }) : entries.cend();
            if (render != entries.cend()) {
                // Same catalog entry and render descriptor. The reflected
                // slots and the process-wide raw-index policy also match.
                selection.pipeline = render->selection.pipeline;
                selection.simd_reference = render->selection.simd_reference;
                selection.vertex_mode = render->selection.vertex_mode;
                selection.batched_vertex = render->selection.batched_vertex;
                selection.primary_material_cube = render->selection.primary_material_cube;
                NativeMetalCost::Count(NativeMetalCost::RenderPipelineReuse, 1);
            } else {
                MTLRenderPipelineDescriptor* ps = [MTLRenderPipelineDescriptor new];
                ps.colorAttachments[0].pixelFormat = p.color_format;
                ps.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
                ps.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
                if (pair.vertex_mode == PicaMetalVertexMode::StageIn)
                    ps.vertexDescriptor = MakePicaStageInDescriptor();
                auto* attachment = ps.colorAttachments[0];
                attachment.writeMask = static_cast<MTLColorWriteMask>(
                    ((p.color_mask&1)?MTLColorWriteMaskRed:0) |
                    ((p.color_mask&2)?MTLColorWriteMaskGreen:0) |
                    ((p.color_mask&4)?MTLColorWriteMaskBlue:0) |
                    ((p.color_mask&8)?MTLColorWriteMaskAlpha:0));
                attachment.blendingEnabled = p.blend;
                if (p.blend) {
                    attachment.rgbBlendOperation = Blend(p.rgb_op);
                    attachment.alphaBlendOperation = Blend(p.alpha_op);
                    attachment.sourceRGBBlendFactor = Factor(p.rgb_src);
                    attachment.destinationRGBBlendFactor = Factor(p.rgb_dst);
                    attachment.sourceAlphaBlendFactor = Factor(p.alpha_src);
                    attachment.destinationAlphaBlendFactor = Factor(p.alpha_dst);
                }

                const auto prepare_stage = [&](StageCache& stages, std::string_view source,
                                               NSString* entry, NativeMetalCost::Phase phase) {
                    if (const auto found = stages.find(source); found != stages.end())
                        return found->second;
                    if (phase == NativeMetalCost::RuntimeVertexLibrary)
                        NativeMetalCost::RecordColdVertexDraw();
                    NativeMetalCost::Scope compile(phase);
                    const auto function = Compile(device,source,entry);
                    if (function)
                        stages.emplace(source,function);
                    return function;
                };
                bool raw_index_function = false;
                selection.simd_reference = pair.runtime && ValidSimdVertexMsl(pair.msl_vertex);
                const bool simd_attempt = selection.simd_reference;
                if (HasSimdVertexTag(pair.vertex)) NativeMetalCost::Count(NativeMetalCost::SimdReferenceRequests,1);
                const auto fallback_simd = [&] {
                    if (!selection.simd_reference) return false;
                    selection.simd_reference=false;
                    NativeMetalCost::Count(NativeMetalCost::SimdReferenceFallbacks,1);
                    LOG_INFO(Render_OpenGL,"SIMD vertex compile/reflection rejected; compiling full original shader");
                    std::string error;
                    auto full=TranslateRuntimeMetalShaders(StripSimdVertexTag(pair.vertex),pair.fragment,
                        RuntimeMetalVertexMode::Packed,true,UseMetalRawIndexConstants(),error);
                    if (!full) return false;
                    auto owned=runtime_sources.Intern({StripSimdVertexTag(pair.vertex),pair.fragment,full->vertex,full->fragment});
                    if (!owned) return false;
                    NativeMetalCost::Count(NativeMetalCost::RuntimeSourceUniqueBytes,owned->added_bytes);
                    ps.vertexFunction=prepare_stage(vertex_functions,owned->sources[2],@"cara_pica_vert",NativeMetalCost::RuntimeVertexLibrary);
                    ps.fragmentFunction=prepare_stage(fragment_functions,owned->sources[3],@"cara_pica_frag",NativeMetalCost::RuntimeFragmentLibrary);
                    return ps.vertexFunction && ps.fragmentFunction;
                };
                if (pair.precompiled) {
                    const auto load = [&](std::string_view entry) {
                        auto* name = [[NSString alloc] initWithBytes:entry.data() length:entry.size()
                                                           encoding:NSUTF8StringEncoding];
                        return [gate_library newFunctionWithName:name];
                    };
                    if (pair.vertex_mode == PicaMetalVertexMode::Packed && UseMetalRawIndices()) {
                        const std::string raw_entry = std::string(pair.msl_vertex) + "_raw_indices";
                        if (UseMetalRawIndexConstants()) {
                            ps.vertexFunction = load(raw_entry + "_constants");
                            Require(ps.vertexFunction != nil, "Missing raw-index constants function");
                        } else {
                            ps.vertexFunction = load(raw_entry);
                        }
                        raw_index_function = ps.vertexFunction != nil;
                    }
                    if (!raw_index_function)
                        ps.vertexFunction = load(pair.msl_vertex);
                    ps.fragmentFunction = load(pair.msl_fragment);
                } else {
                    raw_index_function = pair.runtime && pair.vertex_mode == PicaMetalVertexMode::Packed &&
                                         UseMetalRawIndices();
                    if (selection.simd_reference && ![device supportsFamily:MTLGPUFamilyApple7]) {
                        if (!fallback_simd()) return false;
                    } else {
                        static const auto parallel_override = []() -> std::optional<bool> {
                            if (const char* value = std::getenv("CARA_METAL_PARALLEL_COLD_STAGES"))
                                return std::string_view(value) == "1";
                            return std::nullopt;
                        }();
                        if (parallel_override.value_or(UseMetalGeneralFragment()) &&
                            !vertex_functions.contains(pair.msl_vertex) &&
                            !fragment_functions.contains(pair.msl_fragment)) {
                            NativeMetalCost::RecordColdVertexDraw();
                            const auto vertex_job = [device = device, source = pair.msl_vertex] {
                                @autoreleasepool {
                                    const auto start = std::chrono::steady_clock::now();
                                    auto function = Compile(device, source, @"cara_pica_vert");
                                    const auto duration = std::chrono::duration_cast<
                                        std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start);
                                    return std::pair{function, duration.count()};
                                }
                            };
                            auto vertex_work = std::async(std::launch::async | std::launch::deferred,
                                                          vertex_job);
                            ps.fragmentFunction = prepare_stage(fragment_functions, pair.msl_fragment,
                                                                @"cara_pica_frag",
                                                                NativeMetalCost::RuntimeFragmentLibrary);
                            auto [function, ns] = vertex_work.get();
                            if (NativeMetalCost::Enabled())
                                NativeMetalCost::RecordNs(NativeMetalCost::RuntimeVertexLibrary, ns);
                            if (function)
                                vertex_functions.emplace(pair.msl_vertex, function);
                            ps.vertexFunction = function;
                        } else {
                            ps.vertexFunction = prepare_stage(vertex_functions, pair.msl_vertex, @"cara_pica_vert",
                                                              NativeMetalCost::RuntimeVertexLibrary);
                            ps.fragmentFunction = prepare_stage(fragment_functions, pair.msl_fragment, @"cara_pica_frag",
                                                                NativeMetalCost::RuntimeFragmentLibrary);
                        }
                    }
                }
                if ((!ps.vertexFunction || !ps.fragmentFunction) && !fallback_simd())
                    return false;
                MTLPipelineOption options = MTLPipelineOptionBindingInfo;
#if defined(CARA_PRIVATE_GATE_METAL_CATALOG) && TARGET_OS_IOS
                if (pipeline_archive.mode != PipelineArchiveMode::Off) {
                    if (!pair.precompiled || (pipeline_archive.device && pipeline_archive.device != device))
                        return false;
                    pipeline_archive.device = device;
                    if (pipeline_archive.IsStrict()) {
                        if (!pipeline_archive.Open())
                            return false;
                        ps.binaryArchives = @[pipeline_archive.archive];
                        options = static_cast<MTLPipelineOption>(
                            options | MTLPipelineOptionFailOnBinaryArchiveMiss);
                    }
                }
#endif
                bool archive_hit = false;
                MTLRenderPipelineReflection* reflection = nil;
                NSError* pipeline_error = nil;
                const bool trace_creation = !retain_compiled_descriptor && TraceColdPipelines();
                using Clock = std::chrono::steady_clock;
                const auto start = trace_creation ? Clock::now() : Clock::time_point{};
                {
                    NativeMetalCost::Scope creation(NativeMetalCost::RenderPipelineCreate);
                    if (warmup_archive && (warming || warmup_mode == WarmupLearn)) {
                        ps.binaryArchives = @[warmup_archive];
                        selection.pipeline = [device newRenderPipelineStateWithDescriptor:ps
                            options:static_cast<MTLPipelineOption>(options | MTLPipelineOptionFailOnBinaryArchiveMiss)
                            reflection:&reflection error:&pipeline_error];
                        archive_hit = bool(selection.pipeline);
                        if (archive_hit)
                            ++warmup_archive_hits;
                        else
                            ++warmup_archive_misses;
                        ps.binaryArchives = nil;
                    }
                    if (!selection.pipeline && warming && warmup_mode == WarmupLearn &&
                        !CompleteLearnedPrewarm())
                        return false;
                    if (!selection.pipeline) {
                        pipeline_error = nil;
                        selection.pipeline = [device newRenderPipelineStateWithDescriptor:ps
                            options:options reflection:&reflection error:&pipeline_error];
                    }
                }
                if (trace_creation) {
                    const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                        Clock::now() - start).count();
                    std::ostringstream descriptor;
                    for (std::size_t i = 1; i < PicaMetalRenderKeyFields; ++i)
                        descriptor << ',' << (*key)[i];
                    const auto vs = SourceDigest(pair.msl_vertex);
                    const auto fs = SourceDigest(pair.msl_fragment);
                    LOG_INFO(Render_OpenGL, "Cold PSO refresh={} pair={} vs={} fs={} state={} build_us={} archive_hit={}",
                             VideoCore::GetCurrentFrame() + 1, id, std::string(vs.data(), 12),
                             std::string(fs.data(), 12), descriptor.str(), elapsed_us, archive_hit);
                }
                if (selection.simd_reference && (!selection.pipeline ||
                    !CheckReflection(reflection,pair,selection.primary_material_cube,true))) {
                    if (fallback_simd()) {
                        ps.binaryArchives=nil;
                        selection.pipeline=[device newRenderPipelineStateWithDescriptor:ps
                            options:MTLPipelineOptionBindingInfo reflection:&reflection error:&pipeline_error];
                    }
                }
                if (!selection.pipeline) {
#if defined(CARA_PRIVATE_GATE_METAL_CATALOG) && TARGET_OS_IOS
                    if (pipeline_archive.IsStrict()) {
                        ++pipeline_archive.misses;
                        NativeMetalCost::Count(NativeMetalCost::RenderPipelineArchiveMiss, 1);
                        LOG_ERROR(Render_OpenGL, "Metal pipeline archive {} miss: {} ({}:{})",
                            pipeline_archive.mode == PipelineArchiveMode::EmptyStrict ? "expected empty" : "strict",
                            pipeline_error.localizedDescription.UTF8String ?: "unknown Metal error",
                            pipeline_error.domain.UTF8String ?: "unknown", pipeline_error.code);
                        return false;
                    }
#endif
                    LOG_ERROR(Render_OpenGL, "Gate Metal pipeline creation: {}",
                        pipeline_error.localizedDescription.UTF8String ?: "unknown Metal error");
                    return false;
                }
#if defined(CARA_PRIVATE_GATE_METAL_CATALOG) && TARGET_OS_IOS
                if (pipeline_archive.mode == PipelineArchiveMode::EmptyStrict) {
                    LOG_ERROR(Render_OpenGL, "Metal pipeline archive empty strict unexpectedly created a pipeline");
                    return false;
                }
#endif

                if (!CheckReflection(reflection, pair, selection.primary_material_cube, selection.simd_reference))
                    return false;
#ifdef CARA_RUNTIME_METAL_SHADERS
                if (selection.simd_reference) NativeMetalCost::Count(NativeMetalCost::SimdReferenceCompiled,1);
                else if (HasSimdVertexTag(pair.vertex) && !simd_attempt) {
                    NativeMetalCost::Count(NativeMetalCost::SimdReferenceRejected,1);
                    LOG_INFO(Render_OpenGL,"SIMD vertex translation admission rejected; full shader selected");
                }
                if (pair.runtime && raw_index_function && !selection.simd_reference) {
                    if (const auto batch = PrepareComputeFunctions(device, pair.msl_vertex,
                                                                  p.vertex, p.fragment)) {
                        MTLRenderPipelineDescriptor* alternate = [ps copy];
                        alternate.vertexFunction = batch->raster;
                        NSError* error = nil;
                        auto raster = [device newRenderPipelineStateWithDescriptor:alternate error:&error];
                        if (raster) {
                            selection.batched_vertex = std::make_shared<BatchedMetalVertexProgram>(
                                BatchedMetalVertexProgram{batch->compute, batch->arguments, raster,
                                                          batch->resources,
                                                          batch->gathers_unique_vertices});
                        } else {
                            LOG_ERROR(Render_OpenGL, "Batched PICA raster pipeline: {}",
                                      error.localizedDescription.UTF8String ?: "Metal raster interface");
                        }
                    }
                }
#endif
                if (pair.runtime) {
                    LOG_INFO(Render_OpenGL, "Runtime Metal shader pipeline ready id={} vertex_mode={} units={}/{}",
                             id, unsigned(pair.vertex_mode), pair.sampled_unit, pair.secondary_unit);
                }
#if defined(CARA_PRIVATE_GATE_METAL_CATALOG) && TARGET_OS_IOS
                if (pipeline_archive.mode == PipelineArchiveMode::Strict) {
                    ++pipeline_archive.hits;
                    NativeMetalCost::Count(NativeMetalCost::RenderPipelineArchiveHit, 1);
                } else if (pipeline_archive.mode == PipelineArchiveMode::Record &&
                           !pipeline_archive.Remember(ps)) {
                    return false;
                }
#endif
                if (retain_compiled_descriptor)
                    compiled_descriptor = [ps copy];
                if (warmup_mode == WarmupLearn && !archive_hit && (pair.precompiled || pair.runtime))
                    learned_descriptor = ps;
                if (warmup_mode == WarmupRecord &&
                    warmup_descriptors.size() < WarmupMaxRecipes)
                    warmup_descriptors.push_back([ps copy]);
                NativeMetalCost::Count(NativeMetalCost::RenderPipelineCreateValidated, 1);
                selection.vertex_mode = raw_index_function ? MetalRawIndexMode : pair.vertex_mode;
            }
            selection.depth = [device newDepthStencilStateWithDescriptor:depth];
            selection.texel_fetch = BuildSampler(device,false,p.sampled_unit);
            selection.primary_unit = pair.sampled_unit;
            if (secondary) {
                selection.material = (pair.precompiled || pair.runtime) ? BuildGateSampler(device, sampler)
                                                      : BuildDualSampler(device, *ParityRecipe(sampler));
                const auto other = (pair.precompiled || pair.runtime) ? BuildGateSampler(device, *secondary)
                                                    : BuildDualSampler(device, *ParityRecipe(*secondary));
                if (!other)
                    return false;
                selection.secondary=SecondaryPicaMaterial{nil,other,pair.secondary_unit,
                    pair.secondary_texture_slot,pair.secondary_sampler_slot};
            } else if (p.sampled_unit != NoMaterialUnit) {
                selection.material = (pair.precompiled || pair.runtime) ? BuildGateSampler(device, sampler)
                                                      : BuildSampler(device, true, p.sampled_unit);
            }
            if (tertiary) {
                const auto sampler_state = BuildGateSampler(device, *tertiary);
                if (!sampler_state)
                    return false;
                selection.tertiary = SecondaryPicaMaterial{nil, sampler_state, pair.tertiary_unit,
                    pair.tertiary_texture_slot, pair.tertiary_sampler_slot};
            }
            if (!selection.depth || !selection.texel_fetch ||
                (p.sampled_unit != NoMaterialUnit && !selection.material))
                return false;
            selection.slots = pair.slots;
            entries.push_back(Entry{*key, selection});
            if (warmup_mode == WarmupLearn)
                RememberLearnedPipeline(p, *key, learned_descriptor);
            if (warmup_mode == WarmupRecord) {
                if (warmup_recipes.size() < WarmupMaxRecipes && (pair.precompiled || pair.runtime)) {
                    warmup_recipes.push_back({std::string(p.vertex), std::string(p.fragment), *key,
                        {p.sampled_unit, p.secondary_unit, p.tertiary_unit}, unsigned(p.vertex_mode)});
                } else {
                    warmup_recording_failed = true;
                }
            }
            return true;
        } catch (const std::runtime_error& failure) {
            LOG_ERROR(Render_OpenGL, "Gate Metal pipeline state: {}", failure.what());
            return false;
        }
    }
    return false;
}

void PicaMetalPipelines::PollSpecialization() {
    if (specializer && specializer->result.wait_for(std::chrono::seconds(0)) ==
                           std::future_status::ready) {
        const auto key = specializer->key;
        try {
            if (auto ready = specializer->result.get()) {
                const auto& stages = runtime_pairs[specializer->runtime_index];
                if (ready->reusable_stages) {
                    vertex_functions.try_emplace(stages.msl_vertex, ready->descriptor.vertexFunction);
                    fragment_functions.try_emplace(stages.msl_fragment, ready->descriptor.fragmentFunction);
                }
                if (!Find(key))
                    entries.push_back({key, std::move(ready->selection)});
                if (warmup_mode == WarmupLearn)
                    RememberLearnedPipeline(specializer->inputs, key, ready->descriptor);
                LOG_INFO(Render_OpenGL, "Exact Metal specialization ready from background worker");
            } else {
                failed_specializations.push_back(key);
            }
        } catch (const std::exception& error) {
            LOG_WARNING(Render_OpenGL, "Background Metal specialization failed: {}", error.what());
            failed_specializations.push_back(key);
        }
        specializer.reset();
    }
}

std::optional<PicaMetalPipelineSelection> PicaMetalPipelines::SelectWithGeneral(
    const PicaPipelineInputs& exact, std::string_view general_fragment,
    const PicaSampleInputs& sampler, const std::optional<PicaSampleInputs>& secondary,
    const std::optional<PicaSampleInputs>& tertiary) {
    PollSpecialization();
    if (auto ready = Select(exact, sampler, secondary, tertiary))
        return ready;
    const auto key = MakeKey(exact, sampler, secondary, tertiary);
    const auto runtime_index = FindRuntime(exact.vertex, exact.fragment, exact.vertex_mode);
    if (!key || !runtime_index || warming || warmup_mode == WarmupReplay ||
        warmup_mode == WarmupRecord)
        return std::nullopt;

    auto fallback = exact;
    fallback.fragment = general_fragment;
    const auto units = RuntimeMaterialUnits(fallback.vertex, fallback.fragment, fallback.vertex_mode);
    if (!units || *units != std::array{exact.sampled_unit, exact.secondary_unit, exact.tertiary_unit} ||
        !Prepare(fallback, sampler, secondary, tertiary))
        return std::nullopt;
    auto result = Select(fallback, sampler, secondary, tertiary);
    if (!result)
        return std::nullopt;
    result->general_fragment = true;
    StartSpecialization(*key, *runtime_index, exact, sampler, secondary, tertiary);
    return result;
}

bool PicaMetalPipelines::StartSpecialization(
    const Key& key, unsigned runtime_index, const PicaPipelineInputs& exact,
    const PicaSampleInputs& sampler, const std::optional<PicaSampleInputs>& secondary,
    const std::optional<PicaSampleInputs>& tertiary) {
    if (specializer)
        return specializer->key == key;
    if (std::find(failed_specializations.begin(), failed_specializations.end(), key) !=
        failed_specializations.end())
        return false;
    // Copy the metadata and borrow the immutable, interned source text. The
    // specialization future joins before the owner's source pool is destroyed.
    // Each worker keeps its own mutable caches and publishes only a reflected pipeline.
    const auto& stages = runtime_pairs[runtime_index];
    const auto vertex = vertex_functions.find(stages.msl_vertex);
    const auto fragment = fragment_functions.find(stages.msl_fragment);
    const id<MTLFunction> cached_vertex = vertex == vertex_functions.end() ? nil : vertex->second;
    const id<MTLFunction> cached_fragment = fragment == fragment_functions.end() ? nil : fragment->second;
    try {
        auto task = std::async(std::launch::async,
            [device = device, request = exact, runtime = stages, sampler, secondary, tertiary,
             async_shaders = UseMetalGeneralFragment(), cached_vertex, cached_fragment]() mutable
                -> std::optional<CompiledSpecialization> {
                @autoreleasepool {
                    ConfigureMetalGeneralFragment(async_shaders);
                    PicaMetalPipelines worker(device);
                    worker.retain_compiled_descriptor = true;
                    request.vertex = runtime.vertex_source;
                    request.fragment = runtime.fragment_source;
                    worker.runtime_pairs.push_back(std::move(runtime));
                    const auto& stages = worker.runtime_pairs.front();
                    if (cached_vertex)
                        worker.vertex_functions.emplace(stages.msl_vertex, cached_vertex);
                    if (cached_fragment)
                        worker.fragment_functions.emplace(stages.msl_fragment, cached_fragment);
                    if (!worker.Prepare(request, sampler, secondary, tertiary))
                        return std::nullopt;
                    auto ready = worker.Select(request, sampler, secondary, tertiary);
                    if (!ready)
                        return std::nullopt;
                    const auto descriptor = worker.compiled_descriptor;
                    const auto matches = [](const StageCache& cache, std::string_view source,
                                            id<MTLFunction> function) {
                        const auto found = cache.find(source);
                        return found != cache.end() && found->second == function;
                    };
                    // Catalog pipelines and vertex conversions can replace the
                    // stage functions. Publish only the exact runtime sources.
                    const bool reusable_stages =
                        matches(worker.vertex_functions, stages.msl_vertex, descriptor.vertexFunction) &&
                        matches(worker.fragment_functions, stages.msl_fragment, descriptor.fragmentFunction);
                    return CompiledSpecialization{std::move(*ready), descriptor, reusable_stages};
                }
            });
        auto archive_inputs = exact;
        archive_inputs.vertex = runtime_pairs[runtime_index].vertex_source;
        archive_inputs.fragment = runtime_pairs[runtime_index].fragment_source;
        specializer.emplace(Specialization{key, runtime_index, archive_inputs, std::move(task)});
        return true;
    } catch (const std::system_error& error) {
        LOG_WARNING(Render_OpenGL, "Unable to start Metal specialization worker: {}", error.what());
        failed_specializations.push_back(key);
    }
    return false;
}

bool PicaMetalPipelines::TryVertexFallback(
    const PicaPipelineInputs& exact, std::string_view stage_in_vertex,
    const PicaSampleInputs& sampler, const std::optional<PicaSampleInputs>& secondary,
    const std::optional<PicaSampleInputs>& tertiary) {
    PollSpecialization();
    if (warming || warmup_mode == WarmupReplay || warmup_mode == WarmupRecord ||
        HasSimdVertexTag(exact.vertex))
        return false;
    const auto key = MakeKey(exact, sampler, secondary, tertiary);
    const auto runtime_index = FindRuntime(exact.vertex, exact.fragment, exact.vertex_mode);
    if (!key || !runtime_index || Find(*key))
        return false;
    const auto& stages = runtime_pairs[*runtime_index];
    auto stage_in = exact;
    stage_in.vertex = stage_in_vertex;
    stage_in.vertex_mode = PicaMetalVertexMode::StageIn;
    stage_in.packed_arrays = false;
    // A resident vertex function still needs a linked PSO for each fragment
    // and fixed-state combination. Use StageIn for large multi-texture variants
    // while the worker links the exact packed pipeline.
    if (vertex_functions.contains(stages.msl_vertex) &&
        (!secondary || stages.msl_vertex.size() < 32 * 1024))
        return false;
    if (specializer && runtime_pairs[specializer->runtime_index].msl_vertex != stages.msl_vertex) {
        if (std::getenv("CARA_METAL_VERTEX_FALLBACK_TRACE"))
            LOG_INFO(Render_OpenGL, "Vertex fallback waiting on an unrelated compiler job");
        return false;
    }
    const auto units = RuntimeMaterialUnits(stage_in.vertex, stage_in.fragment, stage_in.vertex_mode);
    if (!units || *units != std::array{exact.sampled_unit, exact.secondary_unit, exact.tertiary_unit} ||
        !Prepare(stage_in, sampler, secondary, tertiary))
        return false;
    if (!specializer && !StartSpecialization(*key, *runtime_index, exact, sampler, secondary, tertiary))
        return false;
    NativeMetalCost::RecordColdVertexDraw();
    return true;
}

std::optional<PicaMetalPipelineSelection> PicaMetalPipelines::Select(
    const PicaPipelineInputs& p, const PicaSampleInputs& sampler,
    const std::optional<PicaSampleInputs>& secondary,
    const std::optional<PicaSampleInputs>& tertiary) const {
    const auto key = MakeKey(p, sampler, secondary, tertiary);
    const auto* entry = key ? Find(*key) : nullptr;
    if (!entry)
        return std::nullopt;
    auto result = entry->selection;
    result.cull = !p.cull ? MTLCullModeNone : p.cull_face == 0x404 ? MTLCullModeFront : MTLCullModeBack;
    result.winding = p.winding == 0x900 ? MTLWindingCounterClockwise : MTLWindingClockwise;
    result.stencil_reference = p.stencil ? p.stencil_reference : 0;
    result.blend_color = p.blend_color;
    return result;
}

void PicaMetalPipelineSelection::Apply(TexturedMetalDraw& draw) const {
    draw.pipeline = pipeline;
    draw.batched_vertex = batched_vertex;
    draw.simd_reference = simd_reference;
    draw.depth_stencil_state = depth;
    draw.texel_fetch_sampler = texel_fetch;
    draw.texture0_sampler = material;
    draw.primary_material_unit = primary_unit;
    draw.primary_material_cube = primary_material_cube;
    draw.vertex_mode = vertex_mode == MetalRawIndexMode ? PicaMetalVertexMode::Packed : vertex_mode;
    draw.secondary_material = secondary;
    draw.tertiary_material = tertiary;
    draw.lut_texture_slot = slots[0];
    draw.lut_sampler_slot = slots[1];
    draw.texture0_slot = slots[2];
    draw.texture0_sampler_slot = slots[3];
    draw.fragment_common_slot = slots[4];
    draw.fragment_light_slot = slots[5];
    draw.cull = cull;
    draw.winding = winding;
    draw.stencil_reference = stencil_reference;
    draw.blend_color = blend_color;
}

} // namespace Cara
