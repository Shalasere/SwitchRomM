#include <switch.h>
#if __has_include(<switch/nxlink.h>)
    #include <switch/nxlink.h>
    #define HAS_NXLINK 1
#else
    #define HAS_NXLINK 0
#endif
#include <SDL2/SDL.h>
#include <switch/services/applet.h>
#include <string>
#include <vector>
#include <array>
#include <sstream>
#include <algorithm>
#include <cstdio>
#include "stb_image.h"
// Fallback declarations in case the minimal stb header wasn't visible for some reason
extern "C" unsigned char *stbi_load_from_memory(const unsigned char *buffer, int len, int *x, int *y, int *channels_in_file, int desired_channels);
extern "C" void stbi_image_free(void *retval_from_stbi_load);
#ifndef STBI_rgb_alpha
#define STBI_rgb_alpha 4
#endif

#include "romm/config.hpp"
#include "romm/status.hpp"
#include "romm/api.hpp"
#include "romm/filesystem.hpp"
#include "romm/input.hpp"
#include "romm/logger.hpp"
#include "romm/downloader.hpp"

using romm::Status;
using romm::Config;

static Status::View gLastLoggedView = Status::View::ERROR;
static int gRomsDebugFrames = 0;
static int gFrameCounter = 0;
static int gViewTraceFrames = 0; // log render view for a few frames after navigation
static SDL_Texture* gCoverTexture = nullptr;

// Load cover art for the given game (blocking). Replaces the global cover texture.
static void loadCoverTextureForGame(const romm::Game& g, SDL_Renderer* renderer, const Config& config) {
    if (gCoverTexture) {
        SDL_DestroyTexture(gCoverTexture);
        gCoverTexture = nullptr;
    }
    if (!renderer) return;
    if (g.coverUrl.empty()) return;

    std::string data;
    std::string err;
    if (!romm::fetchBinary(config, g.coverUrl, data, err)) {
        romm::logLine("Cover fetch failed for " + g.title + ": " + err);
        return;
    }
    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load_from_memory(reinterpret_cast<const unsigned char*>(data.data()),
                                                  static_cast<int>(data.size()),
                                                  &w, &h, &channels, STBI_rgb_alpha);
    if (!pixels) {
        romm::logLine("Cover decode failed for " + g.title);
        return;
    }
    SDL_Texture* tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
    if (!tex) {
        romm::logLine(std::string("Cover texture create failed: ") + SDL_GetError());
        stbi_image_free(pixels);
        return;
    }
    if (SDL_UpdateTexture(tex, nullptr, pixels, w * 4) != 0) {
        romm::logLine(std::string("Cover texture upload failed: ") + SDL_GetError());
        SDL_DestroyTexture(tex);
        stbi_image_free(pixels);
        return;
    }
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    gCoverTexture = tex;
    stbi_image_free(pixels);
    romm::logLine("Loaded cover for " + g.title + " (" + std::to_string(w) + "x" + std::to_string(h) + ")");
}

