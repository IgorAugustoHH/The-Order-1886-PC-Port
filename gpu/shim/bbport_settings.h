// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: user settings changed at run time from the in-game menu (bbport_overlay.h) and kept
// in bbport.ini (BB_CONFIG overrides the path). Environment variables override the file at
// start. Readers load the atomics every frame; writers are the menu and Load().

#pragma once

#include <atomic>

namespace BbSettings {

enum Upscaler : int { UpscalerOff = 0, UpscalerFsr3 = 1, UpscalerFsr4 = 2, UpscalerFsr411 = 3,
                      UpscalerTaa = 4, UpscalerDlss = 5, UpscalerCount };
/// FSR 4 v07 or FSR 4.1.1: the same inputs, settings and placement in the frame.
inline bool IsFsr4(int upscaler) {
    return upscaler == UpscalerFsr4 || upscaler == UpscalerFsr411;
}
enum Preset : int { NativeAA = 0, Quality, Balanced, Performance, UltraPerformance, PresetCount };
enum DebugView : int { DebugNone = 0, DebugReactive = 1, DebugMotion = 2, DebugViewCount };
/// TOport: the game's own temporal AA, or DLSS replacing its resolve (DLAA .. Ultra).
enum OrderAa : int {
    OrderAaGame, OrderAaDlaa, OrderAaQuality, OrderAaBalanced, OrderAaPerformance, OrderAaUltra,
    OrderAaCount
};

/// Game effects switched by the community patches at start (patches.py EFFECTS): ini key,
/// menu label, default (the game's own behaviour).
struct Effect {
    const char* key;
    const char* label;
    bool default_on;
};
inline constexpr Effect Effects[] = {
    {"effect_chromatic_aberration", "Aberração cromática", true},
    {"effect_dof", "Profundidade de campo (DoF)", true},
    {"effect_motion_blur", "Desfoque de movimento", true},
    {"effect_ssao", "Oclusão ambiente (SSAO)", true},
    {"effect_game_aa", "Suavização original do jogo", true},
    {"effect_dynamic_shadows", "Sombras de luzes dinâmicas", true},
    {"effect_ssr", "Reflexos SSR (adicionais)", false},
    {"skip_intro", "Pular vídeos de abertura", false},
    {"debug_camera", "Câmera livre (X + L3)", false},
    {"debug_menu", "Menu de diagnóstico (requer fontes adicionais)", false},
    {"cheat_no_death", "Truque: imortalidade (mínimo de 1 HP)", false},
    {"cheat_stealth", "Truque: invisível para inimigos", false},
    {"cheat_silent", "Truque: inimigos não ouvem", false},
    {"cheat_rally_no_decay", "Truque: recuperação Rally sem expirar", false},
    {"cheat_enemy_control", "Truque: controlar inimigo (R3 / L3)", false},
    {"tweak_no_rally", "Desativar recuperação Rally", false},
    {"tweak_camera_distance", "Afastar câmera", false},
    {"tweak_no_camera_rotation", "Desativar rotação automática da câmera", false},
    {"tweak_easy_run", "Correr com menor inclinação do analógico", false},
    {"tweak_ragdoll", "Física de corpos semelhante a Dark Souls", false},
};
inline constexpr int EffectCount = int(sizeof(Effects) / sizeof(Effects[0]));
/// Live output resolutions: the upscaler's output and the UI host targets.
inline constexpr int OutputWidths[] = {1280, 1920, 2560, 3840};
inline constexpr int OutputHeights[] = {720, 1080, 1440, 2160};
inline constexpr int OutputCount = 4;
inline constexpr int OutputDefault = 1; ///< 1920x1080, the game's own size

struct Values {
    std::atomic<int> upscaler{UpscalerFsr3};
    std::atomic<int> preset{NativeAA};
    std::atomic<bool> sharpen{true};
    std::atomic<float> sharpness{0.3f};
    std::atomic<bool> jitter{true};
    std::atomic<bool> reactive{false};
    std::atomic<bool> object_motion{true};
    std::atomic<float> reactive_scale{1.0f};
    std::atomic<float> reactive_threshold{0.2f};
    std::atomic<float> reactive_max{0.9f};
    std::atomic<int> debug_view{DebugNone};
    std::atomic<bool> show_fps{false};
    // Order experimental menu: distinguish provider selection from recorded GPU work.
    std::atomic<bool> spatial_fsr1{false};
    std::atomic<bool> spatial_active{false};
    std::atomic<unsigned long long> temporal_recorded{0}, spatial_recorded{0};
    std::atomic<const char*> temporal_status{"Aguardando o primeiro quadro"};
    /// bbport: the present thread's current frame limit (F9 changes it), shown by the counter.
    std::atomic<unsigned> frame_limit{0};
    // FSR 4 checks (menu): the provider's auto exposure, the jitter sign it is given.
    std::atomic<bool> fsr4_auto_exposure{true};
    std::atomic<bool> fsr4_invert_jitter{false};
    std::atomic<int> active_render_width{1920}, active_render_height{1080};
    /// Applied at start (patches.py); the menu shows when a restart is needed.
    std::atomic<bool> effects[EffectCount]{};
    std::atomic<int> model_lod{0}; ///< -2 highest .. 2 lowest, 0 the game's
    std::atomic<int> output_res{OutputDefault}; ///< index into OutputWidths
    /// Live resolution and preset changes (run.sh): 0 off by default (startup patch, fastest
    /// on the Steam Deck and older GPUs), -1 auto (strong discrete GPUs), 1 on. On restart.
    std::atomic<int> live_resolution{0};
    /// Why FSR 4 cannot run (assets, device features), or null. Set by the renderer.
    std::atomic<const char*> fsr4_problem{nullptr};
    std::atomic<bool> fsr4_supported{false}, fsr411_supported{false};
    /// DLSS (bbport_dlss.dll, NVIDIA RTX) is ready, or why not (null before the device exists).
    std::atomic<bool> dlss_supported{false};
    std::atomic<const char*> dlss_problem{nullptr};

