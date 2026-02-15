#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace romm {

// Canonical location enforced for the installed app. If argv0 isn't a .nro under sdmc:/switch/,
// fall back to the default homebrew install path.
std::string canonicalSelfNroPath(const std::string& argv0,
                                 const std::string& fallback = "sdmc:/switch/romm_switch_client/romm-switch-client.nro");

// Read a small text file and trim surrounding whitespace/newlines.
bool readTextFileTrim(const std::string& path, std::string& out);

// Write a text file, creating parent directories if needed (best-effort).
bool writeTextFileEnsureParent(const std::string& path, const std::string& text);

// Best-effort file removal.
void removeFileBestEffort(const std::string& path);

// Very lightweight sanity check for NRO magic.
bool fileLooksLikeNro(const std::string& path);

// Updater storage: keep /switch tidy by staging under download cache.
std::string computeUpdateDirFromDownloadDir(const std::string& downloadDir);
std::string defaultStagedUpdatePath(const std::string& updateDir);
std::string defaultBackupPath(const std::string& updateDir);

struct ApplySelfUpdateResult {
    bool hadPending{false};
    bool applied{false};
    bool pendingCleared{false};
    std::string stagedPath;
    std::string error;
};

// Apply a staged update (if pendingPath points at one).
// This is intentionally "apply-on-next-launch" so we never overwrite the running binary.
ApplySelfUpdateResult applyPendingSelfUpdate(const std::string& selfNroPath,
                                            const std::string& pendingPath,
                                            const std::function<void(const std::string&)>& logFn);

} // namespace romm