static const char* viewName(Status::View v) {
    switch (v) {
        case Status::View::PLATFORMS: return "PLATFORMS";
        case Status::View::ROMS: return "ROMS";
        case Status::View::DETAIL: return "DETAIL";
        case Status::View::QUEUE: return "QUEUE";
        case Status::View::DOWNLOADING: return "DOWNLOADING";
        case Status::View::ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}
struct ScrollHold {
    int dir{0}; // -1 up, 1 down
    Uint32 nextMs{0};
    int repeats{0};
};

struct Glyph {
    uint8_t rows[7];
};

// 5x7 uppercase/digits/space. Each byte uses lower 5 bits for pixels.
static const Glyph kFont[37] = {
    /* Space */ {{0,0,0,0,0,0,0}},
    /* 0 */ {{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}},
    /* 1 */ {{0x04,0x0C,0x14,0x04,0x04,0x04,0x1F}},
    /* 2 */ {{0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}},
    /* 3 */ {{0x0E,0x11,0x01,0x06,0x01,0x11,0x0E}},
    /* 4 */ {{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}},
    /* 5 */ {{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}},
    /* 6 */ {{0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}},
    /* 7 */ {{0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},
    /* 8 */ {{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}},
    /* 9 */ {{0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}},
    /* A */ {{0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}},
    /* B */ {{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}},
    /* C */ {{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}},
    /* D */ {{0x1C,0x12,0x11,0x11,0x11,0x12,0x1C}},
    /* E */ {{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}},
    /* F */ {{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}},
    /* G */ {{0x0E,0x11,0x10,0x17,0x11,0x11,0x0E}},
    /* H */ {{0x11,0x11,0x11,0x1F,0x11,0x11,0x11}},
    /* I */ {{0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}},
    /* J */ {{0x07,0x02,0x02,0x02,0x12,0x12,0x0C}},
    /* K */ {{0x11,0x12,0x14,0x18,0x14,0x12,0x11}},
    /* L */ {{0x10,0x10,0x10,0x10,0x10,0x10,0x1F}},
    /* M */ {{0x11,0x1B,0x15,0x15,0x11,0x11,0x11}},
    /* N */ {{0x11,0x19,0x15,0x13,0x11,0x11,0x11}},
    /* O */ {{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}},
    /* P */ {{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},
    /* Q */ {{0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}},
    /* R */ {{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}},
    /* S */ {{0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}},
    /* T */ {{0x1F,0x04,0x04,0x04,0x04,0x04,0x04}},
    /* U */ {{0x11,0x11,0x11,0x11,0x11,0x11,0x0E}},
    /* V */ {{0x11,0x11,0x11,0x11,0x11,0x0A,0x04}},
    /* W */ {{0x11,0x11,0x11,0x15,0x15,0x1B,0x11}},
    /* X */ {{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}},
    /* Y */ {{0x11,0x11,0x0A,0x04,0x04,0x04,0x04}},
    /* Z */ {{0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}},
};

// Punctuation glyphs (5x7)
static const Glyph kEqGlyph       = {{0x00,0x00,0x0E,0x00,0x0E,0x00,0x00}};
static const Glyph kDotGlyph      = {{0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}};
static const Glyph kColonGlyph    = {{0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00}};
static const Glyph kPercentGlyph  = {{0x18,0x19,0x02,0x04,0x08,0x13,0x03}}; // from HD44780
static const Glyph kBangGlyph     = {{0x04,0x04,0x04,0x04,0x00,0x00,0x04}};
static const Glyph kQuestionGlyph = {{0x0E,0x11,0x01,0x02,0x04,0x00,0x04}};
static const Glyph kSlashGlyph    = {{0x00,0x01,0x02,0x04,0x08,0x10,0x00}};
static const Glyph kPlusGlyph     = {{0x00,0x04,0x04,0x1F,0x04,0x04,0x00}};
static const Glyph kMinusGlyph    = {{0x00,0x00,0x00,0x1F,0x00,0x00,0x00}};
static const Glyph kStarGlyph     = {{0x00,0x04,0x15,0x0E,0x15,0x04,0x00}};
static const Glyph kHashGlyph     = {{0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A}};
static const Glyph kDollarGlyph   = {{0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04}};
static const Glyph kLParenGlyph   = {{0x02,0x04,0x08,0x08,0x08,0x04,0x02}};
static const Glyph kRParenGlyph   = {{0x08,0x04,0x02,0x02,0x02,0x04,0x08}};
static const Glyph kCommaGlyph    = {{0x00,0x00,0x00,0x00,0x0C,0x04,0x08}};
// Latin capital/lowercase O with macron (Ō/ō) for Okami label
static const Glyph kOMacron       = {{0x1F,0x0E,0x11,0x11,0x11,0x11,0x0E}};
static const Glyph komacron       = {{0x1F,0x0E,0x11,0x11,0x11,0x11,0x0E}}; // same 5x7 shape at this size
static std::array<Glyph, 192> gHdFont{};
static bool gHdFontLoaded = false;
static size_t gHdFontCount = 0;

// Decode a single UTF-8 codepoint; advances index. Returns false at end-of-string.
static bool decodeUtf8(const std::string& s, size_t& i, uint32_t& cp) {
    if (i >= s.size()) return false;
    unsigned char b = static_cast<unsigned char>(s[i]);
    if (b < 0x80) {
        cp = b;
        i += 1;
        return true;
    } else if ((b & 0xE0) == 0xC0 && i + 1 < s.size()) {
        cp = ((b & 0x1F) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3F);
        i += 2;
        return true;
    } else if ((b & 0xF0) == 0xE0 && i + 2 < s.size()) {
        cp = ((b & 0x0F) << 12) |
             ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 6) |
             (static_cast<unsigned char>(s[i + 2]) & 0x3F);
        i += 3;
        return true;
    } else if ((b & 0xF8) == 0xF0 && i + 3 < s.size()) {
        cp = ((b & 0x07) << 18) |
             ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 12) |
             ((static_cast<unsigned char>(s[i + 2]) & 0x3F) << 6) |
             (static_cast<unsigned char>(s[i + 3]) & 0x3F);
        i += 4;
        return true;
    }
    // Invalid lead byte: skip it.
    cp = '?';
    i += 1;
    return true;
}

static bool loadHd44780Font() {
    FILE* f = fopen("romfs:/HD44780_font.txt", "rb");
    if (!f) return false;
    std::string contents;
    char buf[4096];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        contents.append(buf, n);
    }
    fclose(f);
    // Start parsing after the fontdata declaration to avoid comments like "[cursor]".
    size_t startPos = contents.find("fontdata");
    if (startPos == std::string::npos) startPos = 0;
    std::istringstream lines(contents.substr(startPos));
    std::string line;
    size_t idx = 0;
    while (std::getline(lines, line) && idx < gHdFont.size()) {
        // Expect lines like: [  0,   0,   0,   0,   0,   0,   0, ],
        auto lb = line.find('[');
        auto rb = line.find(']');
        if (lb == std::string::npos || rb == std::string::npos || rb <= lb + 1) continue;
        std::string inside = line.substr(lb + 1, rb - lb - 1);
        std::istringstream ss(inside);
        int val = 0;
        int row = 0;
        Glyph g{};
        while (ss >> val) {
            if (row < 7) g.rows[row] = static_cast<uint8_t>(val & 0x1F);
            row++;
            if (ss.peek() == ',') ss.get();
        }
        if (row >= 7) {
            gHdFont[idx] = g;
            idx++;
        }
    }
    gHdFontCount = idx;
    // Require a reasonable number of glyphs (at least the basic ASCII printable set).
    if (idx >= 96 && idx <= gHdFont.size()) {
        gHdFontLoaded = true;
        romm::logLine("Loaded HD44780 font from romfs (" + std::to_string(idx) + " glyphs)");
        return true;
    }
    gHdFontLoaded = false;
    gHdFontCount = 0;
    romm::logLine("HD44780 font present but invalid glyph count (" + std::to_string(idx) + "); using built-in glyphs.");
    return false;
}

static std::string humanSize(uint64_t bytes) {
    const char* units[] = {"B","KB","MB","GB","TB"};
    double v = static_cast<double>(bytes);
    int idx = 0;
    while (v >= 1024.0 && idx < 4) { v /= 1024.0; idx++; }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f %s", v, units[idx]);
    return std::string(buf);
}

static const Glyph& glyphFor(char c) {
    // Custom markers first.
    if (c == '\x01') return kOMacron; // Ō/ō marker

    unsigned char uc = static_cast<unsigned char>(c);
    if (gHdFontLoaded && uc >= 32) {
        size_t idx = uc - 32;
        if (idx < gHdFontCount) {
            return gHdFont[idx];
        }
    }
    if (c == ' ') return kFont[0];
    if (c == '=') return kEqGlyph;
    if (c == '.') return kDotGlyph;
    if (c == ':') return kColonGlyph;
    if (c == '%') return kPercentGlyph;
    if (c == '!') return kBangGlyph;
    if (c == '?') return kQuestionGlyph;
    if (c == '/') return kSlashGlyph;
    if (c == '+') return kPlusGlyph;
    if (c == '-') return kMinusGlyph;
    if (c == '*') return kStarGlyph;
    if (c == '#') return kHashGlyph;
    if (c == '$') return kDollarGlyph;
    if (c == '(') return kLParenGlyph;
    if (c == ')') return kRParenGlyph;
    if (c == ',') return kCommaGlyph;
    if (c == '\x01') return kOMacron; // marker for Ō/ō
    if (c >= '0' && c <= '9') return kFont[1 + (c - '0')];
    if (c >= 'A' && c <= 'Z') return kFont[11 + (c - 'A')];
    if (c >= 'a' && c <= 'z') return kFont[11 + (c - 'a')];
    return kFont[0];
}

