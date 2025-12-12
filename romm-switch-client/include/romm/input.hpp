#pragma once

#include <SDL2/SDL.h>

namespace romm {

enum class Action {
    None,
    Up,
    Down,
    Left,
    Right,
    Select,
    OpenQueue,
    Back,
    StartDownload,
    Quit
};

Action translateEvent(const SDL_Event& e);

} // namespace romm
