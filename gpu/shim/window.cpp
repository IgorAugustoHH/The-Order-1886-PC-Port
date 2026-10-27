// bbport: SDL3 window for the Vulkan swapchain (X11 or Wayland).
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <SDL3/SDL.h>
#include "common/assert.h"
#include "common/logging/log.h"
#include "sdl_window.h"
#include "bbport_overlay.h"

// TOport: mouse look. While the game has the mouse (window focused, no menu or text entry),
// relative mode hides and holds the cursor and the motion is summed here for runtime_pad.c,
// which reads it as the right stick (bbgpu_mouse_take). BB_MOUSE=0 leaves the mouse alone.
namespace {
std::mutex mouse_mutex;
float mouse_dx = 0.0f, mouse_dy = 0.0f;
std::atomic<bool> mouse_held{false};
bool MouseEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_MOUSE");
        return !(env && env[0] == '0');
    }();
    return enabled;
}
} // namespace

extern "C" int bbgpu_mouse_take(float* dx, float* dy, uint32_t* buttons) {
    std::scoped_lock lock{mouse_mutex};
    *dx = mouse_dx;
    *dy = mouse_dy;
    mouse_dx = mouse_dy = 0.0f;
    if (!mouse_held.load(std::memory_order_relaxed)) {
        *buttons = 0;
        return 0;
    }
    *buttons = SDL_GetMouseState(nullptr, nullptr);
    return 1;
}

namespace Frontend {

WindowSDL::WindowSDL(s32 width_, s32 height_, const char* title) : width{width_}, height{height_} {
    // TOport test: BB_WINDOW_SIZE=WxH opens the window at that size (e.g. a 21:9 window).
    if (const char* size = std::getenv("BB_WINDOW_SIZE")) {
        s32 w = 0, h = 0;
        if (std::sscanf(size, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
            width_ = w;
            height_ = h;
        }
    }
    // Gamepads are sampled by runtime_pad.c; their events are pumped here with the window's.
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        UNREACHABLE_MSG("Failed to initialize SDL video: {}", SDL_GetError());
    }
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
    // TOport test: BB_WINDOW_POS=X,Y places it on the desktop (e.g. a 21:9 window across two
    // monitors) and BB_WINDOW_BORDERLESS=1 drops the title bar.
    s32 pos_x = SDL_WINDOWPOS_CENTERED, pos_y = SDL_WINDOWPOS_CENTERED;
    if (const char* pos = std::getenv("BB_WINDOW_POS")) {
        std::sscanf(pos, "%d,%d", &pos_x, &pos_y);
    }
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, pos_x);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, pos_y);
    if (const char* borderless = std::getenv("BB_WINDOW_BORDERLESS"); borderless && borderless[0] == '1') {
        SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_BORDERLESS_BOOLEAN, true);
    }
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width_);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height_);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN, true);
    const char* fullscreen = std::getenv("BB_FULLSCREEN");
    // BB_FULLSCREEN: 1 borderless fullscreen (desktop mode), 2 exclusive fullscreen at the
    // display mode closest to the game's size (as shadPS4's "Exclusive" mode).
    const bool exclusive = fullscreen && fullscreen[0] == '2';
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN,
                           fullscreen && fullscreen[0] == '1');
    base_title = title;
    window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    ASSERT_MSG(window, "Failed to create window: {}", SDL_GetError());
    if (exclusive) {
        SDL_DisplayMode mode{};
        const SDL_DisplayID display = SDL_GetDisplayForWindow(window);
        if (SDL_GetClosestFullscreenDisplayMode(display, width_, height_, 0.0f, true, &mode) &&
            SDL_SetWindowFullscreenMode(window, &mode)) {
            std::printf("Window: exclusive fullscreen %dx%d @ %.2f Hz\n", mode.w, mode.h,
                        mode.refresh_rate);
        } else {
            std::printf("Window: exclusive mode unavailable (%s); borderless fullscreen\n",
                        SDL_GetError());
        }
        SDL_SetWindowFullscreen(window, true);
        SDL_SyncWindow(window);
    }

    const char* driver = SDL_GetCurrentVideoDriver();
    const SDL_PropertiesID wp = SDL_GetWindowProperties(window);