static void drawText(SDL_Renderer* r, int x, int y, const std::string& txt, SDL_Color color, int scale = 2) {
    SDL_SetRenderDrawColor(r, color.r, color.g, color.b, color.a);
    const int inset = scale * 4;   // extra inset to protect left-most column
    const int spacing = scale;     // tight spacing between glyphs
    int cursor = x + inset;
    // Normalize UTF-8 to our glyph set:
    // - map U+014C/U+014D (Ō/ō) to marker 0x01
    // - map "O"+combining macron (U+0304) and "o"+combining macron to marker 0x01
    // - replace any other non-ASCII with '?'
    std::string norm;
    norm.reserve(txt.size());
    for (size_t i = 0; i < txt.size();) {
        uint32_t cp = 0;
        if (!decodeUtf8(txt, i, cp)) {
            break;
        }

        // Direct macron codepoints (Ō/ō)
        if (cp == 0x014C || cp == 0x014D) {
            norm.push_back('\x01');
            continue;
        }

        // O/o followed by combining macron
        if (cp == 'O' || cp == 'o') {
            size_t peek = i;
            uint32_t nextCp = 0;
            if (decodeUtf8(txt, peek, nextCp) && nextCp == 0x0304) {
                i = peek; // consume combining mark
                norm.push_back('\x01');
                continue;
            }
            norm.push_back(static_cast<char>(cp));
            continue;
        }

        if (cp < 0x80) {
            norm.push_back(static_cast<char>(cp));
        } else {
            norm.push_back('?');
        }
    }
    for (char c : norm) {
        const Glyph& g = glyphFor(c);
        cursor += spacing;
        for (int row = 0; row < 7; ++row) {
            uint8_t bits = g.rows[row];
            for (int col = 0; col < 5; ++col) {
                if (bits & (1 << (4 - col))) {
                    SDL_Rect px{cursor + col * scale, y + row * scale, scale, scale};
                    SDL_RenderFillRect(r, &px);
                }
            }
        }
        cursor += 5 * scale + spacing;
    }
}

