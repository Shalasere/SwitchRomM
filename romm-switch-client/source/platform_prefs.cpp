#include "romm/platform_prefs.hpp"
#include "romm/logger.hpp"

#include <fstream>
#include <algorithm>

#include "mini/json.hpp"

namespace romm {

static std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return s;
}

static std::string normalizeExt(const std::string& ext) {
    std::string e = toLower(ext);
    if (e.empty()) return e;
    if (e.front() != '.') e.insert(e.begin(), '.');
    return e;
}

PlatformPrefs defaultPlatformPrefs() {
    PlatformPrefs prefs;
    prefs.version = 1;
    prefs.defaultMode = "bundle_best";
    prefs.defaultIgnoreExt = {".nfo",".txt",".md",".pdf",".jpg",".png",".webp",".db",".xml",".json",".log"};
    prefs.bySlug["switch"] = PlatformPref{
        /*mode*/"single_best",
        /*preferExt*/{".xci",".nsp"},
        /*ignoreExt*/{}
    };
    return prefs;
}

static bool parsePlatformPrefsJson(const std::string& body, PlatformPrefs& out, std::string& err) {
    mini::Object obj;
    if (!mini::parse(body, obj)) {
        err = "Failed to parse platform prefs JSON";
        return false;
    }
    PlatformPrefs prefs = defaultPlatformPrefs();
    if (auto v = obj.find("version"); v != obj.end() && v->second.type == mini::Value::Type::Number) {
        prefs.version = static_cast<int>(v->second.number);
    }
    if (auto d = obj.find("defaults"); d != obj.end() && d->second.type == mini::Value::Type::Object) {
        const auto& def = d->second.object;
        if (auto m = def.find("mode"); m != def.end() && m->second.type == mini::Value::Type::String) {
            prefs.defaultMode = m->second.str;
        }
        if (auto ig = def.find("ignore_ext"); ig != def.end() && ig->second.type == mini::Value::Type::Array) {
            prefs.defaultIgnoreExt.clear();
            for (const auto& it : ig->second.array) {
                if (it.type == mini::Value::Type::String) prefs.defaultIgnoreExt.push_back(normalizeExt(it.str));
            }
        }
    }
    if (auto p = obj.find("platforms"); p != obj.end() && p->second.type == mini::Value::Type::Object) {
        for (const auto& kv : p->second.object) {
            if (kv.second.type != mini::Value::Type::Object) continue;
            PlatformPref pp;
            const auto& po = kv.second.object;
            if (auto m = po.find("mode"); m != po.end() && m->second.type == mini::Value::Type::String) {
                pp.mode = m->second.str;
            }
            if (auto pe = po.find("prefer_ext"); pe != po.end() && pe->second.type == mini::Value::Type::Array) {
                for (const auto& it : pe->second.array) {
                    if (it.type == mini::Value::Type::String) pp.preferExt.push_back(normalizeExt(it.str));
                }
            }
            if (auto ig = po.find("ignore_ext"); ig != po.end() && ig->second.type == mini::Value::Type::Array) {
                for (const auto& it : ig->second.array) {
                    if (it.type == mini::Value::Type::String) pp.ignoreExt.push_back(normalizeExt(it.str));
                }
            }
            if (auto av = po.find("avoid_name_tokens"); av != po.end() && av->second.type == mini::Value::Type::Array) {
                for (const auto& it : av->second.array) {
                    if (it.type == mini::Value::Type::String) pp.avoidNameTokens.push_back(toLower(it.str));
                }
            }
            prefs.bySlug[toLower(kv.first)] = pp;
        }
    }
    out = std::move(prefs);
    return true;
}

static bool readFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f.good()) return false;
    std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    out.swap(data);
    return true;
}

bool loadPlatformPrefs(const std::string& mode, const std::string& sdPath, const std::string& romfsPath,
                       PlatformPrefs& outPrefs, std::string& outError) {
    // Mode: auto (prefer SD), sd (force SD), romfs (force romfs)
    const std::string m = toLower(mode);
    const bool trySdFirst = (m == "auto" || m == "sd");
    const bool allowRomfs = (m != "sd");
    std::string body;
    if (trySdFirst) {
        if (readFile(sdPath, body)) {
            if (parsePlatformPrefsJson(body, outPrefs, outError)) return true;
            return false;
        } else if (m == "sd") {
            outError = "Platform prefs SD path missing: " + sdPath;
            return false;
        }
    }
    if (allowRomfs) {
        if (readFile(romfsPath, body)) {
            if (parsePlatformPrefsJson(body, outPrefs, outError)) return true;
            return false;
        }
    }
    // Fallback to built-in defaults
    outPrefs = defaultPlatformPrefs();
    if (outError.empty()) {
        outError = "Platform prefs not found; using built-in defaults.";
    }
    return true;
}

} // namespace romm
