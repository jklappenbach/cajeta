// ide-symbol-index Unit 3 — xref on the lint channel, and over a whole root.
//
// The plugin already runs `cajeta --lint <staged-buffer> --source-root <root>
// --shadow <original> --diag-format=json` on every edit (CajetaLintAnnotator).
// Spec §1.5.2: ONE subprocess per edit, not two — so the xref export rides that
// invocation, emitted on the same NDJSON channel as diagnostics and demultiplexed
// by the `kind` field.
//
// What lint-mode xref carries, honestly: declarations, inheritance, enums,
// template members, and parse-time type references. NOT calls or field accesses —
// body-level resolveTypes runs only inside Method::generateCode (the codegen
// phase lint deliberately stops before), so those edges come from a whole-root
// or entry-point compile. Per-edit, what must stay fresh is the buffer's own
// declarations; the plugin keeps its last good edges for the rest.
//
// Pins (plan 3.1.1 - 3.1.5):
//   3.1.1  --emit-xref in lint mode emits kind:"xref" records among diagnostics;
//          the diagnostic records themselves are unchanged.
//   3.1.1b Without --emit-xref, the lint stream carries no xref records (2.0.4).
//   3.1.3  --shadow: records report the ORIGINAL path, not the staged temp file.
//   3.1.4  --lint <directory> --emit-xref=<path> exports one document covering
//          every file under the root.
//   3.1.5  The stream declares its schema version before any record.
//   2.0.5  A syntax-broken buffer still yields a well-formed (near-empty) stream
//          and unchanged diagnostics — the plugin keeps its previous index.

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

#ifdef _WIN32
#  define CAJETA_XLINT_DEVNULL "NUL"
#else
#  define CAJETA_XLINT_DEVNULL "/dev/null"
#endif

    std::string compilerBinary() {
        const char* envRoot = std::getenv("CAJETA_SOURCE_ROOT");
        std::string r = (envRoot && *envRoot) ? envRoot :
#ifdef CAJETA_SOURCE_ROOT_DEFAULT
            CAJETA_SOURCE_ROOT_DEFAULT;
#else
            ".";
#endif
        return r + "/build/src/cajeta";
    }

    fs::path freshTempDir(const std::string& tag) {
        static std::mt19937_64 rng(std::random_device{}());
        auto base = fs::temp_directory_path()
                  / ("cajeta_xlint_" + tag + "_" + std::to_string(rng()));
        fs::create_directories(base);
        return base;
    }

    fs::path writeUnit(const fs::path& root, const std::string& rel,
                       const std::string& text) {
        auto file = root / rel;
        fs::create_directories(file.parent_path());
        std::ofstream out(file);
        out << text;
        return file;
    }

    // A two-file project: the target references a sibling, so --source-root
    // resolution is exercised.
    fs::path makeProject(const std::string& tag) {
        auto root = freshTempDir(tag);
        writeUnit(root, "demo/Helper.cajeta",
            "package demo;\n"
            "public class Helper {\n"
            "    public int32 assist() {\n"
            "        return 40;\n"
            "    }\n"
            "}\n");
        writeUnit(root, "demo/Target.cajeta",
            "package demo;\n"                          // 1
            "public class Target {\n"                  // 2
            "    Helper aide;\n"                       // 3
            "    public Target() {\n"                  // 4
            "        this.aide = heap Helper();\n"     // 5
            "    }\n"
            "    public int32 answer() {\n"            // 7
            "        return 2;\n"
            "    }\n"
            "}\n");
        return root;
    }

    // Run the binary with `args`, capturing stderr. Returns the exit code
    // (-1 if the binary is missing).
    int runCapturingStderr(const std::string& args, std::string& err) {
        auto bin = compilerBinary();
        if (!fs::exists(bin)) return -1;
        auto errFile = freshTempDir("err") / "stderr.txt";
        std::string cmd = bin + " " + args
                        + " > " CAJETA_XLINT_DEVNULL " 2> " + errFile.string();
        int rc = std::system(cmd.c_str());
        std::ifstream f(errFile);
        err.assign((std::istreambuf_iterator<char>(f)),
                    std::istreambuf_iterator<char>());
#ifdef _WIN32
        return rc;
#else
        return WEXITSTATUS(rc);
#endif
    }

    bool has(const std::string& hay, const std::string& needle) {
        return hay.find(needle) != std::string::npos;
    }

    // The NDJSON lines of kind:"xref".
    std::vector<std::string> xrefLines(const std::string& stderrText) {
        std::vector<std::string> out;
        std::istringstream in(stderrText);
        for (std::string line; std::getline(in, line); ) {
            if (line.rfind("{", 0) == 0 && has(line, "\"kind\":\"xref\"")) {
                out.push_back(line);
            }
        }
        return out;
    }

} // namespace