// Render the current view (header/footer + body) based on shared Status state.
// Views: PLATFORMS, ROMS, DETAIL, QUEUE, DOWNLOADING, ERROR.
static void renderStatus(SDL_Renderer* renderer, const Status& status) {
    if (!renderer) return;

    struct Snapshot {
        Status::View view{Status::View::PLATFORMS};
        std::vector<romm::Platform> platforms;
        std::vector<romm::Game> roms;
        std::vector<romm::Game> downloadQueue;
        int selectedPlatformIndex{0};
        int selectedRomIndex{0};
        int selectedQueueIndex{0};
        Status::View prevQueueView{Status::View::PLATFORMS};
        bool downloadCompleted{false};
        bool downloadWorkerRunning{false};
        bool lastDownloadFailed{false};
        std::string lastDownloadError;
        std::string currentDownloadTitle;
        std::string lastError;
    } snap;

    {
        std::lock_guard<std::mutex> guard(status.mutex);
        snap.view = status.currentView;
        snap.platforms = status.platforms;
        snap.roms = status.roms;
        snap.downloadQueue = status.downloadQueue;
        snap.selectedPlatformIndex = status.selectedPlatformIndex;
        snap.selectedRomIndex = status.selectedRomIndex;
        snap.selectedQueueIndex = status.selectedQueueIndex;
        snap.prevQueueView = status.prevQueueView;
        snap.downloadCompleted = status.downloadCompleted;
        snap.downloadWorkerRunning = status.downloadWorkerRunning.load();
        snap.lastDownloadFailed = status.lastDownloadFailed.load();
        snap.lastDownloadError = status.lastDownloadError;
        snap.currentDownloadTitle = status.currentDownloadTitle;
        snap.lastError = status.lastError;
    }

    if (gViewTraceFrames > 0) {
        romm::logDebug("Render trace view=" + std::string(viewName(snap.view)) +
                       " selP=" + std::to_string(snap.selectedPlatformIndex) +
                       " selR=" + std::to_string(snap.selectedRomIndex) +
                       " selQ=" + std::to_string(snap.selectedQueueIndex),
                       "UI");
        gViewTraceFrames--;
    }
    if (snap.view != gLastLoggedView) {
        switch (snap.view) {
            case Status::View::PLATFORMS: romm::logLine("View: PLATFORMS"); break;
            case Status::View::ROMS: romm::logLine("View: ROMS"); gRomsDebugFrames = 3; break;
            case Status::View::DETAIL: romm::logLine("View: DETAIL"); gRomsDebugFrames = 2; break;
            case Status::View::QUEUE: romm::logLine("View: QUEUE"); gRomsDebugFrames = 2; break;
            case Status::View::DOWNLOADING: romm::logLine("View: DOWNLOADING"); break;
            case Status::View::ERROR: romm::logLine("View: ERROR"); break;
            default: break;
        }
        gLastLoggedView = snap.view;
    }

    SDL_Color headerBar{40, 80, 140, 255};
    SDL_Color footerBar{12, 12, 18, 255};

    switch (snap.view) {
        case Status::View::PLATFORMS:
            SDL_SetRenderDrawColor(renderer, 6, 46, 112, 255);
            headerBar = {38, 108, 200, 255};
            break;
        case Status::View::ROMS:
            SDL_SetRenderDrawColor(renderer, 0, 70, 96, 255);
            headerBar = {20, 142, 186, 255};
            break;
        case Status::View::DETAIL:
            SDL_SetRenderDrawColor(renderer, 12, 26, 72, 255);
            headerBar = {54, 110, 210, 255};
            break;
        case Status::View::QUEUE:
            SDL_SetRenderDrawColor(renderer, 52, 26, 88, 255);
            headerBar = {120, 72, 180, 255};
            break;
        case Status::View::DOWNLOADING:
            SDL_SetRenderDrawColor(renderer, 90, 60, 0, 255);
            headerBar = {140, 100, 20, 255};
            break;
        case Status::View::ERROR:
            SDL_SetRenderDrawColor(renderer, 90, 0, 0, 255);
            headerBar = {150, 20, 20, 255};
            break;
    }
    SDL_RenderClear(renderer);

    auto drawHeaderBar = [&](const std::string& text) {
        SDL_Rect bar{0, 0, 1280, 52};
        SDL_SetRenderDrawColor(renderer, headerBar.r, headerBar.g, headerBar.b, 255);
        SDL_RenderFillRect(renderer, &bar);
        SDL_Color fg{255,255,255,255};
        drawText(renderer, 32, 14, text, fg, 2);
    };

    auto drawFooterBar = [&](const std::string& text) {
        SDL_Rect bar{0, 720 - 48, 1280, 48};
        SDL_SetRenderDrawColor(renderer, footerBar.r, footerBar.g, footerBar.b, 255);
        SDL_RenderFillRect(renderer, &bar);
        SDL_Color hint{200, 220, 255, 255};
        drawText(renderer, 32, 720 - 36, text, hint, 2);
    };

    uint64_t totalBytes = status.totalDownloadBytes.load();
    uint64_t totalDoneRaw = status.totalDownloadedBytes.load();
    uint64_t curBytes = status.currentDownloadSize.load();
    uint64_t curDone = status.currentDownloadedBytes.load();
    uint64_t totalDone = (curDone > totalDoneRaw) ? curDone : totalDoneRaw;
    const bool downloadsDone =
        snap.downloadCompleted ||
        (totalBytes > 0 && totalDone >= totalBytes &&
         snap.downloadQueue.empty() && !snap.downloadWorkerRunning);

    if (snap.view == Status::View::DOWNLOADING && totalBytes > 0) {
        float pctTotal = static_cast<float>(totalDone) /
                         static_cast<float>(std::max<uint64_t>(totalBytes, 1));
        float pctCurrent = (curBytes > 0)
            ? static_cast<float>(curDone) / static_cast<float>(curBytes)
            : 0.0f;
        pctTotal = std::clamp(pctTotal, 0.0f, 1.0f);
        pctCurrent = std::clamp(pctCurrent, 0.0f, 1.0f);

        SDL_Color fg{255,255,255,255};
        SDL_Rect outline{ (1280 - 640) / 2, 720/2 - 20, 640, 40 };

        if (downloadsDone) {
            drawText(renderer, outline.x, outline.y - 28, "Downloads complete", fg, 2);
            drawText(renderer, outline.x, outline.y + 50, "All files finalized. Press A to return.", fg, 2);
        } else {
            int barWidth = static_cast<int>(pctTotal * 600);
            SDL_Rect bar{ (1280 - 640) / 2, 720/2 - 20, barWidth, 40 };
            SDL_SetRenderDrawColor(renderer, 180, 220, 80, 255);
            SDL_RenderFillRect(renderer, &bar);
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
            SDL_RenderDrawRect(renderer, &outline);
            int pctInt = static_cast<int>(pctTotal * 100.0f);
            drawText(renderer, outline.x, outline.y - 28, "Downloading " + snap.currentDownloadTitle, fg, 2);
            if (curBytes > 0) {
                int pctCurInt = static_cast<int>(pctCurrent * 100.0f);
                drawText(renderer, outline.x, outline.y + 50,
                         "Current  " + std::to_string(pctCurInt) + "% (" +
                         humanSize(curDone) + " / " +
                         humanSize(curBytes) + ")", fg, 2);
            }
            drawText(renderer, outline.x, outline.y + 80,
                     "Overall  " + std::to_string(pctInt) + "% (" +
                     humanSize(totalDone) + " / " +
                     humanSize(totalBytes) + ")", fg, 2);
            if (totalDone == 0) {
                static const char* dots[] = {"", ".", "..", "..."};
                const int phase = (gFrameCounter / 20) % 4;
                drawText(renderer, outline.x, outline.y + 110,
                         std::string("Connecting") + dots[phase] + " waiting for data",
                         {200,220,255,255}, 2);
            }
            if (snap.lastDownloadFailed) {
                drawText(renderer, outline.x, outline.y + 110, "Failed: " + snap.lastDownloadError, {255,80,80,255}, 2);
            }
        }
    } else if (snap.view == Status::View::DOWNLOADING) {
        SDL_Color fg{255,255,255,255};
        drawText(renderer, (1280/2) - 120, 720/2 - 40, "Connecting...", fg, 2);
        drawText(renderer, (1280/2) - 120, 720/2 + 0, "Waiting for data...", fg, 2);
        if (snap.lastDownloadFailed) {
            drawText(renderer, (1280/2) - 120, 720/2 + 30, "Failed: " + snap.lastDownloadError, {255,80,80,255}, 2);
        }
    }

    int selPlat = snap.selectedPlatformIndex;
    int selRom = snap.selectedRomIndex;
    if (selPlat < 0) selPlat = 0;
    if (selPlat >= (int)snap.platforms.size() && !snap.platforms.empty())
        selPlat = (int)snap.platforms.size() - 1;
    if (selRom < 0) selRom = 0;
    if (selRom >= (int)snap.roms.size() && !snap.roms.empty())
        selRom = (int)snap.roms.size() - 1;

    auto sanitize = [](const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i) {
            unsigned char c = static_cast<unsigned char>(s[i]);
            if (c == 0xC5 && i + 1 < s.size()) {
                unsigned char c2 = static_cast<unsigned char>(s[i + 1]);
                if (c2 == 0x8C || c2 == 0x8D) {
                    out.push_back('');
                    ++i;
                    continue;
                }
            }
            if (c >= 32 && c < 127) {
                out.push_back(static_cast<char>(c));
            } else {
                out.push_back('?');
            }
        }
        return out;
    };

    auto ellipsize = [&](const std::string& s, size_t maxlen) {
        std::string clean = sanitize(s);
        if (clean.size() <= maxlen) return clean;
        return clean.substr(0, maxlen) + "...";
    };

    std::string header;
    std::string controls;
    if (snap.view == Status::View::PLATFORMS) {
        header = "PLATFORMS count=" + std::to_string(snap.platforms.size()) +
                 " sel=" + std::to_string(selPlat);
        SDL_Color fg{255,255,255,255};
        int listHeight = static_cast<int>(snap.platforms.size()) * 26 + 32;
        if (listHeight < 120) listHeight = 120;
        SDL_Rect listBg{48, 60, 560, listHeight};
        SDL_SetRenderDrawColor(renderer, 24, 70, 140, 180);
        SDL_RenderFillRect(renderer, &listBg);
        for (size_t i = 0; i < snap.platforms.size(); ++i) {
            SDL_Rect r{ 64, 72 + static_cast<int>(i)*26, 520, 24 };
            if ((int)i == selPlat)
                SDL_SetRenderDrawColor(renderer, 70, 140, 240, 255);
            else
                SDL_SetRenderDrawColor(renderer, 40, 70, 120, 200);
            SDL_RenderFillRect(renderer, &r);
            drawText(renderer, r.x + 14, r.y + 6, ellipsize(snap.platforms[i].name, 40), fg, 2);
        }
    } else if (snap.view == Status::View::ROMS) {
        std::string platLabel;
        if (selPlat >= 0 && selPlat < (int)snap.platforms.size()) {
            platLabel = ellipsize(snap.platforms[selPlat].name, 18);
        }
        header = "ROMS " + (platLabel.empty() ? "" : ("[" + platLabel + "] ")) +
                 "count=" + std::to_string(snap.roms.size()) +
                 " sel=" + std::to_string(selRom);
        SDL_Color fg{255,255,255,255};
        size_t visible = snap.roms.size() < 18 ? snap.roms.size() : 18;
        size_t start = 0;
        if (!snap.roms.empty()) {
            if (selRom >= (int)(start + visible)) start = selRom - visible + 1;
            if (selRom < (int)start) start = selRom;
            if (start + visible > snap.roms.size()) start = snap.roms.size() - visible;
        }
        if (gRomsDebugFrames > 0) {
            romm::logDebug("Render ROMS dbg: count=" + std::to_string(snap.roms.size()) +
                           " showing=" + std::to_string(visible) +
                           " start=" + std::to_string(start) +
                           " sel=" + std::to_string(selRom),
                           "UI");
            if (!snap.roms.empty()) {
                romm::logDebug(" ROM[0]=" + ellipsize(snap.roms[0].title, 60), "UI");
            }
            gRomsDebugFrames--;
        }
        int listHeight = static_cast<int>(visible) * 26 + 60;
        int maxListHeight = 720 - 64 - 60;
        if (listHeight > maxListHeight) listHeight = maxListHeight;
        if (listHeight < 260) listHeight = 260;
        SDL_Rect listBg{48, 64, 1040, listHeight};
        SDL_SetRenderDrawColor(renderer, 12, 90, 120, 180);
        SDL_RenderFillRect(renderer, &listBg);
        if (snap.roms.empty()) {
            drawText(renderer, 64, 96, "No ROMs found for this platform.", fg, 2);
        }
        for (size_t i = 0; i < visible; ++i) {
            size_t idx = start + i;
            SDL_Rect r{ 64, 88 + static_cast<int>(i)*26, 1008, 22 };
            if ((int)idx == selRom)
                SDL_SetRenderDrawColor(renderer, 80, 150, 240, 255);
            else
                SDL_SetRenderDrawColor(renderer, 34, 90, 140, 200);
            SDL_RenderFillRect(renderer, &r);
            if (idx < snap.roms.size()) {
                drawText(renderer, r.x + 12, r.y + 4, ellipsize(snap.roms[idx].title, 64), fg, 2);
                std::string sz = humanSize(snap.roms[idx].sizeBytes);
                drawText(renderer, r.x + 760, r.y + 4, sz, fg, 2);
            }
        }
    } else if (snap.view == Status::View::DETAIL) {
        header = "DETAIL";
        SDL_Color fg{255,255,255,255};
        if (selRom >= 0 && selRom < (int)snap.roms.size()) {
            const auto& g = snap.roms[selRom];
            header = "DETAIL [" + ellipsize(g.title, 22) + "]";
            SDL_Rect cover{70, 110, 240, 240};
            if (gCoverTexture) {
                SDL_RenderCopy(renderer, gCoverTexture, nullptr, &cover);
            } else {
                SDL_SetRenderDrawColor(renderer, 90, 125, 180, 255);
                SDL_RenderFillRect(renderer, &cover);
            }
            SDL_Rect outline{cover.x-2, cover.y-2, cover.w+4, cover.h+4};
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
            SDL_RenderDrawRect(renderer, &outline);
            SDL_Rect info{330, 110, 880, 240};
            SDL_SetRenderDrawColor(renderer, 32, 64, 130, 220);
            SDL_RenderFillRect(renderer, &info);
            drawText(renderer, cover.x + 12, cover.y + cover.h + 16, ellipsize(g.title, 28), fg, 2);
            drawText(renderer, info.x + 16, info.y + 16, "Platform=" + ellipsize(g.platformSlug.empty() ? g.platformId : g.platformSlug, 22), fg, 2);
            drawText(renderer, info.x + 16, info.y + 56, "Size=" + humanSize(g.sizeBytes), fg, 2);
            drawText(renderer, info.x + 16, info.y + 96, "ID=" + ellipsize(g.id, 22), fg, 2);
            drawText(renderer, info.x + 16, info.y + 136, "FsName=" + ellipsize(g.fsName, 34), fg, 2);
            std::string qCount = "Queue size=" + std::to_string(snap.downloadQueue.size());
            drawText(renderer, info.x + 16, info.y + 176, qCount, fg, 2);
            drawText(renderer, 80, 420, "A=Queue and open queue   B=Back   X=Queue view", fg, 2);
        } else {
            drawText(renderer, 80, 120, "No ROM selected.", fg, 2);
        }
    } else if (snap.view == Status::View::QUEUE) {
        uint64_t total = 0;
        for (auto& q : snap.downloadQueue) total += q.sizeBytes;
        header = "QUEUE items=" + std::to_string(snap.downloadQueue.size()) +
                 " total=" + humanSize(total);
        SDL_Color fg{255,255,255,255};
        size_t visible = snap.downloadQueue.size() < 18 ? snap.downloadQueue.size() : 18;
        size_t start = 0;
        if (!snap.downloadQueue.empty()) {
            if (snap.selectedQueueIndex >= (int)(start + visible)) start = snap.selectedQueueIndex - visible + 1;
            if (snap.selectedQueueIndex < (int)start) start = snap.selectedQueueIndex;
            if (start + visible > snap.downloadQueue.size()) start = snap.downloadQueue.size() - visible;
        }
        int listHeight = static_cast<int>(visible) * 26 + 60;
        int maxListHeight = 720 - 96 - 60;
        if (listHeight > maxListHeight) listHeight = maxListHeight;
        if (listHeight < 200) listHeight = 200;
        SDL_Rect listBg{48, 96, 1040, listHeight};
        SDL_SetRenderDrawColor(renderer, 90, 60, 150, 180);
        SDL_RenderFillRect(renderer, &listBg);
        drawText(renderer, 64, 70, "Total size: " + humanSize(total), fg, 2);
        if (snap.downloadQueue.empty()) {
            std::string msg = snap.downloadCompleted ? "All downloads complete." : "Queue empty. Press A in detail to add.";
            drawText(renderer, 64, 120, msg, fg, 2);
        }
        for (size_t i = 0; i < visible; ++i) {
            size_t idx = start + i;
            SDL_Rect r{ 64, 120 + static_cast<int>(i)*26, 1008, 22 };
            if ((int)idx == snap.selectedQueueIndex)
                SDL_SetRenderDrawColor(renderer, 150, 110, 230, 255);
            else
                SDL_SetRenderDrawColor(renderer, 110, 70, 180, 200);
            SDL_RenderFillRect(renderer, &r);
            if (idx < snap.downloadQueue.size()) {
                const auto& g = snap.downloadQueue[idx];
                drawText(renderer, r.x + 10, r.y + 4, ellipsize(g.title, 58), fg, 2);
                std::string sz = humanSize(g.sizeBytes);
                drawText(renderer, r.x + 740, r.y + 4, sz, fg, 2);
            }
        }
    } else if (snap.view == Status::View::DOWNLOADING) {
        header = "DOWNLOADING sel=" + std::to_string(status.currentDownloadIndex);
    } else if (snap.view == Status::View::ERROR) {
        header = "ERROR";
    }

    switch (snap.view) {
        case Status::View::PLATFORMS:
            controls = "[PLATFORMS] A=open platform B=back X=queue Plus=exit D-Pad=scroll hold";
            break;
        case Status::View::ROMS:
            controls = "[ROMS] A=details B=back X=queue Plus=exit D-Pad=scroll hold";
            break;
        case Status::View::DETAIL:
            controls = "[DETAIL] A=queue+open B=back X=queue Plus=exit";
            break;
        case Status::View::QUEUE:
            controls = snap.downloadWorkerRunning
                ? "[QUEUE] Y=view downloading B=back Plus=exit D-Pad=scroll hold"
                : "[QUEUE] Y=start downloads B=back Plus=exit D-Pad=scroll hold";
            break;
        case Status::View::DOWNLOADING:
            controls = "[DOWNLOADING] B=back Plus=exit";
            break;
        default:
            controls = "Plus=exit";
            break;
    }

    static std::string gLastControls;
    if (controls != gLastControls) {
        romm::logDebug("Controls slug: " + controls, "UI");
        gLastControls = controls;
    }

    if (!header.empty()) {
        header += " [" + std::string(viewName(snap.view)) + "]";
        drawHeaderBar(header);
    }
    if (!controls.empty()) {
        drawFooterBar(controls);
    }

    SDL_RenderPresent(renderer);
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    socketInitializeDefault();
#if HAS_NXLINK
    int nxfd = nxlinkStdio();
    if (nxfd >= 0) {
        romm::logLine("nxlink stdout active.");
    } else {
        consoleDebugInit(debugDevice_SVC); // forward to svc debug for nxlink -s listeners
        romm::logLine("nxlink stdout NOT active; using debug SVC output.");
    }
