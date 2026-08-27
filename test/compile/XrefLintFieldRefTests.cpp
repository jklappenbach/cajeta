// xref-lint-emission-gap Unit 3 — field references on the lint path.
//
// `--lint` now resolves method bodies (Method::resolveBody, Unit 2), which is
// where DotExpression::recordFieldXref runs. Before this, a file like the one
// below emitted its declarations and NOTHING else — no reference for a field
// it reads and writes on four lines — and Ctrl-click on a field resolved
// nowhere, in any project, ever (spec §1.2, §1.4).
//
// These pin the relation itself. Unit 1's XrefRelationCoverage pins that it is
// non-empty at all and agrees with the build path; this file pins WHERE the
// references land, whose field they name, and what happens when a body cannot
// be resolved.
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
#  define CAJETA_XFR_DEVNULL "NUL"
#else
#  define CAJETA_XFR_DEVNULL "/dev/null"
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

bool haveCompiler() { return fs::exists(compilerBinary()); }

fs::path freshTempDir(const std::string& tag) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = fs::temp_directory_path()
              / ("cajeta_xfr_" + tag + "_" + std::to_string(rng()));
    fs::create_directories(base);
    return base;
}

void writeUnit(const fs::path& root, const std::string& rel,
               const std::string& text) {
    auto file = root / rel;
    fs::create_directories(file.parent_path());
    std::ofstream(file) << text;
}