// ---- 3.1.1 — xref records ride the diagnostic channel -------------------------

TEST(XrefLint, LintEmitsXrefRecordsAmongDiagnostics) {
    auto root = makeProject("stream");
    auto target = root / "demo/Target.cajeta";

    std::string err;
    int rc = runCapturingStderr(
        "--lint " + target.string() + " --source-root " + root.string()
        + " --diag-format=json --emit-xref", err);
    ASSERT_NE(rc, -1) << "compiler binary missing";
    EXPECT_EQ(rc, 0) << err;

    auto lines = xrefLines(err);
    ASSERT_FALSE(lines.empty()) << "no xref records in the lint stream:\n" << err;

    // The target's own declarations are present, positioned in the target.
    std::string all;
    for (auto& l : lines) all += l + "\n";
    EXPECT_TRUE(has(all, "\"fqn\": \"demo.Target\"")) << all;
    EXPECT_TRUE(has(all, "\"fqn\": \"demo.Target.answer\"")) << all;
    EXPECT_TRUE(has(all, "\"fqn\": \"demo.Target.aide\"")) << all;
    // The field's type reference resolved against the SIBLING via --source-root.
    EXPECT_TRUE(has(all, "\"target\": \"demo.Helper\""))
        << "no type reference to the sibling-resolved demo.Helper:\n" << all;
}

// ide-symbol-index (allocation-type navigation): the CREATED type of an
// allocation (`heap Helper()` / `stack Helper()`) is recorded as a type
// reference at its token, so Ctrl-click on the allocated type resolves. The
// allocation's type is otherwise resolved only in the codegen pass, which lint
// stops before — so it must be captured at parse time, like declaration-
// position type references. Here Target names Helper ONLY through allocations,
// so a reference to demo.Helper can only have come from the created type.
TEST(XrefLint, AllocationCreatedTypeIsRecordedAsReference) {
    auto root = makeProject("alloc");
    auto target = writeUnit(root, "demo/Target.cajeta",
        "package demo;\n"
        "public class Target {\n"
        "    public void run() {\n"
        "        Object a = heap Helper();\n"
        "        Object b = stack Helper();\n"
        "    }\n"
        "}\n");

    std::string err;
    int rc = runCapturingStderr(
        "--lint " + target.string() + " --source-root " + root.string()
        + " --diag-format=json --emit-xref", err);
    ASSERT_NE(rc, -1) << "compiler binary missing";

    std::string all;
    for (auto& l : xrefLines(err)) all += l + "\n";
    // Two allocation sites → two reference records to the same declaration.
    EXPECT_TRUE(has(all, "\"target\": \"demo.Helper\""))
        << "the allocation's created type was not recorded as a reference:\n" << all;
    int refs = 0;
    for (auto& l : xrefLines(err))
        if (has(l, "\"rel\":\"references\"") && has(l, "\"target\": \"demo.Helper\"")) ++refs;
    EXPECT_GE(refs, 2)
        << "expected a reference for each of `heap Helper()` and `stack Helper()`; got "
        << refs << "\n" << all;
}

// The stream is scoped to the linted file: sibling and stdlib declarations are
// the whole-root export's job (and would bloat every keystroke's output).
TEST(XrefLint, TheStreamCarriesOnlyTheLintedFilesRecords) {
    auto root = makeProject("scope");
    auto target = root / "demo/Target.cajeta";

    std::string err;
    runCapturingStderr(
        "--lint " + target.string() + " --source-root " + root.string()
        + " --diag-format=json --emit-xref", err);

    std::string all;
    for (auto& l : xrefLines(err)) all += l + "\n";
    ASSERT_FALSE(all.empty());
    EXPECT_FALSE(has(all, "\"fqn\": \"demo.Helper\""))
        << "a sibling's declaration leaked into the per-edit stream:\n" << all;
    EXPECT_FALSE(has(all, "\"fqn\": \"cajeta.lang.String\""))
        << "stdlib declarations leaked into the per-edit stream";
}