#else
    consoleDebugInit(debugDevice_SVC);
#endif
    nifmInitialize(NifmServiceType_User);
    fsdevMountSdmc();

    // Initialize logging after SD is mounted so early messages persist.
    romm::initLogFile();
    romm::logLine("Startup.");

    bool romfsReady = false;
    Result rromfs = romfsInit();
    if (R_SUCCEEDED(rromfs)) {
        romfsReady = true;
        romm::logLine("romfs mounted.");
        if (!loadHd44780Font()) {
            romm::logLine("HD44780 font load failed; using built-in glyphs.");
        }
    } else {
        romm::logLine("romfs mount failed; using built-in glyphs.");
    }

    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_GameController* pad = nullptr;
    int numJoy = 0;
    Config config;
    Status status;
    std::string cfgError;
    bool running = true;
    ScrollHold scrollHold;
    auto resetNav = [&]() { status.navStack.clear(); };
    int lastCoverSel = -1;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) != 0) {
        romm::logLine(std::string("SDL_Init failed: ") + SDL_GetError());
        goto exit_app;
    }

    SDL_GameControllerEventState(SDL_ENABLE);
    numJoy = SDL_NumJoysticks();
    romm::logLine("Joysticks detected: " + std::to_string(numJoy));
    if (numJoy > 0) {
        for (int i = 0; i < numJoy; ++i) {
            if (SDL_IsGameController(i)) {
                pad = SDL_GameControllerOpen(i);
                if (pad) { romm::logLine("Opened controller index " + std::to_string(i)); break; }
            }
        }
        if (!pad) romm::logLine("No compatible controller opened.");
    }

    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software"); // enforce software to avoid GPU crashes

    window = SDL_CreateWindow("RomM Switch Client",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1280, 720, SDL_WINDOW_FULLSCREEN);
    if (!window) {
        romm::logLine(std::string("SDL_CreateWindow failed: ") + SDL_GetError());
        goto exit_app;
    }
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer) {
        romm::logLine(std::string("SDL_CreateRenderer (software) failed: ") + SDL_GetError());
        goto exit_app;
    } else {
        romm::logLine("Using SDL software renderer.");
    }

    // Prevent system sleep and screen dimming while the app is active to avoid download interruptions.
    appletSetAutoSleepDisabled(true);
    appletSetMediaPlaybackState(true);
    romm::logLine("Auto-sleep disabled; media playback state set to keep screen on.");

    if (!romm::loadConfig(config, cfgError)) {
        status.currentView = Status::View::ERROR;
        status.lastError = cfgError;
        romm::logLine(cfgError);
    } else {
        romm::setLogLevelFromString(config.logLevel);
        romm::logLine("Config loaded.");
        romm::logLine(" server_url=" + config.serverUrl);
        romm::logLine(" download_dir=" + config.downloadDir);
        romm::logLine(std::string(" fat32_safe=") + (config.fat32Safe ? "true" : "false"));
        romm::ensureDirectory(config.downloadDir);
        std::string err;
        if (!romm::fetchPlatforms(config, status, err)) {
            status.currentView = Status::View::ERROR;
            status.lastError = err;
            romm::logLine("Failed to fetch platforms: " + err);
        }
    }

    // Main loop: poll input -> update state -> render current view
    while (running && appletMainLoop()) {
        bool viewChangedThisFrame = false;
        // TODO(nav): extract view transitions into a ViewController and align hints with mapping.
        // Adjust selection index based on current view (platforms/roms/queue)
        auto adjustSelection = [&](int dir) {
            if (status.currentView == Status::View::PLATFORMS) {
                status.selectedPlatformIndex = std::max(0, std::min((int)status.platforms.size() - 1, status.selectedPlatformIndex + dir));
            } else if (status.currentView == Status::View::ROMS || status.currentView == Status::View::DETAIL) {
                status.selectedRomIndex = std::max(0, std::min((int)status.roms.size() - 1, status.selectedRomIndex + dir));
            } else if (status.currentView == Status::View::QUEUE) {
                status.selectedQueueIndex = std::max(0, std::min((int)status.downloadQueue.size() - 1, status.selectedQueueIndex + dir));
            }
        };
        SDL_Event e;
        // Handle input events until a view change occurs (then render the new view before more input)
        while (!viewChangedThisFrame && SDL_PollEvent(&e)) {
            romm::Action act = romm::translateEvent(e);
            if (act != romm::Action::None) {
                romm::logDebug("Input action: " + std::to_string(static_cast<int>(act)), "INPUT");
            }
            switch (act) {
                case romm::Action::Quit:
                    running = false;
                    romm::stopDownloadWorker();
                    break;
                case romm::Action::Up:
                    adjustSelection(-1);
                    scrollHold.dir = -1;
                    scrollHold.nextMs = SDL_GetTicks() + 240;
                    scrollHold.repeats = 0;
                    break;
                case romm::Action::Down:
                    adjustSelection(1);
                    scrollHold.dir = 1;
                    scrollHold.nextMs = SDL_GetTicks() + 240;
                    scrollHold.repeats = 0;
                    break;
                case romm::Action::Select: {
                    // A/Select: context-sensitive (fetch ROMs, open detail, or enqueue)
                    if (status.currentView == Status::View::PLATFORMS) {
                        if (!status.platforms.empty() && status.selectedPlatformIndex >= 0 &&
                            status.selectedPlatformIndex < (int)status.platforms.size()) {
                            std::string pid = status.platforms[status.selectedPlatformIndex].id;
                            romm::logLine("Fetching ROMs for platform id=" + pid);
                            std::string err;
                            if (romm::fetchGamesForPlatform(config, pid, status, err)) {
                                resetNav();
                                status.currentView = Status::View::ROMS;
                                status.selectedRomIndex = 0;
                                gViewTraceFrames = 8;
                                romm::logLine("Fetched ROMs count=" + std::to_string(status.roms.size()) +
                                              (status.roms.empty() ? "" : " first=" + status.roms[0].title));
                                viewChangedThisFrame = true;
                            } else {
                                status.currentView = Status::View::ERROR;
                                status.lastError = err;
                                romm::logLine("Failed to fetch ROMs: " + err);
                            }
                        } else {
                            romm::logLine("Select on PLATFORMS but index out of range.");
                        }
                    } else if (status.currentView == Status::View::ROMS && !status.roms.empty()) {
                        status.currentView = Status::View::DETAIL;
                        gViewTraceFrames = 8;
                        viewChangedThisFrame = true;
                        romm::logLine("Open DETAIL for idx=" + std::to_string(status.selectedRomIndex));
                      } else if (status.currentView == Status::View::DETAIL && !status.roms.empty()) {
                          if (status.selectedRomIndex >= 0 && status.selectedRomIndex < (int)status.roms.size()) {
                              // Fetch DetailedRom files[] to pick the best downloadable asset (xci/nsp).
                              romm::Game enriched = status.roms[status.selectedRomIndex];
                              std::string err;
                              if (!romm::enrichGameWithFiles(config, enriched, err)) {
                                  status.currentView = Status::View::ERROR;
                                  status.lastError = err;
                                  romm::logLine("Failed to enrich ROM with files: " + err);
                                  break;
                              }
                              // Persist enriched fields back into the ROM list and queue.
                              status.roms[status.selectedRomIndex] = enriched;
                              status.downloadQueue.push_back(enriched);
                              status.selectedQueueIndex = (int)status.downloadQueue.size() - 1;
                              romm::logLine("Queued ROM: " + enriched.title +
                                            " | Queue size=" + std::to_string(status.downloadQueue.size()));
                              status.prevQueueView = Status::View::DETAIL;
                              status.currentView = Status::View::QUEUE; // show queued list immediately
                              gViewTraceFrames = 8;
                              viewChangedThisFrame = true;
                          }
                    }
                    break;
                }
                case romm::Action::Back: {
                    // B/Back: navigate up one level or return from queue
                    romm::logLine(std::string("Back pressed in view=") + viewName(status.currentView));
                    if (status.currentView == Status::View::ROMS) {
                        status.currentView = Status::View::PLATFORMS;
                        resetNav();
                        gViewTraceFrames = 8;
                        romm::logLine("Back to PLATFORMS.");
                        viewChangedThisFrame = true;
                    } else if (status.currentView == Status::View::DETAIL) {
                        status.currentView = Status::View::ROMS;
                        gViewTraceFrames = 8;
                        romm::logLine("Back to ROMS from DETAIL.");
                        viewChangedThisFrame = true;
                    } else if (status.currentView == Status::View::DOWNLOADING) {
                        status.currentView = Status::View::QUEUE;
                        gViewTraceFrames = 8;
                        romm::logLine("Back to QUEUE from DOWNLOADING.");
                        viewChangedThisFrame = true;
                    } else if (status.currentView == Status::View::QUEUE) {
                        Status::View dest = status.prevQueueView;
                        if (dest == Status::View::QUEUE) dest = Status::View::PLATFORMS;
                        if (dest == Status::View::DOWNLOADING) dest = Status::View::PLATFORMS; // avoid loop
                        status.currentView = dest;
                        gViewTraceFrames = 8;
                        romm::logLine(std::string("Back from QUEUE to ") + viewName(dest) + ".");
                        viewChangedThisFrame = true;
                    } else if (status.currentView == Status::View::PLATFORMS) {
                        // No-op to avoid accidental exits; Plus still exits.
                        romm::logLine("Back on PLATFORMS ignored.");
                    } else if (status.currentView == Status::View::ERROR) {
                        running = false;
                    }
                    break;
                }
                case romm::Action::OpenQueue:
                    // Y/X: open queue from any view; remember where we came from (except downloading)
                    // Only update prevQueueView if we're not already in QUEUE
                    if (status.currentView != Status::View::QUEUE &&
                        status.currentView != Status::View::DOWNLOADING) { // keep last real view (plat/roms/detail)
                        status.prevQueueView = status.currentView;
                    }
                    status.currentView = Status::View::QUEUE;
                    if (status.selectedQueueIndex >= (int)status.downloadQueue.size())
                        status.selectedQueueIndex = status.downloadQueue.empty() ? 0 : (int)status.downloadQueue.size() - 1;
                    romm::logLine(std::string("Opened queue view from ") + viewName(status.prevQueueView) +
                                  " items=" + std::to_string(status.downloadQueue.size()));
                    gViewTraceFrames = 8;
                    viewChangedThisFrame = true;
                    break;
                case romm::Action::StartDownload:
                    // X in QUEUE: start downloads and show downloading view
                    // TODO(queue UX): dedupe entries and surface per-item failures/history in UI.
                    if (status.currentView == Status::View::QUEUE && !status.downloadQueue.empty()) {
                        if (status.downloadWorkerRunning.load()) {
                            // Already running: just open the view without resetting counters.
                            romm::logLine("Download already running; opening DOWNLOADING view.");
                            status.currentView = Status::View::DOWNLOADING;
                            gViewTraceFrames = 8;
                            viewChangedThisFrame = true;
                        } else {
                            status.currentView = Status::View::DOWNLOADING;
                            status.currentDownloadIndex = 0;
                            status.currentDownloadedBytes.store(0);
                            status.totalDownloadedBytes.store(0);
                            // Prime totals immediately so HUD isn't zero before worker starts.
                            status.totalDownloadBytes.store(0);
                            for (auto& g : status.downloadQueue) status.totalDownloadBytes.fetch_add(g.sizeBytes);
                            status.currentDownloadSize.store(status.downloadQueue[0].sizeBytes);
                            romm::logLine("Starting downloads for queue size=" + std::to_string(status.downloadQueue.size()) +
                                          " totalBytes=" + std::to_string(status.totalDownloadBytes.load()));
                            startDownloadWorker(status, config);
                            gViewTraceFrames = 8;
                            viewChangedThisFrame = true;
                        }
                    } else {
                        romm::logLine("StartDownload outside QUEUE; ignoring.");
                    }
                    break;
                default:
                    break;
            }
        }

        // Hold-to-scroll with acceleration using controller D-pad
        if (pad) {
            bool upHeld = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP);
            bool downHeld = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
            int dir = 0;
            if (upHeld && !downHeld) dir = -1;
            else if (downHeld && !upHeld) dir = 1;
            Uint32 now = SDL_GetTicks();
            if (dir == 0) {
                scrollHold.dir = 0;
                scrollHold.repeats = 0;
            } else {
                if (scrollHold.dir != dir) {
                    scrollHold.dir = dir;
                    scrollHold.repeats = 0;
                    scrollHold.nextMs = now + 300;
                } else if (now >= scrollHold.nextMs) {
                    int step = dir;
                    adjustSelection(step);
                    scrollHold.repeats++;
                    Uint32 interval = 140;
                    if (scrollHold.repeats > 5) interval = 90;
                    if (scrollHold.repeats > 12) interval = 60;
                    scrollHold.nextMs = now + interval;
                }
            }
        }

        // TODO(async covers): move cover fetch/decode off the render loop; keep placeholder texture.
        // JIT cover loading: when entering or changing selection in DETAIL, load cover; free when leaving.
        if (status.currentView == Status::View::DETAIL) {
            if (status.selectedRomIndex != lastCoverSel) {
                lastCoverSel = status.selectedRomIndex;
                if (status.selectedRomIndex >= 0 && status.selectedRomIndex < (int)status.roms.size()) {
                    loadCoverTextureForGame(status.roms[status.selectedRomIndex], renderer, config);
                }
            }
        } else {
            if (gCoverTexture) {
                SDL_DestroyTexture(gCoverTexture);
                gCoverTexture = nullptr;
            }
            lastCoverSel = -1;
        }

        renderStatus(renderer, status);

    // Lightweight frame log for early frames to catch exit path
    if (gFrameCounter < 5) {
        std::string err = SDL_GetError();
        if (!err.empty()) romm::logDebug("SDL error: " + err, "SDL");
        romm::logDebug("Frame " + std::to_string(gFrameCounter) +
                       " view=" + std::to_string(static_cast<int>(status.currentView)) +
                       " selP=" + std::to_string(status.selectedPlatformIndex) +
                       " selR=" + std::to_string(status.selectedRomIndex) +
                       " plats=" + std::to_string(status.platforms.size()) +
                       " roms=" + std::to_string(status.roms.size()),
                       "UI");
    }
        gFrameCounter++;
    }

exit_app:
    romm::logLine("Exiting main loop. running=" + std::to_string(running));
    romm::stopDownloadWorker();
    // Restore default sleep/dim behavior on exit.
    appletSetMediaPlaybackState(false);
    appletSetAutoSleepDisabled(false);
    if (gCoverTexture) SDL_DestroyTexture(gCoverTexture);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    if (pad) SDL_GameControllerClose(pad);
    SDL_Quit();
    if (romfsReady) romfsExit();
    fsdevUnmountAll();
    nifmExit();
    socketExit();
    return 0;
}
