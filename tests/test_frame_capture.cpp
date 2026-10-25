// SPDX-FileCopyrightText: Copyright 2026 Igor Augusto and TOport contributors (https://github.com/IgorAugustoHH/The-Order-1886-PC-Port)
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include "video_core/renderer_vulkan/vk_frame_capture.h"
#include "video_core/texture_cache/image_info.h"

static void Env(const char* key, const std::string& value) {
#ifdef _WIN32
    _putenv_s(key, value.c_str());
#else
    setenv(key, value.c_str(), 1);
#endif
}

int main(int argc, char** argv) {
    assert(argc == 2 || argc == 3);
    const bool details = argc == 2 || std::string(argv[2]) != "off";
    const std::filesystem::path dir = argv[1];
    std::filesystem::create_directories(dir);
    const auto trigger = dir / "capture.req";
    Env("BB_CAPTURE_DIR", dir.string());
    Env("BB_CAPTURE_TRIGGER", trigger.string());
    Env("BB_CAPTURE_DRAW_RESOURCES", details ? "1" : "0");
    using Vulkan::FrameCapture;
    assert(!FrameCapture::Active());
    FrameCapture::Binding("inactive-binding-must-not-appear");
    FrameCapture::Draw(1, 2, 3, 1);
    VideoCore::ImageInfo display, scene, depth;
    display.guest_address = 0x1100;
    scene.guest_address = 0x2200;
    depth.guest_address = 0x3300;
    depth.num_samples = 2;
    for (auto* image : {&display, &scene, &depth}) {
        image->size = {1280, 720, 1};
        image->pixel_format = vk::Format::eR8G8B8A8Unorm;
    }
    FrameCapture::AddDisplayBuffer(display.guest_address);
    std::ofstream(trigger) << "1";
    FrameCapture::OnFlip(display.guest_address);
    assert(FrameCapture::Active());
    const VideoCore::ImageInfo* colors[] = {&display};
    FrameCapture::BeginPass(colors, 1, nullptr);
    FrameCapture::Draw(10, 20, 3, 1);
    colors[0] = &scene;
    FrameCapture::BeginPass(colors, 1, &depth);
    for (unsigned n = 0; n < 12; ++n) {
        FrameCapture::Binding("material-" + std::to_string(n));
        FrameCapture::RecordDrawState("pipeline_samples 2 guest_aa_samples 4");
        FrameCapture::Note("depth-state-repeated");
        FrameCapture::Draw(100 + n, 200 + n, 30 + n, 1);
    }
    FrameCapture::Binding("compute-binding-must-not-leak-to-next-draw");
    FrameCapture::Dispatch(400, 1, 1, 1);
    FrameCapture::BeginPass(colors, 1, &depth);
    FrameCapture::Draw(500, 600, 3, 1);
    FrameCapture::Binding("indirect-material");
    FrameCapture::Indirect(501, 601, 0x123400, 0x123800, 20, 12);
    for (unsigned n = 0; n < 2050; ++n) FrameCapture::Draw(700, 800, 3, 1);
    colors[0] = &display;
    FrameCapture::BeginPass(colors, 1, nullptr);
    assert(!FrameCapture::Active());
    std::filesystem::path report;
    for (const auto& item : std::filesystem::directory_iterator(dir)) {
        if (item.path().extension() == ".txt") report = item.path();
    }
    assert(!report.empty());
    std::ifstream file(report);
    const std::string text{std::istreambuf_iterator<char>(file), {}};
    assert((text.find("draw detail coverage:") != std::string::npos) == details);
    assert(text.find("inactive-binding") == std::string::npos);
    assert(text.find("compute-binding") == std::string::npos);
    if (details) {
        assert(text.find("recorded 2048, truncated 1") != std::string::npos);
        assert(text.find("    binding material-11") != std::string::npos);
        assert(text.find("    binding indirect-material") != std::string::npos);
        assert(text.find("indirect_args 0x123400 count_address 0x123800 stride 20 max_count 12 counts_unknown_gpu_arguments") != std::string::npos);
        assert(text.find("draw 11 vs 000000000000006f ps 00000000000000d3 indices 41") != std::string::npos);
    } else {
        assert(text.find("    binding ") == std::string::npos);
    }
    const auto note = text.find("depth-state-repeated");
    assert(note != std::string::npos && text.find("depth-state-repeated", note + 1) == std::string::npos);
    puts("PASS frame capture: per-draw associations, >8 hashes, compute isolation, dedupe, bounded coverage");
}
