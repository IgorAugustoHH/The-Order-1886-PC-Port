// SPDX-FileCopyrightText: Copyright 2026 Igor Augusto and TOport contributors (https://github.com/IgorAugustoHH/The-Order-1886-PC-Port)
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <algorithm>
#include "common/types.h"

namespace Vulkan::DofCapture {
// The Order's named PS programs. The shared fullscreen VS is deliberately excluded.
// This selects diagnostic input records only, never rendering or shader specialization.
inline constexpr std::array<u64, 42> Programs{
    0xf77d3cf1, // focus measurement PS: native CB 180/181/345/346
    0x7f67f529, 0x587a065b, 0x5d04018a, 0x4bb962d1,
    0x0c9391e9, 0x32e1e7f1, 0xd63d458f, 0xc62588e4,
    0x34169052, 0x26ae3f6e, 0xb2519df5, 0x15ce7c20,
    0x1014c772, 0x3a85ca37, 0x118a23ca, 0x74995772,
    0x617349dd, 0x24928d76, 0x2ae332c6,
    0xe06578f1, 0xec56aa26, 0x4c267141,
    0xc8fa0aab, 0x7f6645bb, 0x5c52902a,
    0xf3830d11, 0x0949c713, 0xb176596c,
    0x99e1d490, 0x54e756fa, 0xa04130a9,
    0xe3ae0cc5, 0x9786f233, 0xfc53581c,
    0x856111b1, 0x7e2d7056, 0x9f697c56,
    0x40fb4e99, 0xf8431cf8, 0xa0efc78d, 0x808240b5,
};
constexpr bool Includes(u64 hash) {
    return std::ranges::find(Programs, hash) != Programs.end();
}
inline constexpr u32 MaxBuffers = 32;
inline constexpr u64 MaxBufferBytes = 4096;
inline constexpr u64 MaxFrameBytes = 16384;
} // namespace Vulkan::DofCapture
