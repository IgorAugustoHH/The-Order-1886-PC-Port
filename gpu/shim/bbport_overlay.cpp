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
#include "imgui_internal.h"
#include <cstring>
#include <string>
#include <vector>
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
    if (s.order_output == 1080) {
        OrderCombo("aa", "Anti-aliasing", s.order_aa, aa_values, aa_names);
        Hint(L("DLSS (NVIDIA RTX) substitui a resolução temporal do próprio jogo. Abaixo de DLAA, a cena é renderizada menor e reconstruída. Reinicie para aplicar.", "DLSS (NVIDIA RTX) replaces the game's own temporal resolve. Below DLAA the scene is rendered smaller and reconstructed. Restart to apply."));
    } else {
        // Above 1080p the choice is only whether the game's TAA runs before the upscaler; the
        // DLSS modes below DLAA mean the same as DLAA here (shown as "by the upscaler").
        const char* const out_names[] = {L("TAA original + upscaler (recomendado)", "The game's TAA + upscaler (recommended)"),
                                         L("Pelo upscaler (experimental)", "By the upscaler (experimental)")};
        const bool game_taa = s.order_aa == BbSettings::OrderAaGame;
        ImGui::PushID("aa_out");
        if (ImGui::BeginCombo("Anti-aliasing", out_names[game_taa ? 0 : 1])) {
            if (ImGui::Selectable(out_names[0], game_taa) && !game_taa) {
                s.order_aa.store(BbSettings::OrderAaGame);
                dirty = true;
            }
            if (ImGui::Selectable(out_names[1], !game_taa) && game_taa) {
                s.order_aa.store(BbSettings::OrderAaDlaa);
                dirty = true;
            }
            ImGui::EndCombo();
        }
        ImGui::PopID();
        Hint(L("TAA original: o TAA do jogo roda antes e o upscaler amplia. Pelo upscaler: o DLSS ou o FSR 3.1 faz também o anti-aliasing, mas partes de rostos podem sumir (2026-10-19). Reinicie para aplicar.", "The game's TAA: it runs first and the upscaler enlarges. By the upscaler: DLSS or FSR 3.1 also does the anti-aliasing, but parts of faces may vanish (2026-10-19). Restart to apply."));
    }
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

    static const int output_values[] = {1080, 1440, 2160};
    const char* const output_names[] = {L("1080p (a resolução do jogo)", "1080p (the game's resolution)"),
                                        L("1440p (ampliada pelo upscaler, experimental)", "1440p (upscaled, experimental)"),
                                        L("4K (ampliada pelo upscaler, experimental)", "4K (upscaled, experimental)")};
    const int output_before = s.order_output.load();
    OrderCombo("output", L("Resolução de saída", "Output resolution"), s.order_output, output_values,
               output_names);
    if (output_before == 1080 && s.order_output != 1080) {
        s.order_aa.store(BbSettings::OrderAaGame); // the recommended anti-aliasing above 1080p
    }
    if (s.order_output != 1080) {
        // FSR 4 / 4.1.1 stay reachable through bbport.ini (order_upscaler) until tested.
        static const int upscaler_values[] = {BbSettings::UpscalerDlss, BbSettings::UpscalerFsr3,
                                              BbSettings::UpscalerOff};
        const char* const upscaler_names[] = {"NVIDIA DLSS", "AMD FSR 3.1",
                                              L("Nenhum (ampliação simples)", "None (simple scaling)")};
        OrderCombo("upscaler", "Upscaler", s.order_upscaler, upscaler_values, upscaler_names);
        ImGui::PushID("sharpness");
        int sharpness = s.sharpen.load() ? int(s.sharpness.load() * 100.0f + 0.5f) : 0;
        if (ImGui::SliderInt(L("Nitidez", "Sharpening"), &sharpness, 0, 100, "%d%%")) {
            s.sharpness.store(float(sharpness) / 100.0f);
            s.sharpen.store(true); // 0% is off; the launcher writes only sharpness
            dirty = true;
        }
        ImGui::PopID();
    }
    Hint(L("EXPERIMENTAL: para monitores 1440p ou 4K. O jogo continua na própria resolução e o quadro pronto é ampliado pelo upscaler; em monitor 1080p não há ganho. Reinicie para aplicar.", "EXPERIMENTAL: for 1440p or 4K monitors. The game keeps its own resolution and the finished frame is upscaled; on a 1080p monitor there is no gain. Restart to apply."));
    if (s.order_output != 1080) {
        Hint(L("Nitidez: aplicada depois do upscaler (DLSS e FSR 3.1), vale na hora.", "Sharpening: applied after the upscaler (DLSS and FSR 3.1), applies immediately."));
    }

    OrderCheckbox("mblur", L("Desfoque de movimento (motion blur)", "Motion blur"), s.order_motion_blur);
    Hint(L("O borrão do jogo em personagens e câmera em movimento. Vale na hora.", "The game's blur on moving characters and camera. Applies immediately."));
    OrderCheckbox("dof", L("Profundidade de campo (DoF)", "Depth of field"), s.order_dof);
    Hint(L("O desfoque do que está fora de foco (fundo, mira, cutscenes). Vale na hora.", "The blur of what is out of focus (background, aiming, cutscenes). Applies immediately."));

    ImGui::SeparatorText(L("Desempenho", "Performance"));
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
    // TOport: the upscaler runs only above 1080p (the Bloodborne field keeps a stale choice).
    const bool upscaling = s.startup_order_output != 1080;
    const char* const upscaler = !upscaling ? ""
                                 : s.upscaler == BbSettings::UpscalerFsr3   ? "FSR 3.1"
                                 : s.upscaler == BbSettings::UpscalerFsr4   ? "FSR 4"
                                 : s.upscaler == BbSettings::UpscalerFsr411 ? "FSR 4.1.1"
                                 : s.upscaler == BbSettings::UpscalerTaa    ? "TAA"
                                 : s.upscaler == BbSettings::UpscalerDlss   ? "DLSS"
                                                                            : "";
    if (const unsigned limit = s.frame_limit.load()) {
        ImGui::Text("%.0f FPS  %.1f ms  limite %u  %s",
                    frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f, frame_ms_avg, limit,
                    upscaler);
        ImGui::End();
        return;
    }
    ImGui::Text("%.0f FPS  %.1f ms  %s", frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f,
                frame_ms_avg, upscaler);
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

