#include "romm/input.hpp"
#include "romm/logger.hpp"

namespace romm {

Action translateEvent(const SDL_Event& e) {
    if (e.type == SDL_QUIT) return Action::Quit;
    // Use SDL controller events (Nintendo layout) and ignore raw joystick duplicates.
    if (e.type == SDL_JOYBUTTONDOWN) {
        romm::logDebug("Ignoring SDL_JOYBUTTONDOWN code=" + std::to_string(e.jbutton.button), "INPUT");
        return Action::None;
    }
    // Map controller buttons to Actions (Switch/SDL positional codes):
    // Physical Nintendo labels:
    // - B (bottom) -> back
    // - A (right)  -> select
    // - Y (left)   -> queue
    // - X (top)    -> start downloads
    // - Minus      -> search
    // - R          -> diagnostics
    // - L          -> updater
    //
    // SDL positional codes (with SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS=0):
    // - A = bottom (B on Nintendo)
    // - B = right  (A on Nintendo)
    // - X = left   (Y on Nintendo)
    // - Y = top    (X on Nintendo)
    if (e.type == SDL_CONTROLLERBUTTONDOWN) {
        static Uint32 lastTicks[SDL_CONTROLLER_BUTTON_MAX] = {};
        Uint32 now = SDL_GetTicks();
        uint8_t code = e.cbutton.button;
        romm::logDebug("SDL controller button pressed code=" + std::to_string(code), "INPUT");
        if (code < SDL_CONTROLLER_BUTTON_MAX) {
            if (now - lastTicks[code] < 40) { // light debounce for double-fires
                romm::logDebug("Debounced duplicate controller code=" + std::to_string(code), "INPUT");
                return Action::None;
            }
            lastTicks[code] = now;
        }
        Action act = Action::None;
        switch (code) {
            case SDL_CONTROLLER_BUTTON_DPAD_UP: act = Action::Up; break;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN: act = Action::Down; break;
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT: act = Action::Left; break;
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: act = Action::Right; break;
            // Map based on SDL positional codes so on-screen Nintendo labels match physical buttons.
            case SDL_CONTROLLER_BUTTON_A: act = Action::Back; break;            // bottom (B) -> back
            case SDL_CONTROLLER_BUTTON_B: act = Action::Select; break;          // right (A) -> select/confirm
            case SDL_CONTROLLER_BUTTON_X: act = Action::OpenQueue; break;       // left (Y) -> queue view
            case SDL_CONTROLLER_BUTTON_Y: act = Action::StartDownload; break;   // top (X) -> start downloads
            case SDL_CONTROLLER_BUTTON_BACK: act = Action::OpenSearch; break;   // Minus -> search
            case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: act = Action::OpenDiagnostics; break; // R -> diagnostics
            case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: act = Action::OpenUpdater; break; // L -> updater
            case SDL_CONTROLLER_BUTTON_START: act = Action::Quit; break;        // Plus -> exit app
            default: break;
        }
        if (act != Action::None) {
            romm::logDebug("Mapped controller code " + std::to_string(code) +
                           " to action " + std::to_string(static_cast<int>(act)),
                           "INPUT");
        }
        return act;
    }
    return Action::None;
}

} // namespace romm
