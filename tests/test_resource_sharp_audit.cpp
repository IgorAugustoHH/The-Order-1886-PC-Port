// SPDX-FileCopyrightText: Copyright 2026 Igor Augusto and TOport contributors (https://github.com/IgorAugustoHH/The-Order-1886-PC-Port)
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include "shader_recompiler/specialization.h"

extern "C" uint64_t runtime_memory_clamp(uintptr_t address, uint64_t size) {
    return address >= 0x1000 && address < 0x2000 ? size : 0;
}

int main(int argc, char** argv) {
    assert(argc == 2);
#ifdef _WIN32
    _putenv_s("BB_RESOURCE_AUDIT_FILE", argv[1]);
#else
    setenv("BB_RESOURCE_AUDIT_FILE", argv[1], 1);
#endif
    Shader::Info info{};
    info.pgm_hash = 0xabc;
    info.sw_stage = Shader::SwStage::Fragment;
    info.flattened_ud_buf.resize(32);
    AmdGpu::Buffer buffer{};
    buffer.base_address = 0x1000; buffer.num_records = 16;
    std::memcpy(info.flattened_ud_buf.data() + 16, &buffer, sizeof(buffer));
    Shader::BufferResource desc{};
    desc.buffer_type = Shader::BufferType::Guest;
    desc.sharp_fetch.load_mask = 15;
    for (size_t i = 0; i < 4; ++i) desc.sharp_fetch.offsets[i] = u16(16 + i);
    Shader::StageSpecialization stage;
    stage.info = &info;
    Shader::BufferResourceList descriptors;
    descriptors.push_back(desc);
    boost::container::small_vector<Shader::BufferSpecialization, 16> specs;
    unsigned accepted = 0;
    const auto specialize = [&](auto&, const auto&, auto) { ++accepted; };
    stage.ForEachSharp(specs, descriptors, specialize);
    assert(accepted == 1 && desc.GetSharp(info) == buffer);
    // Change the address while keeping descriptor syntax valid: existing guard must reject.
    buffer.base_address = 0x9000;
    std::memcpy(info.flattened_ud_buf.data() + 16, &buffer, sizeof(buffer));
    specs.clear(); stage.ForEachSharp(specs, descriptors, specialize);
    assert(accepted == 1 && desc.GetSharp(info) == buffer);
    // Unknown table origin remains null. Audit must not dereference UNKNOWN_LOCATION.
    descriptors[0].sharp_fetch.offsets[0] = Shader::UNKNOWN_LOCATION;
    specs.clear(); stage.ForEachSharp(specs, descriptors, specialize);
    assert(accepted == 1 && !descriptors[0].GetSharp(info));
    // Audit of an out-of-range origin can inspect metadata without making an unsafe fetch.
    descriptors[0].sharp_fetch.offsets[0] = 65530;
    stage.AuditSharp(2, descriptors[0], AmdGpu::Buffer::Null(), false);
    BbResourceAudit::PollFaults();
    std::puts("Resource origins: accepted/unmapped/unknown/bounds; diagnostics preserve values and guards");
}
