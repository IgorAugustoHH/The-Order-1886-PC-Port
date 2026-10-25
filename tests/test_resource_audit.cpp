// SPDX-FileCopyrightText: Copyright 2026 Igor Augusto and TOport contributors (https://github.com/IgorAugustoHH/The-Order-1886-PC-Port)
// SPDX-License-Identifier: GPL-2.0-or-later
#include "bb_resource_audit.h"
#include <cassert>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
    using namespace BbResourceAudit;
    assert(argc == 3);
#ifdef _WIN32
    _putenv_s("BB_RESOURCE_AUDIT_FILE", std::string_view(argv[2]) == "disabled" ? "" : argv[1]);
#else
    setenv("BB_RESOURCE_AUDIT_FILE", std::string_view(argv[2]) == "disabled" ? "" : argv[1], 1);
#endif
    if (std::string_view(argv[2]) == "disabled") {
        assert(!Enabled());
        RecordFault(reinterpret_cast<void*>(1), reinterpret_cast<void*>(2));
        assert(fault_cursor.load() == 0);
        Write("disabled", "");
        return 0;
    }
    assert(Enabled());
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < 4; ++t) threads.emplace_back([] {
        for (unsigned i = 0; i < 100; ++i) {
            const auto id = std::to_string(i);
            Write("concurrent", "\"id\":" + id + ",\"escaped\":" + Quote("\"\\\n\t"), id);
        }
    });
    for (auto& thread : threads) thread.join();
    assert(seen.size() == 100);
    for (std::uintptr_t i = 0; i < 128; ++i)
        RecordFault(reinterpret_cast<void*>(i + 1), reinterpret_cast<void*>(i + 1000));
    PollFaults();
    assert(faults_dropped.load() == 0);
    for (const auto& slot : faults) assert(slot.state.load() == 0);
    RecordFault(reinterpret_cast<void*>(999), reinterpret_cast<void*>(9999));
    PollFaults();
    if (std::string_view(argv[2]) == "limit") {
        Write("too_large", std::string(MaxBytes, 'x'));
        assert(exhausted);
    } else {
        for (size_t i = 100; i <= MaxUnique; ++i)
            Write("detail", "\"id\":" + std::to_string(i), std::to_string(i));
        assert(details_exhausted);
        Write("summary_after_detail_limit", "\"retained\":true", {}, true);
    }
    if (output) { std::fclose(output); output = nullptr; }
}
