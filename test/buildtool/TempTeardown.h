// Best-effort recursive delete for build-tool test teardown.
//
// On Windows a just-written artifact (a fetched/staged `.cja`) can stay briefly
// locked by another process — Defender scanning it, or a lazy handle release —
// so the throwing std::filesystem::remove_all aborts the test even though the
// test itself succeeded. Retry a few times and never throw: a teardown hiccup
// must not turn a passing test red. On POSIX it removes on the first try.

#pragma once

#include <chrono>
#include <filesystem>
#include <system_error>
#include <thread>

inline void rmTree(const std::filesystem::path& p) {
    namespace fs = std::filesystem;
    std::error_code ec;
    for (int i = 0; i < 20; ++i) {
        fs::remove_all(p, ec);
        if (!ec) return;
        if (!fs::exists(p, ec)) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
}
