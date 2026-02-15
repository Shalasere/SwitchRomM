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
#include <ctime>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>
#include <optional>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <iomanip>
#include <cctype>
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
#include "romm/job_manager.hpp"
#include "romm/logger.hpp"
#include "romm/http_common.hpp"
#include "romm/update.hpp"
#include "romm/version.hpp"
#include "romm/errors.hpp"
#include "romm/downloader.hpp"
#include "romm/cover_loader.hpp"
#include "romm/planner.hpp"
#include "romm/platform_prefs.hpp"
#include "romm/queue_policy.hpp"
#include "romm/queue_store.hpp"
#include "romm/speed_test.hpp"

using romm::Status;
using romm::Config;

static Status::View gLastLoggedView = Status::View::ERROR;
static int gRomsDebugFrames = 0;
static int gFrameCounter = 0;
static int gViewTraceFrames = 0; // log render view for a few frames after navigation
static SDL_Texture* gCoverTexture = nullptr;
static std::string gCoverTextureUrl;
static romm::CoverLoader gCoverLoader;
static std::string gLastCoverRequested;

static bool fetchCoverData(const std::string& url, const Config& cfg, std::vector<unsigned char>& outData, std::string& err) {
    std::string body;
    if (!romm::fetchBinary(cfg, url, body, err)) return false;
    outData.assign(body.begin(), body.end());
    return true;
}

static void processCoverResult(SDL_Renderer* renderer) {
    auto res = gCoverLoader.poll();
    if (!res) return;
    if (!renderer) return;
    if (res->ok && !res->pixels.empty()) {
        SDL_Texture* tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888,
                                             SDL_TEXTUREACCESS_STATIC, res->w, res->h);
        if (tex && SDL_UpdateTexture(tex, nullptr, res->pixels.data(), res->w * 4) == 0) {
            SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
            if (gCoverTexture) SDL_DestroyTexture(gCoverTexture);
            gCoverTexture = tex;
            gCoverTextureUrl = res->url;
            romm::logLine("Loaded cover for " + res->title +
                          " (" + std::to_string(res->w) + "x" + std::to_string(res->h) + ")");
            return;
        }
        if (tex) SDL_DestroyTexture(tex);
        romm::logLine("Cover texture upload failed for " + res->title);
    } else {
        romm::logLine("Cover fetch failed for " + res->title + ": " + res->error);
    }
}

