#pragma once

#include "romm/status.hpp"
#include <mutex>

namespace romm {

// Decide whether a game can be enqueued in the current session.
// Blocks if the same game id is already in the active queue or appears in downloadHistory as Completed.
inline bool canEnqueueGame(const Status& status, const Game& game) {
    const std::string& id = game.id;
    std::lock_guard<std::mutex> lock(status.mutex);
    for (const auto& qi : status.downloadQueue) {
        if (qi.game.id == id) return false;
    }
    for (const auto& qi : status.downloadHistory) {
        if (qi.game.id == id && qi.state == QueueState::Completed) return false;
    }
    return true;
}

} // namespace romm
