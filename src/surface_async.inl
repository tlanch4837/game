// Surface maps made off the main thread, for scene loads and streaming alike. Materials draw with the neutral default until their
// maps arrive; useMaterial in RenderFrame asks Lookup for what to bind, so meshes never need rebuilding. Results are cached on disk
// by content, next to the executable. Each job holds full-size images and mip chains (tens of MB), so at most kMaxHeld are made or
// waiting for the GPU at once; the rest wait as requests, which hold only the files' compressed bytes. Finished maps upload for up
// to kPumpMs a frame, so a slow loading frame cannot let them pile up.
namespace surface_async {

struct Entry { tbd::TextureId surface = tbd::kNoTexture, normal = tbd::kNoTexture, extra = tbd::kNoTexture; bool ownsNormal = false; };
struct Done {
    std::pair<tbd::TextureId, tbd::TextureId> key;
    tbd::surface_maps::Result result;
    std::shared_ptr<tbd::MipChain> surface, normal, extra;
};
constexpr int kMaxHeld = 6;
constexpr double kPumpMs = 1.5; // uploads are cheap now (a ring copy), so a little a frame keeps up
struct Shared {
    std::mutex mutex;
    std::vector<Done> done;
    std::deque<std::function<void()>> waiting; // main thread only
    std::atomic<int> held{0};                  // running, or done and not yet uploaded
    std::map<std::tuple<uint64_t, uint64_t, uint64_t, bool>, int> unused;
};
inline Shared& Queue() { static Shared s; return s; }
inline std::map<std::pair<tbd::TextureId, tbd::TextureId>, Entry>& Table() { static std::map<std::pair<tbd::TextureId, tbd::TextureId>, Entry> t; return t; }

inline std::wstring CacheDir() {
    static const std::wstring cached = [] { // once: it was a module query and a CreateDirectory on every request
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring dir = exe;
        dir = dir.substr(0, dir.find_last_of(L"\\/")) + L"\\surfacecache";
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir;
    }();
    return cached;
}

inline tbd::surface_maps::Result Derive(const std::wstring& dir, const std::vector<uint8_t>& albedo, const std::vector<uint8_t>& normal, const std::vector<uint8_t>& rough) {
    tbd::surface_maps::Result out;
    wchar_t name[96];
    swprintf(name, 96, L"\\%016llx%016llx%016llx.bin", (unsigned long long)ContentHash(albedo), (unsigned long long)ContentHash(normal), (unsigned long long)ContentHash(rough));
    const std::wstring file = dir + name;
    if (FILE* f = _wfopen(file.c_str(), L"rb")) {
        int head[2] = {};
        bool ok = false;
        if (fread(head, sizeof(int), 2, f) == 2 && head[0] >= 0 && head[0] <= 512) {
            const size_t bytes = (size_t)head[0] * head[0] * 4;
            out.size = head[0];
            out.surface.resize(bytes);
            if (head[1]) out.normal.resize(bytes);
            ok = fread(out.surface.data(), 1, bytes, f) == bytes && (!head[1] || fread(out.normal.data(), 1, bytes, f) == bytes);
        }
        fclose(f);
        if (ok) return out;
        out = {};
    }
    std::vector<uint8_t> a, n, r;
    int aw = 0, ah = 0, nw = 0, nh = 0, rw = 0, rh = 0;
    if (!tbd::DecodeImage(albedo.data(), albedo.size(), a, aw, ah)) return out;
    bool cutout = false;
    for (size_t i = 3; i < a.size() && !cutout; i += 4) cutout = a[i] < 200;
    if (!cutout) {
        const bool hasN = !normal.empty() && tbd::DecodeImage(normal.data(), normal.size(), n, nw, nh);
        const bool hasR = !rough.empty() && tbd::DecodeImage(rough.data(), rough.size(), r, rw, rh);
        out = tbd::surface_maps::Make(a.data(), aw, ah, hasN ? n.data() : nullptr, nw, nh, hasR ? r.data() : nullptr, rw, rh);
    }
    if (FILE* f = _wfopen(file.c_str(), L"wb")) { // size 0 remembers a cut-out
        const int head[2] = {out.size, out.normal.empty() ? 0 : 1};
        fwrite(head, sizeof(int), 2, f);
        fwrite(out.surface.data(), 1, out.surface.size(), f);
        if (head[1]) fwrite(out.normal.data(), 1, out.normal.size(), f);
        fclose(f);
    }
    return out;
}

// The extra map (metalness, baked occlusion, emissive) from the glTF's own images, cached on disk like the rest. Empty when they say nothing.
inline std::vector<uint8_t> DeriveExtra(const std::wstring& dir, const tbd::surface_maps::ExtraInputs& in, int& size) {
    static const std::vector<uint8_t> none;
    const auto bytes = [&](const std::shared_ptr<std::vector<uint8_t>>& b) -> const std::vector<uint8_t>& { return b ? *b : none; };
    unsigned long long f = 0;
    for (float v : {in.emissiveFactor[0], in.emissiveFactor[1], in.emissiveFactor[2], in.metallic}) f = f * 1099511628211ull + (unsigned long long)std::lround(v * 1000.0f);
    wchar_t name[112];
    swprintf(name, 112, L"\\x%016llx%016llx%016llx%016llx.bin", (unsigned long long)ContentHash(bytes(in.rough)), (unsigned long long)ContentHash(bytes(in.occlusion)), (unsigned long long)ContentHash(bytes(in.emissive)), f);
    const std::wstring file = dir + name;
    if (FILE* fp = _wfopen(file.c_str(), L"rb")) {
        int n = 0;
        std::vector<uint8_t> out;
        bool ok = fread(&n, sizeof(int), 1, fp) == 1 && n >= 0 && n <= 512;
        if (ok && n) { out.resize((size_t)n * n * 4); ok = fread(out.data(), 1, out.size(), fp) == out.size(); }
        fclose(fp);
        if (ok) { size = n; return out; }
    }
    tbd::surface_maps::DecodedImage r, o, e;
    const auto decode = [&](const std::shared_ptr<std::vector<uint8_t>>& b, tbd::surface_maps::DecodedImage& d) {
        return b && !b->empty() && tbd::DecodeImage(b->data(), b->size(), d.rgba, d.w, d.h);
    };
    const bool hr = decode(in.rough, r), ho = decode(in.occlusion, o), he = decode(in.emissive, e);
    std::vector<uint8_t> out = tbd::surface_maps::MakeExtra(hr ? &r : nullptr, ho ? &o : nullptr, he ? &e : nullptr, in.emissiveFactor, in.metallic, size);
    if (out.empty()) size = 0;
    if (FILE* fp = _wfopen(file.c_str(), L"wb")) { // size 0 remembers "says nothing"
        fwrite(&size, sizeof(int), 1, fp);
        fwrite(out.data(), 1, out.size(), fp);
        fclose(fp);
    }
    return out;
}

using Bytes = std::shared_ptr<const std::vector<uint8_t>>;
inline void Request(tbd::TextureId albedoId, tbd::TextureId normalId, Bytes a, Bytes n, Bytes r, bool art, const tbd::surface_maps::ExtraInputs& extraIn);

// Main thread. The image bytes are copied, so the model may go away.
inline void Request(tbd::TextureId albedoId, tbd::TextureId normalId, const std::vector<uint8_t>& albedo, const std::vector<uint8_t>* normal, const std::vector<uint8_t>* rough, bool art, const tbd::surface_maps::ExtraInputs& extraIn = {}) {
    if (albedoId == tbd::kNoTexture || !g_engine.jobs || !g_engine.renderer) return;
    if (Table().count(std::make_pair(albedoId, normalId))) return;
    Request(albedoId, normalId, std::make_shared<const std::vector<uint8_t>>(albedo), std::make_shared<const std::vector<uint8_t>>(normal ? *normal : std::vector<uint8_t>()),
            std::make_shared<const std::vector<uint8_t>>(rough ? *rough : std::vector<uint8_t>()), art, extraIn);
}

// Main thread. Shares the bytes (streaming already holds them in shared buffers), so nothing is copied here. Null: none.
inline void Request(tbd::TextureId albedoId, tbd::TextureId normalId, Bytes a, Bytes n, Bytes r, bool art, const tbd::surface_maps::ExtraInputs& extraIn) {
    if (albedoId == tbd::kNoTexture || !g_engine.jobs || !g_engine.renderer || !a) return;
    static const Bytes none = std::make_shared<const std::vector<uint8_t>>();
    if (!n) n = none;
    if (!r) r = none;
    const auto key = std::make_pair(albedoId, normalId);
    if (Table().count(key)) return;
    Table()[key] = {};
    if (art) { // signs and lettering: a one-texel map that tells the shader to leave them alone
        // No-wait: the waiting CreateTexture drains the whole GPU queue, and a town publishes hundreds of these.
        static const std::vector<std::vector<uint8_t>> texel = {{128, 255, 128, 255}};
        static const std::vector<std::pair<int, int>> dims = {{1, 1}};
        Table()[key].surface = g_engine.renderer->CreateTextureNoWait(texel, dims);
        return;
    }
    const std::wstring dir = CacheDir();
    Queue().waiting.push_back([key, a, n, r, dir, extraIn] { g_engine.jobs->RunBackground([key, a, n, r, dir, extraIn] {
        Done d;
        d.key = key;
        try {
            d.result = Derive(dir, *a, *n, *r);
            if (d.result.size) {
                d.surface = std::make_shared<tbd::MipChain>(tbd::BuildMipChain(d.result.surface.data(), d.result.size, d.result.size));
                if (!d.result.normal.empty()) d.normal = std::make_shared<tbd::MipChain>(tbd::BuildMipChain(d.result.normal.data(), d.result.size, d.result.size));
                int extraSize = 0;
                if (extraIn.Any()) {
                    const std::vector<uint8_t> extra = DeriveExtra(dir, extraIn, extraSize);
                    if (extraSize) d.extra = std::make_shared<tbd::MipChain>(tbd::BuildMipChain(extra.data(), extraSize, extraSize));
                }
            }
        } catch (...) { d.surface.reset(); d.normal.reset(); d.extra.reset(); }
        d.result = {};
        std::lock_guard<std::mutex> lock(Queue().mutex);
        Queue().done.push_back(std::move(d));
    }); });
}

inline void UploadOne(Done& d);

inline void Pump() { // once per frame
    while (Queue().held.load() < kMaxHeld && !Queue().waiting.empty()) {
        ++Queue().held;
        std::function<void()> start = std::move(Queue().waiting.front());
        Queue().waiting.pop_front();
        start();
    }
    LARGE_INTEGER t0, now, freq;
    QueryPerformanceCounter(&t0);
    QueryPerformanceFrequency(&freq);
    for (;;) {
        Done d;
        {
            std::lock_guard<std::mutex> lock(Queue().mutex);
            if (Queue().done.empty()) return;
            d = std::move(Queue().done.front()); // oldest first
            Queue().done.erase(Queue().done.begin());
        }
        UploadOne(d);
        --Queue().held;
        QueryPerformanceCounter(&now);
        if ((now.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart >= kPumpMs) return;
    }
}

inline void UploadOne(Done& d) {
    const auto it = Table().find(d.key);
    if (it == Table().end() || !d.surface || !g_engine.renderer) return;
    it->second.surface = g_engine.renderer->CreateTextureNoWait(d.surface->levels, d.surface->dims);
    if (d.normal) { it->second.normal = g_engine.renderer->CreateTextureNoWait(d.normal->levels, d.normal->dims); it->second.ownsNormal = true; }
    if (d.extra) {
        it->second.extra = g_engine.renderer->CreateTextureNoWait(d.extra->levels, d.extra->dims);
        g_engine.renderer->SetSurfaceExtra(it->second.surface, it->second.extra);
    }
}

// A streamed texture was destroyed: its id may be reused, so what was made for it goes too.
inline void Forget(tbd::TextureId id) {
    for (auto it = Table().begin(); it != Table().end();) {
        if (it->first.first != id && it->first.second != id) { ++it; continue; }
        if (g_engine.renderer) {
            if (it->second.surface != tbd::kNoTexture) g_engine.renderer->DestroyTexture(it->second.surface);
            if (it->second.ownsNormal && it->second.normal != tbd::kNoTexture) g_engine.renderer->DestroyTexture(it->second.normal);
            if (it->second.extra != tbd::kNoTexture) g_engine.renderer->DestroyTexture(it->second.extra);
        }
        it = Table().erase(it);
    }
}
inline void Reset() { // renderer changed; jobs still running finish into `done` and are dropped by the next Reset or upload
    Table().clear();
    Queue().waiting.clear();
    std::lock_guard<std::mutex> lock(Queue().mutex);
    Queue().held -= (int)Queue().done.size();
    Queue().done.clear();
}

inline void Apply(tbd::TextureId albedo, tbd::TextureId& normal, tbd::TextureId& surface) {
    if (surface != tbd::kNoTexture) return; // skin keeps its own
    const auto it = Table().find({albedo, normal});
    if (it == Table().end()) return;
    surface = it->second.surface;
    if (normal == tbd::kNoTexture) normal = it->second.normal;
}

} // namespace surface_async
