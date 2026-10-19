// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_overlay.h"

#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#include <SDL3/SDL.h>
#include "bbport_settings.h"
#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

// DejaVu Sans (Cyrillic), embedded (third_party/fonts, Bitstream Vera license).
#ifdef _WIN32
asm(".section .rdata,\"dr\"\n"
    ".balign 16\n"
    ".global bb_font_ttf\n"
    "bb_font_ttf:\n"
    ".incbin \"" BB_FONT_PATH "\"\n"
    ".global bb_font_ttf_end\n"
    "bb_font_ttf_end:\n"
    ".text\n");
#else
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden bb_font_ttf\n"
    ".global bb_font_ttf\n"
    "bb_font_ttf:\n"
    ".incbin \"" BB_FONT_PATH "\"\n"
    ".hidden bb_font_ttf_end\n"
    ".global bb_font_ttf_end\n"
    "bb_font_ttf_end:\n"
    ".previous\n");
#endif
extern "C" const unsigned char bb_font_ttf[];
extern "C" const unsigned char bb_font_ttf_end[];

extern "C" void runtime_restart(void); // bb-probe (probe.c)

namespace BbOverlay {

namespace {

std::mutex imgui_mutex; // the ImGui context: window thread (input) and present thread
bool initialized = false;
std::atomic<bool> menu_open{false};
bool dirty = false; // settings changed while open: saved on close
// The game's text dialog (SetTextEntry), guarded by imgui_mutex.
bool text_entry_active = false;
std::string text_entry_prompt, text_entry_text;
float base_scale = 1.0f;

// Present rate for the FPS counter.
std::chrono::steady_clock::time_point last_present{};
float frame_ms_avg = 0.0f;

void SetOpen(bool value) {
    if (menu_open.exchange(value) == value) {
        return;
    }
    ImGui::GetIO().MouseDrawCursor = value;
    if (!value && dirty) {
        dirty = false;
        BbSettings::Save();
    }
}

ImGuiKey KeyFromSdl(SDL_Keycode key) {
    switch (key) {
    case SDLK_TAB: return ImGuiKey_Tab;
    case SDLK_LEFT: return ImGuiKey_LeftArrow;
    case SDLK_RIGHT: return ImGuiKey_RightArrow;
    case SDLK_UP: return ImGuiKey_UpArrow;
    case SDLK_DOWN: return ImGuiKey_DownArrow;
    case SDLK_PAGEUP: return ImGuiKey_PageUp;
    case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
    case SDLK_HOME: return ImGuiKey_Home;
    case SDLK_END: return ImGuiKey_End;
    case SDLK_DELETE: return ImGuiKey_Delete;
    case SDLK_BACKSPACE: return ImGuiKey_Backspace;
    case SDLK_SPACE: return ImGuiKey_Space;
    case SDLK_RETURN: return ImGuiKey_Enter;
    case SDLK_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDLK_ESCAPE: return ImGuiKey_Escape;
    case SDLK_LCTRL: return ImGuiKey_LeftCtrl;
    case SDLK_RCTRL: return ImGuiKey_RightCtrl;
    case SDLK_LSHIFT: return ImGuiKey_LeftShift;
    case SDLK_RSHIFT: return ImGuiKey_RightShift;
    case SDLK_LALT: return ImGuiKey_LeftAlt;
    case SDLK_RALT: return ImGuiKey_RightAlt;
    default: return ImGuiKey_None;
    }
}

ImGuiKey KeyFromGamepad(u8 button) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return ImGuiKey_GamepadFaceDown;
    case SDL_GAMEPAD_BUTTON_EAST: return ImGuiKey_GamepadFaceRight;
    case SDL_GAMEPAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
    case SDL_GAMEPAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ImGuiKey_GamepadL1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ImGuiKey_GamepadR1;
    case SDL_GAMEPAD_BUTTON_START: return ImGuiKey_GamepadStart;
    case SDL_GAMEPAD_BUTTON_BACK: return ImGuiKey_GamepadBack;
    default: return ImGuiKey_None;
    }
}

float PixelDensity(SDL_WindowID id) {
    SDL_Window* window = SDL_GetWindowFromID(id);
    const float density = window ? SDL_GetWindowPixelDensity(window) : 1.0f;
    return density > 0.0f ? density : 1.0f;
}

// Marks the settings dirty when a widget changed them.
template <typename T>
void Store(std::atomic<T>& target, T value, bool changed) {
    if (changed) {
        target = value;
        dirty = true;
    }
}

