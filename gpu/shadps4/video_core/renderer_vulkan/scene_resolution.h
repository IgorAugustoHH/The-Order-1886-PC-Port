// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace Vulkan::SceneResolution {
struct Size {
    uint32_t width = 1920, height = 1080;
    bool operator==(const Size&) const = default;
};
/// The Order: 1886's full scene size, the game resolution patched at start (BB_ORDER_RESOLUTION:
/// 720 the game's default, 1080, 2160 experimental). The scene targets, their proxies
/// and DLSS follow it.
inline Size GameSize() {
    static const Size size = [] {
        const char* env = std::getenv("BB_ORDER_RESOLUTION");
        const int lines = env ? std::atoi(env) : 0;
        // retail: the 2.40:1 camera draws the scene into 1920x800 targets (the bars are outside
        // them); the reduced scene and DLSS work at that size.
        if (env && env[0] == 'r') {
            // TOport (experimental): BB_ORDER_ASPECT changes the camera (e.g. 21:9); the band
            // is 1920 / aspect lines, even.
            if (const char* aspect = std::getenv("BB_ORDER_ASPECT")) {
                const double value = std::atof(aspect);
                if (value >= 1.78 && value <= 2.4) {
                    return Size{1920, uint32_t(std::lround(1920.0 / value / 2.0)) * 2};
                }
            }
            return Size{1920, 800};
        }
        return lines == 2160 ? Size{3840, 2160}
             : lines == 1080 ? Size{1920, 1080} : Size{1280, 720};
    }();
    return size;
}
/// TOport: BB_ORDER_OUTPUT=WxH, the display size the port upscales the finished frame to (the
/// game keeps drawing GameSize()); 1920x1080 (no upscaling) when unset or invalid.
inline Size OrderOutput() {
    static const Size size = [] {
        const char* env = std::getenv("BB_ORDER_OUTPUT");
        unsigned w = 0, h = 0;
        if (!env || std::sscanf(env, "%ux%u", &w, &h) != 2 || w < 1920 || h < 1080) {
            return Size{};
        }
        return Size{w, h};
    }();
    return size;
}
/// The finished frame is upscaled past the game's 1920x1080 display buffer.
inline bool OrderUpscaling() {
    return OrderOutput() != Size{};
}
inline Size ForPreset(int preset, Size output = {}) {
    // Even dimensions for half-resolution effects; match the preset labels exactly.
    constexpr std::array<double, 5> scales{1.0, 1.5, 1.7, 2.0, 3.0};
    const double scale = scales[std::clamp(preset, 0, 4)];
    return {uint32_t(std::max(1l, std::lround(output.width / scale / 2)) * 2),
            uint32_t(std::max(1l, std::lround(output.height / scale / 2)) * 2)};
}
constexpr uint32_t Pack(Size size) { return size.width | (size.height << 16); }
constexpr Size Unpack(uint32_t packed) {
    return packed ? Size{packed & 65535u, packed >> 16} : Size{};
}
// A proxy and the guest-size image hold the same logical surface. Native shader reads
// need a resolve after proxy writes; native writes invalidate the cached proxy.
struct Coherence {
    bool valid = false, dirty = false;
    void ProxyWrite() { valid = dirty = true; }
    void CopiedToProxy() { valid = true; dirty = false; }
    void Resolved() { dirty = false; }
    void NativeWrite() { valid = dirty = false; }
};
} // namespace Vulkan::SceneResolution
