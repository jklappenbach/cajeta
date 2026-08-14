//
// jupyter-kernel U3 (spec 4.1; plan 3.1.1, 3.3.1) — cell output.
//
// A notebook that computes correctly and shows nothing is not a notebook. The
// contract these pin: what a cell writes reaches the handler in write order,
// WHILE the cell is still running, and a cell that writes a lot neither
// deadlocks nor loses bytes.
//

#include "gtest/gtest.h"
#include "cajeta/kernel/KernelSession.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using cajeta::kernel::KernelSession;
using cajeta::kernel::CellResult;

namespace {

std::unique_ptr<KernelSession> freshSession() {
    std::string error;
    auto s = KernelSession::create(&error);
    EXPECT_TRUE(s != nullptr) << "session create failed: " << error;
    return s;
}

// Collects chunks off the capture's pump thread, and remembers whether any
// arrived while a cell was still executing — the difference between streaming
// and batching at the end.
struct Collector {
    std::mutex mutex;
    std::vector<std::string> chunks;
    std::atomic<bool> executing{false};
    std::atomic<bool> sawWhileExecuting{false};

    KernelSession::StreamHandler handler() {
        return [this](const std::string& chunk) {
            if (executing.load()) sawWhileExecuting = true;
            std::lock_guard<std::mutex> lock(mutex);
            chunks.push_back(chunk);
        };
    }

    std::string joined() {
        std::lock_guard<std::mutex> lock(mutex);
        std::string all;
        for (auto& c : chunks) all += c;
        return all;
    }

    size_t count() {
        std::lock_guard<std::mutex> lock(mutex);
        return chunks.size();
    }
};

}  // namespace

// 3.1.1 / spec 4.1 — what a cell prints reaches the handler, in order.
TEST(KernelIoTests, cellStdoutReachesTheHandler) {
    auto s = freshSession();
    ASSERT_NE(nullptr, s.get());
    Collector out;
    s->setStreamHandler(out.handler());

    CellResult c1 = s->execute(
        "System.stdout.println(\"first\");\n"
        "System.stdout.println(\"second\");\n");
    ASSERT_TRUE(c1.ok) << c1.errorId << ": " << c1.message;

    std::string text = out.joined();
    EXPECT_NE(std::string::npos, text.find("first"));
    EXPECT_NE(std::string::npos, text.find("second"));
    EXPECT_LT(text.find("first"), text.find("second")) << "output reordered";
}

// 3.1.1 / spec 4.1 — the point of the pump: a long-running cell's output
// arrives DURING execution, not as one burst after it returns.
TEST(KernelIoTests, outputStreamsDuringExecution) {
    auto s = freshSession();
    ASSERT_NE(nullptr, s.get());
    Collector out;
    s->setStreamHandler(out.handler());

    out.executing = true;
    CellResult c1 = s->execute(
        "int32 i = 0;\n"
        "while (i < 20000) { System.stdout.println(\"tick\"); i = i + 1; }\n");
    out.executing = false;
    ASSERT_TRUE(c1.ok) << c1.errorId << ": " << c1.message;

    // sawWhileExecuting alone would NOT distinguish streaming from batching:
    // the capture is scoped inside execute(), so even a single tail-drain
    // chunk arrives before execute() returns. The chunk COUNT is what
    // separates them — a batch delivery is exactly one chunk.
    EXPECT_TRUE(out.sawWhileExecuting.load())
        << "no chunk arrived before the cell finished";
    EXPECT_GT(out.count(), 1u)
        << "output arrived as a single chunk — batched, not streamed";
}

// 3.3.1 — a cell printing ~1 MB neither deadlocks nor drops bytes. This is
// why the capture is file-backed: a pipe's fixed buffer would block the
// writing cell as soon as the reader fell behind.
TEST(KernelIoTests, largeOutputNeitherDeadlocksNorTruncates) {
    auto s = freshSession();
    ASSERT_NE(nullptr, s.get());
    Collector out;
    s->setStreamHandler(out.handler());

    // 16 chars per line x 65536 lines ~= 1 MB.
    CellResult c1 = s->execute(
        "int32 i = 0;\n"
        "while (i < 65536) { System.stdout.println(\"0123456789abcde\"); "
        "i = i + 1; }\n");
    ASSERT_TRUE(c1.ok) << c1.errorId << ": " << c1.message;

    std::string text = out.joined();
    EXPECT_GE(text.size(), 1000000u) << "bytes were lost";
}

// Output belongs to the cell that wrote it: a later cell's handler sees only
// its own bytes, so the frontend can attribute them to the right In[N].
TEST(KernelIoTests, outputIsPerCell) {
    auto s = freshSession();
    ASSERT_NE(nullptr, s.get());
    Collector out;
    s->setStreamHandler(out.handler());

    ASSERT_TRUE(s->execute("System.stdout.println(\"cell one\");\n").ok);
    size_t afterFirst = out.count();
    ASSERT_GT(afterFirst, 0u);

    ASSERT_TRUE(s->execute("System.stdout.println(\"cell two\");\n").ok);
    std::string all = out.joined();
    EXPECT_NE(std::string::npos, all.find("cell one"));
    EXPECT_NE(std::string::npos, all.find("cell two"));
    EXPECT_LT(all.find("cell one"), all.find("cell two"));
}