void Checkbox(const char* label, std::atomic<bool>& value) {
    bool v = value;
    Store(value, v, ImGui::Checkbox(label, &v));
}

void Slider(const char* label, std::atomic<float>& value, float lo, float hi) {
    float v = value;
    Store(value, v, ImGui::SliderFloat(label, &v, lo, hi, "%.2f"));
}

void Hint(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// TOport: The Order: 1886 settings only (no Bloodborne options). Choices are written to
// bbport.ini when the menu closes; run.py applies the restart ones on the next start.
bool OrderCheckbox(const char* id, const char* label, std::atomic<bool>& value) {
    ImGui::PushID(id);
    bool v = value.load();
    const bool changed = ImGui::Checkbox(label, &v);
    if (changed) {
        value.store(v);
        dirty = true;
    }
    ImGui::PopID();
    return changed;
}

template <typename T, size_t N>
void OrderCombo(const char* id, const char* label, std::atomic<T>& value,
                const T (&values)[N], const char* const (&names)[N]) {
    ImGui::PushID(id);
    const T current = value.load();
    const char* preview = "?";
    for (size_t i = 0; i < N; ++i) {
        if (values[i] == current) preview = names[i];
    }
    if (ImGui::BeginCombo(label, preview)) {
        for (size_t i = 0; i < N; ++i) {
            if (ImGui::Selectable(names[i], values[i] == current)) {
                value.store(values[i]);
                dirty = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::PopID();
}

/// TOport: the menu in the language chosen in the launcher (bbport.ini ui_language).
const char* L(const char* pt, const char* en) {
    return BbSettings::Get().ui_english.load() ? en : pt;
}

void Menu() {
    auto& s = BbSettings::Get();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + 40.0f * base_scale,
                                   viewport->WorkPos.y + 40.0f * base_scale),
                            ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(620.0f * base_scale, 0.0f), ImGuiCond_Appearing);
    bool keep_open = true;
    if (!ImGui::Begin("The Order: 1886 — TOport (Insert)", &keep_open,
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    ImGui::Text("%.0f FPS  (%.1f ms)", frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f,
                frame_ms_avg);

    ImGui::SeparatorText(L("Imagem", "Graphics"));
    static const int aa_values[] = {BbSettings::OrderAaGame,        BbSettings::OrderAaDlaa,
                                    BbSettings::OrderAaQuality,     BbSettings::OrderAaBalanced,
                                    BbSettings::OrderAaPerformance, BbSettings::OrderAaUltra};
    const char* const aa_names[] = {L("TAA original do jogo", "The game's original TAA"), L("DLSS DLAA (resolução nativa)", "DLSS DLAA (native resolution)"),
                                           L("DLSS Qualidade", "DLSS Quality"), L("DLSS Equilibrado", "DLSS Balanced"),
                                           L("DLSS Desempenho", "DLSS Performance"), L("DLSS Ultra Desempenho", "DLSS Ultra Performance")};
    OrderCombo("aa", "Anti-aliasing", s.order_aa, aa_values, aa_names);
    Hint(L("DLSS (NVIDIA RTX) substitui a resolução temporal do próprio jogo. Abaixo de DLAA, a cena é renderizada menor e reconstruída. Reinicie para aplicar.", "DLSS (NVIDIA RTX) replaces the game's own temporal resolve. Below DLAA the scene is rendered smaller and reconstructed. Restart to apply."));
    if (s.order_aa != BbSettings::OrderAaGame && s.dlss_problem.load()) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "DLSS: %s", s.dlss_problem.load());
    }
    // 4K (BB_ORDER_RESOLUTION=2160, the game's -use4K branch) is left out: artifacts on PC.
    static const int resolution_values[] = {800, 1080, 720};
    const char* const resolution_names[] = {L("Original do PS4: 1920 x 800 (2.40:1)", "PS4 original: 1920 x 800 (2.40:1)"),
                                                   L("1920 x 1080 tela cheia (16:9)", "1920 x 1080 full screen (16:9)"),
                                                   L("1280 x 720 tela cheia (16:9)", "1280 x 720 full screen (16:9)")};
    OrderCombo("res", L("Resolução do jogo", "Game resolution"), s.order_resolution, resolution_values,
               resolution_names);
    Hint(L("Original: como no PS4, cena em 2.40:1 com faixas pretas, e a iluminação correta. As opções 16:9 usam os patches do illusion0001 que vieram no executável do dump; o autor avisa que 1080p em 16:9 tem problemas visuais. Reinicie para aplicar.", "Original: as on PS4, a 2.40:1 scene with black bars and correct lighting. The 16:9 options use illusion0001's patches found in the dump's executable; their author warns that 16:9 at 1080p has visual issues. Restart to apply."));

    ImGui::SeparatorText(L("Desempenho", "Performance"));
    static const unsigned game_fps_values[] = {30, 60};
    const char* const game_fps_names[] = {L("30 FPS (original)", "30 FPS (original)"),
                                          L("60 FPS (experimental)", "60 FPS (experimental)")};
    OrderCombo("gamefps", L("Taxa de quadros do jogo", "Game frame rate"), s.order_fps, game_fps_values,
               game_fps_names);
    Hint(L("O jogo foi feito para 30 FPS. 60 FPS usa o patch do illusion0001; os QTEs dos capítulos 6 e 9 travam com ele: volte a 30 para passar por eles. Reinicie para aplicar.", "The game was made for 30 FPS. 60 FPS uses illusion0001's patch; QTEs in chapters 6 and 9 softlock with it: switch back to 30 to get past them. Restart to apply."));
    static const unsigned fps_values[] = {30, 60, 90, 120};
    static const char* const fps_names[] = {"30 FPS", "60 FPS", "90 FPS", "120 FPS"};
    OrderCombo("fps", L("Limite de quadros (F9)", "Frame limit (F9)"), s.fps_limit, fps_values, fps_names);
    Hint(L("Aplicado na hora. F9 alterna 30/60/90/120 durante o jogo. Algumas cutscenes foram feitas para 30 FPS.", "Applied immediately. F9 cycles 30/60/90/120 in game. Some cutscenes were made for 30 FPS."));
    OrderCheckbox("fast", L("Carregamento acelerado", "Fast loading"), s.fast_loading);
    Hint(L("Filas de compute e de leitura sem esperas desnecessárias: recursos e iluminação chegam mais rápido, sem perder elementos da cena. Reinicie para aplicar.", "Compute and read queues without needless waits: resources and lighting arrive faster, without losing anything in the scene. Restart to apply."));
    OrderCheckbox("preload", L("Pré-carregar shaders ao abrir", "Preload shaders at start"), s.preload_shaders);
    Hint(L("Monta, antes de o jogo abrir, os shaders usados nas duas últimas sessões: menos travadas ao entrar nas áreas, abertura um pouco mais lenta. Reinicie para aplicar.", "Builds the shaders used in the last two sessions before the game opens: fewer stutters entering areas, a slightly slower start. Restart to apply."));

    ImGui::SeparatorText(L("Jogo", "Game"));
    static int language_values[std::size(BbSettings::GameLanguages)];
    static const char* language_names[std::size(BbSettings::GameLanguages)];
    for (size_t i = 0; i < std::size(BbSettings::GameLanguages); ++i) {
        language_values[i] = BbSettings::GameLanguages[i].code;
        language_names[i] = BbSettings::GameLanguages[i].name;
    }
    OrderCombo("lang", L("Idioma do jogo", "Game language"), s.game_language, language_values, language_names);
    Hint(L("Idioma do sistema informado ao jogo (textos e áudio que o disco tiver). Reinicie para aplicar.", "System language given to the game (the text and audio on the disc). Restart to apply."));
    OrderCheckbox("photo", L("Câmera livre nas cutscenes (modo foto)", "Free camera in cutscenes (photo mode)"), s.photo_camera);
    Hint(L("Libera o controle da câmera do modo foto durante as cutscenes. Reinicie para aplicar.", "Unlocks the photo mode camera during cutscenes. Restart to apply."));

    ImGui::SeparatorText(L("Tela", "Display"));
    OrderCheckbox("showfps", L("Mostrar FPS no canto", "Show FPS in the corner"), s.show_fps);

    if (BbSettings::OrderNeedsRestart()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                           L("As alterações serão aplicadas após reiniciar", "Changes apply after a restart"));
        if (ImGui::Button(L("Aplicar e reiniciar o jogo", "Apply and restart the game"))) {
            BbSettings::Save();
            runtime_restart();
        }
    }

    ImGui::Spacing();
    if (ImGui::Button(L("Fechar", "Close"))) {
        keep_open = false;
    }
    ImGui::SameLine();
    ImGui::TextDisabled(L("As configurações são salvas ao fechar o menu", "Settings are saved when the menu closes"));
    ImGui::End();
    if (!keep_open) {
        SetOpen(false);
    }
}

// The game's text dialog: what is typed, and how to finish (keyboard or controller).
void TextEntryBox() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.42f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(viewport->WorkSize.x * 0.32f, 0.0f),
                                        ImVec2(viewport->WorkSize.x * 0.8f, FLT_MAX));
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGui::Begin("##text_entry", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.45f, 1.0f), "%s", text_entry_prompt.c_str());
    ImGui::Separator();
    ImGui::SetWindowFontScale(1.4f);
    ImGui::Text("%s_", text_entry_text.c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Separator();
    ImGui::TextDisabled("Keyboard: type, Backspace to erase, Enter = OK, Esc = cancel");
    ImGui::TextDisabled("Controller: Cross (A) = OK, Circle (B) = cancel");
    ImGui::End();
}