std::string slurp(const fs::path& p) {
    std::ifstream f(p);
    if (!f) return "";
    return std::string((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
}

// Whole-root lint export.
std::string lintRoot(const fs::path& root) {
    auto out = freshTempDir("doc") / "xref.json";
    std::string cmd = compilerBinary() + " --lint " + root.string()
                    + " --emit-xref=" + out.string() + " --diag-format=json"
                    + " > " CAJETA_XFR_DEVNULL " 2>&1";
    (void) std::system(cmd.c_str());
    return slurp(out);
}

// One record per line inside each relation's array — the writer's own
// determinism pin keeps that stable.
std::vector<std::string> relation(const std::string& doc, const std::string& name) {
    std::vector<std::string> out;
    const std::string key = "\"" + name + "\": [";
    auto at = doc.find(key);
    if (at == std::string::npos) return out;
    std::istringstream in(doc.substr(at + key.size()));
    for (std::string line; std::getline(in, line); ) {
        auto b = line.find_first_not_of(" \t");
        if (b == std::string::npos) continue;
        auto e = line.find_last_not_of(" \t\r");
        std::string t = line.substr(b, e - b + 1);
        if (t.rfind("]", 0) == 0) break;
        if (!t.empty() && t.back() == ',') t.pop_back();
        if (t.rfind("{", 0) == 0) out.push_back(t);
    }
    return out;
}

std::vector<std::string> fieldRefs(const std::string& doc) {
    std::vector<std::string> out;
    for (const auto& r : relation(doc, "references"))
        if (r.find("\"kind\": \"field\"") != std::string::npos) out.push_back(r);
    return out;
}

bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// Counter.cajeta reads or writes `v` on four lines (5, 8 twice, 11).
fs::path makeCounterProject(const std::string& tag) {
    auto root = freshTempDir(tag);
    writeUnit(root, "demo/Counter.cajeta",
        "package demo;\n"                          // 1
        "public class Counter {\n"                 // 2
        "    int32 v;\n"                           // 3
        "    public Counter() {\n"                 // 4
        "        this.v = 0;\n"                    // 5   write
        "    }\n"                                  // 6
        "    public void bump() {\n"               // 7
        "        this.v = this.v + 1;\n"           // 8   write + read
        "    }\n"                                  // 9
        "    public int32 value() {\n"             // 10
        "        return this.v;\n"                 // 11  read
        "    }\n"                                  // 12
        "}\n");                                    // 13
    return root;
}

} // namespace

// ── 3.1.1 — the file that emitted nothing but declarations ────────────────

TEST(XrefLintFieldRef, EveryFieldAccessInAFileGetsAReference) {
    if (!haveCompiler()) GTEST_SKIP() << "compiler binary not built";
    auto root = makeCounterProject("counter");
    const std::string doc = lintRoot(root);
    ASSERT_FALSE(doc.empty()) << "the lint export wrote nothing";

    const auto fields = fieldRefs(doc);
    ASSERT_FALSE(fields.empty())
        << "Counter.cajeta reads and writes `v` on four lines and the lint "
           "export carries no field reference at all";

    // Four accesses: line 5 (write), line 8 (write AND read), line 11 (read).
    int atLine5 = 0, atLine8 = 0, atLine11 = 0;
    for (const auto& r : fields) {
        if (!has(r, "demo.Counter.v")) continue;
        if (has(r, "\"line\": 5"))  ++atLine5;
        if (has(r, "\"line\": 8"))  ++atLine8;
        if (has(r, "\"line\": 11")) ++atLine11;
    }
    EXPECT_GE(atLine5, 1)  << "no field reference for the write on line 5";
    EXPECT_GE(atLine8, 2)  << "line 8 writes AND reads `v`; got " << atLine8
                           << " reference(s)";
    EXPECT_GE(atLine11, 1) << "no field reference for the read on line 11";
}

// The reference names the field, and sits at the identifier — not at the
// receiver, and not at the statement. A misattributed position sends
// "who uses this" to the wrong line, which is worse than a missing edge
// (spec 2.1.2).
TEST(XrefLintFieldRef, AReferenceNamesTheFieldAndSitsAtItsOwnIdentifier) {
    if (!haveCompiler()) GTEST_SKIP() << "compiler binary not built";
    auto root = makeCounterProject("position");
    const auto fields = fieldRefs(lintRoot(root));
    ASSERT_FALSE(fields.empty()) << "no field references to position-check";

    // Columns are 0-BASED in this schema — the declaration of `v` on
    // "    int32 v;" reports col 10, and 10 is its 0-based offset. So the
    // reference on "        this.v = 0;" must report 13 (the `v`), NOT 12
    // (the dot) and not 8 (the receiver). Asserted numerically: a substring
    // check for `"col": 1` also matches 13, which is how the first draft of
    // this test managed to fail against a correct position.
    auto columnOf = [](const std::string& rec) -> int {
        auto at = rec.find("\"col\": ");
        if (at == std::string::npos) return -1;
        return std::atoi(rec.c_str() + at + 7);
    };

    bool sawLine5 = false;
    for (const auto& r : fields) {
        if (!has(r, "demo.Counter.v")) continue;
        EXPECT_TRUE(has(r, "Counter.cajeta"))
            << "field reference attributed to the wrong file: " << r;
        if (has(r, "\"line\": 5")) {
            sawLine5 = true;
            EXPECT_EQ(columnOf(r), 13)
                << "the reference does not sit on the identifier `v` (13 is "
                   "its 0-based column; 12 would be the dot, 8 the receiver): "
                << r;
        }
    }
    EXPECT_TRUE(sawLine5)
        << "no field reference on line 5, where `this.v` is written";
}

// ── 3.1.2 — an inherited field resolves to its DECLARING ancestor ─────────

TEST(XrefLintFieldRef, AnInheritedFieldResolvesToTheAncestorThatDeclaresIt) {
    if (!haveCompiler()) GTEST_SKIP() << "compiler binary not built";
    auto root = makeCounterProject("inherited");
    writeUnit(root, "demo/Doubler.cajeta",
        "package demo;\n"
        "public class Doubler extends Counter {\n"
        "    public void twice() {\n"
        "        this.v = this.v + 2;\n"       // v is declared on Counter
        "    }\n"
        "}\n");

    const auto fields = fieldRefs(lintRoot(root));
    ASSERT_FALSE(fields.empty()) << "no field references emitted";

    bool sawFromDoubler = false;
    for (const auto& r : fields) {
        if (!has(r, "Doubler.cajeta")) continue;
        sawFromDoubler = true;
        EXPECT_TRUE(has(r, "demo.Counter.v"))
            << "an inherited field resolved to the RECEIVER's class rather "
               "than the ancestor that declares it — Ctrl-click would open "
               "the wrong type: " << r;
    }
    EXPECT_TRUE(sawFromDoubler)
        << "Doubler.twice reads and writes an inherited field and produced no "
           "reference";
}

// ── 3.1.3 — one bad body does not sink the file's other records ───────────

TEST(XrefLintFieldRef, AnUnresolvableBodyDoesNotSinkTheOtherRecords) {
    if (!haveCompiler()) GTEST_SKIP() << "compiler binary not built";
    auto root = freshTempDir("badbody");
    writeUnit(root, "demo/Mixed.cajeta",
        "package demo;\n"                           // 1
        "public class Mixed {\n"                    // 2
        "    int32 good;\n"                         // 3
        "    public void fine() {\n"                // 4
        "        this.good = 1;\n"                  // 5  resolvable
        "    }\n"                                   // 6
        "    public void broken() {\n"              // 7
        "        this.good = nosuchthing.at.all;\n" // 8  will not resolve
        "    }\n"                                   // 9
        "}\n");                                     // 10

    const std::string doc = lintRoot(root);
    ASSERT_FALSE(doc.empty())
        << "a single unresolvable body sank the whole export";

    // The declarations survive...
    EXPECT_TRUE(has(doc, "\"demo.Mixed\""))
        << "the class declaration was lost to one bad method body";
    // ...and so does the good method's field reference.
    bool sawGood = false;
    for (const auto& r : fieldRefs(doc))
        if (has(r, "demo.Mixed.good") && has(r, "\"line\": 5")) sawGood = true;
    EXPECT_TRUE(sawGood)
        << "the resolvable method's field reference was lost because a "
           "SIBLING method's body could not resolve — body resolution is "
           "supposed to be per-method best-effort";
}

// ── 3.1.4 — a reference whose target is not declared is omitted ───────────

TEST(XrefLintFieldRef, AnUnresolvedFieldIsOmittedRatherThanDangling) {
    if (!haveCompiler()) GTEST_SKIP() << "compiler binary not built";
    auto root = freshTempDir("dangling");
    writeUnit(root, "demo/Ghost.cajeta",
        "package demo;\n"
        "public class Ghost {\n"
        "    int32 real;\n"
        "    public void touch() {\n"
        "        this.real = 1;\n"
        "    }\n"
        "}\n");

    const std::string doc = lintRoot(root);
    ASSERT_FALSE(doc.empty());

    // Every field reference must name a target the export also DECLARES.
    // A dangling edge navigates nowhere and is worse than no edge (spec
    // 2.1.2); the existing prune is what must still cover these.
    const auto decls = relation(doc, "declarations");
    for (const auto& r : fieldRefs(doc)) {
        auto t = r.find("\"target\": \"");
        if (t == std::string::npos) continue;
        auto s = t + 11;
        auto e = r.find('"', s);
        const std::string target = r.substr(s, e - s);
        bool declared = false;
        for (const auto& d : decls)
            if (has(d, "\"fqn\": \"" + target + "\"")) { declared = true; break; }
        EXPECT_TRUE(declared)
            << "field reference points at an undeclared target (dangling): "
            << target;
    }
}
