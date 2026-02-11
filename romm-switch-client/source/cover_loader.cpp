#include "romm/cover_loader.hpp"
#include "stb_image.h"

namespace romm {

CoverLoader::CoverLoader() = default;
CoverLoader::~CoverLoader() { stop(); }

void CoverLoader::start(FetchFn fetchFn) {
    fetch_ = fetchFn;
    // Small coalesce window keeps latest-wins behavior for rapid selection changes.
    worker_.start([this](const CoverJob& job) { return runJob(job); }, 10);
}

void CoverLoader::stop() {
    worker_.stop();
}

void CoverLoader::request(const CoverJob& job, const std::string& currentTextureUrl) {
    if (job.url.empty()) return;
    // Dedup: avoid requesting if it matches current texture URL, pending job URL, or active job URL.
    if (!currentTextureUrl.empty() && currentTextureUrl == job.url) return;
    if (auto pending = worker_.pendingJob(); pending && pending->url == job.url) return;
    if (auto active = worker_.activeJob(); active && active->url == job.url) return;
    worker_.submit(job);
}

std::optional<CoverResult> CoverLoader::poll() {
    return worker_.pollResult();
}

CoverResult CoverLoader::runJob(const CoverJob& job) {
    CoverResult res;
    res.url = job.url;
    res.title = job.title;
    if (!fetch_) {
        res.ok = false;
        res.error = "no fetch function";
        return res;
    }

    std::vector<unsigned char> data;
    std::string err;
    if (fetch_(job.url, job.cfg, data, err)) {
#ifdef UNIT_TEST
        // In tests, skip real image decoding; treat payload as raw RGBA if present.
        res.ok = true;
        res.w = 1;
        res.h = 1;
        if (data.size() >= 4) {
            res.pixels.assign(data.begin(), data.begin() + 4);
        } else {
            res.pixels.assign({0xFF, 0, 0, 0xFF}); // opaque red fallback
        }
#else
        int w = 0, h = 0, channels = 0;
        unsigned char* pixels = stbi_load_from_memory(
            reinterpret_cast<const unsigned char*>(data.data()),
            static_cast<int>(data.size()), &w, &h, &channels, STBI_rgb_alpha);
        if (pixels) {
            res.ok = true;
            res.w = w;
            res.h = h;
            res.pixels.assign(pixels, pixels + (w * h * 4));
            stbi_image_free(pixels);
        } else {
            res.ok = false;
            res.error = "decode failed";
        }
#endif
    } else {
        res.ok = false;
        res.error = err.empty() ? "fetch failed" : err;
    }

    return res;
}

} // namespace romm