#ifdef _WIN32
    if (driver && !std::strcmp(driver, "windows")) {
        window_info.type = WindowSystemType::Windows;
        window_info.render_surface = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    } else
#endif
    if (driver && !std::strcmp(driver, "x11")) {
        window_info.type = WindowSystemType::X11;
        window_info.display_connection = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
        window_info.render_surface = reinterpret_cast<void*>(SDL_GetNumberProperty(wp, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
    } else if (driver && !std::strcmp(driver, "wayland")) {
        window_info.type = WindowSystemType::Wayland;
        window_info.display_connection = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
        window_info.render_surface = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
    } else {
        UNREACHABLE_MSG("Unsupported SDL video driver {}", driver ? driver : "(none)");
    }
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    width = w;
    height = h;
    LOG_INFO(Frontend, "Window {}x{} on {}", w, h, driver);
}

WindowSDL::~WindowSDL() {
    SDL_DestroyWindow(window);
}

void WindowSDL::BeginTextInput(const std::string& initial, const std::string& prompt) {
    std::scoped_lock lock{text_mutex};
    text = initial;
    text_prompt = prompt;
    text_state = 0;
    text_requested = true;
}

int WindowSDL::PollTextInput(std::string& out) {
    std::scoped_lock lock{text_mutex};
    out = text;
    return text_state;
}

void WindowSDL::UpdateTextTitle() {
    const std::string title = text_active ? base_title + " \u2014 " + text_prompt + ": " + text + "_  (Enter = OK, Esc = cancel)"
                                          : base_title;
    SDL_SetWindowTitle(window, title.c_str());
    BbOverlay::SetTextEntry(text_active, text_prompt, text);
}

bool WindowSDL::PollEvents() {
    {
        std::scoped_lock lock{text_mutex};
        if (text_requested) { // SDL text input must be toggled from the window thread
            text_requested = false;
            text_active = true;
            SDL_StartTextInput(window);
            UpdateTextTitle();
        }
    }
    if (!text_active) {
        BbOverlay::UpdateTextInput(window);
    }
    if (MouseEnabled()) {
        const bool focused = (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
        const bool hold = focused && !text_active && !BbOverlay::CapturesInput();
        if (hold != mouse_held.load(std::memory_order_relaxed)) {
            SDL_SetWindowRelativeMouseMode(window, hold);
            mouse_held.store(hold, std::memory_order_relaxed);
            std::scoped_lock lock{mouse_mutex};
            mouse_dx = mouse_dy = 0.0f;
        }
    }
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_MOUSE_MOTION && mouse_held.load(std::memory_order_relaxed)) {
            std::scoped_lock lock{mouse_mutex};
            mouse_dx += event.motion.xrel;
            mouse_dy += event.motion.yrel;
        }
        if (text_active && (event.type == SDL_EVENT_TEXT_INPUT || event.type == SDL_EVENT_KEY_DOWN)) {
            std::scoped_lock lock{text_mutex};
            if (event.type == SDL_EVENT_TEXT_INPUT) {
                text += event.text.text;
            } else if (event.key.key == SDLK_BACKSPACE && !text.empty()) {
                size_t cut = text.size() - 1; // drop one UTF-8 code point
                while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
                text.erase(cut);
            } else if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER || event.key.key == SDLK_ESCAPE) {
                text_state = event.key.key == SDLK_ESCAPE ? 2 : 1;
                text_active = false;
                SDL_StopTextInput(window);
            }
            UpdateTextTitle();
            continue;
        }
        // The controller finishes the text dialog too: Cross (A) accepts, Circle (B) cancels.
        if (text_active && event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
            (event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH ||
             event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST)) {
            std::scoped_lock lock{text_mutex};
            text_state = event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH ? 1 : 2;
            text_active = false;
            SDL_StopTextInput(window);
            UpdateTextTitle();
            continue;
        }
        if (BbOverlay::HandleEvent(event)) {
            continue;
        }
        switch (event.type) {
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_RESIZED: {
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(window, &w, &h);
            width = w;
            height = h;
            break;
        }
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            is_open = false;
            break;
        default:
            break;
        }
    }
    return is_open;
}

} // namespace Frontend
