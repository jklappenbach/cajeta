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

// --- Out[N] / unit result -------------------------------------------------
//
// The decision these rest on is made in CODEGEN, not in synthesis. A cell's
// trailing expression displays its value only if it HAS one, and `void` is a
// type answer: the synthesizer splices token text before any type exists and
// cannot tell `x + y;` from `xs.add(1);`. So Block MARKS the statement (it
// knows which) and ExpressionStatement decides (it knows the type). An
// earlier attempt that rewrote the trailing statement into the entry's
// `return "" + (EXPR)` foundered on exactly that, turning void calls into
// results and breaking four KernelCellTests.
//
// The result rides a side channel in the session runtime rather than the
// entry's return value, for the same reason: the signature is fixed long
// before the type is known.

// 3.1.2 / spec 4.2 — a cell ending in an expression has that value as its
// result, rendered as text for Out[N].
TEST(KernelIoTests, unitResultRendersPrimitive) {
    auto s = freshSession();
    ASSERT_NE(nullptr, s.get());

    CellResult c1 = s->execute("int32 x = 3;\nint32 y = 4;\nx + y;\n");
    ASSERT_TRUE(c1.ok) << c1.errorId << ": " << c1.message;
    EXPECT_TRUE(c1.hasResult) << "no Out[N] for a trailing expression";
    EXPECT_EQ("7", c1.result);
    EXPECT_EQ(1, c1.executionCount);
}

// A String result renders as its text, not as a quoted or decorated form.
TEST(KernelIoTests, unitResultRendersString) {
    auto s = freshSession();
    ASSERT_NE(nullptr, s.get());

    CellResult c1 = s->execute("\"hello\";\n");
    ASSERT_TRUE(c1.ok) << c1.errorId << ": " << c1.message;
    EXPECT_TRUE(c1.hasResult);
    EXPECT_EQ("hello", c1.result);
}

// 4.3 — an object's result goes through toString(), by the same virtual
// lookup @ToString emits, so an override on the runtime class wins.
TEST(KernelIoTests, unitResultRendersObjectViaToString) {
    auto s = freshSession();
    ASSERT_NE(nullptr, s.get());

    CellResult c1 = s->execute(
        "public class Point { public int32 x; public int32 y;\n"
        "  public Point(int32 x, int32 y) { this.x = x; this.y = y; }\n"
        "  public String toString() { return \"Point(\" + this.x + \",\" "
        "+ this.y + \")\"; } }\n"
        "Point p = heap Point(1, 2);\n"
        "p;\n");
    ASSERT_TRUE(c1.ok) << c1.errorId << ": " << c1.message;
    EXPECT_TRUE(c1.hasResult);
    EXPECT_EQ("Point(1,2)", c1.result);
}

// The case that reverted the first attempt: a trailing VOID call is a
// statement, not a result. Nothing to display, and the cell still runs.
TEST(KernelIoTests, trailingVoidCallIsNotAResult) {
    auto s = freshSession();
    ASSERT_NE(nullptr, s.get());

    CellResult c1 = s->execute(
        "ArrayList<int32> xs = heap ArrayList<int32>();\n"
        "xs.add(1);\n");
    ASSERT_TRUE(c1.ok) << c1.errorId << ": " << c1.message;
    EXPECT_FALSE(c1.hasResult) << "a void call rendered as a result";
}

// Not every cell has a result: one ending in a declaration, a return or a
// loop has no Out[N], and the frontend must be able to tell that apart from
// a result that rendered as the empty string.
TEST(KernelIoTests, cellsWithoutTrailingExpressionHaveNoResult) {
    auto s = freshSession();
    ASSERT_NE(nullptr, s.get());

    CellResult decl = s->execute("int32 z = 9;\n");
    ASSERT_TRUE(decl.ok) << decl.errorId << ": " << decl.message;
    EXPECT_FALSE(decl.hasResult);

    CellResult ret = s->execute("return 5;\n");
    ASSERT_TRUE(ret.ok) << ret.errorId << ": " << ret.message;
    EXPECT_FALSE(ret.hasResult);
    EXPECT_EQ(5, ret.value);
}

