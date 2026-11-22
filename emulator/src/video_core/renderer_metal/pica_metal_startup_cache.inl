// SPDX-License-Identifier: GPL-2.0-or-later
// Included by pica_metal_pipelines.mm with the Metal descriptor helpers in scope.

namespace {
constexpr std::string_view WarmupMagic = "CaraMetalRecipes1";
constexpr unsigned WarmupMaxRecipes = 512;
constexpr unsigned WarmupMaxSource = 512 * 1024;
constexpr unsigned WarmupMaxFile = 64 * 1024 * 1024;
constexpr unsigned WarmupRecord = 1;
constexpr unsigned WarmupReplay = 2;
constexpr unsigned WarmupLearn = 3;
constexpr unsigned LearnPreloadLimit = 8;
constexpr auto LearnPreloadBudget = std::chrono::milliseconds(125);
constexpr unsigned LearnMaxDescriptors = 64;
constexpr std::uintmax_t LearnMaxArchiveBytes = 128 * 1024 * 1024;

struct WarmupConfiguration {
    std::string path;
    unsigned mode{};
    bool device_bound = false;
};

thread_local WarmupConfiguration warmup_configuration;

bool CompleteLearnedPrewarm() {
    static const bool complete = [] {
        const auto* flag = std::getenv("CARA_METAL_PREWARM_ALL");
        return flag && std::string_view(flag) == "1";
    }();
    return complete;
}

bool WriteWord(std::ostream& out, std::uint32_t value) {
    const std::array<unsigned char, 4> bytes{
        static_cast<unsigned char>(value), static_cast<unsigned char>(value >> 8),
        static_cast<unsigned char>(value >> 16), static_cast<unsigned char>(value >> 24)};
    return bool(out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

bool ReadWord(std::istream& in, std::uint32_t& value) {
    std::array<unsigned char, 4> bytes{};
    if (!in.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
        return false;
    value = std::uint32_t(bytes[0]) | (std::uint32_t(bytes[1]) << 8) |
            (std::uint32_t(bytes[2]) << 16) | (std::uint32_t(bytes[3]) << 24);
    return true;
}

bool WriteSource(std::ostream& out, std::string_view source) {
    return !source.empty() && source.size() <= WarmupMaxSource &&
           WriteWord(out, static_cast<unsigned>(source.size())) &&
           bool(out.write(source.data(), source.size()));
}

bool ReadSource(std::istream& in, std::string& source) {
    std::uint32_t size{};
    if (!ReadWord(in, size) || !size || size > WarmupMaxSource)
        return false;
    source.resize(size);
    return bool(in.read(source.data(), source.size()));
}

constexpr std::string_view MslCacheMagic = "CaraRuntimeMSL1";
constexpr unsigned MslCacheMaxPairs = 256;
constexpr std::uintmax_t MslCacheMaxBytes = 40 * 1024 * 1024;

struct RuntimeMslDiskCache {
    std::filesystem::path path;
    std::array<char, 64> identity{};
    bool read = false, record = false, learn = false, opened = false;
    struct Pair {
        std::string vertex, fragment;
        unsigned mode{};
        RuntimeMetalShaderPair shaders;
    };
    std::vector<Pair> pairs;
    std::future<std::vector<Pair>> loading;
};

thread_local RuntimeMslDiskCache runtime_msl_disk;
// BeginWarmup joins its worker before the render lane can access the cache again.
thread_local RuntimeMslDiskCache* warmup_msl_cache = nullptr;

class ScopedWarmupMslCache {
public:
    explicit ScopedWarmupMslCache(RuntimeMslDiskCache* cache) noexcept {
        warmup_msl_cache = cache;
    }
    ~ScopedWarmupMslCache() { warmup_msl_cache = nullptr; }
    ScopedWarmupMslCache(const ScopedWarmupMslCache&) = delete;
    ScopedWarmupMslCache& operator=(const ScopedWarmupMslCache&) = delete;
};

bool ReadMslPair(std::istream& in, RuntimeMslDiskCache::Pair& pair) {
    auto& shader = pair.shaders;
    if (!ReadWord(in, pair.mode) ||
        (pair.mode > unsigned(PicaMetalVertexMode::StageIn) &&
         pair.mode != unsigned(PicaMetalVertexMode::PairIndexed)) ||
        !ReadSource(in, pair.vertex) || !ReadSource(in, pair.fragment) ||
        !ReadSource(in, shader.vertex) || !ReadSource(in, shader.fragment))
        return false;
    if (HasSimdVertexTag(pair.vertex) != ValidSimdVertexMsl(shader.vertex)) return false;
    if (HasSimdVertexTag(pair.vertex) &&
        (pair.mode != unsigned(PicaMetalVertexMode::Packed) || !RemoveVertexReference(pair.vertex))) return false;
    for (auto& unit : shader.units)
        if (!ReadWord(in, unit) || unit > 3)
            return false;
    const auto slot = [&](unsigned& value) {
        return ReadWord(in, value) && (value <= 3 || value == RuntimeMetalUnusedSlot);
    };
    for (auto& value : shader.slots)
        if (!slot(value))
            return false;
    return slot(shader.secondary_texture) && slot(shader.secondary_sampler) &&
           slot(shader.tertiary_texture) && slot(shader.tertiary_sampler);
}

std::vector<RuntimeMslDiskCache::Pair> LoadMslCache(
    const std::filesystem::path& path, const std::array<char, 64>& identity) {
    std::error_code error;
    const auto bytes = std::filesystem::file_size(path, error);
    if (error || bytes > MslCacheMaxBytes || bytes <= MslCacheMagic.size() + 128)
        return {};
    std::ifstream input(path, std::ios::binary);
    std::string file(bytes, '\0');
    if (!input.read(file.data(), file.size()) || input.peek() != std::char_traits<char>::eof())
        return {};
    const std::string_view all = file;
    const auto body = all.substr(64);
    const auto digest = SourceDigest(body);
    if (all.substr(0, 64) != std::string_view(digest.data(), digest.size()) ||
        !body.starts_with(MslCacheMagic) ||
        body.substr(MslCacheMagic.size(), 64) != std::string_view(identity.data(), 64))
        return {};
    std::istringstream in(std::string(body.substr(MslCacheMagic.size() + 64)),
                          std::ios::binary);
    unsigned count;
    if (!ReadWord(in, count) || !count || count > MslCacheMaxPairs)
        return {};
    std::vector<RuntimeMslDiskCache::Pair> pairs(count);
    for (auto& pair : pairs)
        if (!ReadMslPair(in, pair))
            return {};
    if (in.peek() != std::char_traits<char>::eof())
        return {};
    return pairs;
}

std::optional<RuntimeMetalShaderPair> ReadMslDisk(std::string_view vertex,
                                               std::string_view fragment, unsigned mode) {
    auto& cache = warmup_msl_cache ? *warmup_msl_cache : runtime_msl_disk;
    if (!cache.read)
        return std::nullopt;
    if (!cache.opened) {
        NativeMetalCost::Scope load(NativeMetalCost::RuntimeMslLoadHost);
        cache.opened = true;
        try {
            cache.pairs = cache.loading.valid() ? cache.loading.get()
                                               : LoadMslCache(cache.path, cache.identity);
        } catch (const std::exception& error) {
            LOG_WARNING(Render_OpenGL, "Unable to load the local MSL cache: {}", error.what());
        }
    }
    for (const auto& pair : cache.pairs)
        if (pair.mode == mode && pair.vertex == vertex && pair.fragment == fragment) {
            NativeMetalCost::Count(NativeMetalCost::RuntimeMslDiskHits, 1);
            return pair.shaders;
        }
    return std::nullopt;
}

bool SaveMslCache(const auto& pairs) {
    const auto& cache = runtime_msl_disk;
    struct View {
        std::string_view vertex, fragment;
        unsigned mode;
        const RuntimeMetalShaderPair* shaders;
        std::string_view msl_vertex, msl_fragment;
    };
    std::vector<View> selected;
    selected.reserve(MslCacheMaxPairs);
    std::size_t bytes = 64 + MslCacheMagic.size() + cache.identity.size() + sizeof(std::uint32_t);
    const auto add = [&](View view) {
        const auto& stages = *view.shaders;
        if (HasSimdVertexTag(view.vertex) != ValidSimdVertexMsl(view.msl_vertex)) return false;
        const std::array sources{view.vertex, view.fragment, view.msl_vertex, view.msl_fragment};
        std::size_t size = sizeof(std::uint32_t) *
            (1 + sources.size() + stages.units.size() + stages.slots.size() + 4);
        for (const auto source : sources) {
            if (source.empty() || source.size() > WarmupMaxSource)
                return false;
            size += source.size();
        }
        if (selected.size() == MslCacheMaxPairs || size > MslCacheMaxBytes - bytes)
            return false;
        const auto same = [&](const View& entry) {
            return entry.mode == view.mode && entry.vertex == view.vertex &&
                   entry.fragment == view.fragment;
        };
        if (std::none_of(selected.begin(), selected.end(), same)) {
            selected.push_back(view);
            bytes += size;
        }
        return true;
    };
    for (const auto& pair : pairs) {
        if (!add({pair.vertex_source, pair.fragment_source, unsigned(pair.mode), &pair.stages,
                  pair.msl_vertex,pair.msl_fragment}) &&
            !cache.learn)
            return false;
    }
    if (cache.learn) {
        // Keep previously learned entries when a session visits only part of the game.
        for (const auto& pair : cache.pairs)
            add({pair.vertex, pair.fragment, pair.mode, &pair.shaders,
                 pair.shaders.vertex,pair.shaders.fragment});
    }
    if (selected.empty())
        return false;
    std::ostringstream out(std::ios::binary);
    out.write(MslCacheMagic.data(), MslCacheMagic.size());
    out.write(cache.identity.data(), cache.identity.size());
    WriteWord(out, static_cast<unsigned>(selected.size()));
    for (const auto& pair : selected) {
        const auto& shaders = *pair.shaders;
        if (!WriteWord(out, pair.mode) || !WriteSource(out, pair.vertex) ||
            !WriteSource(out, pair.fragment) || !WriteSource(out, pair.msl_vertex) ||
            !WriteSource(out, pair.msl_fragment))
            return false;
        for (const auto unit : shaders.units) WriteWord(out, unit);
        for (const auto slot : shaders.slots) WriteWord(out, slot);
        WriteWord(out, shaders.secondary_texture);
        WriteWord(out, shaders.secondary_sampler);
        WriteWord(out, shaders.tertiary_texture);
        WriteWord(out, shaders.tertiary_sampler);
    }
    if (!out || out.tellp() > MslCacheMaxBytes - 64)
        return false;
    const auto body = std::move(out).str();
    const auto digest = SourceDigest(body);
    auto temp = cache.path;
    temp += std::string(".") + NSUUID.UUID.UUIDString.UTF8String;
    {
        std::ofstream file(temp, std::ios::binary);
        file.write(digest.data(), digest.size());
        file.write(body.data(), body.size());
        file.close();
        if (!file) {
            std::error_code ignored;
            std::filesystem::remove(temp, ignored);
            return false;
        }
    }
    std::error_code error;
    std::filesystem::rename(temp, cache.path, error);
    if (error) {
        std::error_code ignored;
        std::filesystem::remove(temp, ignored);
    }
    return !error;
}

std::optional<PicaSampleInputs> ReplaySampler(unsigned word, unsigned s, unsigned t) {
    PicaSampleInputs result{};
    result.min_filter = word & 0xffff;
    result.mag_filter = 0x2600 | ((word >> 16) & 1);
    result.wrap_s = s;
    result.wrap_t = t;
    result.min_lod = (word >> 17) & 7;
    result.max_lod = result.max_level = (word >> 20) & 7;
    result.opaque_black_border = (word >> 23) & 1;
    result.levels = result.max_level + 1;
    result.rgba8 = result.normalized = result.identity_swizzle = true;
    const auto key = GateSamplerKey(result);
    if (!key || key->color_filter != word)
        return std::nullopt;
    return result;
}

NSURL* ArchiveUrl(const std::string& path) {
    return [NSURL fileURLWithPath:[[NSString alloc] initWithUTF8String:path.c_str()]];
}
} // namespace

void ConfigureNativeMetalWarmup(const std::string& user, std::string_view rom_name) {
    runtime_msl_disk = {};
    if (const auto* value = std::getenv("CARA_METAL_RUNTIME_MSL_CACHE");
        value && (std::string_view(value) == "read" || std::string_view(value) == "record")) {
        auto directory = std::filesystem::path(user);
        if (directory.filename().empty())
            directory = directory.parent_path();
        runtime_msl_disk.path = directory.parent_path() / "cara-metal-runtime-msl-1119-97.cache";
        runtime_msl_disk.read = std::string_view(value) == "read";
        runtime_msl_disk.record = !runtime_msl_disk.read;
        const auto identity = Core::GetSaveStateBuildFingerprint() + std::string("|") +
            [[NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleVersion"] UTF8String] +
            "|" + NSProcessInfo.processInfo.operatingSystemVersionString.UTF8String + "|" +
            (UseMetalRawIndices() ? "raw" : "decoded") + "|" +
            (UseMetalRawIndexConstants() ? "constants" : "buffer");
        runtime_msl_disk.identity = SourceDigest(identity);
    }
    warmup_configuration = {};
    const auto* setting = std::getenv("CARA_METAL_WARMUP");
    if (!setting || (std::string_view(setting) != "record" && std::string_view(setting) != "replay"))
        return;
    const auto* hash = std::getenv("CARA_METAL_WARMUP_ROM_SHA256");
    const std::string_view digest = hash ? hash : "";
    if (digest.size() != 64 || digest.find_first_not_of("0123456789abcdef") != std::string_view::npos)
        return;
    const std::string identity = std::string(digest) + std::string(rom_name) +
        Core::GetSaveStateBuildFingerprint() +
        [[NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleVersion"] UTF8String] +
        NSProcessInfo.processInfo.operatingSystemVersionString.UTF8String +
        (UseMetalRawIndices() ? "raw" : "decoded") +
        (UseMetalRawIndexConstants() ? "constants" : "buffer") +
        (ReuseRenderPipelines() ? "reuse" : "plain");
    const auto name = SourceDigest(identity);
    auto directory = std::filesystem::path(user);
    if (directory.filename().empty())
        directory = directory.parent_path();
    warmup_configuration.path = (directory.parent_path() /
        ("cara-metal-warmup-" + std::string(name.data(), name.size()))).string();
    warmup_configuration.mode = std::string_view(setting) == "record" ? WarmupRecord : WarmupReplay;
}

bool ConfigureNativeMetalLearning(const std::string& user, std::uint64_t title,
                                  const std::array<std::uint8_t, 32>& game_hash, std::string_view version) {
    warmup_configuration = {};
    runtime_msl_disk = {};
    if (!title || std::none_of(game_hash.begin(), game_hash.end(), [](auto byte) { return byte; }))
        return false;
    NSString* bundle_version = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleVersion"];
    std::string identity = std::to_string(title) + ':' + std::string(version) + ':' +
        Core::GetSaveStateBuildFingerprint() + ':' + ([bundle_version UTF8String] ?: "") + ':' +
        (NSProcessInfo.processInfo.operatingSystemVersionString.UTF8String ?: "") + ':' +
        (UseMetalRawIndices() ? "raw" : "decoded") + ':' +
        (UseMetalRawIndexConstants() ? "constants" : "buffer") + ':' +
        (ReuseRenderPipelines() ? "reuse" : "plain") + ':';
    identity.append(reinterpret_cast<const char*>(game_hash.data()), game_hash.size());
    const auto name = SourceDigest(identity);
    auto directory = std::filesystem::path(user) / "metal-pipelines";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error)
        return false;
    warmup_configuration.path = (directory / std::string(name.data(), name.size())).string();
    warmup_configuration.mode = WarmupLearn;
    if (const auto* value = std::getenv("CARA_METAL_LEARN_MSL");
        !value || std::string_view(value) == "1") {
        // Reuse the title, content, backend and build identity of the learned archive.
        runtime_msl_disk.path = warmup_configuration.path + ".msl";
        runtime_msl_disk.identity = name;
        runtime_msl_disk.learn = runtime_msl_disk.read = true;
    }
    return true;
}

bool PicaMetalPipelines::BeginWarmup() {
    auto& msl = runtime_msl_disk;
    if (warmup_configuration.mode == WarmupLearn && !warmup_configuration.device_bound) {
        // Bind the cache to the device actually used by this renderer, rather
        // than assuming the system default device matches this pipeline owner.
        // The existing prefix already includes title/content/build/OS policy.
        std::string identity = std::string(device.name.UTF8String ?: "") + ':' +
            std::to_string(device.registryID) + ':';
#ifdef CARA_SHADER_CACHE_SIGNATURE
        identity += CARA_SHADER_CACHE_SIGNATURE;
#else
        identity += Core::GetSaveStateBuildFingerprint();
#endif
        const auto device_key = SourceDigest(identity);
        warmup_configuration.path += '-' + std::string(device_key.data(), device_key.size());
        warmup_configuration.device_bound = true;
        if (msl.learn) {
            msl.path = warmup_configuration.path + ".msl";
            const auto source_identity = std::string(msl.identity.data(), msl.identity.size()) +
                ':' + identity;
            msl.identity = SourceDigest(source_identity);
        }
        LOG_INFO(Render_OpenGL, "Metal learned cache device={} registry_id={} full_prewarm={}",
                 device.name.UTF8String ?: "", device.registryID, CompleteLearnedPrewarm());
    }
    if (!msl.learn && (msl.read || msl.record)) {
        const auto* background = std::getenv("CARA_METAL_ASYNC_MSL_LOAD");
        if (msl.read && background && std::string_view(background) == "1" &&
            !msl.opened && !msl.loading.valid()) {
            try {
                // The worker owns only the immutable file identity. The render
                // lane remains the exclusive owner of pipelines and guest state.
                msl.loading = std::async(std::launch::async, [path = msl.path, key = msl.identity] {
                    return LoadMslCache(path, key);
                });
            } catch (const std::exception& error) {
                LOG_WARNING(Render_OpenGL, "Unable to start MSL cache loader: {}", error.what());
            }
        }
        return true;
    }
    warmup_mode = warmup_configuration.mode;
    warmup_path = warmup_configuration.path;
    if (warmup_mode != WarmupReplay && warmup_mode != WarmupLearn)
        return warmup_mode == WarmupRecord;
    if (!entries.empty() || !runtime_pairs.empty())
        return false;
    const auto start = std::chrono::steady_clock::now();
    // The cache is still exclusive to this owner until the worker joins. No
    // UIKit, guest memory, queue submission, or draw occurs on this worker.
    bool loaded = false;
    try {
        loaded = std::async(std::launch::async, [this, cache = msl.learn ? &msl : nullptr] {
            @autoreleasepool {
                const ScopedWarmupMslCache use_cache(cache);
                return LoadWarmup();
            }
        }).get();
    } catch (const std::exception& failure) {
        LOG_ERROR(Render_OpenGL, "Metal warmup startup: {}", failure.what());
    }
    const auto ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    LOG_INFO(Render_OpenGL, "Metal warmup startup loaded={} prepared={} recipes={} archive_hits={} "
             "archive_misses={} full_prewarm={} wall_ms={}", loaded, warmup_prepared,
             entries.size(), warmup_archive_hits, warmup_archive_misses,
             CompleteLearnedPrewarm(), ms);
    warming = false;
    return warmup_mode == WarmupLearn || loaded;
}

bool PicaMetalPipelines::LoadWarmup() {
    if (@available(macOS 13.0, iOS 16.0, *)) {
        const auto path = warmup_path + ".recipes";
        std::error_code ec;
        const auto recipe_bytes = std::filesystem::file_size(path, ec);
        if (ec || recipe_bytes > WarmupMaxFile)
            return false;
        std::ifstream in(path, std::ios::binary);
        std::string magic(WarmupMagic.size(), '\0');
        std::uint32_t count{};
        if (!in.read(magic.data(), magic.size()) || magic != WarmupMagic || !ReadWord(in, count) ||
            count == 0 || count > WarmupMaxRecipes)
            return false;
        std::vector<WarmupRecipe> recipes;
        recipes.reserve(count);
        for (unsigned i = 0; i < count; ++i) {
            WarmupRecipe recipe;
            if (!ReadSource(in, recipe.vertex) || !ReadSource(in, recipe.fragment) ||
                !ReadWord(in, recipe.mode) ||
                (recipe.mode > unsigned(PicaMetalVertexMode::StageIn) &&
                 recipe.mode != unsigned(PicaMetalVertexMode::PairIndexed)))
                return false;
            for (auto& unit : recipe.units)
                if (!ReadWord(in, unit) || unit > NoMaterialUnit)
                    return false;
            for (auto& word : recipe.key)
                if (!ReadWord(in, word))
                    return false;
            recipes.push_back(std::move(recipe));
        }
        if (in.peek() != std::char_traits<char>::eof() || in.bad())
            return false;

        auto* descriptor = [MTLBinaryArchiveDescriptor new];
        descriptor.url = ArchiveUrl(warmup_path + ".metallib");
        NSError* error = nil;
        const auto archive_path = warmup_path + ".metallib";
        const auto archive_bytes = std::filesystem::file_size(archive_path, ec);
        if (warmup_mode != WarmupLearn || (!ec && archive_bytes <= LearnMaxArchiveBytes))
            warmup_archive = [device newBinaryArchiveWithDescriptor:descriptor error:&error];
        if (!warmup_archive)
            LOG_WARNING(Render_OpenGL, "Metal warmup binary archive unavailable: {}",
                        error.localizedDescription.UTF8String ?: "unknown error");
        if (warmup_mode == WarmupLearn) {
            warmup_recipes = std::move(recipes);
            warmup_recipe_bytes = recipe_bytes;
            if (!warmup_archive && !CompleteLearnedPrewarm())
                return false;
            const auto deadline = std::chrono::steady_clock::now() + LearnPreloadBudget;
            const bool complete = CompleteLearnedPrewarm();
            unsigned ready = 0;
            warming = true;
            for (const auto& recipe : warmup_recipes) {
                if (!complete && (warmup_prepared == LearnPreloadLimit ||
                    std::chrono::steady_clock::now() >= deadline))
                    break;
                ++warmup_prepared;
                if (PrepareRecipe(recipe))
                    ++ready;
                else
                    LOG_WARNING(Render_OpenGL, "Metal learned prewarm recipe {} unavailable; runtime fallback retained",
                                warmup_prepared);
            }
            LOG_INFO(Render_OpenGL, "Metal learned prewarm attempted={} ready={} unavailable={} total={} full={}",
                     warmup_prepared, ready, warmup_prepared - ready, warmup_recipes.size(), complete);
        } else {
            warming = true;
            for (const auto& recipe : recipes)
                if (!PrepareRecipe(recipe))
                    return false;
        }
        warming = false;
        return true;
    }
    return false;
}

bool PicaMetalPipelines::PrepareRecipe(const WarmupRecipe& recipe) {
    const auto& k = recipe.key;
    PicaPipelineInputs p{};
    p.vertex = recipe.vertex;
    p.fragment = recipe.fragment;
    p.vertex_mode = static_cast<PicaMetalVertexMode>(recipe.mode);
    p.packed_arrays = p.vertex_mode == PicaMetalVertexMode::Packed ||
                      p.vertex_mode == PicaMetalVertexMode::PairIndexed;
    p.sampled_unit = recipe.units[0];
    p.secondary_unit = recipe.units[1];
    p.tertiary_unit = recipe.units[2];
    if (GateMaterialUnits(p.vertex, p.fragment, p.vertex_mode) != recipe.units)
        return false;
    p.clip0 = true;
    p.clip1 = p.vertex.starts_with(RuntimeMetalUserClip);
    p.dither = false;
    p.winding = 0x900;
    p.blend = k[1];
    p.color_mask = k[2];
    p.rgb_op = k[3]; p.alpha_op = k[4];
    p.rgb_src = k[5]; p.rgb_dst = k[6];
    p.alpha_src = k[7]; p.alpha_dst = k[8];
    p.color_format = static_cast<MTLPixelFormat>(k[9]);
    p.depth_test = k[10] != 0x207 || k[11];
    p.depth_compare = k[10];
    p.depth_write = k[11];
    p.stencil = k[12];
    p.stencil_compare = k[13]; p.stencil_read = k[14]; p.stencil_write = k[15];
    p.stencil_fail = k[16]; p.stencil_depth_fail = k[17]; p.stencil_pass = k[18];
    const auto read_sampler = [&](unsigned index) -> std::optional<PicaSampleInputs> {
        const auto offset = 19 + 3 * index;
        return recipe.units[index] == NoMaterialUnit ? std::nullopt :
            ReplaySampler(k[offset], k[offset + 1], k[offset + 2]);
    };
    const auto first = read_sampler(0);
    const auto second = read_sampler(1);
    const auto third = read_sampler(2);
    const auto sampler = first.value_or(PicaSampleInputs{});
    const auto key = MakeKey(p, sampler, second, third);
    if (!key || !std::equal(key->begin() + 1, key->end(), k.begin() + 1))
        return false;
    return Prepare(p, sampler, second, third);
}

void PicaMetalPipelines::RememberLearnedPipeline(const PicaPipelineInputs& p, const Key& key,
                                                MTLRenderPipelineDescriptor* descriptor) {
    if ((warming && !CompleteLearnedPrewarm()) ||
        p.vertex.empty() || p.fragment.empty() ||
        p.vertex.size() > WarmupMaxSource || p.fragment.size() > WarmupMaxSource)
        return;
    const std::array units{p.sampled_unit, p.secondary_unit, p.tertiary_unit};
    const auto same_recipe = [&](const WarmupRecipe& recipe) {
        // The first word is an in-process program index. It can change on relaunch.
        return recipe.mode == unsigned(p.vertex_mode) && recipe.units == units &&
            recipe.vertex == p.vertex && recipe.fragment == p.fragment &&
            std::equal(recipe.key.begin() + 1, recipe.key.end(), key.begin() + 1);
    };
    // The caller supplies a descriptor only when this pipeline missed the
    // archive. Recipe coverage has its own larger budget; exhausting this
    // session's archive-addition budget must not hide later observed recipes.
    const auto remember_descriptor = [&] {
        if (descriptor && warmup_descriptors.size() < LearnMaxDescriptors)
            warmup_descriptors.push_back([descriptor copy]);
    };
    if (std::any_of(warmup_recipes.begin(), warmup_recipes.end(), same_recipe)) {
        remember_descriptor();
        return;
    }
    if (!warmup_recipe_bytes)
        warmup_recipe_bytes = WarmupMagic.size() + sizeof(std::uint32_t);
    const auto bytes = p.vertex.size() + p.fragment.size() +
        (3 + units.size() + key.size()) * sizeof(std::uint32_t);
    if (warmup_recipes.size() == WarmupMaxRecipes || bytes > WarmupMaxFile - warmup_recipe_bytes)
        return;
    warmup_recipes.push_back({std::string(p.vertex), std::string(p.fragment), key, units,
                              unsigned(p.vertex_mode)});
    warmup_recipe_bytes += bytes;
    remember_descriptor();
    ++warmup_new_recipes;
}

bool PicaMetalPipelines::FinishWarmup() {
    if (runtime_msl_disk.record)
        return SaveMslCache(runtime_pairs);
    if (!warmup_mode)
        return true;
    if (warmup_mode == WarmupReplay) {
        LOG_INFO(Render_OpenGL, "Metal warmup replay draw_misses={} runtime_translation_misses={}",
                 warmup_draw_misses, warmup_translation_misses);
        return !warmup_draw_misses && !warmup_translation_misses;
    }
    if (warmup_mode == WarmupLearn) {
        auto& cache = runtime_msl_disk;
        // A failed read or write affects startup performance only.
        if (cache.learn && !runtime_pairs.empty()) {
            const bool all_known = std::all_of(runtime_pairs.begin(), runtime_pairs.end(),
                [&](const auto& pair) {
                    return std::any_of(cache.pairs.begin(), cache.pairs.end(), [&](const auto& old) {
                        return old.mode == unsigned(pair.mode) &&
                            old.vertex == pair.vertex_source && old.fragment == pair.fragment_source;
                    });
                });
            if (!all_known) {
                try {
                    const bool saved = SaveMslCache(runtime_pairs);
                    LOG_INFO(Render_OpenGL, "Metal learned MSL save pairs={} saved={}",
                             runtime_pairs.size(), saved);
                } catch (const std::exception& error) {
                    LOG_WARNING(Render_OpenGL, "Unable to save learned MSL: {}", error.what());
                }
            }
        }
        // Local diagnostic counts only. Source and binary archives stay in the app container.
        LOG_INFO(Render_OpenGL, "Metal pipeline learning preload_attempts={} archive_hits={} "
                 "archive_misses={} draw_misses={} learned={} recipes_total={} archive_additions={}",
                 warmup_prepared, warmup_archive_hits, warmup_archive_misses,
                 warmup_draw_misses, warmup_new_recipes, warmup_recipes.size(),
                 warmup_descriptors.size());
        if (warmup_descriptors.empty() && !warmup_new_recipes)
            return true;
        try {
            const bool saved = std::async(std::launch::async, [this] {
                @autoreleasepool {
                    return SaveWarmup();
                }
            }).get();
            if (!saved)
                LOG_WARNING(Render_OpenGL,
                            "Metal learned cache persistence incomplete; rendering and runtime compilation remain available");
        } catch (const std::exception& failure) {
            LOG_WARNING(Render_OpenGL, "Metal pipeline cache save: {}", failure.what());
        }
        // Learning is an optional startup-performance cache. Its failure must
        // never turn successfully rendered guest work into a frontend failure.
        return true;
    }
    if (warmup_recording_failed || warmup_recipes.empty())
        return false;
    if (@available(macOS 13.0, iOS 16.0, *)) {
        const bool saved = std::async(std::launch::async, [this] {
            @autoreleasepool {
                return SaveWarmup();
            }
        }).get();
        LOG_INFO(Render_OpenGL, "Metal warmup export recipes={} binary_descriptors={} saved={}",
                 warmup_recipes.size(), warmup_descriptors.size(), saved);
        return saved;
    }
    return false;
}

bool PicaMetalPipelines::SaveWarmup() {
    const auto recipe_path = warmup_path + ".recipes";
    const auto archive_path = warmup_path + ".metallib";
    const bool learning = warmup_mode == WarmupLearn;
    if (!learning && std::filesystem::exists(recipe_path))
        return false;
    const auto suffix = std::string(NSUUID.UUID.UUIDString.UTF8String);
    const auto recipe_temp = recipe_path + "." + suffix;
    const auto archive_temp = archive_path + "." + suffix;
    const auto write = [&] {
        std::ofstream out(recipe_temp, std::ios::binary);
        if (!out.write(WarmupMagic.data(), WarmupMagic.size()) ||
            !WriteWord(out, static_cast<unsigned>(warmup_recipes.size())))
            return false;
        for (const auto& recipe : warmup_recipes) {
            if (!WriteSource(out, recipe.vertex) || !WriteSource(out, recipe.fragment) ||
                !WriteWord(out, recipe.mode))
                return false;
            for (const auto unit : recipe.units)
                if (!WriteWord(out, unit))
                    return false;
            for (const auto word : recipe.key)
                if (!WriteWord(out, word))
                    return false;
        }
        if (out.tellp() < 0 || out.tellp() > WarmupMaxFile)
            return false;
        out.close();
        return bool(out);
    };
    bool success = write();
    if (success) {
        NSError* error = nil;
        auto* config = [MTLBinaryArchiveDescriptor new];
        const auto archive = learning && warmup_archive ? warmup_archive :
            [device newBinaryArchiveWithDescriptor:config error:&error];
        bool archived = archive != nil;
        for (auto* descriptor : warmup_descriptors) {
            if (!archived || ![archive addRenderPipelineFunctionsWithDescriptor:descriptor error:&error]) {
                archived = false;
                break;
            }
        }
        // A new sampler/recipe can reuse an already archived PSO. Publish that
        // recipe without requiring a new archive descriptor or rewriting the
        // unchanged archive. Archive hits never consume the addition budget.
        const bool unchanged_archive = learning && warmup_archive && warmup_descriptors.empty();
        archived = archived && (unchanged_archive || (warmup_descriptors.size() &&
                   [archive serializeToURL:ArchiveUrl(archive_temp) error:&error]));
        std::error_code ec;
        if (archived && learning && !unchanged_archive) {
            const auto bytes = std::filesystem::file_size(archive_temp, ec);
            archived = !ec && bytes <= LearnMaxArchiveBytes;
        }
        if (archived && !unchanged_archive) {
            std::filesystem::rename(archive_temp, archive_path, ec);
            archived = !ec;
        }
        LOG_INFO(Render_OpenGL, "Metal warmup binary archive saved={} detail={}", archived,
                 error.localizedDescription.UTF8String ?: "");
        success = !learning || archived;
        if (success || learning) {
            // Known recipes remain useful to full prewarm even when Apple's
            // binary serializer cannot represent a pipeline. Publish only the
            // completely written recipe file; leave the old archive untouched
            // unless a complete replacement was serialized and renamed above.
            ec.clear();
            std::filesystem::rename(recipe_temp, recipe_path, ec);
            const bool recipes_saved = !ec;
            LOG_INFO(Render_OpenGL, "Metal warmup recipes saved={} archive_saved={} learning={}",
                     recipes_saved, archived, learning);
            success = success && recipes_saved;
        }
    }
    std::error_code ignored;
    std::filesystem::remove(recipe_temp, ignored);
    std::filesystem::remove(archive_temp, ignored);
    return success;
}