void FpsCounter() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - pad,
                                   viewport->WorkPos.y + pad),
                            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.5f);
    ImGui::Begin("##fps", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    const auto& s = BbSettings::Get();
    if (const unsigned limit = s.frame_limit.load()) {
        ImGui::Text("%.0f FPS  %.1f ms  limite %u  %s",
                    frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f, frame_ms_avg, limit,
                    s.upscaler == BbSettings::UpscalerFsr3   ? "FSR 3.1"
                    : s.upscaler == BbSettings::UpscalerFsr4 ? "FSR 4"
                    : s.upscaler == BbSettings::UpscalerFsr411 ? "FSR 4.1.1"
                    : s.upscaler == BbSettings::UpscalerTaa ? "TAA"
                    : s.upscaler == BbSettings::UpscalerDlss ? "DLSS selecionado"
                                                             : "");
        ImGui::End();
        return;
    }
    ImGui::Text("%.0f FPS  %.1f ms  %s", frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f,
                frame_ms_avg,
                s.upscaler == BbSettings::UpscalerFsr3   ? "FSR 3.1"
                : s.upscaler == BbSettings::UpscalerFsr4 ? "FSR 4"
                : s.upscaler == BbSettings::UpscalerFsr411 ? "FSR 4.1.1"
                : s.upscaler == BbSettings::UpscalerTaa ? "TAA"
                : s.upscaler == BbSettings::UpscalerDlss ? "DLSS"
                                                         : "");
    ImGui::End();
}

} // namespace

