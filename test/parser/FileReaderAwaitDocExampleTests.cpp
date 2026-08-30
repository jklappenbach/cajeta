//
// pollable-stdin unit 4 — the documented line-buffering example, executed.
//
// Spec 5.2.3 asks the docs to SHOW the incremental buffering shape rather
// than gesture at it, because readiness is bytes and not lines: a read can
// land mid-line, and the caller has to carry the tail across passes.
//
// This test runs the example from `FileReader.awaitReadable`'s docstring
// verbatim. That is not ceremony — NOTHING ELSE COMPILES IT. Checked
// 2026-08-30: `check-docstring-examples.sh` is a LINT (it rejects the
// invalid `#Type local =` pattern and nothing more), and
// `check-doc-snippets.sh` compiles ```cajeta blocks only out of MARKDOWN
// (docs/guide, docs/stdlib, README). A fenced block inside a `.cajeta`
// doc comment is covered by neither. Plan 4.1.1 assumed a checker that
// compiles docstring examples; there isn't one, so this is it.
//
// The fixture deliberately ends WITHOUT a trailing newline, so the final
// line stays in `pending` — that is the partial-tail case the example
// exists to demonstrate, and an implementation that only ever saw whole
// lines would look correct without it.
//
// Pins (plan ids in brackets):
//   [4.1.1] the documented example compiles and produces the right count
//   [4.2.3] the buffering shape is real code, and it works
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

using cajeta_test::CajetaJit;

namespace {

std::string writeFixture(const std::string& name, const std::string& body) {
    std::string dir = "tmp/pollable-stdin";
    std::filesystem::create_directories(dir);
    std::string path = dir + "/" + name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << body;
    out.close();
    return std::filesystem::absolute(path).string();
}

// The docstring example, with `File.openRead(path)` standing in for
// `heap FileReader(0)` so the test can supply deterministic input.
// Returns lines*1000 + pending, so both the completed-line count and the
// carried tail are asserted from one run.
std::string docExample(const std::string& path) {
    return
        "package test;\n"
        "import cajeta.io.file.File;\n"
        "import cajeta.io.file.FileReader;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        FileReader in #= File.openRead(\"" + path + "\");\n"
        "        int8[] buf  = heap int8[4096];\n"
        "        int8[] tail = heap int8[65536];\n"
        "        int32 pending = 0;\n"
        "        int32 lines = 0;\n"
        "        while (in.awaitReadable(0 - 1)) {\n"
        "            int32 n = in.read(buf, 4096);\n"
        "            if (n == 0) { break; }\n"
        "            int32 i = 0;\n"
        "            while (i < n) {\n"
        "                if (buf[i] == 10) {\n"
        "                    lines = lines + 1;\n"
        "                    pending = 0;\n"
        "                } else {\n"
        "                    tail[pending] = buf[i];\n"
        "                    pending = pending + 1;\n"
        "                }\n"
        "                i = i + 1;\n"
        "            }\n"
        "        }\n"
        "        in.close();\n"
        "        return lines * 1000 + pending;\n"
        "    }\n"
        "}\n";
}

} // namespace

#if defined(__linux__)

// [4.1.1][4.2.3] Three complete lines plus an unterminated tail of 4
// bytes. The tail is the point: it proves the buffer survives the read
// that did not end on a newline.
TEST(FileReaderAwaitDocExampleTests, documentedLoopCountsLinesAndKeepsTheTail) {
    auto path = writeFixture("doc-lines.jsonl",
        "{\"id\":1}\n"
        "{\"id\":2}\n"
        "{\"id\":3}\n"
        "frag");                       // no trailing newline
    auto jit = CajetaJit::compile(docExample(path), "test.D");
    int32_t rc = jit->lookup<int32_t (*)()>("run")();

    EXPECT_EQ(3, rc / 1000) << "expected 3 completed lines, got " << (rc / 1000);
    EXPECT_EQ(4, rc % 1000)
        << "expected 4 bytes carried in the tail (\"frag\"), got "
        << (rc % 1000) << " — the partial line was lost";
}

// The loop must TERMINATE on EOF rather than spin. At end of input the fd
// reports ready and the read returns 0; if `awaitReadable` reported
// not-ready there instead, this would hang rather than fail, so the
// assertion is really "the test finished at all".
TEST(FileReaderAwaitDocExampleTests, documentedLoopTerminatesOnEof) {
    auto path = writeFixture("doc-empty.jsonl", "");
    auto jit = CajetaJit::compile(docExample(path), "test.D");
    int32_t rc = jit->lookup<int32_t (*)()>("run")();
    EXPECT_EQ(0, rc) << "an empty input must yield no lines and no tail";
}

#endif // __linux__