    /// TOport (The Order: 1886): applied at start by run.py, which turns them into the
    /// BB_ORDER_* / BB_LANGUAGE / pipe environment; changes need a restart (except fps_limit).
    std::atomic<int> order_aa{OrderAaQuality};   ///< OrderAa: game TAA, DLAA, DLSS modes
    std::atomic<int> order_resolution{800};      ///< 800 retail (2.40:1), 1080 or 720 16:9
    std::atomic<int> game_language{17};          ///< PS4 system language code (17 pt-BR)
    std::atomic<bool> photo_camera{true};        ///< free photo camera in cutscenes
    std::atomic<bool> fast_loading{true};        ///< compute pipe parts ade + command pipe
    std::atomic<bool> preload_shaders{true};     ///< build last sessions' pipelines at start
    std::atomic<bool> ui_english{false};         ///< menu language: ui_language=en (launcher)
    std::atomic<unsigned> fps_limit{60};         ///< 30/60/90/120; F9 cycles it, applied live
    std::atomic<unsigned> order_fps{30};         ///< the game's own rate: 30, or 60 (unlock patch)
    int startup_order_aa = OrderAaQuality;
    int startup_order_resolution = 800;
    int startup_game_language = 17;
    bool startup_photo_camera = true;
    bool startup_fast_loading = true;
    bool startup_preload_shaders = true;
    unsigned startup_order_fps = 30;

    /// Startup settings for the explicit BB_RENDER_RES compatibility patch only.
    int startup_preset = NativeAA;
    int startup_upscaler = UpscalerFsr3;
    bool startup_object_motion = true;
    bool startup_effects[EffectCount]{};
    int startup_model_lod = 0;
    int startup_output_res = OutputDefault;
    int startup_live_resolution = 0;
};

Values& Get();

/// Reads the file, then the environment overrides. Called once at start.
void Load();
/// Checks the loaded choice before the first frame; unsupported FSR 4 uses FSR 3.1.
void ConfigureUpscalerSupport(bool fsr4, bool fsr411);
/// After device creation: DLSS availability; a DLSS setting falls back to FSR 3.1 without it.
void ConfigureDlssSupport(bool available, const char* problem);
/// Startup-patched scene dimensions cannot change until run.sh prepares a new image.
bool FixedRenderSession();
int RenderPreset();
bool ResolutionNeedsRestart();
/// Writes the file (menu changes).
void Save();
/// TOport settings changed since start (they are applied by run.py on restart).
bool OrderNeedsRestart();

const char* OrderAaName(int mode);

struct GameLanguage {
    int code;
    const char* name;
};
/// PS4 system language codes the menu and launcher offer.
inline constexpr GameLanguage GameLanguages[] = {
    {17, "Português (Brasil)"}, {7, "Português (Portugal)"}, {1, "English (US)"},
    {18, "English (UK)"},       {3, "Español (España)"},   {20, "Español (Latinoamérica)"},
    {2, "Français"},            {22, "Français (Canada)"},  {4, "Deutsch"},
    {5, "Italiano"},            {6, "Nederlands"},          {8, "Русский"},
    {16, "Polski"},             {19, "Türkçe"},             {12, "Suomi"},
    {13, "Svenska"},            {14, "Dansk"},              {15, "Norsk"},
    {21, "العربية (Arabic)"},   {0, "日本語 (Japanese)"},    {9, "한국어 (Korean)"},
    {10, "中文 繁體 (Traditional Chinese)"}, {11, "中文 简体 (Simplified Chinese)"},
};

/// Render resolution divisor of a preset (1.0 native, 1.5 quality, ...).
float PresetScale(int preset);
const char* PresetName(int preset);
const char* UpscalerName(int upscaler);

} // namespace BbSettings