// Diagnostics must be byte-for-byte what they were: same fields, same channel.
// The plugin's existing parser must not notice the feature exists.
TEST(XrefLint, DiagnosticRecordsAreUnchangedByXrefEmission) {
    auto root = freshTempDir("diagsame");
    // An unknown TYPE: the diagnostic lint genuinely produces. (An undefined
    // identifier in a body would not do — body resolution is codegen-phase,
    // which lint stops before.)
    auto file = writeUnit(root, "demo/Bad.cajeta",
        "package demo;\n"
        "public class Bad {\n"
        "    UnknownAbc u;\n"
        "}\n");

    std::string with, without;
    runCapturingStderr("--lint " + file.string() + " --diag-format=json", without);
    runCapturingStderr("--lint " + file.string()
                       + " --diag-format=json --emit-xref", with);

    // Every diagnostic line from the plain run appears verbatim in the xref run.
    std::istringstream in(without);
    int checked = 0;
    for (std::string line; std::getline(in, line); ) {
        if (line.rfind("{", 0) != 0 || !has(line, "\"severity\"")) continue;
        ++checked;
        EXPECT_TRUE(has(with, line))
            << "diagnostic changed by --emit-xref:\n" << line;
    }
    EXPECT_GT(checked, 0) << "fixture produced no diagnostics; test is vacuous";
}

// ---- 2.0.4 — opt-in: no flag, no records ---------------------------------------

TEST(XrefLint, WithoutTheFlagTheStreamHasNoXrefRecords) {
    auto root = makeProject("optout");
    auto target = root / "demo/Target.cajeta";

    std::string err;
    runCapturingStderr(
        "--lint " + target.string() + " --source-root " + root.string()
        + " --diag-format=json", err);
    EXPECT_TRUE(xrefLines(err).empty())
        << "xref records emitted without --emit-xref";
}

// ---- 3.1.5 — the stream declares its schema version before any record ---------

TEST(XrefLint, TheStreamDeclaresItsSchemaVersionFirst) {
    auto root = makeProject("version");
    auto target = root / "demo/Target.cajeta";

    std::string err;
    runCapturingStderr(
        "--lint " + target.string() + " --source-root " + root.string()
        + " --diag-format=json --emit-xref", err);

    auto lines = xrefLines(err);
    ASSERT_FALSE(lines.empty());
    EXPECT_TRUE(has(lines.front(), "\"rel\":\"version\"")) << lines.front();
    EXPECT_TRUE(has(lines.front(), "\"major\": 1")) << lines.front();
}

// ---- 3.1.3 — --shadow: records report the ORIGINAL path ------------------------
//
// The plugin lints a STAGED COPY of the buffer (a temp file) and passes the real
// file as --shadow. An xref record naming the temp path would be ingested under a
// key no editor buffer will ever ask about — worse than useless, it would orphan
// the file's index entry on every keystroke.

TEST(XrefLint, ShadowRemapsRecordPathsToTheOriginal) {
    auto root = makeProject("shadow");
    auto original = root / "demo/Target.cajeta";

    // Stage a copy elsewhere, the way CajetacRunner does.
    auto staged = freshTempDir("staged") / "cajeta-lint-Target-1234.cajeta";
    fs::copy_file(original, staged);

    std::string err;
    int rc = runCapturingStderr(
        "--lint " + staged.string() + " --source-root " + root.string()
        + " --shadow " + original.string()
        + " --diag-format=json --emit-xref", err);
    EXPECT_EQ(rc, 0) << err;

    std::string all;
    for (auto& l : xrefLines(err)) all += l + "\n";
    ASSERT_FALSE(all.empty()) << err;

    EXPECT_TRUE(has(all, "\"file\": \"demo/Target.cajeta\""))
        << "records not reported against the original root-relative path:\n" << all;
    EXPECT_FALSE(has(all, "cajeta-lint-Target-1234"))
        << "the staged temp path leaked into the xref stream:\n" << all;
}

// ---- 2.0.5 — a broken buffer yields a well-formed, near-empty stream -----------
//
// Mid-edit the buffer is broken most of the time. The right answer is: unchanged
// diagnostics, a version line, and NO declaration records for the file — the
// plugin then keeps its previous index for that file (a stale answer beats a
// wrong one, spec §7). Records invented from a half-parsed buffer would be wrong.

TEST(XrefLint, ABrokenBufferYieldsDiagnosticsAndNoInventedRecords) {
    auto root = makeProject("broken");
    auto target = writeUnit(root, "demo/Target.cajeta",
        "package demo;\n"
        "public class Target {\n"
        "    public int32 answer( {\n"
        "        return ;;;\n"
        "    }\n"
        "}\n");

    std::string err;
    int rc = runCapturingStderr(
        "--lint " + target.string() + " --source-root " + root.string()
        + " --diag-format=json --emit-xref", err);
    EXPECT_NE(rc, 0) << "the buffer is broken; lint must still fail";

    EXPECT_TRUE(has(err, "\"severity\""))
        << "syntax diagnostics vanished under --emit-xref:\n" << err;

    std::string all;
    for (auto& l : xrefLines(err)) all += l + "\n";
    EXPECT_FALSE(has(all, "\"fqn\": \"demo.Target"))
        << "declaration records invented from a buffer that did not parse:\n" << all;
}