/// TOport: loading_1.bmp .. loading_3.bmp next to bb-probe.exe (24/32-bit BMP, any size), shown
/// as a carousel behind the preload screen. None: the plain screen.
std::vector<ImTextureData*> LoadingImages() {
    static std::vector<ImTextureData*> images;
    static bool loaded = false;
    if (loaded) {
        return images;
    }
    loaded = true;
    const char* base = SDL_GetBasePath();
    for (int i = 1; i <= 3; ++i) {
        const std::string path = std::string(base ? base : "") + "loading_" + std::to_string(i) + ".bmp";
        SDL_Surface* bmp = SDL_LoadBMP(path.c_str());
        if (!bmp) {
            continue;
        }
        SDL_Surface* rgba = SDL_ConvertSurface(bmp, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(bmp);
        if (!rgba) {
            continue;
        }
        auto* tex = IM_NEW(ImTextureData)();
        tex->Create(ImTextureFormat_RGBA32, rgba->w, rgba->h);
        tex->UseColors = true;
        for (int y = 0; y < rgba->h; ++y) {
            std::memcpy(static_cast<u8*>(tex->GetPixels()) + size_t(y) * rgba->w * 4,
                        static_cast<const u8*>(rgba->pixels) + size_t(y) * rgba->pitch,
                        size_t(rgba->w) * 4);
        }
        SDL_DestroySurface(rgba);
        ImGui::RegisterUserTexture(tex);
        images.push_back(tex);
    }
    std::printf("Loading screen: %zu image(s)\n", images.size());
    return images;
}

void RenderLoading(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent, u32 built,
                   u32 total, double seconds) {
    if (!initialized) {
        return;
    }
    std::scoped_lock lock{imgui_mutex};
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(extent.width), float(extent.height));
    io.DeltaTime = 1.0f / 30.0f;
    const float scale = std::max(float(extent.height) / 1080.0f, 0.75f);
    if (std::abs(scale - base_scale) > 0.01f) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(scale / base_scale);
        style.FontScaleMain = scale;
        base_scale = scale;
    }

    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const ImVec2 size = io.DisplaySize;
    const ImU32 gold = IM_COL32(201, 162, 92, 255);
    const ImU32 dim = IM_COL32(150, 140, 125, 255);
    // Dark vignette: a warm center fading to black at the edges.
    draw->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(size.x, size.y * 0.5f),
                                  IM_COL32(6, 5, 4, 255), IM_COL32(6, 5, 4, 255),
                                  IM_COL32(24, 19, 14, 255), IM_COL32(24, 19, 14, 255));
    draw->AddRectFilledMultiColor(ImVec2(0, size.y * 0.5f), size, IM_COL32(24, 19, 14, 255),
                                  IM_COL32(24, 19, 14, 255), IM_COL32(6, 5, 4, 255),
                                  IM_COL32(6, 5, 4, 255));

    ImFont* font = ImGui::GetFont();
    const auto centered = [&](const char* text, float y, float px, ImU32 color) {
        const ImVec2 ts = font->CalcTextSizeA(px, FLT_MAX, 0.0f, text);
        draw->AddText(font, px, ImVec2((size.x - ts.x) * 0.5f, y), color, text);
        return ts.y;
    };
    // Images: cover the screen (cropped, never stretched); one every 5 s with a 1 s fade.
    const auto images = LoadingImages();
    const auto cover = [&](ImTextureData* tex, int alpha) {
        const float sa = size.x / size.y, ia = float(tex->Width) / float(tex->Height);
        ImVec2 uv0(0, 0), uv1(1, 1);
        if (ia > sa) {
            const float w = sa / ia;
            uv0.x = (1.0f - w) * 0.5f, uv1.x = uv0.x + w;
        } else {
            const float h = ia / sa;
            uv0.y = (1.0f - h) * 0.5f, uv1.y = uv0.y + h;
        }
        draw->AddImage(tex->GetTexRef(), ImVec2(0, 0), size, uv0, uv1, IM_COL32(255, 255, 255, alpha));
    };
    float cy = size.y * 0.42f;
    if (!images.empty()) {
        const double period = 5.0;
        const size_t n = images.size();
        const size_t cur = size_t(seconds / period) % n;
        const float into = float(std::fmod(seconds, period));
        cover(images[cur], 255);
        if (n > 1 && into > period - 1.0) {
            cover(images[(cur + 1) % n], int(255 * (into - (period - 1.0))));
        }
        // Dark band at the bottom so the bar and text stay readable.
        draw->AddRectFilledMultiColor(ImVec2(0, size.y * 0.72f), size, IM_COL32(0, 0, 0, 0),
                                      IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 220),
                                      IM_COL32(0, 0, 0, 220));
        cy = size.y - 190.0f * scale;
    } else {
        centered("THE ORDER: 1886", cy - 70.0f * scale, 56.0f * scale, gold);
        const float line_w = 260.0f * scale;
        draw->AddLine(ImVec2((size.x - line_w) * 0.5f, cy + 4.0f * scale),
                      ImVec2((size.x + line_w) * 0.5f, cy + 4.0f * scale),
                      IM_COL32(201, 162, 92, 140), 1.5f * scale);
    }
    centered(L("Preparando os shaders", "Preparing the shaders"), cy + 22.0f * scale,
             22.0f * scale, IM_COL32(225, 215, 195, 255));

    // Progress bar: thin gold line; with no known total a segment sweeps across.
    const float bar_w = std::min(560.0f * scale, size.x * 0.6f);
    const float bar_h = 4.0f * scale;
    const ImVec2 b0((size.x - bar_w) * 0.5f, cy + 80.0f * scale);
    const ImVec2 b1(b0.x + bar_w, b0.y + bar_h);
    draw->AddRectFilled(b0, b1, IM_COL32(60, 50, 38, 255), bar_h);
    if (total) {
        const float f = std::clamp(float(built) / float(total), 0.0f, 1.0f);
        draw->AddRectFilled(b0, ImVec2(b0.x + bar_w * f, b1.y), gold, bar_h);
    } else {
        const float t = float(std::fmod(seconds * 0.6, 1.0));
        const float x0 = b0.x + bar_w * std::max(0.0f, t * 1.25f - 0.25f);
        const float x1 = b0.x + bar_w * std::min(1.0f, t * 1.25f);
        draw->AddRectFilled(ImVec2(x0, b0.y), ImVec2(x1, b1.y), gold, bar_h);
    }

    char info[128];
    if (total) {
        std::snprintf(info, sizeof(info), L("%u de %u  ·  %.0f s", "%u of %u  ·  %.0f s"),
                      std::min(built, total), total, seconds);
    } else {
        std::snprintf(info, sizeof(info), L("%u prontos  ·  %.0f s", "%u ready  ·  %.0f s"),
                      built, seconds);
    }
    centered(info, b1.y + 14.0f * scale, 17.0f * scale, dim);
    centered(L("Só nas primeiras aberturas depois de atualizar demora mais.",
               "Only the first starts after an update take longer."),
             size.y - 34.0f * scale, 15.0f * scale, IM_COL32(110, 102, 92, 255));
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
        std::scoped_lock submit_lock{Vulkan::Scheduler::submit_mutex};
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmdbuf);
    }
    cmdbuf.endRendering();
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
