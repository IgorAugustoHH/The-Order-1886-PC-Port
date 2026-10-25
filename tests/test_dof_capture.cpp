// SPDX-FileCopyrightText: Copyright 2026 Igor Augusto and TOport contributors (https://github.com/IgorAugustoHH/The-Order-1886-PC-Port)
// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cassert>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include "video_core/renderer_vulkan/dof_capture.h"
#include "video_core/renderer_vulkan/vk_frame_capture.h"
#include "video_core/texture_cache/image_info.h"

static void Env(const char* key, const std::string& value) {
#ifdef _WIN32
    _putenv_s(key, value.c_str());
#else
    setenv(key, value.c_str(), 1);
#endif
}
static std::string ReadCapture(const std::filesystem::path& dir) {
    std::filesystem::path report;
    auto newest = std::filesystem::file_time_type::min();
    for (const auto& item : std::filesystem::directory_iterator(dir)) {
        if (item.path().extension() != ".txt") continue;
        const auto written = item.last_write_time();
        if (report.empty() || written >= newest) { report = item.path(); newest = written; }
    }
    assert(!report.empty());
    std::ifstream file(report);
    return {std::istreambuf_iterator<char>(file), {}};
}
int main(int argc, char** argv) {
    assert(argc == 3);
    const std::filesystem::path dir = argv[1];
    const std::string mode = argv[2];
    assert(mode == "off" || mode == "bytes" || mode == "buffers" || mode == "associations" || mode == "focus");
    std::filesystem::create_directories(dir);
    const auto trigger = dir / "capture.req";
    Env("BB_CAPTURE_DIR", dir.string());
    Env("BB_CAPTURE_TRIGGER", trigger.string());
    Env("BB_CAPTURE_DRAW_RESOURCES", "0");
    Env("BB_CAPTURE_DOF_CONSTANTS", mode == "off" ? "0" : "1");
    Env("BB_CAPTURE_FOCUS_DEPTH", mode == "focus" ? "1" : "0");
    using Vulkan::FrameCapture;
    using Source = FrameCapture::DofSource;
    constexpr u64 ps = 0x7f6645bb;
    assert(Vulkan::DofCapture::Includes(ps));
    assert(Vulkan::DofCapture::Includes(0xf77d3cf1));
    assert(!FrameCapture::FocusDepthWanted(0xf77d3cf1));
    assert(!Vulkan::DofCapture::Includes(0xb9c87fc1)); // shared VS cannot select a camera effect
    assert(!Vulkan::DofCapture::Includes(ps | (u64(1) << 32)));
    // No dereference when idle, armed, disabled or outside the named PS family.
    const auto* invalid = reinterpret_cast<const void*>(1);
    FrameCapture::DofBuffer(ps, 0, 0, invalid, 8, Source::RingSnapshot);
    VideoCore::ImageInfo display, scene;
    for (auto* image : {&display, &scene}) {
        image->size = {1280, 720, 1};
        image->pixel_format = vk::Format::eR8G8B8A8Unorm;
    }
    display.guest_address = 0x1100; scene.guest_address = 0x2200;
    FrameCapture::AddDisplayBuffer(display.guest_address);
    std::ofstream(trigger) << '1';
    FrameCapture::OnFlip(display.guest_address);
    FrameCapture::DofBuffer(ps, 0, 0, invalid, 8, Source::RingSnapshot);
    const VideoCore::ImageInfo* colors[] = {&display};
    FrameCapture::BeginPass(colors, 1, nullptr);
    FrameCapture::Draw(10, 20, 3, 1);
    if (mode == "focus") {
        assert(FrameCapture::FocusDepthWanted(0xf77d3cf1));
        assert(!FrameCapture::FocusDepthWanted(ps));
        VideoCore::ImageInfo depth = scene;
        assert(FrameCapture::FocusDepthPath(depth, 17).empty());
        depth.pixel_format = vk::Format::eD32Sfloat;
        depth.props.is_depth = true;
        depth.resources.layers = 1;
        depth.num_samples = 2;
        depth.size = {8192, 8192, 1};
        assert(FrameCapture::FocusDepthPath(depth, 17).empty());
        depth.size = {1280, 720, 1};
        const auto path = FrameCapture::FocusDepthPath(depth, 17);
        assert(!path.empty() && path.find("focus_depth_") != std::string::npos);
        assert(!FrameCapture::FocusDepthWanted(0xf77d3cf1));
        assert(FrameCapture::FocusDepthPath(depth, 18).empty());
        assert(FrameCapture::FocusScalarPath(depth, 18).empty());
        VideoCore::ImageInfo scalar = depth;
        scalar.guest_address = 0x3300;
        scalar.pixel_format = vk::Format::eR32Sfloat;
        scalar.props.is_depth = false;
        scalar.size = {1, 1, 1};
        scalar.num_samples = 1;
        scalar.resources.levels = 1;
        const auto scalar_path = FrameCapture::FocusScalarPath(scalar, 18);
        assert(!scalar_path.empty() && scalar_path.find("focus_scalar_") != std::string::npos);
        assert(FrameCapture::FocusScalarPath(scalar, 19).empty());
        std::array<u32, 22> focus_words{};
        focus_words[18] = 0x3f800000;
        focus_words[19] = 0x3f000000;
        focus_words[20] = 0x44a00000; // 1280.0
        focus_words[21] = 0x44340000; // 720.0
        FrameCapture::DofBuffer(0xf77d3cf1, 0, 0, focus_words.data(), sizeof(focus_words), Source::FlatBufferCPU);
        FrameCapture::Draw(0x0ece32eb, 0xf77d3cf1, 3, 1);
    } else {
        assert(!FrameCapture::FocusDepthWanted(0xf77d3cf1));
    }
    FrameCapture::DofBuffer(0xb9c87fc1, 0, 0, invalid, 8, Source::RingSnapshot);
    FrameCapture::DofBuffer(ps, 0, 0, nullptr, 8, Source::RingSnapshot);
    std::array<u8, 17> unaligned{};
    const std::array<u32, 4> bits{0x80000000, 0x7fc00001, 0x3f800001, 0xffffffff};
    std::memcpy(unaligned.data() + 1, bits.data(), sizeof(bits));
    const auto initial = unaligned;
    // BindBuffers records before BeginRendering selects the new pass.
    FrameCapture::DofBuffer(ps, 7, 0x123400, unaligned.data() + 1, sizeof(bits), Source::RingSnapshot, 0x200);
    assert(unaligned == initial); // diagnostics do not modify the input
    unaligned.fill(0); // the record must own its bytes, not retain the live pointer
    colors[0] = &scene;
    FrameCapture::BeginPass(colors, 1, nullptr);
    std::vector<u8> payload(6000, 0x33);
    if (mode == "buffers") {
        for (u32 i = 1; i < 40; ++i)
            FrameCapture::DofBuffer(ps, i + 7, 0, payload.data(), 4, Source::FlatBufferCPU);
    } else {
        for (u32 i = 0; i < 10; ++i)
            FrameCapture::DofBuffer(ps, i + 8, 0, payload.data(), payload.size(), Source::FlatBufferCPU);
    }
    FrameCapture::DofBuffer(ps, 99, 0, invalid, 8, Source::FlatUserData); // budget exhausted, or disabled
    FrameCapture::Draw(0xb9c87fc1, ps, 3, 1);
    FrameCapture::Dispatch(0x804aeead, 1, 1, 1);
    colors[0] = &display;
    FrameCapture::BeginPass(colors, 1, nullptr);
    assert(!FrameCapture::Active());
    const auto text = ReadCapture(dir);
    if (mode == "off") {
        assert(text.find("dof input") == std::string::npos);
    } else {
        if (mode == "focus")
        {
            assert(text.find("focus depth snapshot: image_id 17 address 0x2200 1280x720 samples 2 sample 0 file") != std::string::npos);
            assert(text.find("focus scalar snapshot: image_id 18 address 0x3300 file") != std::string::npos);
            assert(text.find("stage 00000000f77d3cf1 slot 0") != std::string::npos);
            assert(text.find("00000000 00000000 3f800000 3f000000 44a00000 44340000") != std::string::npos);
        }
        else
        {
            assert(text.find("focus depth snapshot:") == std::string::npos);
            assert(text.find("focus scalar snapshot:") == std::string::npos);
        }
        assert(text.find("source ring_snapshot ring_offset 0x200 size 16 captured 16 truncated 0") != std::string::npos);
        assert(text.find("80000000 7fc00001 3f800001 ffffffff") != std::string::npos);
        const auto pass = text.find("#1 PASS");
        const auto input = text.find("  dof input draw 0 stage 000000007f6645bb");
        const auto compute = text.find("#2 COMPUTE");
        assert(pass < input && input < compute); // buffers stay with their consuming draw
        assert(text.find("stage 00000000b9c87fc1 slot") == std::string::npos);
        if (mode != "buffers") {
            assert(text.find("captured_bytes 16384 truncated 1") != std::string::npos);
            assert(text.find("size 6000 captured 4096 truncated 1") != std::string::npos);
        } else {
            assert(text.find("buffers 32 captured_bytes 140 truncated 1") != std::string::npos);
        }
        // A second request resets the limits; it cannot reuse old per-frame records.
        std::ofstream(trigger) << '1';
        FrameCapture::OnFlip(display.guest_address);
        FrameCapture::BeginPass(colors, 1, nullptr);
        FrameCapture::Draw(10, 20, 3, 1);
        if (mode == "focus") assert(FrameCapture::FocusDepthWanted(0xf77d3cf1));
        colors[0] = &scene;
        FrameCapture::BeginPass(colors, 1, nullptr);
        for (u32 i = 0; i < 5; ++i) FrameCapture::Draw(100, 200, 3, 1);
        FrameCapture::DofBuffer(ps, 1, 0, bits.data(), sizeof(bits), Source::GuestCPU);
        FrameCapture::Draw(0xb9c87fc1, ps, 3, 1);
        colors[0] = &display;
        FrameCapture::BeginPass(colors, 1, nullptr);
        const auto second = ReadCapture(dir);
        assert(second.find("buffers 1 captured_bytes 16 truncated 0") != std::string::npos);
        assert(second.find("dof input draw 5 stage") != std::string::npos);
        assert(second.find("source flat_buffer_cpu") == std::string::npos);
        assert(second.find("focus depth snapshot:") == std::string::npos);
        assert(second.find("focus scalar snapshot:") == std::string::npos);
        if (mode == "associations") {
            std::ofstream(trigger) << '1';
            FrameCapture::OnFlip(display.guest_address);
            FrameCapture::BeginPass(colors, 1, nullptr);
            FrameCapture::Draw(10, 20, 3, 1);
            colors[0] = &scene;
            FrameCapture::BeginPass(colors, 1, nullptr);
            FrameCapture::DofBuffer(ps, 1, 0, bits.data(), sizeof(bits), Source::RingSnapshot);
            FrameCapture::Draw(0xb9c87fc1, 0x0949c713, 3, 1); // old PS inputs cannot attach to another PS
            FrameCapture::DofBuffer(ps, 1, 0, bits.data(), sizeof(bits), Source::RingSnapshot);
            FrameCapture::Dispatch(0x804aeead, 1, 1, 1); // no graphics-to-compute leakage
            colors[0] = &display;
            FrameCapture::BeginPass(colors, 1, nullptr);
            const auto third = ReadCapture(dir);
            assert(third.find("buffers 0 captured_bytes 0 truncated 1 unassociated 2") != std::string::npos);
            assert(third.find("  dof input draw") == std::string::npos);
        }
    }
    std::printf("PASS DoF constants %s: exact words, snapshot ownership, pass association, limits and reset\n", mode.c_str());
}