// ---- 3.1.4 — whole-root export: one document, every file -----------------------

TEST(XrefLint, WholeRootExportCoversEveryFileUnderTheRoot) {
    auto root = makeProject("wholeroot");
    writeUnit(root, "demo/sub/Third.cajeta",
        "package demo.sub;\n"
        "public class Third {\n"
        "    public int32 three() {\n"
        "        return 3;\n"
        "    }\n"
        "}\n");

    auto out = root / "xref.json";
    std::string err;
    int rc = runCapturingStderr(
        "--lint " + root.string() + " --emit-xref=" + out.string()
        + " --diag-format=json", err);
    EXPECT_EQ(rc, 0) << err;

    ASSERT_TRUE(fs::exists(out)) << "whole-root export wrote no document";
    std::ifstream f(out);
    std::string doc((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());

    // Every file under the root contributed, nested directories included.
    EXPECT_TRUE(has(doc, "\"demo.Target\""));
    EXPECT_TRUE(has(doc, "\"demo.Helper\""));
    EXPECT_TRUE(has(doc, "\"demo.sub.Third\""));
    // Paths are root-relative.
    EXPECT_TRUE(has(doc, "\"file\": \"demo/sub/Third.cajeta\"")) << doc.substr(0, 600);
    // And it is the versioned document, not the NDJSON stream.
    EXPECT_TRUE(has(doc, "\"major\": 1"));
}

// A broken file must not sink the rest of the root (same guarantee the compile
// path holds since 2.1.8) — cold-indexing a project with one bad file must
// still index the other N-1.

TEST(XrefLint, WholeRootExportSurvivesABrokenFile) {
    auto root = makeProject("wholerootbroken");
    writeUnit(root, "demo/Broken.cajeta",
        "package demo;\n"
        "public class Broken {\n"
        "    public int32 oops( {\n"
        "}\n");

    auto out = root / "xref.json";
    std::string err;
    runCapturingStderr(
        "--lint " + root.string() + " --emit-xref=" + out.string()
        + " --diag-format=json", err);

    ASSERT_TRUE(fs::exists(out)) << "one broken file sank the whole-root export";
    std::ifstream f(out);
    std::string doc((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    EXPECT_TRUE(has(doc, "\"demo.Target\""));
    EXPECT_TRUE(has(doc, "\"demo.Helper\""));
    EXPECT_FALSE(has(doc, "\"demo.Broken\""));
}

// A directory target without --emit-xref=<path> is an error, not a guess: linting
// "every file, diagnostics only" is not a thing the plugin asks for, and silently
// linting the first file would be a wrong answer dressed as a right one.

TEST(XrefLint, ADirectoryTargetRequiresAnExportPath) {
    auto root = makeProject("dirnoflag");
    std::string err;
    int rc = runCapturingStderr(
        "--lint " + root.string() + " --diag-format=json", err);
    EXPECT_NE(rc, 0);
}

// ---- 4.4 — annotation declarations (found by Unit 4 acceptance) -----------------
//
// Linting the extracted stdlib showed every `annotation`-only file (cajeta/aot/*)
// silent in the export: annotations register as name-only types in canonicalMap
// (never in structures) and carried no declaring position, so the collector had
// nothing to place. The exported FQN is the COMPILER'S canonical identity —
// `code.<Name>` regardless of the file's package (visitAnnotationTypeDeclaration
// unifies `@Foo` usage and declaration under the synthetic "code" package) —
// because an FQN the resolver doesn't use would be a wrong answer.

TEST(XrefLint, AnAnnotationDeclarationIsExportedAtItsOwnToken) {
    auto root = freshTempDir("annot");
    writeUnit(root, "demo/Retry.cajeta",
        "package demo;\n"                  // 1
        "\n"                               // 2
        "annotation Retry {\n"             // 3, identifier at col 11
        "    int32 attempts() default 3;\n"
        "}\n");

    std::string err;
    int rc = runCapturingStderr(
        "--lint " + (root / "demo/Retry.cajeta").string()
        + " --source-root " + root.string()
        + " --diag-format=json --emit-xref", err);
    ASSERT_NE(rc, -1) << "compiler binary missing";
    EXPECT_EQ(rc, 0) << err;

    std::string all;
    for (auto& l : xrefLines(err)) all += l + "\n";
    EXPECT_TRUE(has(all, "\"fqn\": \"code.Retry\""))
        << "annotation declaration missing from the stream:\n" << all;
    EXPECT_TRUE(has(all, "\"kind\": \"annotation\"")) << all;
    EXPECT_TRUE(has(all, "\"file\": \"demo/Retry.cajeta\", \"line\": 3")) << all;
}