// 3.1.3 / spec 4 — "rendering failures degrade to a type-name placeholder;
// display must never fail a successfully executed cell". A class with no
// toString anywhere in its chain has nothing to render; the cell still runs
// and still produces a payload.
TEST(KernelIoTests, renderFailureDegradesToTypeName) {
    auto s = freshSession();
    ASSERT_NE(nullptr, s.get());

    CellResult c1 = s->execute(
        "public class Opaque { public int32 n;\n"
        "  public Opaque(int32 n) { this.n = n; } }\n"
        "Opaque o = heap Opaque(7);\n"
        "o;\n");
    ASSERT_TRUE(c1.ok) << c1.errorId << ": " << c1.message;
    EXPECT_TRUE(c1.hasResult) << "an unrenderable value produced no payload";
    EXPECT_NE(std::string::npos, c1.result.find("Opaque"))
        << "placeholder was not the type name: " << c1.result;
}

// 3.1.4 / spec 4.4 — a cell's diagnostics arrive STRUCTURED: severity, code,
// the cell's name and the user's line, parsed from the compiler's own NDJSON
// rather than scraped out of its prose.
TEST(KernelIoTests, compilerJsonlBridged) {
    auto s = freshSession();
    ASSERT_NE(nullptr, s.get());

    CellResult ok = s->execute("int32 a = 1;\n");
    ASSERT_TRUE(ok.ok) << ok.errorId << ": " << ok.message;

    // Line 2 of the CELL, not of the wrapper the synthesizer built.
    CellResult bad = s->execute("int32 b = 2;\nint32 c = notAThing();\n");
    ASSERT_FALSE(bad.ok) << "expected the cell to fail";
    ASSERT_FALSE(bad.diagnostics.empty()) << "no structured diagnostic";

    const cajeta::kernel::CellDiagnostic* err = nullptr;
    for (auto& d : bad.diagnostics) {
        if (d.severity == "error") { err = &d; break; }
    }
    ASSERT_NE(nullptr, err) << "no error-severity diagnostic";
    EXPECT_FALSE(err->message.empty());
    EXPECT_EQ("In[2]", err->file) << "diagnostic did not name the cell";
    EXPECT_EQ(2, err->line) << "line is not the user's";
    // The fatal error is in both places: the flat fields a simple host reads,
    // and the structured list a frontend renders.
    EXPECT_EQ(bad.errorId, err->code);
    EXPECT_EQ(bad.message, err->message);
}

// A clean cell reports no diagnostics — so a non-empty list always means the
// compiler had something to say, and a frontend can show it unconditionally.
TEST(KernelIoTests, cleanCellHasNoDiagnostics) {
    auto s = freshSession();
    ASSERT_NE(nullptr, s.get());

    CellResult c1 = s->execute("int32 q = 1;\nq + 1;\n");
    ASSERT_TRUE(c1.ok) << c1.errorId << ": " << c1.message;
    EXPECT_TRUE(c1.diagnostics.empty())
        << "clean cell reported " << c1.diagnostics.size() << " diagnostics, "
        << "first: " << c1.diagnostics[0].message;
}

// The counter advances on every execute, including a failed cell (spec 2.2),
// so Out[N] never reuses a number.
TEST(KernelIoTests, executionCountAdvancesAcrossFailures) {
    auto s = freshSession();
    ASSERT_NE(nullptr, s.get());

    CellResult c1 = s->execute("1 + 1;\n");
    ASSERT_TRUE(c1.ok);
    EXPECT_EQ(1, c1.executionCount);

    CellResult bad = s->execute("return notAThing();\n");
    ASSERT_FALSE(bad.ok);
    EXPECT_EQ(2, bad.executionCount);

    CellResult c3 = s->execute("2 + 2;\n");
    ASSERT_TRUE(c3.ok) << c3.errorId << ": " << c3.message;
    EXPECT_EQ(3, c3.executionCount);
    EXPECT_EQ("4", c3.result);
}