void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count) {
    std::scoped_lock lock{imgui_mutex};
    if (initialized) {
        return;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // window positions are not kept
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendPlatformName = "bbport";

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.Colors[ImGuiCol_WindowBg].w = 0.92f;

    ImFontConfig font_config;
    font_config.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(bb_font_ttf),
                                   int(bb_font_ttf_end - bb_font_ttf), 18.0f, &font_config);

    const vk::Instance vk_instance = instance.GetInstance();
    ImGui_ImplVulkan_LoadFunctions(
        instance.ApiVersion(),
        [](const char* name, void* user) {
            return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr(
                *static_cast<const vk::Instance*>(user), name);
        },
        const_cast<vk::Instance*>(&vk_instance));

    const VkFormat color_format = static_cast<VkFormat>(format);
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = instance.ApiVersion();
    info.Instance = vk_instance;
    info.PhysicalDevice = instance.GetPhysicalDevice();
    info.Device = instance.GetDevice();
    info.QueueFamily = instance.GetGraphicsQueueFamilyIndex();
    info.Queue = instance.GetGraphicsQueue();
    info.DescriptorPoolSize = 16;
    info.MinImageCount = std::max(image_count, 2u);
    info.ImageCount = std::max(image_count, 2u);
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_format,
    };
    if (!ImGui_ImplVulkan_Init(&info)) {
        std::printf("Overlay: ImGui Vulkan backend init failed\n");
        ImGui::DestroyContext();
        return;
    }
    initialized = true;
    std::printf("Overlay: menu ready (Insert)\n");
}

void UpdateTextInput(SDL_Window* window) {
    bool want = false;
    {
        std::scoped_lock lock{imgui_mutex};
        want = initialized && menu_open && ImGui::GetIO().WantTextInput;
    }
    if (want != SDL_TextInputActive(window)) {
        if (want) {
            SDL_StartTextInput(window);
        } else {
            SDL_StopTextInput(window);
        }
    }
}