static const char* viewName(Status::View v) {
    switch (v) {
        case Status::View::PLATFORMS: return "PLATFORMS";
        case Status::View::ROMS: return "ROMS";
        case Status::View::DETAIL: return "DETAIL";
        case Status::View::QUEUE: return "QUEUE";
        case Status::View::DOWNLOADING: return "DOWNLOADING";
        case Status::View::ERROR: return "ERROR";
        case Status::View::DIAGNOSTICS: return "DIAGNOSTICS";
        case Status::View::UPDATER: return "UPDATER";
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

// Fold common Latin-1/Latin-Extended codepoints to ASCII so list/search remain usable
// even when the bitmap glyph set does not include those codepoints.
static char foldCodepointToAscii(uint32_t cp) {
    switch (cp) {
        // Whitespace and punctuation
        case 0x00A0: return ' ';
        case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212: return '-';
        case 0x2018: case 0x2019: case 0x201A: case 0x2032: return '\'';
        case 0x201C: case 0x201D: case 0x201E: case 0x2033: return '"';
        case 0x2026: return '.';
        // Special letters and ligatures
        case 0x00C6: case 0x01E2: case 0x01FC: return 'A';
        case 0x00E6: case 0x01E3: case 0x01FD: return 'a';
        case 0x0152: return 'O';
        case 0x0153: return 'o';
        case 0x00DF: return 's';
        case 0x00DE: return 'T';
        case 0x00FE: return 't';
        case 0x00D0: return 'D';
        case 0x00F0: return 'd';
        // A/a
        case 0x00C0: case 0x00C1: case 0x00C2: case 0x00C3: case 0x00C4: case 0x00C5:
        case 0x0100: case 0x0102: case 0x0104: case 0x01CD: case 0x01DE: case 0x01E0:
            return 'A';
        case 0x00E0: case 0x00E1: case 0x00E2: case 0x00E3: case 0x00E4: case 0x00E5:
        case 0x0101: case 0x0103: case 0x0105: case 0x01CE: case 0x01DF: case 0x01E1:
            return 'a';
        // C/c
        case 0x00C7: case 0x0106: case 0x0108: case 0x010A: case 0x010C: return 'C';
        case 0x00E7: case 0x0107: case 0x0109: case 0x010B: case 0x010D: return 'c';
        // D/d
        case 0x010E: case 0x0110: return 'D';
        case 0x010F: case 0x0111: return 'd';
        // E/e
        case 0x00C8: case 0x00C9: case 0x00CA: case 0x00CB: case 0x0112: case 0x0114:
        case 0x0116: case 0x0118: case 0x011A:
            return 'E';
        case 0x00E8: case 0x00E9: case 0x00EA: case 0x00EB: case 0x0113: case 0x0115:
        case 0x0117: case 0x0119: case 0x011B:
            return 'e';
        // G/g
        case 0x011C: case 0x011E: case 0x0120: case 0x0122: return 'G';
        case 0x011D: case 0x011F: case 0x0121: case 0x0123: return 'g';
        // I/i
        case 0x00CC: case 0x00CD: case 0x00CE: case 0x00CF: case 0x0128: case 0x012A:
        case 0x012C: case 0x012E: case 0x0130:
            return 'I';
        case 0x00EC: case 0x00ED: case 0x00EE: case 0x00EF: case 0x0129: case 0x012B:
        case 0x012D: case 0x012F: case 0x0131:
            return 'i';
        // N/n
        case 0x00D1: case 0x0143: case 0x0145: case 0x0147: return 'N';
        case 0x00F1: case 0x0144: case 0x0146: case 0x0148: return 'n';
        // O/o
        case 0x00D2: case 0x00D3: case 0x00D4: case 0x00D5: case 0x00D6: case 0x00D8:
        case 0x014C: case 0x014E: case 0x0150:
            return 'O';
        case 0x00F2: case 0x00F3: case 0x00F4: case 0x00F5: case 0x00F6: case 0x00F8:
        case 0x014D: case 0x014F: case 0x0151:
            return 'o';
        // R/r
        case 0x0154: case 0x0156: case 0x0158: return 'R';
        case 0x0155: case 0x0157: case 0x0159: return 'r';
        // S/s
        case 0x015A: case 0x015C: case 0x015E: case 0x0160: return 'S';
        case 0x015B: case 0x015D: case 0x015F: case 0x0161: case 0x017F: return 's';
        // T/t
        case 0x0162: case 0x0164: case 0x0166: return 'T';
        case 0x0163: case 0x0165: case 0x0167: return 't';
        // U/u
        case 0x00D9: case 0x00DA: case 0x00DB: case 0x00DC: case 0x0168: case 0x016A:
        case 0x016C: case 0x016E: case 0x0170: case 0x0172:
            return 'U';
        case 0x00F9: case 0x00FA: case 0x00FB: case 0x00FC: case 0x0169: case 0x016B:
        case 0x016D: case 0x016F: case 0x0171: case 0x0173:
            return 'u';
        // Y/y
        case 0x00DD: case 0x0178: return 'Y';
        case 0x00FD: case 0x00FF: return 'y';
        // Z/z
        case 0x0179: case 0x017B: case 0x017D: return 'Z';
        case 0x017A: case 0x017C: case 0x017E: return 'z';
        default:
            break;
    }
    return 0;
}

static std::string foldUtf8ToAscii(const std::string& in, bool replaceUnknown) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size();) {
        uint32_t cp = 0;
        if (!decodeUtf8(in, i, cp)) break;
        if (cp < 0x80) {
            char c = static_cast<char>(cp);
            out.push_back((c >= 32 && c < 127) ? c : (replaceUnknown ? '?' : ' '));
            continue;
        }
        if (cp >= 0x0300 && cp <= 0x036F) {
            // Ignore standalone combining marks.
            continue;
        }
        char mapped = foldCodepointToAscii(cp);
        if (mapped != 0) {
            out.push_back(mapped);
        } else if (replaceUnknown) {
            out.push_back('?');
        }
    }
    return out;
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

static std::string normalizeSearchText(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size();) {
        uint32_t cp = 0;
        if (!decodeUtf8(in, i, cp)) break;

        char mapped = 0;
        if (cp < 0x80) {
            mapped = static_cast<char>(cp);
        } else if (cp >= 0x0300 && cp <= 0x036F) {
            continue; // combining mark
        } else {
            mapped = foldCodepointToAscii(cp);
        }
        if (mapped == 0) continue;
        unsigned char ch = static_cast<unsigned char>(mapped);
        if (ch >= 'A' && ch <= 'Z') {
            out.push_back(static_cast<char>(ch - 'A' + 'a'));
        } else if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) {
            out.push_back(static_cast<char>(ch));
        } else if (std::isspace(ch) || ch == '-' || ch == '_' || ch == '/') {
            if (out.empty() || out.back() == ' ') continue;
            out.push_back(' ');
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

static const char* romFilterLabel(romm::RomFilter f) {
    switch (f) {
        case romm::RomFilter::All: return "All";
        case romm::RomFilter::Queued: return "Queued";
        case romm::RomFilter::Resumable: return "Resumable";
        case romm::RomFilter::Failed: return "Failed";
        case romm::RomFilter::Completed: return "Completed";
        case romm::RomFilter::NotQueued: return "NotQueued";
        default: return "All";
    }
}

static const char* romSortLabel(romm::RomSort s) {
    switch (s) {
        case romm::RomSort::TitleAsc: return "Title A-Z";
        case romm::RomSort::TitleDesc: return "Title Z-A";
        case romm::RomSort::SizeDesc: return "Size High-Low";
        case romm::RomSort::SizeAsc: return "Size Low-High";
        default: return "Title A-Z";
    }
}

static bool promptSearchQuery(std::string& query) {
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0))) {
        return false;
    }
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetHeaderText(&kbd, "ROM Search");
    swkbdConfigSetGuideText(&kbd, "Enter title text (blank clears filter)");
    swkbdConfigSetInitialText(&kbd, query.c_str());
    char buf[256] = {};
    Result rc = swkbdShow(&kbd, buf, sizeof(buf));
    swkbdClose(&kbd);
    if (R_FAILED(rc)) return false;
    query = buf;
    return true;
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
    // - preserve Ō/ō via marker 0x01
    // - fold common accented Latin chars to ASCII
    // - replace unknown codepoints with '?'
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
            continue;
        }

        if (cp >= 0x0300 && cp <= 0x036F) {
            // Unattached combining mark.
            continue;
        }

        char mapped = foldCodepointToAscii(cp);
        norm.push_back(mapped != 0 ? mapped : '?');
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
// Views: PLATFORMS, ROMS, DETAIL, QUEUE, DOWNLOADING, ERROR, DIAGNOSTICS.
static void renderStatus(SDL_Renderer* renderer, const Status& status, const Config& config) {
    if (!renderer) return;

    struct Snapshot {
        Status::View view{Status::View::PLATFORMS};
        std::vector<romm::Platform> platforms;
        std::vector<romm::Game> romsVisible;
        size_t romsStart{0};
        size_t romsCount{0};
        uint64_t romsRevision{0};
        std::vector<romm::QueueItem> queueVisible;
        size_t queueStart{0};
        size_t queueCount{0};
        uint64_t queueTotalBytes{0};
        uint64_t downloadQueueRevision{0};
        uint64_t downloadHistoryRevision{0};
        uint64_t historyCount{0};
        int selectedPlatformIndex{0};
        int selectedRomIndex{0};
        int selectedQueueIndex{0};
        std::string currentPlatformId;
        std::string currentPlatformSlug;
        std::string currentPlatformName;
        std::string romSearchQuery;
        romm::RomFilter romFilter{romm::RomFilter::All};
        romm::RomSort romSort{romm::RomSort::TitleAsc};
        Status::View prevQueueView{Status::View::PLATFORMS};
        Status::View prevDiagnosticsView{Status::View::PLATFORMS};
        bool downloadCompleted{false};
        bool downloadWorkerRunning{false};
        bool lastDownloadFailed{false};
        std::string lastDownloadError;
        std::string currentDownloadTitle;
        int currentDownloadIndex{0};
        int currentDownloadFileCount{0};
        uint64_t totalDownloadBytes{0};
        uint64_t totalDownloadedBytes{0};
        uint64_t currentDownloadSize{0};
        uint64_t currentDownloadedBytes{0};
        uint64_t failedHistoryCount{0};
        std::vector<romm::QueueItem> recentFailed;
        bool netBusy{false};
        uint32_t netBusySinceMs{0};
        std::string netBusyWhat;
        std::string lastError;
        romm::ErrorInfo lastErrorInfo{};
        double lastSpeedMBps{0.0};
        bool queueReorderActive{false};
        bool burnInMode{false};
        bool diagnosticsServerReachableKnown{false};
        bool diagnosticsServerReachable{false};
        bool diagnosticsProbeInFlight{false};
        uint32_t diagnosticsLastProbeMs{0};
        std::string diagnosticsLastProbeDetail;
        bool updateCheckInFlight{false};
        bool updateChecked{false};
        bool updateAvailable{false};
        std::string updateLatestTag;
        std::string updateLatestName;
        std::string updateLatestPublishedAt;
        std::string updateReleaseHtmlUrl;
        std::string updateAssetName;
        uint64_t updateAssetSizeBytes{0};
        bool updateDownloadInFlight{false};
        bool updateDownloaded{false};
        std::string updateStagedPath;
        std::string updateStatus;
        std::string updateError;
    } snap;

    static std::unordered_map<std::string, romm::QueueState> sQueueStateById;
    static uint64_t sQueueRev = 0;
    static uint64_t sHistRev = 0;
    std::vector<romm::QueueItem> rebuildQueueCopy;
    std::vector<romm::QueueItem> rebuildHistCopy;
    uint64_t rebuildQueueRev = 0;
    uint64_t rebuildHistRev = 0;
    bool needRebuildQueueState = false;

    {
        std::lock_guard<std::mutex> guard(status.mutex);
        snap.view = status.currentView;
        snap.platforms = status.platforms; // platforms are typically small; copy is OK
        snap.romsRevision = status.romsRevision;
        snap.romsCount = status.roms.size();
        snap.queueCount = status.downloadQueue.size();
        snap.downloadQueueRevision = status.downloadQueueRevision;
        snap.downloadHistoryRevision = status.downloadHistoryRevision;
        snap.historyCount = status.downloadHistory.size();
        snap.selectedPlatformIndex = status.selectedPlatformIndex;
        snap.selectedRomIndex = status.selectedRomIndex;
        snap.selectedQueueIndex = status.selectedQueueIndex;
        snap.currentPlatformId = status.currentPlatformId;
        snap.currentPlatformSlug = status.currentPlatformSlug;
        snap.currentPlatformName = status.currentPlatformName;
        snap.romSearchQuery = status.romSearchQuery;
        snap.romFilter = status.romFilter;
        snap.romSort = status.romSort;
        snap.prevQueueView = status.prevQueueView;
        snap.prevDiagnosticsView = status.prevDiagnosticsView;
        snap.downloadCompleted = status.downloadCompleted;
        snap.downloadWorkerRunning = status.downloadWorkerRunning.load();
        snap.lastDownloadFailed = status.lastDownloadFailed.load();
        snap.lastDownloadError = status.lastDownloadError;
        snap.currentDownloadTitle = status.currentDownloadTitle;
        snap.currentDownloadIndex = status.currentDownloadIndex.load();
        snap.currentDownloadFileCount = static_cast<int>(status.currentDownloadFileCount.load());
        snap.totalDownloadBytes = status.totalDownloadBytes.load();
        snap.totalDownloadedBytes = status.totalDownloadedBytes.load();
        snap.currentDownloadSize = status.currentDownloadSize.load();
        snap.currentDownloadedBytes = status.currentDownloadedBytes.load();
        snap.netBusy = status.netBusy.load();
        snap.netBusySinceMs = status.netBusySinceMs.load();
        snap.netBusyWhat = status.netBusyWhat;
        snap.lastError = status.lastError;
        snap.lastErrorInfo = status.lastErrorInfo;
        snap.lastSpeedMBps = status.lastSpeedMBps;
        snap.queueReorderActive = status.queueReorderActive;
        snap.burnInMode = status.burnInMode;
        snap.diagnosticsServerReachableKnown = status.diagnosticsServerReachableKnown;
        snap.diagnosticsServerReachable = status.diagnosticsServerReachable;
        snap.diagnosticsProbeInFlight = status.diagnosticsProbeInFlight;
        snap.diagnosticsLastProbeMs = status.diagnosticsLastProbeMs;
        snap.diagnosticsLastProbeDetail = status.diagnosticsLastProbeDetail;
        snap.updateCheckInFlight = status.updateCheckInFlight;
        snap.updateChecked = status.updateChecked;
        snap.updateAvailable = status.updateAvailable;
        snap.updateLatestTag = status.updateLatestTag;
        snap.updateLatestName = status.updateLatestName;
        snap.updateLatestPublishedAt = status.updateLatestPublishedAt;
        snap.updateReleaseHtmlUrl = status.updateReleaseHtmlUrl;
        snap.updateAssetName = status.updateAssetName;
        snap.updateAssetSizeBytes = status.updateAssetSizeBytes;
        snap.updateDownloadInFlight = status.updateDownloadInFlight;
        snap.updateDownloaded = status.updateDownloaded;
        snap.updateStagedPath = status.updateStagedPath;
        snap.updateStatus = status.updateStatus;
        snap.updateError = status.updateError;

        // Copy only the visible slice for large lists to avoid per-frame O(N) copies.
        if (snap.view == Status::View::ROMS) {
            const size_t visible = status.roms.size() < 18 ? status.roms.size() : 18;
            size_t start = 0;
            int sel = status.selectedRomIndex;
            if (sel < 0) sel = 0;
            if (!status.roms.empty() && sel >= (int)status.roms.size()) sel = (int)status.roms.size() - 1;
            if (!status.roms.empty()) {
                if (sel >= (int)(start + visible)) start = (size_t)sel - visible + 1;
                if (sel < (int)start) start = (size_t)sel;
                if (start + visible > status.roms.size()) start = status.roms.size() - visible;
            }
            snap.romsStart = start;
            snap.romsVisible.clear();
            snap.romsVisible.reserve(visible);
            for (size_t i = 0; i < visible; ++i) {
                snap.romsVisible.push_back(status.roms[start + i]);
            }
        } else if (snap.view == Status::View::DETAIL) {
            int sel = status.selectedRomIndex;
            if (sel < 0) sel = 0;
            if (!status.roms.empty() && sel >= (int)status.roms.size()) sel = (int)status.roms.size() - 1;
            if (sel >= 0 && sel < (int)status.roms.size()) {
                snap.romsStart = (size_t)sel;
                snap.romsVisible.clear();
                snap.romsVisible.push_back(status.roms[(size_t)sel]);
            }
        }

        if (snap.view == Status::View::QUEUE || snap.view == Status::View::DOWNLOADING) {
            const size_t visible = status.downloadQueue.size() < 18 ? status.downloadQueue.size() : 18;
            size_t start = 0;
            int sel = status.selectedQueueIndex;
            if (sel < 0) sel = 0;
            if (!status.downloadQueue.empty() && sel >= (int)status.downloadQueue.size()) sel = (int)status.downloadQueue.size() - 1;
            if (!status.downloadQueue.empty()) {
                if (sel >= (int)(start + visible)) start = (size_t)sel - visible + 1;
                if (sel < (int)start) start = (size_t)sel;
                if (start + visible > status.downloadQueue.size()) start = status.downloadQueue.size() - visible;
            }
            snap.queueStart = start;
            snap.queueVisible.clear();
            snap.queueVisible.reserve(visible);
            snap.queueTotalBytes = 0;
            for (size_t i = 0; i < visible; ++i) {
                snap.queueVisible.push_back(status.downloadQueue[start + i]);
            }
            // Total size is small enough to compute under lock; avoids copying the full queue.
            for (const auto& q : status.downloadQueue) {
                snap.queueTotalBytes += q.game.sizeBytes;
            }
        }

        // Rebuild queue state lookup only when queue/history actually change.
        if (snap.downloadQueueRevision != sQueueRev || snap.downloadHistoryRevision != sHistRev) {
            rebuildQueueCopy = status.downloadQueue;
            rebuildHistCopy = status.downloadHistory;
            rebuildQueueRev = snap.downloadQueueRevision;
            rebuildHistRev = snap.downloadHistoryRevision;
            needRebuildQueueState = true;
        }

        constexpr size_t kRecentFailedMax = 3;
        snap.recentFailed.clear();
        for (auto it = status.downloadHistory.rbegin(); it != status.downloadHistory.rend(); ++it) {
            if (it->state != romm::QueueState::Failed && it->state != romm::QueueState::Cancelled) continue;
            snap.failedHistoryCount++;
            if (snap.recentFailed.size() < kRecentFailedMax) {
                snap.recentFailed.push_back(*it);
            }
        }
        std::reverse(snap.recentFailed.begin(), snap.recentFailed.end());
    }
    if (needRebuildQueueState) {
        std::unordered_map<std::string, romm::QueueState> tmp;
        tmp.reserve(rebuildQueueCopy.size() + rebuildHistCopy.size());
        for (const auto& qi : rebuildHistCopy) {
            if (qi.state == romm::QueueState::Failed ||
                qi.state == romm::QueueState::Completed ||
                qi.state == romm::QueueState::Resumable ||
                qi.state == romm::QueueState::Cancelled ||
                qi.state == romm::QueueState::Finalizing) {
                tmp[qi.game.id] = qi.state;
            }
        }
        for (const auto& qi : rebuildQueueCopy) {
            tmp[qi.game.id] = qi.state; // live queue overrides history
        }
        sQueueStateById.swap(tmp);
        sQueueRev = rebuildQueueRev;
        sHistRev = rebuildHistRev;
    }

    // Completion checks are filesystem-expensive; cache per-ROM results on demand for visible rows only.
    struct CompletedEntry {
        bool completed{false};
        std::chrono::steady_clock::time_point at{};
    };
    static std::unordered_map<std::string, CompletedEntry> completedCache;
    static uint64_t completedCacheRevision = 0;
    if (completedCacheRevision != snap.romsRevision) {
        completedCache.clear();
        completedCacheRevision = snap.romsRevision;
    }
    auto nowSteady = std::chrono::steady_clock::now();
    auto completionKey = [](const romm::Game& g) -> std::string {
        return !g.id.empty() ? g.id : g.fsName;
    };
    auto isCompletedOnDiskCached = [&](const romm::Game& g) -> bool {
        std::string key = completionKey(g);
        if (key.empty()) return false;
        auto it = completedCache.find(key);
        if (it != completedCache.end()) {
            if (nowSteady - it->second.at < std::chrono::seconds(5)) {
                return it->second.completed;
            }
        }
        bool found = isGameCompletedOnDisk(g, config);
        completedCache[key] = CompletedEntry{found, nowSteady};
        return found;
    };

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
            case Status::View::DIAGNOSTICS: romm::logLine("View: DIAGNOSTICS"); break;
            case Status::View::UPDATER: romm::logLine("View: UPDATER"); break;
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
            if (snap.burnInMode) {
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
                headerBar = {0, 0, 0, 255};
                footerBar = {0, 0, 0, 255};
            } else {
                SDL_SetRenderDrawColor(renderer, 90, 60, 0, 255);
                headerBar = {140, 100, 20, 255};
            }
            break;
        case Status::View::ERROR:
            SDL_SetRenderDrawColor(renderer, 90, 0, 0, 255);
            headerBar = {150, 20, 20, 255};
            break;
        case Status::View::DIAGNOSTICS:
            SDL_SetRenderDrawColor(renderer, 30, 70, 40, 255);
            headerBar = {40, 120, 70, 255};
            break;
        case Status::View::UPDATER:
            SDL_SetRenderDrawColor(renderer, 16, 20, 70, 255);
            headerBar = {50, 70, 170, 255};
            break;
    }
    SDL_RenderClear(renderer);

    auto drawHeaderBar = [&](const std::string& left, const std::string& right, bool showThrobber) {
        SDL_Rect bar{0, 0, 1280, 52};
        SDL_SetRenderDrawColor(renderer, headerBar.r, headerBar.g, headerBar.b, 255);
        SDL_RenderFillRect(renderer, &bar);
        SDL_Color fg{255,255,255,255};
        // Left-aligned header text.
        drawText(renderer, 32, 14, left, fg, 2);
        // Right-aligned system info.
        if (!right.empty()) {
            int charW = 6 * 2; // glyph width (5) + spacing, scaled by 2
            int textW = static_cast<int>(right.size()) * charW;
            int x = 1280 - 72 - textW; // leave extra padding on the right to avoid clipping battery/time
            if (x < 32) x = 32;
            drawText(renderer, x, 14, right, fg, 2);

            if (showThrobber) {
                static const char* frames[] = {"|", "/", "-", "\\"};
                Uint32 t = SDL_GetTicks();
                const char* frame = frames[(t / 150) % 4];
                std::string thr = std::string("[") + frame + "]";
                int thrW = static_cast<int>(thr.size()) * charW;
                int tx = x - 24 - thrW; // place in dead space between left title and right sys info
                // Keep a minimum gap from left-side header to avoid overlap.
                int leftW = static_cast<int>(left.size()) * charW;
                int minX = 32 + leftW + 24;
                if (tx < minX) tx = minX;
                drawText(renderer, tx, 14, thr, fg, 2);
            }
        }
    };

    auto drawFooterBar = [&](const std::string& left, const std::string& right) {
        SDL_Rect bar{0, 720 - 48, 1280, 48};
        SDL_SetRenderDrawColor(renderer, footerBar.r, footerBar.g, footerBar.b, 255);
        SDL_RenderFillRect(renderer, &bar);
        SDL_Color hint{200, 220, 255, 255};
        drawText(renderer, 32, 720 - 36, left, hint, 2);
    };

    // Cache system time/battery once per second to avoid overcalling services.
    static time_t lastTimeSec = 0;
    static std::string sysInfo;
    time_t nowSec = time(nullptr);
    if (nowSec != lastTimeSec) {
        lastTimeSec = nowSec;
        char buf[16]{};
        struct tm tm{};
        localtime_r(&nowSec, &tm);
        std::snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
        u32 batt = 0;
        if (R_SUCCEEDED(psmGetBatteryChargePercentage(&batt))) {
            std::ostringstream oss;
            oss << buf << "  " << batt << "%";
            sysInfo = oss.str();
        } else {
            sysInfo = buf;
        }
    }

    uint64_t totalBytes = snap.totalDownloadBytes;
    uint64_t totalDoneRaw = snap.totalDownloadedBytes;
    uint64_t curBytes = snap.currentDownloadSize;
    uint64_t curDone = snap.currentDownloadedBytes;
    uint64_t totalDone = (curDone > totalDoneRaw) ? curDone : totalDoneRaw;
    const bool downloadsDone =
        snap.downloadCompleted ||
        (totalBytes > 0 && totalDone >= totalBytes &&
         snap.queueCount == 0 && !snap.downloadWorkerRunning);

    // If we have a speed reading, prepend it to the right-hand status.
    std::vector<std::string> rightParts;
    if (snap.lastSpeedMBps > 0.05) {
        std::ostringstream oss;
        oss << "SPD:" << std::fixed << std::setprecision(1) << snap.lastSpeedMBps << " MB/s";
        rightParts.push_back(oss.str());
    } else if (snap.lastSpeedMBps < 0.0) {
        rightParts.push_back(snap.lastSpeedMBps == -2.0 ? "SPD:err" : "SPD:...");
    }
    rightParts.push_back(sysInfo);
    std::string rightInfo;
    for (size_t i = 0; i < rightParts.size(); ++i) {
        if (i) rightInfo += "  ";
        rightInfo += rightParts[i];
    }

    if (snap.view == Status::View::DOWNLOADING && snap.burnInMode) {
        // Burn-in prevention mode: full black screen with a bouncing "DVD logo"-style block.
        Uint32 nowMs = SDL_GetTicks();
        static bool init = false;
        static float x = 120.0f, y = 120.0f;
        static float vx = 210.0f, vy = 165.0f;
        static Uint32 lastMs = 0;
        if (!init) {
            init = true;
            lastMs = nowMs;
        }
        float dt = (nowMs > lastMs) ? (float)(nowMs - lastMs) / 1000.0f : 0.0f;
        lastMs = nowMs;
        if (dt > 0.25f) dt = 0.25f; // avoid huge jumps on hitches

        uint64_t totalBytes = snap.totalDownloadBytes;
        uint64_t totalDoneRaw = snap.totalDownloadedBytes;
        uint64_t curDone = snap.currentDownloadedBytes;
        uint64_t totalDone = (curDone > totalDoneRaw) ? curDone : totalDoneRaw;
        int pctInt = 0;
        if (totalBytes > 0) {
            float pctTotal = (float)totalDone / (float)std::max<uint64_t>(totalBytes, 1);
            pctTotal = std::clamp(pctTotal, 0.0f, 1.0f);
            pctInt = (int)(pctTotal * 100.0f);
        }
        const bool workerRunning = snap.downloadWorkerRunning;
        const bool queueEmpty = (snap.queueCount == 0);
        const bool finished = (!workerRunning && queueEmpty) &&
                              (snap.downloadCompleted || (totalBytes > 0 && totalDone >= totalBytes));
        const bool empty = (!workerRunning && queueEmpty) &&
                           (!snap.downloadCompleted) &&
                           (totalBytes == 0);

        std::string titleLine;
        std::string label;
        SDL_Color fill{18, 18, 18, 255};
        SDL_Color outline{245, 245, 245, 255};
        SDL_Color text{245, 245, 245, 255};

        if (finished) {
            titleLine = "All Items Finished!";
            label.clear();
            fill = {18, 56, 22, 255};
            outline = {90, 245, 120, 255};
        } else if (empty) {
            titleLine = "Queue Empty";
            label.clear();
            fill = {64, 14, 14, 255};
            outline = {255, 110, 110, 255};
        } else {
            if (!snap.currentDownloadTitle.empty()) {
                titleLine = foldUtf8ToAscii(snap.currentDownloadTitle, true);
            } else {
                titleLine = workerRunning ? "Downloading" : "Preparing...";
            }
            label = (totalBytes > 0)
                ? ("Progress " + std::to_string(pctInt) + "%")
                : "Connecting...";
        }

        // Size the block to match drawText()'s spacing/inset so we never clip glyphs.
        const int scale = 3;
        const int spacing = scale;
        const int inset = scale * 4;
        const int advance = 5 * scale + 2 * spacing; // drawText(): +spacing, draw 5 cols, +(5*scale+spacing)
        const int charH = 7 * scale;
        auto clampToScreen = [&](std::string s) -> std::string {
            // Keep the bouncing block fully on-screen by limiting the rendered glyph count.
            const int padL = 10;
            const int padR = 14;
            const int maxW = 1280 - 40; // leave a small outer margin
            const int maxTextW = std::max(0, maxW - (padL + padR));
            int maxGlyphs = (maxTextW - inset) / advance;
            if (maxGlyphs < 0) maxGlyphs = 0;
            if ((int)s.size() <= maxGlyphs) return s;
            if (maxGlyphs <= 3) return std::string((size_t)std::max(0, maxGlyphs), '.');
            return s.substr(0, (size_t)(maxGlyphs - 3)) + "...";
        };
        titleLine = clampToScreen(titleLine);
        if (!label.empty()) label = clampToScreen(label);

        const int textWTitle = inset + (int)titleLine.size() * advance;
        const int textWLabel = label.empty() ? 0 : (inset + (int)label.size() * advance);
        const int textW = std::max(textWTitle, textWLabel);
        const int padL = 10;
        const int padR = 14;
        const int padY = 10;
        const int gapY = scale * 3;
        int w = padL + textW + padR;
        int h = label.empty()
            ? (charH + padY * 2)
            : ((charH * 2) + gapY + padY * 2);
        if (w < 120) w = 120;
        if (h < 44) h = 44;

        x += vx * dt;
        y += vy * dt;
        if (x < 0) { x = 0; vx = std::fabs(vx); }
        if (y < 0) { y = 0; vy = std::fabs(vy); }
        if (x + w > 1280) { x = (float)(1280 - w); vx = -std::fabs(vx); }
        if (y + h > 720) { y = (float)(720 - h); vy = -std::fabs(vy); }

        SDL_Rect box{ (int)x, (int)y, w, h };
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        SDL_SetRenderDrawColor(renderer, fill.r, fill.g, fill.b, fill.a);
        SDL_RenderFillRect(renderer, &box);
        SDL_SetRenderDrawColor(renderer, outline.r, outline.g, outline.b, outline.a);
        SDL_RenderDrawRect(renderer, &box);
        drawText(renderer, box.x + padL, box.y + padY, titleLine, text, scale);
        if (!label.empty()) {
            drawText(renderer, box.x + padL, box.y + padY + charH + gapY, label, text, scale);
        }
        SDL_RenderPresent(renderer);
        return;
    }

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
            drawText(renderer, outline.x, outline.y + 50, "All files finalized. Press B to return.", fg, 2);
        } else {
            int barWidth = static_cast<int>(pctTotal * 600);
            SDL_Rect bar{ (1280 - 640) / 2, 720/2 - 20, barWidth, 40 };
            SDL_SetRenderDrawColor(renderer, 180, 220, 80, 255);
            SDL_RenderFillRect(renderer, &bar);
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
            SDL_RenderDrawRect(renderer, &outline);
            int pctInt = static_cast<int>(pctTotal * 100.0f);
            std::string titleLine = "Downloading " + snap.currentDownloadTitle;
            if (snap.currentDownloadFileCount > 1) {
                int fileIdx = snap.currentDownloadIndex + 1;
                if (fileIdx < 1) fileIdx = 1;
                if (fileIdx > snap.currentDownloadFileCount) fileIdx = snap.currentDownloadFileCount;
                titleLine += "  (" + std::to_string(fileIdx) + "/" + std::to_string(snap.currentDownloadFileCount) + ")";
            }
            drawText(renderer, outline.x, outline.y - 28, titleLine, fg, 2);
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
                     humanSize(totalBytes) + ")" +
                     (snap.lastSpeedMBps > 0.1 ? ("  @" + [] (double mbps) {
                        std::ostringstream oss;
                        oss << std::fixed << std::setprecision(1) << " " << mbps << " MB/s";
                        return oss.str();
                     }(snap.lastSpeedMBps)) : std::string()),
                     fg, 2);
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
        bool resuming = (snap.currentDownloadedBytes > 0);
        drawText(renderer, (1280/2) - 120, 720/2 - 40, resuming ? "Resuming download..." : "Connecting...", fg, 2);
        std::string line2 = resuming
            ? ("Already have " + humanSize(snap.currentDownloadedBytes) + " on disk")
            : "Waiting for data...";
        drawText(renderer, (1280/2) - 120, 720/2 + 0, line2, fg, 2);
        if (snap.lastDownloadFailed) {
            drawText(renderer, (1280/2) - 120, 720/2 + 30, "Failed: " + snap.lastDownloadError, {255,80,80,255}, 2);
        }
    }

    int selPlat = snap.selectedPlatformIndex;
    int selRom = snap.selectedRomIndex;
    int selQueue = snap.selectedQueueIndex;
    if (selPlat < 0) selPlat = 0;
    if (selPlat >= (int)snap.platforms.size() && !snap.platforms.empty())
        selPlat = (int)snap.platforms.size() - 1;
    if (selRom < 0) selRom = 0;
    if (snap.romsCount > 0 && selRom >= (int)snap.romsCount)
        selRom = (int)snap.romsCount - 1;
    if (selQueue < 0) selQueue = 0;
    if (snap.queueCount > 0 && selQueue >= (int)snap.queueCount)
        selQueue = (int)snap.queueCount - 1;

    auto sanitize = [](const std::string& s) {
        return foldUtf8ToAscii(s, true);
    };

    auto ellipsize = [&](const std::string& s, size_t maxlen) {
        std::string clean = sanitize(s);
        if (clean.size() <= maxlen) return clean;
        return clean.substr(0, maxlen) + "...";
    };
    auto ellipsizeTight = [&](const std::string& s, double maxUnits) {
        std::string clean = sanitize(s);
        std::string out;
        double units = 0.0;
        for (char c : clean) {
            double w = (c == ' ') ? 0.5 : 1.0; // tighter spacing for spaces
            if (units + w > maxUnits) {
                out += "...";
                return out;
            }
            out.push_back(c);
            units += w;
        }
        return out;
    };

    std::optional<romm::QueueState> selectedStateForFooter;
    std::string header;
    std::string controls;
    if (snap.view == Status::View::PLATFORMS) {
        header = "PLATFORMS  Count: " + std::to_string(snap.platforms.size());
        if (selPlat >= 0 && selPlat < (int)snap.platforms.size()) {
            const auto& p = snap.platforms[selPlat];
            if (!p.slug.empty()) header += "  RomM Platform: " + ellipsize(p.slug, 16);
            else if (!p.id.empty()) header += "  RomM Platform: " + ellipsize(p.id, 16);
        }
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
        std::string platName = snap.currentPlatformName;
        if (platName.empty() && selPlat >= 0 && selPlat < (int)snap.platforms.size()) {
            platName = snap.platforms[selPlat].name;
        }
        std::string platLabel;
        if (!platName.empty()) {
            platLabel = ellipsize(platName, 18);
        }
        header = "ROMS " + (platLabel.empty() ? "" : ("[" + platLabel + "] ")) +
                 "Count: " + std::to_string(snap.romsCount);
        header += "  Filter: " + std::string(romFilterLabel(snap.romFilter));
        header += "  Sort: " + std::string(romSortLabel(snap.romSort));
        if (!snap.romSearchQuery.empty()) {
            header += "  Search: " + ellipsize(snap.romSearchQuery, 12);
        }
        SDL_Color fg{255,255,255,255};
        auto drawFilledCircle = [&](int cx, int cy, int r, SDL_Color c) {
            SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, c.a);
            for (int dy = -r; dy <= r; ++dy) {
                int dx = static_cast<int>(std::sqrt(static_cast<double>(r * r - dy * dy)));
                SDL_RenderDrawLine(renderer, cx - dx, cy + dy, cx + dx, cy + dy);
            }
        };
        auto drawCircleOutline = [&](int cx, int cy, int r, SDL_Color c) {
            SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, c.a);
            int x = r;
            int y = 0;
            int err = 0;
            while (x >= y) {
                SDL_RenderDrawPoint(renderer, cx + x, cy + y);
                SDL_RenderDrawPoint(renderer, cx + y, cy + x);
                SDL_RenderDrawPoint(renderer, cx - y, cy + x);
                SDL_RenderDrawPoint(renderer, cx - x, cy + y);
                SDL_RenderDrawPoint(renderer, cx - x, cy - y);
                SDL_RenderDrawPoint(renderer, cx - y, cy - x);
                SDL_RenderDrawPoint(renderer, cx + y, cy - x);
                SDL_RenderDrawPoint(renderer, cx + x, cy - y);
                if (err <= 0) {
                    ++y;
                    err += 2 * y + 1;
                }
                if (err > 0) {
                    --x;
                    err -= 2 * x + 1;
                }
            }
        };
        auto drawBadge = [&](std::optional<romm::QueueState> st, int x, int y) {
            const int r = 7;
            drawCircleOutline(x + r, y + r, r, SDL_Color{0,0,0,220});
            if (!st.has_value()) {
                drawCircleOutline(x + r, y + r, r - 2, SDL_Color{100,100,100,180});
                return;
            }
            switch (*st) {
                case romm::QueueState::Pending:     // grey = queued
                    drawFilledCircle(x + r, y + r, r - 2, SDL_Color{140,140,140,255});
                    break;
                case romm::QueueState::Downloading: // white = active download
                    drawFilledCircle(x + r, y + r, r - 2, SDL_Color{230,230,230,255});
                    break;
                case romm::QueueState::Finalizing:  // yellow = moving/renaming
                    drawFilledCircle(x + r, y + r, r - 2, SDL_Color{200,200,120,255});
                    break;
                case romm::QueueState::Completed:   // green = done
                    drawFilledCircle(x + r, y + r, r - 2, SDL_Color{50,200,110,255});
                    break;
                case romm::QueueState::Resumable:   // orange = resumable
                    drawFilledCircle(x + r, y + r, r - 2, SDL_Color{230,150,60,255});
                    break;
                case romm::QueueState::Failed:      // red = failed
                    drawFilledCircle(x + r, y + r, r - 2, SDL_Color{220,70,70,255});
                    break;
                case romm::QueueState::Cancelled:   // amber = cancelled by user
                    drawFilledCircle(x + r, y + r, r - 2, SDL_Color{255,180,80,255});
                    break;
            }
        };
        const size_t visible = snap.romsVisible.size();
        const size_t start = snap.romsStart;
        if (gRomsDebugFrames > 0) {
            romm::logDebug("Render ROMS dbg: count=" + std::to_string(snap.romsCount) +
                           " showing=" + std::to_string(visible) +
                           " start=" + std::to_string(start) +
                           " sel=" + std::to_string(selRom),
                           "UI");
            if (!snap.romsVisible.empty()) {
                romm::logDebug(" ROM[v0]=" + ellipsize(snap.romsVisible[0].title, 60), "UI");
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
        if (snap.romsCount == 0) {
            if (snap.netBusy) {
                drawText(renderer, 64, 96, "Loading ROM list...", fg, 2);
            } else {
                drawText(renderer, 64, 96, "No ROMs found for this platform.", fg, 2);
            }
        }
        if (selRom >= 0 && selRom < (int)snap.romsCount && !snap.romsVisible.empty()) {
            size_t selOffset = 0;
            if ((size_t)selRom >= start && (size_t)selRom < start + visible) {
                selOffset = (size_t)selRom - start;
            }
            const auto& gsel = snap.romsVisible[selOffset];
            if (isCompletedOnDiskCached(gsel)) {
                selectedStateForFooter = romm::QueueState::Completed;
            } else if (auto it = sQueueStateById.find(gsel.id); it != sQueueStateById.end()) {
                selectedStateForFooter = it->second;
            }
        }
        for (size_t i = 0; i < visible; ++i) {
            size_t idx = start + i;
            SDL_Rect r{ 64, 88 + static_cast<int>(i)*26, 1008, 22 };
            if ((int)idx == selRom)
                SDL_SetRenderDrawColor(renderer, 80, 150, 240, 255);
            else
                SDL_SetRenderDrawColor(renderer, 34, 90, 140, 200);
            SDL_RenderFillRect(renderer, &r);
            const auto& g = snap.romsVisible[i];
            // TODO(UI): switch long titles to a scrolling marquee instead of hard ellipsis.
            drawText(renderer, r.x + 12, r.y + 4, ellipsizeTight(g.title, 43.0), fg, 2);
            std::string sz = humanSize(g.sizeBytes);
            drawText(renderer, r.x + 760, r.y + 4, sz, fg, 2);
            std::optional<romm::QueueState> st;
            if (auto it = sQueueStateById.find(g.id); it != sQueueStateById.end()) {
                st = it->second;
            }
            if (isCompletedOnDiskCached(g)) {
                st = romm::QueueState::Completed;
            }
            drawBadge(st, r.x + 930, r.y + 4);
        }
    } else if (snap.view == Status::View::DETAIL) {
        header = "DETAIL";
        SDL_Color fg{255,255,255,255};
        if (!snap.romsVisible.empty()) {
            const auto& g = snap.romsVisible[0];
            header = "DETAIL [" + ellipsize(g.title, 22) + "]";
            SDL_Rect cover{70, 110, 240, 240};
            if (g.coverUrl.empty()) {
                SDL_SetRenderDrawColor(renderer, 90, 125, 180, 255);
                SDL_RenderFillRect(renderer, &cover);
                drawText(renderer, cover.x + 12, cover.y + cover.h / 2 - 8, "No cover URL", fg, 2);
            } else if (gCoverTexture && g.coverUrl == gCoverTextureUrl) {
                SDL_RenderCopy(renderer, gCoverTexture, nullptr, &cover);
            } else {
                SDL_SetRenderDrawColor(renderer, 90, 125, 180, 255);
                SDL_RenderFillRect(renderer, &cover);
                drawText(renderer, cover.x + 12, cover.y + cover.h / 2 - 8, "Loading cover...", fg, 2);
                romm::CoverJob job{g.coverUrl, g.title, config};
                if (g.coverUrl != gLastCoverRequested) {
                    romm::logLine("Requesting cover: " + g.coverUrl);
                    gLastCoverRequested = g.coverUrl;
                } else {
                    romm::logDebug("Cover already requested: " + g.coverUrl, "COVER");
                }
                gCoverLoader.request(job, gCoverTextureUrl);
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
            std::string qCount = "Queue size=" + std::to_string(snap.queueCount);
            drawText(renderer, info.x + 16, info.y + 176, qCount, fg, 2);
            drawText(renderer, 80, 420, "A=Queue and open queue   B=Back   Y=Queue view", fg, 2);
        } else {
            drawText(renderer, 80, 120, "No ROM selected.", fg, 2);
        }
    } else if (snap.view == Status::View::QUEUE) {
        uint64_t total = snap.queueTotalBytes;
        header = "QUEUE  Items: " + std::to_string(snap.queueCount) +
                 "  Total: " + humanSize(snap.queueTotalBytes);
        if (snap.failedHistoryCount > 0) {
            header += "  Failed: " + std::to_string(snap.failedHistoryCount);
        }
        SDL_Color fg{255,255,255,255};
        size_t visible = snap.queueVisible.size();
        size_t start = snap.queueStart;
        int listHeight = static_cast<int>(visible) * 26 + 60;
        int maxListHeight = 720 - 96 - 60;
        if (listHeight > maxListHeight) listHeight = maxListHeight;
        if (listHeight < 200) listHeight = 200;
        SDL_Rect listBg{48, 96, 1040, listHeight};
        SDL_SetRenderDrawColor(renderer, 90, 60, 150, 180);
        SDL_RenderFillRect(renderer, &listBg);
        drawText(renderer, 64, 70, "Total size: " + humanSize(total), fg, 2);
        if (snap.queueCount == 0) {
            std::string msg = snap.downloadCompleted ? "All downloads complete." : "Queue empty. Press A in detail to add.";
            drawText(renderer, 64, 120, msg, fg, 2);
            int failY = 152;
            if (!snap.recentFailed.empty()) {
                drawText(renderer, 64, failY, "Recent failures:", SDL_Color{255,210,210,255}, 2);
                failY += 24;
                for (const auto& q : snap.recentFailed) {
                    std::string detail = ellipsize(q.game.title + (q.error.empty() ? "" : (": " + q.error)), 62);
                    drawText(renderer, 64, failY, detail, SDL_Color{255,160,160,255}, 2);
                    failY += 24;
                }
            }
        }
        for (size_t i = 0; i < visible; ++i) {
            size_t idx = start + i;
            SDL_Rect r{ 64, 120 + static_cast<int>(i)*26, 1008, 22 };
            if ((int)idx == selQueue)
                SDL_SetRenderDrawColor(renderer, 150, 110, 230, 255);
            else
                SDL_SetRenderDrawColor(renderer, 110, 70, 180, 200);
            SDL_RenderFillRect(renderer, &r);
            if (i < snap.queueVisible.size()) {
                const auto& q = snap.queueVisible[i];
                drawText(renderer, r.x + 10, r.y + 4, ellipsize(q.game.title, 58), fg, 2);
                uint64_t qSize = q.bundle.totalSize();
                if (qSize == 0) qSize = q.game.sizeBytes;
                std::string sz = humanSize(qSize);
                std::string stateStr;
                switch (q.state) {
                    case romm::QueueState::Pending: stateStr = "pending"; break;
                    case romm::QueueState::Downloading: stateStr = "downloading"; break;
                    case romm::QueueState::Finalizing: stateStr = "finalizing"; break;
                    case romm::QueueState::Completed: stateStr = "done"; break;
                    case romm::QueueState::Resumable: stateStr = "resumable"; break;
                    case romm::QueueState::Failed: stateStr = "failed"; break;
                    case romm::QueueState::Cancelled: stateStr = "cancelled"; break;
                }
                drawText(renderer, r.x + 680, r.y + 4, sz + " " + stateStr, fg, 2);
                if ((q.state == romm::QueueState::Failed || q.state == romm::QueueState::Resumable || q.state == romm::QueueState::Cancelled) && !q.error.empty()) {
                    drawText(renderer, r.x + 10, r.y + 22, ellipsize(q.error, 58), SDL_Color{255,160,160,255}, 2);
                }
            }
        }
    } else if (snap.view == Status::View::DOWNLOADING) {
        header = "DOWNLOADING";
    } else if (snap.view == Status::View::DIAGNOSTICS) {
        header = "DIAGNOSTICS";
        SDL_Color fg{255,255,255,255};
        SDL_Color sub{210,240,220,255};
        SDL_Rect box{64, 96, 1280 - 128, 720 - 96 - 64 - 48};
        SDL_SetRenderDrawColor(renderer, 10, 60, 28, 220);
        SDL_RenderFillRect(renderer, &box);
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 90);
        SDL_RenderDrawRect(renderer, &box);

        uint64_t freeBytes = romm::getFreeSpace(config.downloadDir);
        std::string reach = "Unknown";
        if (snap.diagnosticsProbeInFlight) {
            reach = "Checking...";
        } else if (snap.diagnosticsServerReachableKnown) {
            reach = snap.diagnosticsServerReachable ? "Reachable" : "Unreachable";
        }

        int y = box.y + 18;
        drawText(renderer, box.x + 16, y, "Config", fg, 2); y += 26;
        drawText(renderer, box.x + 16, y, "Server: " + ellipsize(config.serverUrl, 58), sub, 2); y += 24;
        drawText(renderer, box.x + 16, y, "DownloadDir: " + ellipsize(config.downloadDir, 50), sub, 2); y += 24;
        drawText(renderer, box.x + 16, y,
                 "Timeout: " + std::to_string(config.httpTimeoutSeconds) +
                 "s  FAT32: " + std::string(config.fat32Safe ? "true" : "false") +
                 "  Log: " + config.logLevel,
                 sub, 2); y += 30;

        drawText(renderer, box.x + 16, y, "Health", fg, 2); y += 26;
        drawText(renderer, box.x + 16, y, "Server: " + reach, sub, 2); y += 24;
        if (!snap.diagnosticsLastProbeDetail.empty()) {
            drawText(renderer, box.x + 16, y, "Probe: " + ellipsize(snap.diagnosticsLastProbeDetail, 62), sub, 2); y += 24;
        } else {
            drawText(renderer, box.x + 16, y, "Probe: (none yet)", sub, 2); y += 24;
        }
        drawText(renderer, box.x + 16, y, "SD Free: " + humanSize(freeBytes), sub, 2); y += 30;

        drawText(renderer, box.x + 16, y, "Queue", fg, 2); y += 26;
        drawText(renderer, box.x + 16, y,
                 "Active: " + std::to_string(snap.queueCount) +
                 "  History: " + std::to_string(snap.historyCount) +
                 "  Downloading: " + std::string(snap.downloadWorkerRunning ? "yes" : "no"),
                 sub, 2); y += 30;

        drawText(renderer, box.x + 16, y, "Last Error", fg, 2); y += 26;
        std::string errHead = std::string(romm::errorCategoryLabel(snap.lastErrorInfo.category)) +
                              " / " + romm::errorCodeLabel(snap.lastErrorInfo.code);
        drawText(renderer, box.x + 16, y, errHead, sub, 2); y += 24;
        drawText(renderer, box.x + 16, y, ellipsize(snap.lastError.empty() ? "(none)" : snap.lastError, 64), sub, 2); y += 24;
        drawText(renderer, box.x + 16, box.y + box.h - 52,
                 "A=export summary to log  B=back  R=refresh probe",
                 fg, 2);
    } else if (snap.view == Status::View::UPDATER) {
        header = "UPDATER";
        SDL_Color fg{255,255,255,255};
        SDL_Color sub{220,230,255,255};
        SDL_Color warn{255,210,160,255};
        SDL_Color ok{190,255,210,255};
        SDL_Color bad{255,170,170,255};

        SDL_Rect box{64, 96, 1280 - 128, 720 - 96 - 64 - 48};
        SDL_SetRenderDrawColor(renderer, 10, 12, 40, 230);
        SDL_RenderFillRect(renderer, &box);
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 90);
        SDL_RenderDrawRect(renderer, &box);

        int y = box.y + 18;
        drawText(renderer, box.x + 16, y, "Update", fg, 2); y += 26;
        drawText(renderer, box.x + 16, y, std::string("Current: v") + romm::appVersion(), sub, 2); y += 24;

        std::string latestLine = "Latest: (not checked)";
        if (snap.updateCheckInFlight) {
            latestLine = "Latest: checking...";
        } else if (snap.updateChecked) {
            latestLine = "Latest: " + (snap.updateLatestTag.empty() ? std::string("(unknown)") : snap.updateLatestTag);
            if (!snap.updateLatestPublishedAt.empty()) {
                latestLine += "  " + ellipsize(snap.updateLatestPublishedAt, 20);
            }
        }
        drawText(renderer, box.x + 16, y, latestLine, sub, 2); y += 30;

        if (!snap.updateError.empty()) {
            drawText(renderer, box.x + 16, y, "Error: " + ellipsize(snap.updateError, 64), bad, 2); y += 26;
        } else if (!snap.updateStatus.empty()) {
            drawText(renderer, box.x + 16, y, "Status: " + ellipsize(snap.updateStatus, 64), warn, 2); y += 26;
        }

        if (snap.updateChecked) {
            if (snap.updateAvailable) {
                std::string up = "Update available.";
                if (!snap.updateLatestName.empty()) up += "  " + ellipsize(snap.updateLatestName, 40);
                drawText(renderer, box.x + 16, y, up, ok, 2); y += 24;
                if (!snap.updateAssetName.empty()) {
                    drawText(renderer, box.x + 16, y,
                             "Asset: " + ellipsize(snap.updateAssetName, 42) +
                             "  " + humanSize(snap.updateAssetSizeBytes),
                             sub, 2);
                    y += 24;
                }
            } else {
                drawText(renderer, box.x + 16, y, "You're up to date.", ok, 2); y += 24;
            }
        }

        if (snap.updateDownloaded) {
            drawText(renderer, box.x + 16, y, "Update downloaded.", ok, 2); y += 24;
            if (!snap.updateStagedPath.empty()) {
                drawText(renderer, box.x + 16, y, "Staged: " + ellipsize(snap.updateStagedPath, 62), sub, 2); y += 24;
            }
            drawText(renderer, box.x + 16, y, "Restart the app to apply.", warn, 2); y += 24;
        } else if (snap.updateDownloadInFlight) {
            drawText(renderer, box.x + 16, y, "Downloading update...", warn, 2); y += 24;
        }

        drawText(renderer, box.x + 16, box.y + box.h - 52,
                 "A=check updates  X=download update  B=back  Plus=exit",
                 fg, 2);
    } else if (snap.view == Status::View::ERROR) {
        header = "ERROR";
        SDL_Color fg{255,255,255,255};
        SDL_Color sub{255,200,200,255};
        SDL_Rect box{64, 96, 1280 - 128, 720 - 96 - 64 - 48};
        SDL_SetRenderDrawColor(renderer, 60, 0, 0, 220);
        SDL_RenderFillRect(renderer, &box);
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 90);
        SDL_RenderDrawRect(renderer, &box);

        auto wrapLines = [](const std::string& s, size_t maxChars) -> std::vector<std::string> {
            std::vector<std::string> out;
            std::string cur;
            cur.reserve(maxChars + 8);

            auto flush = [&]() {
                if (!cur.empty()) out.push_back(cur);
                cur.clear();
            };

            size_t i = 0;
            while (i < s.size()) {
                // Respect explicit newlines.
                if (s[i] == '\n') {
                    flush();
                    ++i;
                    continue;
                }
                // Skip repeated whitespace (but not newlines).
                if (std::isspace(static_cast<unsigned char>(s[i]))) {
                    ++i;
                    continue;
                }
                // Read a word token.
                size_t j = i;
                while (j < s.size() && s[j] != '\n' && !std::isspace(static_cast<unsigned char>(s[j]))) ++j;
                std::string word = s.substr(i, j - i);
                i = j;

                if (cur.empty()) {
                    // If the token itself is too long, hard-wrap it.
                    while (word.size() > maxChars) {
                        out.push_back(word.substr(0, maxChars));
                        word.erase(0, maxChars);
                    }
                    cur = word;
                } else if (cur.size() + 1 + word.size() <= maxChars) {
                    cur.push_back(' ');
                    cur += word;
                } else {
                    flush();
                    while (word.size() > maxChars) {
                        out.push_back(word.substr(0, maxChars));
                        word.erase(0, maxChars);
                    }
                    cur = word;
                }
            }
            flush();
            return out;
        };

        std::string user = snap.lastErrorInfo.userMessage.empty() ? "Unexpected error." : snap.lastErrorInfo.userMessage;
        std::string codeLine = std::string("Type: ") +
                               romm::errorCategoryLabel(snap.lastErrorInfo.category) +
                               " / " + romm::errorCodeLabel(snap.lastErrorInfo.code);
        if (snap.lastErrorInfo.httpStatus > 0) {
            codeLine += "  HTTP " + std::to_string(snap.lastErrorInfo.httpStatus);
        }
        codeLine += snap.lastErrorInfo.retryable ? "  Retry: yes" : "  Retry: no";

        drawText(renderer, box.x + 16, box.y + 16, user, fg, 2);
        drawText(renderer, box.x + 16, box.y + 38, codeLine, sub, 2);

        std::string reason = snap.lastError.empty() ? "Unknown error." : snap.lastError;
        drawText(renderer, box.x + 16, box.y + 64, "Detail:", fg, 2);

        auto lines = wrapLines(reason, 78);
        int y = box.y + 92;
        for (size_t li = 0; li < lines.size() && li < 12; ++li) {
            drawText(renderer, box.x + 16, y, lines[li], sub, 2);
            y += 22;
        }

        int hintY = box.y + box.h - 78;
        drawText(renderer, box.x + 16, hintY, "Check log: sdmc:/switch/romm_switch_client/log.txt", fg, 2);
        drawText(renderer, box.x + 16, hintY + 26, "Press B or Plus to exit.", fg, 2);
    }

    switch (snap.view) {
        case Status::View::PLATFORMS:
            controls = "A=open platform B=back Y=queue R=diagnostics L=updater Plus=exit D-Pad=scroll hold";
            break;
        case Status::View::ROMS:
            controls = "A=details B=back Y=queue Minus=search DPad L/R=filter/sort";
            break;
        case Status::View::DETAIL:
            controls = "A=queue+open B=back Y=queue Plus=exit";
            break;
        case Status::View::QUEUE:
            if (snap.queueReorderActive) {
                controls = snap.downloadWorkerRunning
                    ? "DPad=move A=drop B=drop Minus=delete X=view downloading Plus=exit"
                    : "DPad=move A=drop B=drop Minus=delete X=start downloads Plus=exit";
            } else {
                controls = snap.downloadWorkerRunning
                    ? "A=select DPad=scroll X=view downloading B=back Plus=exit"
                    : "A=select DPad=scroll X=start downloads B=back Plus=exit";
            }
            break;
        case Status::View::DOWNLOADING:
            controls = snap.burnInMode ? "R=burn-in off B=back Plus=exit" : "R=burn-in B=back Plus=exit";
            break;
        case Status::View::ERROR:
            controls = "B=exit Plus=exit";
            break;
        case Status::View::DIAGNOSTICS:
            controls = "A=export summary B=back R=refresh Plus=exit";
            break;
        case Status::View::UPDATER:
            controls = "A=check X=download B=back Plus=exit";
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

    std::string footerRight;
    std::string footerStatusValue;
    if (snap.view == Status::View::ROMS) {
        auto stateToText = [](const std::optional<romm::QueueState>& st) -> std::string {
            if (!st.has_value()) return "not queued";
            switch (*st) {
                case romm::QueueState::Pending:     return "queued";
                case romm::QueueState::Downloading: return "downloading";
                case romm::QueueState::Finalizing:  return "finalizing";
                case romm::QueueState::Completed:   return "completed";
                case romm::QueueState::Resumable:   return "resumable";
                case romm::QueueState::Failed:      return "failed";
                case romm::QueueState::Cancelled:   return "cancelled";
            }
            return "unknown";
        };
        footerStatusValue = stateToText(selectedStateForFooter);
    }

    if (!header.empty()) {
        bool showThr = false;
        if (snap.netBusy) {
            Uint32 nowMs = SDL_GetTicks();
            if (nowMs >= snap.netBusySinceMs && (nowMs - snap.netBusySinceMs) >= 2000) {
                showThr = true;
            }
        }
        drawHeaderBar(header, rightInfo, showThr);
    }
    if (!controls.empty()) {
        // Draw only the left footer text here; status is drawn with fixed positioning below.
        drawFooterBar(controls, "");
        if (snap.view == Status::View::ROMS) {
            SDL_Color hint{200, 220, 255, 255};
            const int labelX = 960;
            const int valueX = labelX + 110; // extra spacing to avoid clipping long values
            drawText(renderer, labelX, 720 - 36, "Status:", hint, 2);
            drawText(renderer, valueX, 720 - 36, footerStatusValue, hint, 2);
        } else if (!footerRight.empty()) {
            // Fallback for other views if right text is provided.
            SDL_Color hint{200, 220, 255, 255};
            int charW = 6 * 2;
            int textW = static_cast<int>(footerRight.size()) * charW;
            int x = 1280 - 160 - textW;
            if (x < 32) x = 32;
            drawText(renderer, x, 720 - 36, footerRight, hint, 2);
        }
    }

    SDL_RenderPresent(renderer);
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    socketInitializeDefault();
    std::thread speedTestThread;
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
    timeInitialize();
    psmInitialize();

    // Initialize logging after SD is mounted so early messages persist.
    romm::initLogFile();
    romm::logLine("Startup.");

    std::string selfNroPath;
    std::string stagedNroPath;

    // Determine our running NRO path (used for staging self-updates).
    // On Switch homebrew, argv[0] is typically the full sdmc:/... path to the .nro.
    if (argc > 0 && argv && argv[0]) {
        selfNroPath = argv[0];
    }
    if (selfNroPath.find(".nro") == std::string::npos) {
        // Fallback to the default expected install path.
        selfNroPath = "sdmc:/switch/romm_switch_client/romm-switch-client.nro";
    }
    stagedNroPath = selfNroPath + ".new";
    romm::logLine("Self NRO path: " + selfNroPath);

    auto fileLooksLikeNro = [&](const std::string& path) -> bool {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return false;
        char magic[4]{};
        size_t n = std::fread(magic, 1, sizeof(magic), f);
        std::fclose(f);
        return n == sizeof(magic) && magic[0] == 'N' && magic[1] == 'R' && magic[2] == 'O' && magic[3] == '0';
    };

    // If a staged update exists from a prior session, apply it before starting the UI.
    {
        std::error_code ec;
        const bool stagedExists = std::filesystem::exists(stagedNroPath, ec);
        if (!ec && stagedExists && fileLooksLikeNro(stagedNroPath)) {
            std::string bak = selfNroPath + ".bak";
            // Best-effort remove old backup.
            std::filesystem::remove(bak, ec);
            ec.clear();
            std::filesystem::rename(selfNroPath, bak, ec);
            if (ec) {
                romm::logLine("Self-update apply: could not backup current NRO: " + ec.message());
            }
            ec.clear();
            std::filesystem::rename(stagedNroPath, selfNroPath, ec);
            if (ec) {
                romm::logLine("Self-update apply failed: " + ec.message());
                // Try to restore backup if we created one.
                std::error_code ec2;
                if (std::filesystem::exists(bak, ec2) && !ec2) {
                    std::filesystem::rename(bak, selfNroPath, ec2);
                }
            } else {
                romm::logLine("Self-update applied successfully.");
            }
        }
    }

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
    constexpr uint32_t kPlatformRomsCacheTtlMs = 2 * 60 * 1000; // 2 minutes
    constexpr size_t kPlatformRomsCacheMaxEntries = 2;
    constexpr size_t kRomsFirstPageLimit = 250;
    constexpr size_t kRomsNextPageLimit = 500;
    constexpr size_t kRemoteSearchThreshold = 1200;
    constexpr size_t kRemoteSearchLimit = 250;
    Config config;
    Status status;
    {
        std::error_code ec;
        bool stagedExists = std::filesystem::exists(stagedNroPath, ec) && !ec;
        std::lock_guard<std::mutex> lock(status.mutex);
        status.updateStagedPath = stagedNroPath;
        status.updateDownloaded = stagedExists;
        status.updateStatus = stagedExists ? "Update staged; restart app to apply." : "Press A to check for updates.";
    }
    struct CachedPlatformRoms {
        std::vector<romm::Game> games;
        std::string slug;
        std::string name;
        std::string identifierDigest;
        uint32_t fetchedAtMs{0};
    };
    std::unordered_map<std::string, CachedPlatformRoms> platformRomsCache;
    uint32_t currentPlatformFetchedAtMs = 0;
    std::string currentPlatformIdentifierDigest;
    size_t pagedFetchNextOffset = 0;
    size_t pagedFetchPageLimit = kRomsNextPageLimit;
    std::vector<romm::Game> remoteSearchGames;
    bool remoteSearchActive = false;
    std::string remoteSearchQuery;
    std::string remoteSearchPlatformId;
    uint64_t remoteSearchRevision = 0;
    uint64_t remoteSearchGeneration = 0;
    bool remoteSearchInFlight = false;
    struct PendingRomFetch {
        enum class Mode { Probe, Page } mode{Mode::Page};
        std::string pid;
        std::string slug;
        std::string name;
        std::string cachedIdentifierDigest;
        size_t offset{0};
        size_t limit{kRomsFirstPageLimit};
        uint64_t generation{0};
    };
    struct RomFetchResult {
        PendingRomFetch req;
        bool ok{false};
        std::vector<romm::Game> games;
        size_t offset{0};
        size_t limit{0};
        bool hasMore{false};
        size_t nextOffset{0};
        size_t total{0};
        bool totalKnown{false};
        bool probeOnly{false};
        bool probeUnchanged{false};
        bool probeFailed{false};
        std::string identifierDigest;
        std::string error;
        romm::ErrorInfo errorInfo{};
    };
    struct PendingRemoteSearch {
        std::string pid;
        std::string query;
        size_t limit{kRemoteSearchLimit};
        uint64_t generation{0};
    };
    struct RemoteSearchResult {
        PendingRemoteSearch req;
        bool ok{false};
        std::vector<romm::Game> games;
        std::string error;
        romm::ErrorInfo errorInfo{};
    };
    struct DiagProbeReq {
        uint64_t generation{0};
    };
    struct DiagProbeResult {
        uint64_t generation{0};
        bool ok{false};
        std::string detail;
        romm::ErrorInfo errorInfo{};
    };
    struct UpdateCheckReq {
        uint64_t generation{0};
    };
    struct UpdateCheckResult {
        uint64_t generation{0};
        bool ok{false};
        romm::GitHubRelease release;
        romm::GitHubAsset asset;
        bool updateAvailable{false};
        std::string error;
        romm::ErrorInfo errorInfo{};
    };
    struct UpdateDownloadReq {
        uint64_t generation{0};
        std::string url;
        std::string outPath;
    };
    struct UpdateDownloadResult {
        uint64_t generation{0};
        bool ok{false};
        std::string outPath;
        uint64_t bytes{0};
        std::string error;
        romm::ErrorInfo errorInfo{};
    };
    romm::LatestJobWorker<PendingRomFetch, RomFetchResult> romFetchJobs;
    romm::LatestJobWorker<PendingRemoteSearch, RemoteSearchResult> remoteSearchJobs;
    romm::LatestJobWorker<DiagProbeReq, DiagProbeResult> diagProbeJobs;
    romm::LatestJobWorker<UpdateCheckReq, UpdateCheckResult> updateCheckJobs;
    romm::LatestJobWorker<UpdateDownloadReq, UpdateDownloadResult> updateDownloadJobs;
    uint64_t updateGeneration = 0;
    uint64_t updateCheckGenSubmitted = 0;
    uint64_t updateDownloadGenSubmitted = 0;
    std::string cfgError;
    romm::ErrorInfo cfgErrInfo;
    bool running = true;
    uint64_t appliedRomsAllRev = 0;
    uint64_t appliedRomsOptionsRev = 0;
    uint64_t appliedQueueRevForRoms = 0;
    uint64_t appliedHistRevForRoms = 0;
    ScrollHold scrollHold;
    auto resetNav = [&]() { status.navStack.clear(); };

    auto prunePlatformCache = [&]() {
        while (platformRomsCache.size() > kPlatformRomsCacheMaxEntries) {
            auto oldest = platformRomsCache.begin();
            for (auto it = platformRomsCache.begin(); it != platformRomsCache.end(); ++it) {
                if (it->second.fetchedAtMs < oldest->second.fetchedAtMs) oldest = it;
            }
            platformRomsCache.erase(oldest);
        }
    };

    // Background ROM fetch logic: main thread submits requests and applies results via poll.
    auto runRomFetch = [&](const PendingRomFetch& req) -> RomFetchResult {
        RomFetchResult out;
        out.req = req;
        std::string err;
        romm::ErrorInfo errInfo;

        if (req.mode == PendingRomFetch::Mode::Probe) {
            out.probeOnly = true;
            std::string digest;
            if (!romm::fetchRomsIdentifiersDigest(config, req.pid, digest, err, &errInfo)) {
                out.ok = true;
                out.probeFailed = true;
                out.error = err;
                out.errorInfo = errInfo;
                return out;
            }
            out.ok = true;
            out.identifierDigest = digest;
            out.probeUnchanged = !digest.empty() && digest == req.cachedIdentifierDigest;
            return out;
        }

        romm::GamesPage page;
        if (!romm::fetchGamesPageForPlatform(config, req.pid, req.offset, req.limit, page, err, &errInfo)) {
            out.ok = false;
            out.error = err;
            out.errorInfo = errInfo;
            return out;
        }

        std::vector<romm::Game> games = std::move(page.games);

        // Guardrail: Some server versions may ignore platform_id filter and return all ROMs.
        if (!req.pid.empty()) {
            bool anyDifferentId = false;
            bool anyHasId = false;
            for (const auto& r : games) {
                if (!r.platformId.empty()) {
                    anyHasId = true;
                    if (r.platformId != req.pid) { anyDifferentId = true; break; }
                }
            }
            if (anyHasId && anyDifferentId) {
                size_t before = games.size();
                games.erase(std::remove_if(games.begin(), games.end(),
                    [&](const romm::Game& r) {
                        return !r.platformId.empty() && r.platformId != req.pid;
                    }),
                    games.end());
                romm::logLine("Client-side filtered ROMs by platform_id: " +
                              std::to_string(before) + " -> " + std::to_string(games.size()));
            }
        }

        if (!req.slug.empty()) {
            bool anyDifferentSlug = false;
            bool anyHasSlug = false;
            for (const auto& r : games) {
                if (!r.platformSlug.empty()) {
                    anyHasSlug = true;
                    if (r.platformSlug != req.slug) { anyDifferentSlug = true; break; }
                }
            }
            if (anyHasSlug && anyDifferentSlug) {
                size_t before = games.size();
                games.erase(std::remove_if(games.begin(), games.end(),
                    [&](const romm::Game& r) {
                        return !r.platformSlug.empty() && r.platformSlug != req.slug;
                    }),
                    games.end());
                romm::logLine("Client-side filtered ROMs by platform_slug: " +
                              std::to_string(before) + " -> " + std::to_string(games.size()));
            }
            for (auto& r : games) {
                if (r.platformSlug.empty()) r.platformSlug = req.slug;
            }
        }

        if (req.offset == 0) {
            std::string digest;
            std::string digestErr;
            if (romm::fetchRomsIdentifiersDigest(config, req.pid, digest, digestErr, nullptr)) {
                out.identifierDigest = digest;
            }
        }

        out.ok = true;
        out.offset = page.offset;
        out.limit = page.limit;
        out.hasMore = page.hasMore;
        out.nextOffset = page.offset + page.games.size();
        out.total = page.total;
        out.totalKnown = page.totalKnown;
        out.games = std::move(games);
        return out;
    };

    auto runRemoteSearch = [&](const PendingRemoteSearch& req) -> RemoteSearchResult {
        RemoteSearchResult out;
        out.req = req;
        std::string err;
        romm::ErrorInfo info;
        if (!romm::searchGamesRemote(config, req.pid, req.query, req.limit, out.games, err, &info)) {
            out.ok = false;
            out.error = err;
            out.errorInfo = info;
            return out;
        }
        out.ok = true;
        return out;
    };

    auto runDiagProbe = [&]() -> DiagProbeResult {
        DiagProbeResult out;
        std::string body;
        std::string err;
        romm::ErrorInfo info;
        const std::string url = config.serverUrl + "/api/platforms?limit=1";
        if (!romm::fetchBinary(config, url, body, err, &info)) {
            out.ok = false;
            out.detail = err;
            out.errorInfo = info;
            return out;
        }
        out.ok = true;
        out.detail = "HTTP OK";
        return out;
    };

    auto filterNeedsState = [](romm::RomFilter f) {
        return f != romm::RomFilter::All;
    };

    // Rebuild `status.roms` from `status.romsAll` using cached normalized titles.
    // Must be called with `status.mutex` held.
    auto rebuildVisibleRomsLocked = [&](bool resetSelection) {
        static uint64_t sIndexBuiltFor = 0;
        static std::vector<std::string> sNormalizedTitles;
        static bool sIndexBuiltForRemote = false;
        static uint64_t sCompletionCacheBuiltFor = 0;
        static std::unordered_map<std::string, bool> sCompletionById;
        static bool sCompletionBuiltForRemote = false;

        const bool useRemoteSource = remoteSearchActive &&
                                     status.currentPlatformId == remoteSearchPlatformId &&
                                     status.romSearchQuery == remoteSearchQuery;
        const auto& sourceRoms = useRemoteSource ? remoteSearchGames : status.romsAll;
        const uint64_t sourceRev = useRemoteSource ? remoteSearchRevision : status.romsAllRevision;

        if (sIndexBuiltFor != sourceRev || sIndexBuiltForRemote != useRemoteSource ||
            sNormalizedTitles.size() != sourceRoms.size()) {
            sNormalizedTitles.clear();
            sNormalizedTitles.reserve(sourceRoms.size());
            for (const auto& g : sourceRoms) {
                sNormalizedTitles.push_back(normalizeSearchText(g.title));
            }
            sIndexBuiltFor = sourceRev;
            sIndexBuiltForRemote = useRemoteSource;
        }
        if (sCompletionCacheBuiltFor != sourceRev || sCompletionBuiltForRemote != useRemoteSource) {
            sCompletionById.clear();
            sCompletionCacheBuiltFor = sourceRev;
            sCompletionBuiltForRemote = useRemoteSource;
        }

        std::unordered_map<std::string, romm::QueueState> stateById;
        stateById.reserve(status.downloadQueue.size() + status.downloadHistory.size());
        for (const auto& qi : status.downloadHistory) {
            if (!qi.game.id.empty()) stateById[qi.game.id] = qi.state;
        }
        for (const auto& qi : status.downloadQueue) {
            if (!qi.game.id.empty()) stateById[qi.game.id] = qi.state;
        }

        auto isCompletedCached = [&](const romm::Game& g) -> bool {
            if (g.id.empty()) return false;
            auto it = sCompletionById.find(g.id);
            if (it != sCompletionById.end()) return it->second;
            bool v = romm::isGameCompletedOnDisk(g, config);
            sCompletionById[g.id] = v;
            return v;
        };

        auto matchesFilter = [&](const romm::Game& g) -> bool {
            auto it = g.id.empty() ? stateById.end() : stateById.find(g.id);
            std::optional<romm::QueueState> st;
            if (it != stateById.end()) st = it->second;
            switch (status.romFilter) {
                case romm::RomFilter::All:
                    return true;
                case romm::RomFilter::Queued:
                    return st.has_value() &&
                           (*st == romm::QueueState::Pending ||
                            *st == romm::QueueState::Downloading ||
                            *st == romm::QueueState::Finalizing);
                case romm::RomFilter::Resumable:
                    return st.has_value() && *st == romm::QueueState::Resumable;
                case romm::RomFilter::Failed:
                    return st.has_value() && *st == romm::QueueState::Failed;
                case romm::RomFilter::Completed:
                    return (st.has_value() && *st == romm::QueueState::Completed) || isCompletedCached(g);
                case romm::RomFilter::NotQueued:
                    return !st.has_value() && !isCompletedCached(g);
                default:
                    return true;
            }
        };

        std::vector<size_t> indices;
        indices.reserve(sourceRoms.size());
        std::string searchNorm = normalizeSearchText(status.romSearchQuery);
        for (size_t i = 0; i < sourceRoms.size(); ++i) {
            if (!searchNorm.empty()) {
                if (i >= sNormalizedTitles.size() || sNormalizedTitles[i].find(searchNorm) == std::string::npos) {
                    continue;
                }
            }
            if (!matchesFilter(sourceRoms[i])) continue;
            indices.push_back(i);
        }

        auto cmpTitleAsc = [&](size_t a, size_t b) {
            const std::string& ta = (a < sNormalizedTitles.size()) ? sNormalizedTitles[a] : sourceRoms[a].title;
            const std::string& tb = (b < sNormalizedTitles.size()) ? sNormalizedTitles[b] : sourceRoms[b].title;
            if (ta != tb) return ta < tb;
            return sourceRoms[a].id < sourceRoms[b].id;
        };

        switch (status.romSort) {
            case romm::RomSort::TitleAsc:
                std::sort(indices.begin(), indices.end(), cmpTitleAsc);
                break;
            case romm::RomSort::TitleDesc:
                std::sort(indices.begin(), indices.end(), [&](size_t a, size_t b) { return cmpTitleAsc(b, a); });
                break;
            case romm::RomSort::SizeDesc:
                std::sort(indices.begin(), indices.end(), [&](size_t a, size_t b) {
                    if (sourceRoms[a].sizeBytes != sourceRoms[b].sizeBytes) {
                        return sourceRoms[a].sizeBytes > sourceRoms[b].sizeBytes;
                    }
                    return cmpTitleAsc(a, b);
                });
                break;
            case romm::RomSort::SizeAsc:
                std::sort(indices.begin(), indices.end(), [&](size_t a, size_t b) {
                    if (sourceRoms[a].sizeBytes != sourceRoms[b].sizeBytes) {
                        return sourceRoms[a].sizeBytes < sourceRoms[b].sizeBytes;
                    }
                    return cmpTitleAsc(a, b);
                });
                break;
        }

        std::vector<romm::Game> rebuilt;
        rebuilt.reserve(indices.size());
        for (size_t idx : indices) rebuilt.push_back(sourceRoms[idx]);
        status.roms = std::move(rebuilt);
        status.romsRevision++;

        if (resetSelection) {
            status.selectedRomIndex = 0;
        } else if (status.selectedRomIndex >= (int)status.roms.size()) {
            status.selectedRomIndex = status.roms.empty() ? 0 : (int)status.roms.size() - 1;
        } else if (status.selectedRomIndex < 0) {
            status.selectedRomIndex = 0;
        }
    };

    auto exportDiagnosticsSummary = [&]() {
        std::vector<std::string> lines;
        {
            std::lock_guard<std::mutex> lock(status.mutex);
            lines.push_back("Diagnostics Summary");
            lines.push_back("View=" + std::string(viewName(status.currentView)));
            lines.push_back("ServerURL=" + config.serverUrl);
            lines.push_back("DownloadDir=" + config.downloadDir);
            lines.push_back("TimeoutSec=" + std::to_string(config.httpTimeoutSeconds));
            lines.push_back(std::string("Fat32Safe=") + (config.fat32Safe ? "true" : "false"));
            lines.push_back("LogLevel=" + config.logLevel);
            lines.push_back("CurrentPlatformSlug=" + status.currentPlatformSlug);
            lines.push_back("ROMsVisible=" + std::to_string(status.roms.size()) +
                            " ROMsAll=" + std::to_string(status.romsAll.size()));
            lines.push_back("ROMFilter=" + std::string(romFilterLabel(status.romFilter)) +
                            " Sort=" + std::string(romSortLabel(status.romSort)) +
                            " Search=" + status.romSearchQuery);
            lines.push_back("Queue=" + std::to_string(status.downloadQueue.size()) +
                            " History=" + std::to_string(status.downloadHistory.size()) +
                            " WorkerRunning=" + std::string(status.downloadWorkerRunning.load() ? "yes" : "no"));
            lines.push_back("ServerReachableKnown=" + std::string(status.diagnosticsServerReachableKnown ? "yes" : "no") +
                            " Reachable=" + std::string(status.diagnosticsServerReachable ? "yes" : "no") +
                            " ProbeInFlight=" + std::string(status.diagnosticsProbeInFlight ? "yes" : "no"));
            if (!status.diagnosticsLastProbeDetail.empty()) {
                lines.push_back("ProbeDetail=" + status.diagnosticsLastProbeDetail);
            }
            lines.push_back("LastErrorType=" + std::string(romm::errorCategoryLabel(status.lastErrorInfo.category)) +
                            "/" + romm::errorCodeLabel(status.lastErrorInfo.code));
            if (!status.lastError.empty()) lines.push_back("LastErrorDetail=" + status.lastError);
            lines.push_back("SD_Free=" + humanSize(romm::getFreeSpace(config.downloadDir)));
        }
        romm::logLine("=== BEGIN DIAGNOSTICS SUMMARY ===");
        for (const auto& l : lines) romm::logLine(l);
        romm::logLine("=== END DIAGNOSTICS SUMMARY ===");
    };

    auto persistQueueState = [&]() {
        std::string qerr;
        if (!romm::saveQueueState(status, qerr)) {
            romm::logLine("Queue state save warning: " + qerr);
        }
    };

    auto submitRomFetch = [&](PendingRomFetch req, const char* busyWhat, bool startNewGeneration) {
        {
            std::lock_guard<std::mutex> lock(status.mutex);
            if (startNewGeneration) {
                status.romFetchGeneration++;
            }
            req.generation = status.romFetchGeneration;
            status.netBusy.store(true);
            status.netBusySinceMs.store(SDL_GetTicks());
            status.netBusyWhat = busyWhat;
        }
        if (req.mode == PendingRomFetch::Mode::Probe) {
            romm::logLine("Queued ROM identifiers probe id=" + req.pid);
        } else {
            romm::logLine("Queued ROM page fetch id=" + req.pid +
                          " offset=" + std::to_string(req.offset) +
                          " limit=" + std::to_string(req.limit));
        }
        romFetchJobs.submit(req);
    };

    auto submitRemoteSearch = [&](PendingRemoteSearch req) {
        {
            std::lock_guard<std::mutex> lock(status.mutex);
            remoteSearchGeneration++;
            req.generation = remoteSearchGeneration;
            remoteSearchInFlight = true;
            status.netBusy.store(true);
            status.netBusySinceMs.store(SDL_GetTicks());
            status.netBusyWhat = "Remote search...";
        }
        romm::logLine("Queued remote search query=\"" + req.query + "\"");
        remoteSearchJobs.submit(req);
    };

    auto submitDiagnosticsProbe = [&]() {
        DiagProbeReq req{};
        {
            std::lock_guard<std::mutex> lock(status.mutex);
            status.diagnosticsProbeGeneration++;
            req.generation = status.diagnosticsProbeGeneration;
            status.diagnosticsProbeInFlight = true;
            status.diagnosticsLastProbeMs = SDL_GetTicks();
            status.diagnosticsLastProbeDetail.clear();
        }
        diagProbeJobs.submit(req);
    };

    constexpr const char* kUpdateRepoOwner = "Shalasere";
    constexpr const char* kUpdateRepoName = "SwitchRomM";
    const std::string kUpdateLatestUrl =
        std::string("https://api.github.com/repos/") + kUpdateRepoOwner + "/" + kUpdateRepoName + "/releases/latest";

    auto submitUpdateCheck = [&]() {
        UpdateCheckReq req{};
        {
            std::lock_guard<std::mutex> lock(status.mutex);
            updateGeneration++;
            req.generation = updateGeneration;
            updateCheckGenSubmitted = req.generation;
            status.updateCheckInFlight = true;
            status.updateChecked = false;
            status.updateAvailable = false;
            status.updateError.clear();
            status.updateStatus = "Checking GitHub releases...";
        }
        updateCheckJobs.submit(req);
    };

    auto submitUpdateDownload = [&]() {
        UpdateDownloadReq req{};
        {
            std::lock_guard<std::mutex> lock(status.mutex);
            if (!status.updateAvailable || status.updateAssetUrl.empty()) {
                status.updateStatus = "No update available to download.";
                return;
            }
            if (status.updateDownloadInFlight) {
                status.updateStatus = "Update download already in progress.";
                return;
            }
            updateGeneration++;
            req.generation = updateGeneration;
            updateDownloadGenSubmitted = req.generation;
            req.url = status.updateAssetUrl;
            req.outPath = stagedNroPath;
            status.updateDownloadInFlight = true;
            status.updateDownloaded = false;
            status.updateError.clear();
            status.updateStatus = "Downloading update...";
        }
        updateDownloadJobs.submit(req);
    };

    romFetchJobs.start(runRomFetch);
    remoteSearchJobs.start(runRemoteSearch, 120);
    diagProbeJobs.start([&](const DiagProbeReq& req) -> DiagProbeResult {
        DiagProbeResult out = runDiagProbe();
        out.generation = req.generation;
        return out;
    });
    updateCheckJobs.start([&](const UpdateCheckReq& req) -> UpdateCheckResult {
        UpdateCheckResult out;
        out.generation = req.generation;
        std::string err;
        romm::HttpTransaction tx;
        romm::HttpRequestOptions opt;
        opt.timeoutSec = (config.httpTimeoutSeconds > 0) ? config.httpTimeoutSeconds : 20;
        opt.keepAlive = true;
        opt.decodeChunked = true;
        opt.maxBodyBytes = 2 * 1024 * 1024;

        std::vector<std::pair<std::string, std::string>> headers;
        headers.emplace_back("User-Agent", "romm-switch-client");
        headers.emplace_back("Accept", "application/vnd.github+json");

        if (!romm::httpRequestBuffered("GET", kUpdateLatestUrl, headers, opt, tx, err)) {
            out.ok = false;
            out.error = err;
            out.errorInfo = romm::classifyError(err, romm::ErrorCategory::Network);
            return out;
        }
        if (tx.parsed.statusCode != 200) {
            out.ok = false;
            out.error = "GitHub latest release request failed (HTTP " + std::to_string(tx.parsed.statusCode) + ")";
            out.errorInfo.category = romm::ErrorCategory::Network;
            out.errorInfo.code = romm::ErrorCode::HttpStatus;
            out.errorInfo.httpStatus = tx.parsed.statusCode;
            out.errorInfo.retryable = false;
            out.errorInfo.userMessage = "GitHub API request failed.";
            out.errorInfo.detail = out.error;
            return out;
        }

        romm::GitHubRelease rel;
        if (!romm::parseGitHubLatestReleaseJson(tx.body, rel, err)) {
            out.ok = false;
            out.error = err;
            out.errorInfo = romm::classifyError(err, romm::ErrorCategory::Data);
            return out;
        }
        romm::GitHubAsset asset;
        if (!romm::pickReleaseNroAsset(rel, asset, err, "romm-switch-client.nro")) {
            out.ok = false;
            out.release = rel;
            out.error = err;
            out.errorInfo = romm::classifyError(err, romm::ErrorCategory::Data);
            return out;
        }

        out.ok = true;
        out.release = std::move(rel);
        out.asset = std::move(asset);
        out.updateAvailable = (romm::compareVersions(out.release.tagName, romm::appVersion()) > 0);
        return out;
    });
    updateDownloadJobs.start([&](const UpdateDownloadReq& req) -> UpdateDownloadResult {
        UpdateDownloadResult out;
        out.generation = req.generation;
        out.outPath = req.outPath;
        std::string err;

        const std::string tmp = req.outPath + ".part";
        std::FILE* f = std::fopen(tmp.c_str(), "wb");
        if (!f) {
            out.ok = false;
            out.error = "Failed to open update temp file for write: " + tmp;
            out.errorInfo = romm::classifyError(out.error, romm::ErrorCategory::Filesystem);
            return out;
        }

        romm::HttpRequestOptions opt;
        opt.timeoutSec = (config.httpTimeoutSeconds > 0) ? config.httpTimeoutSeconds : 20;
        opt.keepAlive = false;
        opt.decodeChunked = true;
        opt.followRedirects = true; // GitHub asset downloads commonly redirect to a CDN host

        std::vector<std::pair<std::string, std::string>> headers;
        headers.emplace_back("User-Agent", "romm-switch-client");
        romm::ParsedHttpResponse parsed;

        uint64_t bytes = 0;
        bool ok = romm::httpRequestStreamed("GET",
                                            req.url,
                                            headers,
                                            opt,
                                            parsed,
                                            [&](const char* data, size_t n) -> bool {
                                                if (!data || n == 0) return true;
                                                size_t w = std::fwrite(data, 1, n, f);
                                                if (w != n) return false;
                                                bytes += (uint64_t)n;
                                                return true;
                                            },
                                            err);
        std::fclose(f);
        out.bytes = bytes;
        if (!ok) {
            out.ok = false;
            out.error = err.empty() ? "Update download failed." : err;
            out.errorInfo = romm::classifyError(out.error, romm::ErrorCategory::Network);
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
            return out;
        }
        if (parsed.statusCode != 200) {
            out.ok = false;
            out.error = "Update download failed (HTTP " + std::to_string(parsed.statusCode) + ")";
            out.errorInfo.category = romm::ErrorCategory::Network;
            out.errorInfo.code = romm::ErrorCode::HttpStatus;
            out.errorInfo.httpStatus = parsed.statusCode;
            out.errorInfo.retryable = false;
            out.errorInfo.userMessage = "Download request failed.";
            out.errorInfo.detail = out.error;
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
            return out;
        }

        // Basic sanity check: NRO magic.
        auto fileLooksLikeNro = [&](const std::string& path) -> bool {
            std::FILE* rf = std::fopen(path.c_str(), "rb");
            if (!rf) return false;
            char magic[4]{};
            size_t rn = std::fread(magic, 1, sizeof(magic), rf);
            std::fclose(rf);
            return rn == sizeof(magic) && magic[0] == 'N' && magic[1] == 'R' && magic[2] == 'O' && magic[3] == '0';
        };
        if (!fileLooksLikeNro(tmp)) {
            out.ok = false;
            out.error = "Downloaded file does not look like a valid NRO.";
            out.errorInfo = romm::classifyError(out.error, romm::ErrorCategory::Data);
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
            return out;
        }

        std::error_code ec;
        std::filesystem::remove(req.outPath, ec); // overwrite any prior staged file
        ec.clear();
        std::filesystem::rename(tmp, req.outPath, ec);
        if (ec) {
            out.ok = false;
            out.error = "Failed to finalize staged update: " + ec.message();
            out.errorInfo = romm::classifyError(out.error, romm::ErrorCategory::Filesystem);
            std::error_code ec2;
            std::filesystem::remove(tmp, ec2);
            return out;
        }

        out.ok = true;
        return out;
    });

    // Use positional button codes on Switch (A=bottom, B=right, X=left, Y=top).
    // We keep the UI text in Nintendo labels, but map input based on what SDL actually reports.
    SDL_SetHintWithPriority(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0", SDL_HINT_OVERRIDE);
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software"); // enforce software before creating renderer
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

    if (!romm::loadConfig(config, cfgError, &cfgErrInfo)) {
        {
            std::lock_guard<std::mutex> lock(status.mutex);
            status.currentView = Status::View::ERROR;
            status.lastError = cfgError;
            status.lastErrorInfo = cfgErrInfo.code == romm::ErrorCode::None
                ? romm::classifyError(cfgError, romm::ErrorCategory::Config)
                : cfgErrInfo;
        }
        romm::logLine(cfgError);
    } else {
        romm::setLogLevelFromString(config.logLevel);
        romm::logLine("Config loaded.");
        romm::logLine(" server_url=" + config.serverUrl);
        romm::logLine(" download_dir=" + config.downloadDir);
        romm::logLine(std::string(" fat32_safe=") + (config.fat32Safe ? "true" : "false"));
        // Load platform prefs and stash in status for planner use.
        {
            std::lock_guard<std::mutex> lock(status.mutex);
            status.platformPrefs = romm::defaultPlatformPrefs();
        }
        {
            std::string prefsErr;
            romm::PlatformPrefs prefs;
            if (romm::loadPlatformPrefs(config.platformPrefsMode, config.platformPrefsPathSd, config.platformPrefsPathRomfs, prefs, prefsErr)) {
                romm::logLine("Platform prefs loaded (mode=" + config.platformPrefsMode + ")");
                std::lock_guard<std::mutex> lock(status.mutex);
                status.platformPrefs = prefs;
            } else if (!prefsErr.empty()) {
                romm::logLine("Platform prefs load failed: " + prefsErr);
            }
        }
        romm::ensureDirectory(config.downloadDir);
        if (!config.speedTestUrl.empty()) {
            // Kick off a background speed test (20MB range) without blocking UI; join on exit.
            const Config cfgCopy = config;
            {
                std::lock_guard<std::mutex> lock(status.mutex);
                status.lastSpeedMBps = -1.0; // pending
            }
            speedTestThread = std::thread([cfgCopy, &status]() {
                std::string err;
                constexpr uint64_t kProbeBytes = 40ULL * 1024ULL * 1024ULL;
                if (romm::runSpeedTest(cfgCopy, status, kProbeBytes, err)) {
                    std::lock_guard<std::mutex> lock(status.mutex);
                    romm::logLine("Startup speed test: " + std::to_string(status.lastSpeedMBps) + " MB/s");
                } else {
                    std::lock_guard<std::mutex> lock(status.mutex);
                    status.lastSpeedMBps = -2.0; // failed
                    romm::logLine("Startup speed test failed: " + err);
                }
            });
        }
        std::string histErr;
        if (!romm::loadLocalManifests(status, config, histErr) && !histErr.empty()) {
            romm::logLine("Manifest load warning: " + histErr);
        }
        std::string queueErr;
        if (!romm::loadQueueState(status, config, queueErr) && !queueErr.empty()) {
            romm::logLine("Queue state load warning: " + queueErr);
        } else {
            persistQueueState();
        }
        std::string err;
        romm::ErrorInfo errInfo;
        if (!romm::fetchPlatforms(config, status, err, &errInfo)) {
            {
                std::lock_guard<std::mutex> lock(status.mutex);
                status.currentView = Status::View::ERROR;
                status.lastError = err;
                status.lastErrorInfo = errInfo.code == romm::ErrorCode::None
                    ? romm::classifyError(err, romm::ErrorCategory::Network)
                    : errInfo;
            }
            romm::logLine("Failed to fetch platforms: " + err);
        }
        // Start cover loader once config is available.
        gCoverLoader.start(fetchCoverData);
    }

    // Main loop: poll input -> update state -> render current view
    while (running && appletMainLoop()) {
        // If a previous worker has finished, join and release it so we can start a fresh session.
        romm::reapDownloadWorkerIfDone();
        if (auto done = romFetchJobs.pollResult()) {
            size_t appliedCount = 0;
            std::string firstTitle;
            std::string fetchErr;
            bool applyOk = false;
            bool applyErr = false;
            bool queueNextPage = false;
            PendingRomFetch nextReq;
            {
                std::lock_guard<std::mutex> lock(status.mutex);
                // Ignore stale results from cancelled/older generations.
                bool staleResult = (done->req.generation != status.romFetchGeneration);
                if (staleResult) {
                    if (!romFetchJobs.busy() && !remoteSearchInFlight) {
                        status.netBusy.store(false);
                        status.netBusyWhat.clear();
                    }
                } else if (done->probeOnly) {
                    if (done->probeFailed) {
                        romm::logLine("ROM identifiers probe failed; falling back to full fetch: " + done->error);
                    }
                    bool usedProbeCache = false;
                    if (done->probeUnchanged) {
                        uint32_t nowMs = SDL_GetTicks();
                        if (status.currentPlatformId == done->req.pid && !status.romsAll.empty()) {
                            status.currentView = Status::View::ROMS;
                            status.navStack.clear();
                            currentPlatformFetchedAtMs = nowMs;
                            if (!done->identifierDigest.empty()) {
                                currentPlatformIdentifierDigest = done->identifierDigest;
                            }
                            usedProbeCache = true;
                        } else {
                            auto hit = platformRomsCache.find(done->req.pid);
                            if (hit != platformRomsCache.end() && !hit->second.games.empty()) {
                                if (!status.currentPlatformId.empty() &&
                                    status.currentPlatformId != done->req.pid &&
                                    !status.romsAll.empty()) {
                                    CachedPlatformRoms keep;
                                    keep.games = std::move(status.romsAll);
                                    keep.slug = status.currentPlatformSlug;
                                    keep.name = status.currentPlatformName;
                                    keep.identifierDigest = currentPlatformIdentifierDigest;
                                    keep.fetchedAtMs = currentPlatformFetchedAtMs;
                                    platformRomsCache[status.currentPlatformId] = std::move(keep);
                                    prunePlatformCache();
                                }
                                status.romsAll = std::move(hit->second.games);
                                status.romsAllRevision++;
                                rebuildVisibleRomsLocked(true);
                                status.currentPlatformId = done->req.pid;
                                status.currentPlatformSlug = hit->second.slug.empty() ? done->req.slug : hit->second.slug;
                                status.currentPlatformName = hit->second.name.empty() ? done->req.name : hit->second.name;
                                status.currentView = Status::View::ROMS;
                                status.navStack.clear();
                                currentPlatformFetchedAtMs = nowMs;
                                currentPlatformIdentifierDigest =
                                    !done->identifierDigest.empty() ? done->identifierDigest : hit->second.identifierDigest;
                                usedProbeCache = true;
                            }
                        }
                    }
                    if (usedProbeCache) {
                        status.netBusy.store(false);
                        status.netBusyWhat.clear();
                        applyOk = true;
                        appliedCount = status.romsAll.size();
                        if (!status.romsAll.empty()) firstTitle = status.romsAll[0].title;
                    } else {
                        PendingRomFetch req;
                        req.mode = PendingRomFetch::Mode::Page;
                        req.pid = done->req.pid;
                        req.slug = done->req.slug;
                        req.name = done->req.name;
                        req.offset = 0;
                        req.limit = kRomsFirstPageLimit;
                        queueNextPage = true;
                        nextReq = req;
                        status.netBusyWhat = "Fetching ROMs...";
                    }
                } else if (!done->ok) {
                    status.netBusy.store(false);
                    status.netBusyWhat.clear();
                    fetchErr = done->error;
                    if (done->offset == 0) {
                        status.currentView = Status::View::ERROR;
                        status.lastError = done->error;
                        status.lastErrorInfo = done->errorInfo.code == romm::ErrorCode::None
                            ? romm::classifyError(done->error, romm::ErrorCategory::Network)
                            : done->errorInfo;
                        applyErr = true;
                    } else {
                        romm::logLine("Background ROM page fetch failed offset=" +
                                      std::to_string(done->offset) + ": " + done->error);
                    }
                } else {
                    if (done->offset == 0) {
                        appliedCount = done->games.size();
                        if (!done->games.empty()) firstTitle = done->games[0].title;
                        uint32_t nowMs = SDL_GetTicks();
                        if (!status.currentPlatformId.empty() &&
                            status.currentPlatformId != done->req.pid &&
                            !status.romsAll.empty()) {
                            CachedPlatformRoms keep;
                            keep.games = std::move(status.romsAll);
                            keep.slug = status.currentPlatformSlug;
                            keep.name = status.currentPlatformName;
                            keep.identifierDigest = currentPlatformIdentifierDigest;
                            keep.fetchedAtMs = currentPlatformFetchedAtMs;
                            platformRomsCache[status.currentPlatformId] = std::move(keep);
                            prunePlatformCache();
                        }
                        status.romsAll = std::move(done->games);
                        status.romsAllRevision++;
                        rebuildVisibleRomsLocked(true);
                        status.currentPlatformId = done->req.pid;
                        status.currentPlatformSlug = done->req.slug;
                        status.currentPlatformName = done->req.name;
                        currentPlatformFetchedAtMs = nowMs;
                        if (!done->identifierDigest.empty()) {
                            currentPlatformIdentifierDigest = done->identifierDigest;
                        }
                        status.navStack.clear();
                        status.currentView = Status::View::ROMS;
                        remoteSearchActive = false;
                        remoteSearchGames.clear();
                        remoteSearchQuery.clear();
                        remoteSearchPlatformId.clear();
                        remoteSearchRevision++;
                        pagedFetchNextOffset = done->nextOffset;
                        pagedFetchPageLimit = kRomsNextPageLimit;
                        if (done->hasMore) {
                            status.netBusy.store(true);
                            status.netBusyWhat = "Loading remaining ROMs...";
                            PendingRomFetch req;
                            req.mode = PendingRomFetch::Mode::Page;
                            req.pid = done->req.pid;
                            req.slug = done->req.slug;
                            req.name = done->req.name;
                            req.offset = pagedFetchNextOffset;
                            req.limit = pagedFetchPageLimit;
                            queueNextPage = true;
                            nextReq = req;
                        } else {
                            status.netBusy.store(false);
                            status.netBusyWhat.clear();
                        }
                        applyOk = true;
                    } else {
                        size_t before = status.romsAll.size();
                        std::unordered_set<std::string> existing;
                        existing.reserve(status.romsAll.size() + done->games.size());
                        for (const auto& g : status.romsAll) existing.insert(g.id);
                        for (auto& g : done->games) {
                            if (existing.insert(g.id).second) {
                                status.romsAll.push_back(std::move(g));
                            }
                        }
                        size_t added = status.romsAll.size() - before;
                        status.romsAllRevision++;
                        appliedCount = added;
                        if (done->hasMore) {
                            pagedFetchNextOffset = done->nextOffset;
                            PendingRomFetch req;
                            req.mode = PendingRomFetch::Mode::Page;
                            req.pid = done->req.pid;
                            req.slug = done->req.slug;
                            req.name = done->req.name;
                            req.offset = pagedFetchNextOffset;
                            req.limit = pagedFetchPageLimit;
                            queueNextPage = true;
                            nextReq = req;
                            status.netBusy.store(true);
                            status.netBusyWhat = "Loading remaining ROMs...";
                        } else {
                            status.netBusy.store(false);
                            status.netBusyWhat.clear();
                        }
                        applyOk = true;
                    }
                }
            }
            if (queueNextPage) {
                submitRomFetch(nextReq, nextReq.offset == 0 ? "Fetching ROMs..." : "Loading remaining ROMs...", false);
            }
            if (applyErr) {
                romm::logLine("Failed to fetch ROMs: " + fetchErr);
            } else if (applyOk) {
                gViewTraceFrames = 8;
                romm::logLine("Fetched ROMs count=" + std::to_string(appliedCount) +
                              (firstTitle.empty() ? "" : " first=" + firstTitle));
            }
        }
        if (auto searchDone = remoteSearchJobs.pollResult()) {
            std::lock_guard<std::mutex> lock(status.mutex);
            if (searchDone->req.generation != remoteSearchGeneration) {
                if (!romFetchJobs.busy() && !remoteSearchInFlight) {
                    status.netBusy.store(false);
                    status.netBusyWhat.clear();
                }
            } else {
                remoteSearchInFlight = false;
                if (searchDone->ok &&
                    !searchDone->req.query.empty() &&
                    searchDone->req.pid == status.currentPlatformId &&
                    searchDone->req.query == status.romSearchQuery) {
                    remoteSearchGames = std::move(searchDone->games);
                    remoteSearchActive = true;
                    remoteSearchQuery = searchDone->req.query;
                    remoteSearchPlatformId = searchDone->req.pid;
                    remoteSearchRevision++;
                    status.romListOptionsRevision++;
                    romm::logLine("Remote search applied results=" + std::to_string(remoteSearchGames.size()));
                } else if (!searchDone->ok) {
                    romm::logLine("Remote search failed, using local index: " + searchDone->error);
                    remoteSearchActive = false;
                    remoteSearchGames.clear();
                    remoteSearchQuery.clear();
                    remoteSearchPlatformId.clear();
                    remoteSearchRevision++;
                    status.romListOptionsRevision++;
                }
                if (!romFetchJobs.busy()) {
                    status.netBusy.store(false);
                    status.netBusyWhat.clear();
                }
            }
        }
        if (auto probe = diagProbeJobs.pollResult()) {
            std::lock_guard<std::mutex> lock(status.mutex);
            if (probe->generation == status.diagnosticsProbeGeneration) {
                status.diagnosticsProbeInFlight = false;
                status.diagnosticsServerReachableKnown = true;
                status.diagnosticsServerReachable = probe->ok;
                status.diagnosticsLastProbeMs = SDL_GetTicks();
                status.diagnosticsLastProbeDetail = probe->ok
                    ? probe->detail
                    : probe->detail + " (" + romm::errorCodeLabel(probe->errorInfo.code) + ")";
            }
        }
        if (auto upd = updateCheckJobs.pollResult()) {
            std::lock_guard<std::mutex> lock(status.mutex);
            if (upd->generation == updateCheckGenSubmitted) {
                status.updateCheckInFlight = false;
                status.updateChecked = upd->ok;
                status.updateError.clear();
                status.updateStatus.clear();
                if (!upd->ok) {
                    status.updateError = upd->error.empty() ? "Update check failed." : upd->error;
                    status.updateStatus = "Press A to retry.";
                } else {
                    status.updateLatestTag = upd->release.tagName;
                    status.updateLatestName = upd->release.name;
                    status.updateLatestPublishedAt = upd->release.publishedAt;
                    status.updateReleaseHtmlUrl = upd->release.htmlUrl;
                    status.updateAssetName = upd->asset.name;
                    status.updateAssetUrl = upd->asset.downloadUrl;
                    status.updateAssetSizeBytes = upd->asset.sizeBytes;
                    status.updateAvailable = upd->updateAvailable;
                    status.updateStatus = upd->updateAvailable ? "Update available." : "Up to date.";
                }
            }
        }
        if (auto dl = updateDownloadJobs.pollResult()) {
            std::lock_guard<std::mutex> lock(status.mutex);
            if (dl->generation == updateDownloadGenSubmitted) {
                status.updateDownloadInFlight = false;
                status.updateError.clear();
                if (!dl->ok) {
                    status.updateDownloaded = false;
                    status.updateError = dl->error.empty() ? "Update download failed." : dl->error;
                    status.updateStatus = "Download failed. Press X to retry.";
                } else {
                    status.updateDownloaded = true;
                    status.updateStatus = "Download complete. Restart app to apply.";
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(status.mutex);
            bool needRebuild = false;
            if (status.romsAllRevision != appliedRomsAllRev ||
                status.romListOptionsRevision != appliedRomsOptionsRev) {
                needRebuild = true;
            } else if (filterNeedsState(status.romFilter) &&
                       (status.downloadQueueRevision != appliedQueueRevForRoms ||
                        status.downloadHistoryRevision != appliedHistRevForRoms)) {
                needRebuild = true;
            }
            if (needRebuild) {
                rebuildVisibleRomsLocked(false);
            }
            appliedRomsAllRev = status.romsAllRevision;
            appliedRomsOptionsRev = status.romListOptionsRevision;
            appliedQueueRevForRoms = status.downloadQueueRevision;
            appliedHistRevForRoms = status.downloadHistoryRevision;
        }
        processCoverResult(renderer);
        bool viewChangedThisFrame = false;
        // TODO(nav): extract view transitions into a ViewController and align hints with mapping.
        // Adjust selection index based on current view (platforms/roms/queue)
        auto adjustSelection = [&](int dir) {
            std::lock_guard<std::mutex> lock(status.mutex);
            if (status.currentView == Status::View::PLATFORMS) {
                status.selectedPlatformIndex = std::max(0, std::min((int)status.platforms.size() - 1, status.selectedPlatformIndex + dir));
            } else if (status.currentView == Status::View::ROMS || status.currentView == Status::View::DETAIL) {
                status.selectedRomIndex = std::max(0, std::min((int)status.roms.size() - 1, status.selectedRomIndex + dir));
            } else if (status.currentView == Status::View::QUEUE) {
                if (status.queueReorderActive) {
                    if (status.downloadQueue.empty()) return;
                    // Don't allow reordering across any active/non-pending prefix.
                    int barrier = 0;
                    while (barrier < (int)status.downloadQueue.size() &&
                           status.downloadQueue[(size_t)barrier].state != romm::QueueState::Pending) {
                        barrier++;
                    }
                    if (barrier >= (int)status.downloadQueue.size()) return;
                    if (status.selectedQueueIndex < barrier) status.selectedQueueIndex = barrier;
                    int idx = status.selectedQueueIndex;
                    int next = idx + dir;
                    if (next < barrier || next >= (int)status.downloadQueue.size()) return;
                    // Only pending items are reorderable; keep it strict for worker safety.
                    if (status.downloadQueue[(size_t)idx].state != romm::QueueState::Pending ||
                        status.downloadQueue[(size_t)next].state != romm::QueueState::Pending) {
                        return;
                    }
                    std::swap(status.downloadQueue[(size_t)idx], status.downloadQueue[(size_t)next]);
                    status.selectedQueueIndex = next;
                    status.downloadQueueRevision++;
                } else {
                    status.selectedQueueIndex = std::max(0, std::min((int)status.downloadQueue.size() - 1, status.selectedQueueIndex + dir));
                }
            }
        };
        auto recomputeTotals = [&]() {
            std::lock_guard<std::mutex> lock(status.mutex);
            uint64_t remaining = 0;
            // Include current download remainder if active.
            if (status.downloadWorkerRunning.load()) {
                uint64_t curSize = status.currentDownloadSize.load();
                uint64_t curDone = status.currentDownloadedBytes.load();
                if (curSize > curDone) {
                    remaining += (curSize - curDone);
                }
            }
            for (const auto& q : status.downloadQueue) {
                remaining += q.bundle.totalSize();
            }
            uint64_t already = status.totalDownloadedBytes.load();
            status.totalDownloadBytes.store(already + remaining);
        };

        {
            std::lock_guard<std::mutex> lock(status.mutex);
            if (!status.workerEvents.empty()) {
                for (const auto& ev : status.workerEvents) {
                    if (ev.type == romm::WorkerEventType::DownloadFailureState) {
                        status.lastDownloadFailed.store(ev.failed);
                        status.lastDownloadError = ev.message;
                    } else if (ev.type == romm::WorkerEventType::DownloadCompletion) {
                        status.downloadCompleted = true;
                    }
                }
                status.workerEvents.clear();
                status.workerEventsRevision++;
            }
        }

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
                case romm::Action::Left: {
                    bool changed = false;
                    {
                        std::lock_guard<std::mutex> lock(status.mutex);
                        if (status.currentView == Status::View::ROMS) {
                            switch (status.romFilter) {
                                case romm::RomFilter::All: status.romFilter = romm::RomFilter::Queued; break;
                                case romm::RomFilter::Queued: status.romFilter = romm::RomFilter::Resumable; break;
                                case romm::RomFilter::Resumable: status.romFilter = romm::RomFilter::Failed; break;
                                case romm::RomFilter::Failed: status.romFilter = romm::RomFilter::Completed; break;
                                case romm::RomFilter::Completed: status.romFilter = romm::RomFilter::NotQueued; break;
                                case romm::RomFilter::NotQueued: status.romFilter = romm::RomFilter::All; break;
                            }
                            status.romListOptionsRevision++;
                            changed = true;
                        }
                    }
                    if (changed) {
                        romm::logLine(std::string("ROM filter -> ") + romFilterLabel(status.romFilter));
                    }
                    break;
                }
                case romm::Action::Right: {
                    bool changed = false;
                    {
                        std::lock_guard<std::mutex> lock(status.mutex);
                        if (status.currentView == Status::View::ROMS) {
                            switch (status.romSort) {
                                case romm::RomSort::TitleAsc: status.romSort = romm::RomSort::TitleDesc; break;
                                case romm::RomSort::TitleDesc: status.romSort = romm::RomSort::SizeDesc; break;
                                case romm::RomSort::SizeDesc: status.romSort = romm::RomSort::SizeAsc; break;
                                case romm::RomSort::SizeAsc: status.romSort = romm::RomSort::TitleAsc; break;
                            }
                            status.romListOptionsRevision++;
                            changed = true;
                        }
                    }
                    if (changed) {
                        romm::logLine(std::string("ROM sort -> ") + romSortLabel(status.romSort));
                    }
                    break;
                }
                case romm::Action::Select: {
                    // A/Select: context-sensitive (fetch ROMs, open detail, or enqueue)
                    Status::View currentView;
                    {
                        std::lock_guard<std::mutex> lock(status.mutex);
                        currentView = status.currentView;
                    }
                    if (currentView == Status::View::UPDATER) {
                        submitUpdateCheck();
                        viewChangedThisFrame = true;
                        break;
                    }
                    if (currentView == Status::View::QUEUE) {
                        bool didToggle = false;
                        bool nowActive = false;
                        {
                            std::lock_guard<std::mutex> lock(status.mutex);
                            if (!status.downloadQueue.empty()) {
                                if (!status.queueReorderActive) {
                                    // Only allow reordering within the pending tail (don't cross active download prefix).
                                    int barrier = 0;
                                    while (barrier < (int)status.downloadQueue.size() &&
                                           status.downloadQueue[(size_t)barrier].state != romm::QueueState::Pending) {
                                        barrier++;
                                    }
                                    if (barrier >= (int)status.downloadQueue.size()) {
                                        romm::logLine("Queue reorder: no pending items to move.");
                                    } else {
                                        if (status.selectedQueueIndex < barrier) status.selectedQueueIndex = barrier;
                                        // Require pending state for selection.
                                        if (status.downloadQueue[(size_t)status.selectedQueueIndex].state == romm::QueueState::Pending) {
                                            status.queueReorderActive = true;
                                            didToggle = true;
                                            nowActive = true;
                                        } else {
                                            romm::logLine("Queue reorder: selected item is not pending; cannot move.");
                                        }
                                    }
                                } else {
                                    status.queueReorderActive = false;
                                    didToggle = true;
                                    nowActive = false;
                                }
                            }
                        }
                        if (didToggle) {
                            if (!nowActive) persistQueueState();
                            romm::logLine(std::string("Queue reorder ") + (nowActive ? "enabled" : "disabled") +
                                          " idx=" + std::to_string(status.selectedQueueIndex));
                        }
                        break;
                    }
                    if (currentView == Status::View::PLATFORMS) {
                        int sel = -1;
                        std::string pid;
                        {
                            std::lock_guard<std::mutex> lock(status.mutex);
                            if (!status.platforms.empty() &&
                                status.selectedPlatformIndex >= 0 &&
                                status.selectedPlatformIndex < (int)status.platforms.size()) {
                                sel = status.selectedPlatformIndex;
                                pid = status.platforms[sel].id;
                            }
                        }
                        if (sel < 0 || pid.empty()) {
                            romm::logLine("Select on PLATFORMS but index out of range.");
                            break;
                        }
                        std::string selSlug;
                        std::string selName;
                        {
                            std::lock_guard<std::mutex> lock(status.mutex);
                            if (sel >= 0 && sel < (int)status.platforms.size()) {
                                selSlug = status.platforms[sel].slug;
                                selName = status.platforms[sel].name;
                            }
                        }
                        bool usedCache = false;
                        bool submittedFetch = false;
                        PendingRomFetch fetchReq;
                        const uint32_t nowMs = SDL_GetTicks();
                        if (!romFetchJobs.busy()) {
                            std::lock_guard<std::mutex> lock(status.mutex);
                            remoteSearchActive = false;
                            remoteSearchGames.clear();
                            remoteSearchQuery.clear();
                            remoteSearchPlatformId.clear();
                            remoteSearchRevision++;
                            if (status.currentPlatformId == pid && !status.romsAll.empty()) {
                                if ((nowMs - currentPlatformFetchedAtMs) <= kPlatformRomsCacheTtlMs) {
                                    status.currentView = Status::View::ROMS;
                                    status.navStack.clear();
                                    usedCache = true;
                                } else if (!currentPlatformIdentifierDigest.empty()) {
                                    fetchReq.mode = PendingRomFetch::Mode::Probe;
                                    fetchReq.pid = pid;
                                    fetchReq.slug = selSlug;
                                    fetchReq.name = selName;
                                    fetchReq.cachedIdentifierDigest = currentPlatformIdentifierDigest;
                                    submittedFetch = true;
                                }
                            }
                            if (!usedCache && !submittedFetch) {
                                auto hit = platformRomsCache.find(pid);
                                if (hit != platformRomsCache.end()) {
                                    bool fresh = (nowMs - hit->second.fetchedAtMs) <= kPlatformRomsCacheTtlMs;
                                    if (fresh && !hit->second.games.empty()) {
                                        if (!status.currentPlatformId.empty() &&
                                            status.currentPlatformId != pid &&
                                            !status.romsAll.empty()) {
                                            CachedPlatformRoms keep;
                                            keep.games = std::move(status.romsAll);
                                            keep.slug = status.currentPlatformSlug;
                                            keep.name = status.currentPlatformName;
                                            keep.identifierDigest = currentPlatformIdentifierDigest;
                                            keep.fetchedAtMs = currentPlatformFetchedAtMs;
                                            platformRomsCache[status.currentPlatformId] = std::move(keep);
                                            prunePlatformCache();
                                        }
                                        status.romsAll = std::move(hit->second.games);
                                        status.romsAllRevision++;
                                        rebuildVisibleRomsLocked(true);
                                        status.currentPlatformId = pid;
                                        status.currentPlatformSlug = hit->second.slug.empty() ? selSlug : hit->second.slug;
                                        status.currentPlatformName = hit->second.name.empty() ? selName : hit->second.name;
                                        status.navStack.clear();
                                        status.currentView = Status::View::ROMS;
                                        currentPlatformFetchedAtMs = nowMs;
                                        currentPlatformIdentifierDigest = hit->second.identifierDigest;
                                        usedCache = true;
                                        platformRomsCache.erase(hit);
                                    } else if (!hit->second.identifierDigest.empty()) {
                                        fetchReq.mode = PendingRomFetch::Mode::Probe;
                                        fetchReq.pid = pid;
                                        fetchReq.slug = selSlug;
                                        fetchReq.name = selName;
                                        fetchReq.cachedIdentifierDigest = hit->second.identifierDigest;
                                        submittedFetch = true;
                                    } else {
                                        platformRomsCache.erase(hit);
                                    }
                                }
                            }
                            if (!usedCache && !submittedFetch) {
                                fetchReq.mode = PendingRomFetch::Mode::Page;
                                fetchReq.pid = pid;
                                fetchReq.slug = selSlug;
                                fetchReq.name = selName;
                                fetchReq.offset = 0;
                                fetchReq.limit = kRomsFirstPageLimit;
                                submittedFetch = true;
                            }
                        } else {
                            fetchReq.mode = PendingRomFetch::Mode::Page;
                            fetchReq.pid = pid;
                            fetchReq.slug = selSlug;
                            fetchReq.name = selName;
                            fetchReq.offset = 0;
                            fetchReq.limit = kRomsFirstPageLimit;
                            submittedFetch = true;
                        }
                        if (usedCache) {
                            gViewTraceFrames = 8;
                            romm::logLine("ROM fetch cache hit for platform id=" + pid);
                            viewChangedThisFrame = true;
                            break;
                        }
                        if (submittedFetch) {
                            const bool isProbe = (fetchReq.mode == PendingRomFetch::Mode::Probe);
                            const char* busyWhat = isProbe ? "Checking changes..." : (romFetchJobs.busy() ? "Switching platform..." : "Fetching ROMs...");
                            // Async fetch so UI can keep rendering (including throbber) while the HTTP call runs.
                            if (isProbe) {
                                submitRomFetch(fetchReq, busyWhat, true);
                            } else {
                                pagedFetchNextOffset = 0;
                                submitRomFetch(fetchReq, busyWhat, true);
                            }
                        } else {
                            romm::logLine("Platform select produced no fetch request; staying on current view.");
                        }
                        viewChangedThisFrame = true;
                    } else if (currentView == Status::View::ROMS) {
                        {
                            std::lock_guard<std::mutex> lock(status.mutex);
                            if (!status.roms.empty()) {
                                status.currentView = Status::View::DETAIL;
                                viewChangedThisFrame = true;
                                romm::logLine("Open DETAIL for idx=" + std::to_string(status.selectedRomIndex));
                            }
                        }
                        gViewTraceFrames = 8;
                      } else if (currentView == Status::View::DETAIL) {
                          int sel = -1;
                          {
                              std::lock_guard<std::mutex> lock(status.mutex);
                              if (status.selectedRomIndex >= 0 && status.selectedRomIndex < (int)status.roms.size()) {
                                  sel = status.selectedRomIndex;
                              }
                          }
                          if (sel >= 0) {
                              romm::Game enriched;
                              {
                                  std::lock_guard<std::mutex> lock(status.mutex);
                                  enriched = status.roms[sel];
                              }
                              std::string err;
                              romm::ErrorInfo errInfo;
                              if (!romm::enrichGameWithFiles(config, enriched, err, &errInfo)) {
                                  std::lock_guard<std::mutex> lock(status.mutex);
                                  status.currentView = Status::View::ERROR;
                                  status.lastError = err;
                                  status.lastErrorInfo = errInfo.code == romm::ErrorCode::None
                                      ? romm::classifyError(err, romm::ErrorCategory::Data)
                                      : errInfo;
                                  romm::logLine("Failed to enrich ROM with files: " + err);
                                  break;
                              }
                              romm::DownloadBundle bundle = romm::buildBundleFromGame(enriched, status.platformPrefs);
                              if (!bundle.files.empty()) {
                                  enriched.sizeBytes = bundle.totalSize();
                              }
                              if (!romm::canEnqueueGame(status, enriched)) {
                                  romm::logLine("ROM already queued this session: " + enriched.title);
                                  gViewTraceFrames = 4;
                                  break;
                              }
                              {
                                  std::lock_guard<std::mutex> lock(status.mutex);
                                  if (sel >= 0 && sel < (int)status.roms.size()) {
                                      status.roms[sel] = enriched;
                                  }
                                  for (auto& mg : status.romsAll) {
                                      if (mg.id == enriched.id) {
                                          mg = enriched;
                                          break;
                                      }
                                  }
                                  romm::QueueItem qi;
                                  qi.game = enriched;
                                  qi.bundle = bundle;
                                  qi.state = romm::QueueState::Pending;
                                  status.downloadQueue.push_back(std::move(qi));
                                  status.downloadQueueRevision++;
                                  if (filterNeedsState(status.romFilter)) {
                                      rebuildVisibleRomsLocked(false);
                                  } else {
                                      status.romsRevision++;
                                  }
                                  status.selectedQueueIndex = 0;
                                  status.queueReorderActive = false;
                                  status.downloadCompleted = false; // new work pending, clear stale banner
                                  status.prevQueueView = Status::View::DETAIL;
                                  status.currentView = Status::View::QUEUE;
                              }
                              recomputeTotals();
                              persistQueueState();
                              romm::logLine("Queued ROM: " + enriched.title +
                                            " | Queue size=" + std::to_string(status.downloadQueue.size()));
                              gViewTraceFrames = 8;
                              viewChangedThisFrame = true;
                          }
                    } else if (currentView == Status::View::DIAGNOSTICS) {
                        exportDiagnosticsSummary();
                        if (!diagProbeJobs.busy()) submitDiagnosticsProbe();
                    }
                    break;
                }
                case romm::Action::OpenSearch: {
                    // Minus: ROM search (ROMS view) OR queue delete (QUEUE view when reorder-active).
                    {
                        bool didDelete = false;
                        std::string deletedTitle;
                        {
                            std::lock_guard<std::mutex> lock(status.mutex);
                            if (status.currentView == Status::View::QUEUE && status.queueReorderActive) {
                                int idx = status.selectedQueueIndex;
                                if (idx >= 0 && idx < (int)status.downloadQueue.size()) {
                                    const auto& qi = status.downloadQueue[(size_t)idx];
                                    if (qi.state == romm::QueueState::Pending) {
                                        deletedTitle = qi.game.title;
                                        status.downloadQueue.erase(status.downloadQueue.begin() + idx);
                                        status.downloadQueueRevision++;
                                        status.queueReorderActive = false;
                                        if (status.selectedQueueIndex >= (int)status.downloadQueue.size()) {
                                            status.selectedQueueIndex = status.downloadQueue.empty() ? 0 : (int)status.downloadQueue.size() - 1;
                                        }
                                        didDelete = true;
                                    } else {
                                        romm::logLine("Queue delete ignored (only pending items can be removed).");
                                    }
                                }
                            }
                        }
                        if (didDelete) {
                            recomputeTotals();
                            persistQueueState();
                            romm::logLine("Removed from queue: " + deletedTitle);
                        }
                        if (didDelete) break;
                    }

                    std::string curQuery;
                    bool inRoms = false;
                    std::string platformId;
                    size_t romCount = 0;
                    {
                        std::lock_guard<std::mutex> lock(status.mutex);
                        inRoms = (status.currentView == Status::View::ROMS);
                        curQuery = status.romSearchQuery;
                        platformId = status.currentPlatformId;
                        romCount = status.romsAll.size();
                    }
                    if (!inRoms) break;
                    std::string next = curQuery;
                    if (promptSearchQuery(next)) {
                        next = normalizeSearchText(next);
                        bool submitRemote = false;
                        PendingRemoteSearch req;
                        if (next != curQuery) {
                            {
                                std::lock_guard<std::mutex> lock(status.mutex);
                                status.romSearchQuery = next;
                                status.romListOptionsRevision++;
                                status.selectedRomIndex = 0;
                                if (next.empty()) {
                                    remoteSearchActive = false;
                                    remoteSearchGames.clear();
                                    remoteSearchQuery.clear();
                                    remoteSearchPlatformId.clear();
                                    remoteSearchRevision++;
                                } else if (!platformId.empty() && romCount >= kRemoteSearchThreshold) {
                                    req.pid = platformId;
                                    req.query = next;
                                    req.limit = kRemoteSearchLimit;
                                    submitRemote = true;
                                } else {
                                    remoteSearchActive = false;
                                    remoteSearchGames.clear();
                                    remoteSearchQuery.clear();
                                    remoteSearchPlatformId.clear();
                                    remoteSearchRevision++;
                                }
                            }
                            if (submitRemote) {
                                submitRemoteSearch(req);
                            }
                            romm::logLine("ROM search updated: " + (next.empty() ? std::string("<cleared>") : next));
                        }
                    }
                    break;
                }
                case romm::Action::OpenDiagnostics: {
                    {
                        bool toggled = false;
                        bool now = false;
                        std::lock_guard<std::mutex> lock(status.mutex);
                        if (status.currentView == Status::View::DOWNLOADING) {
                            status.burnInMode = !status.burnInMode;
                            toggled = true;
                            now = status.burnInMode;
                        }
                        if (toggled) {
                            romm::logLine(std::string("Burn-in prevention screen ") + (now ? "enabled" : "disabled") + ".");
                            viewChangedThisFrame = true;
                            break;
                        }
                    }
                    bool shouldProbe = false;
                    {
                        std::lock_guard<std::mutex> lock(status.mutex);
                        if (status.currentView == Status::View::PLATFORMS) {
                            status.prevDiagnosticsView = status.currentView;
                            status.currentView = Status::View::DIAGNOSTICS;
                            viewChangedThisFrame = true;
                            shouldProbe = !status.diagnosticsProbeInFlight;
                        } else if (status.currentView == Status::View::DIAGNOSTICS) {
                            shouldProbe = !status.diagnosticsProbeInFlight;
                        } // Ignore from all other views.
                    }
                    if (shouldProbe) submitDiagnosticsProbe();
                    break;
                }
                case romm::Action::OpenUpdater: {
                    bool opened = false;
                    {
                        std::lock_guard<std::mutex> lock(status.mutex);
                        if (status.currentView == Status::View::PLATFORMS) {
                            status.prevUpdaterView = status.currentView;
                            status.currentView = Status::View::UPDATER;
                            opened = true;
                        }
                    }
                    if (opened) {
                        romm::logLine("Opened UPDATER view.");
                        gViewTraceFrames = 6;
                        viewChangedThisFrame = true;
                    }
                    break;
                }
                case romm::Action::Back: {
                    // B/Back: navigate up one level or return from queue
                    Status::View cur;
                    {
                        std::lock_guard<std::mutex> lock(status.mutex);
                        cur = status.currentView;
                    }
                    romm::logLine(std::string("Back pressed in view=") + viewName(cur));
                    bool persistQueueAfter = false;
                    {
                        std::lock_guard<std::mutex> lock(status.mutex);
                        // Allow cancelling a platform ROM fetch while staying on PLATFORMS.
                        if (cur == Status::View::PLATFORMS && status.netBusy.load()) {
                            status.romFetchGeneration++;
                            status.netBusy.store(false);
                            status.netBusyWhat.clear();
                            remoteSearchInFlight = false;
                            romFetchJobs.clearPending();
                            remoteSearchJobs.clearPending();
                            romm::logLine("Cancelled ROM fetch.");
                            viewChangedThisFrame = true;
                        } else
                        if (cur == Status::View::ROMS) {
                            status.currentView = Status::View::PLATFORMS;
                            remoteSearchActive = false;
                            remoteSearchGames.clear();
                            remoteSearchQuery.clear();
                            remoteSearchPlatformId.clear();
                            remoteSearchRevision++;
                            resetNav();
                            gViewTraceFrames = 8;
                            romm::logLine("Back to PLATFORMS.");
                            viewChangedThisFrame = true;
                        } else if (cur == Status::View::DETAIL) {
                            status.currentView = Status::View::ROMS;
                            gViewTraceFrames = 8;
                            romm::logLine("Back to ROMS from DETAIL.");
                            viewChangedThisFrame = true;
                        } else if (cur == Status::View::DOWNLOADING) {
                            status.currentView = Status::View::QUEUE;
                            status.burnInMode = false;
                            gViewTraceFrames = 8;
                            romm::logLine("Back to QUEUE from DOWNLOADING.");
                            viewChangedThisFrame = true;
                        } else if (cur == Status::View::QUEUE) {
                            if (status.queueReorderActive) {
                                status.queueReorderActive = false;
                                // Persist order changes as the user "drops" the selection.
                                // (No per-swap disk writes; this keeps SD I/O low.)
                                romm::logLine("Queue reorder disabled.");
                                persistQueueAfter = true;
                                viewChangedThisFrame = true;
                            } else {
                                Status::View dest = status.prevQueueView;
                                if (dest == Status::View::QUEUE || dest == Status::View::DOWNLOADING) dest = Status::View::PLATFORMS;
                                status.currentView = dest;
                                gViewTraceFrames = 8;
                                romm::logLine(std::string("Back from QUEUE to ") + viewName(dest) + ".");
                                viewChangedThisFrame = true;
                            }
                        } else if (cur == Status::View::PLATFORMS) {
                            romm::logLine("Back on PLATFORMS ignored.");
                        } else if (cur == Status::View::DIAGNOSTICS) {
                            status.currentView = status.prevDiagnosticsView;
                            gViewTraceFrames = 8;
                            romm::logLine(std::string("Back from DIAGNOSTICS to ") + viewName(status.currentView) + ".");
                            viewChangedThisFrame = true;
                        } else if (cur == Status::View::UPDATER) {
                            status.currentView = status.prevUpdaterView;
                            gViewTraceFrames = 8;
                            romm::logLine(std::string("Back from UPDATER to ") + viewName(status.currentView) + ".");
                            viewChangedThisFrame = true;
                        } else if (cur == Status::View::ERROR) {
                            running = false;
                        }
                    }
                    if (persistQueueAfter) persistQueueState();
                    break;
                }
                case romm::Action::OpenQueue:
                    // Y/X: open queue from any view; remember where we came from (except downloading)
                    // Only update prevQueueView if we're not already in QUEUE
                    {
                        std::lock_guard<std::mutex> lock(status.mutex);
                        if (status.currentView != Status::View::QUEUE &&
                            status.currentView != Status::View::DOWNLOADING) { // keep last real view (plat/roms/detail)
                            status.prevQueueView = status.currentView;
                        }
                        status.currentView = Status::View::QUEUE;
                        status.selectedQueueIndex = 0;
                        status.queueReorderActive = false;
                        romm::logLine(std::string("Opened queue view from ") + viewName(status.prevQueueView) +
                                      " items=" + std::to_string(status.downloadQueue.size()));
                    }
                    gViewTraceFrames = 8;
                    viewChangedThisFrame = true;
                    break;
                case romm::Action::StartDownload:
                    {
                        Status::View v;
                        {
                            std::lock_guard<std::mutex> lock(status.mutex);
                            v = status.currentView;
                        }
                        if (v == Status::View::UPDATER) {
                            submitUpdateDownload();
                            viewChangedThisFrame = true;
                            break;
                        }
                    }
                    // X in QUEUE: start downloads and show downloading view
                    // TODO(queue UX): dedupe entries and surface per-item failures/history in UI.
                    {
                        bool allowStart = false;
                        {
                            std::lock_guard<std::mutex> lock(status.mutex);
                            allowStart = (status.currentView == Status::View::QUEUE && !status.downloadQueue.empty());
                        }
                        if (allowStart) {
                            if (status.downloadWorkerRunning.load()) {
                                romm::logLine("Download already running; opening DOWNLOADING view.");
                                std::lock_guard<std::mutex> lock(status.mutex);
                                status.currentView = Status::View::DOWNLOADING;
                                gViewTraceFrames = 8;
                                viewChangedThisFrame = true;
                            } else {
                                {
                                    std::lock_guard<std::mutex> lock(status.mutex);
                                    status.currentView = Status::View::DOWNLOADING;
                                    status.burnInMode = false;
                                    status.currentDownloadIndex.store(0);
                                    status.currentDownloadedBytes.store(0);
                                    status.totalDownloadedBytes.store(0);
                                    status.totalDownloadBytes.store(0);
                                    status.downloadCompleted = false; // clear any prior completion banner
                                    for (auto& q : status.downloadQueue) {
                                        uint64_t sz = q.bundle.totalSize();
                                        if (sz == 0) sz = q.game.sizeBytes;
                                        status.totalDownloadBytes.fetch_add(sz);
                                    }
                                    if (!status.downloadQueue.empty()) {
                                        const auto& first = status.downloadQueue[0];
                                        uint64_t firstSize = first.bundle.totalSize();
                                        if (firstSize == 0) firstSize = first.game.sizeBytes;
                                        status.currentDownloadSize.store(firstSize);
                                        status.currentDownloadTitle = first.bundle.title.empty() ? first.game.title : first.bundle.title;
                                        status.downloadQueue[0].state = romm::QueueState::Downloading;
                                        status.downloadQueueRevision++;
                                    }
                                }
                                romm::logLine("Starting downloads for queue size=" + std::to_string(status.downloadQueue.size()) +
                                              " totalBytes=" + std::to_string(status.totalDownloadBytes.load()));
                                startDownloadWorker(status, config);
                                gViewTraceFrames = 8;
                                viewChangedThisFrame = true;
                            }
                        } else {
                            romm::logLine("StartDownload outside QUEUE; ignoring.");
                        }
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

        renderStatus(renderer, status, config);

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
    persistQueueState();
    if (speedTestThread.joinable()) {
        speedTestThread.join();
    }
    romFetchJobs.stop();
    remoteSearchJobs.stop();
    diagProbeJobs.stop();
    // Restore default sleep/dim behavior on exit.
    appletSetMediaPlaybackState(false);
    appletSetAutoSleepDisabled(false);
    if (gCoverTexture) SDL_DestroyTexture(gCoverTexture);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    if (pad) SDL_GameControllerClose(pad);
    SDL_Quit();
    if (romfsReady) romfsExit();
    gCoverLoader.stop();
    psmExit();
    timeExit();
    fsdevUnmountAll();
    nifmExit();
    socketExit();
    return 0;
}