bool HandleEvent(const SDL_Event& event) {
    std::scoped_lock lock{imgui_mutex};
    if (!initialized) {
        return false;
    }
    ImGuiIO& io = ImGui::GetIO();
    const bool is_open = menu_open;
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        if (down && !event.key.repeat &&
            (event.key.key == SDLK_INSERT || (is_open && event.key.key == SDLK_ESCAPE))) {
            SetOpen(event.key.key == SDLK_INSERT ? !is_open : false);
            return true;
        }
        if (!is_open) {
            return false;
        }
        io.AddKeyEvent(ImGuiMod_Ctrl, (event.key.mod & SDL_KMOD_CTRL) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (event.key.mod & SDL_KMOD_SHIFT) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (event.key.mod & SDL_KMOD_ALT) != 0);
        if (const ImGuiKey key = KeyFromSdl(event.key.key); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        const bool down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        const u8 button = event.gbutton.button;
        // TOport: the menu opens with Insert only. The Order uses L3+R3 in play and the
        // touchpad / Select button for its photo mode, so every controller button stays the
        // game's (navigation keys below act only while the menu is open).
        if (!is_open) {
            return false;
        }
        if (const ImGuiKey key = KeyFromGamepad(button); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_TEXT_INPUT: {
        // Typed characters (Ctrl+click on a slider, a text field): key events alone erase but
        // do not type. SDL sends them while text input is on (UpdateTextInput).
        if (!is_open) {
            return false;
        }
        io.AddInputCharactersUTF8(event.text.text);
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        if (!is_open) {
            return false;
        }
        const float density = PixelDensity(event.motion.windowID);
        io.AddMousePosEvent(event.motion.x * density, event.motion.y * density);
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (!is_open) {
            return false;
        }
        const int button = event.button.button == SDL_BUTTON_LEFT    ? 0
                           : event.button.button == SDL_BUTTON_RIGHT  ? 1
                           : event.button.button == SDL_BUTTON_MIDDLE ? 2
                                                                      : -1;
        if (button >= 0) {
            io.AddMouseButtonEvent(button, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        }
        return true;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (!is_open) {
            return false;
        }
        io.AddMouseWheelEvent(event.wheel.x, event.wheel.y);
        return true;
    default:
        return false;
    }
}

bool Visible() {
    return initialized && (menu_open || text_entry_active || BbSettings::Get().show_fps);
}

bool CapturesInput() {
    return menu_open || text_entry_active;
}

void SetTextEntry(bool active, const std::string& prompt, const std::string& text) {
    std::scoped_lock lock{imgui_mutex};
    text_entry_active = active;
    text_entry_prompt = prompt;
    text_entry_text = text;
}

void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent) {
    // Present interval for the FPS readout (measured also while nothing is drawn).
    const auto now = std::chrono::steady_clock::now();
    const float ms = std::chrono::duration<float, std::milli>(now - last_present).count();
    last_present = now;
    if (ms > 0.0f && ms < 1000.0f) {
        frame_ms_avg = frame_ms_avg == 0.0f ? ms : frame_ms_avg * 0.95f + ms * 0.05f;
    }
    if (!Visible()) {
        return;
    }
    std::scoped_lock lock{imgui_mutex};
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(extent.width), float(extent.height));
    io.DeltaTime = ms > 0.0f && ms < 1000.0f ? ms / 1000.0f : 1.0f / 60.0f;
    // UI scale follows the display height (1080p = 1).
    const float scale = std::max(float(extent.height) / 1080.0f, 0.75f);
    if (std::abs(scale - base_scale) > 0.01f) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(scale / base_scale);
        style.FontScaleMain = scale;
        base_scale = scale;
    }

    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    if (menu_open) {
        Menu();
    }
    if (BbSettings::Get().show_fps && !menu_open) {
        FpsCounter();
    }
    if (text_entry_active) {
        TextEntryBox();
    }
    ImGui::Render();

    const vk::RenderingAttachmentInfo attachment{
        .imageView = view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    cmdbuf.beginRendering(vk::RenderingInfo{
        .renderArea = {{0, 0}, extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    });
    {
        // Font atlas uploads submit to the graphics queue themselves.
        std::scoped_lock submit_lock{Vulkan::Scheduler::submit_mutex};
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmdbuf);
    }
    cmdbuf.endRendering();
}

} // namespace BbOverlay
