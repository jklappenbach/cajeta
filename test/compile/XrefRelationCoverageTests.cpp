// xref-lint-emission-gap Unit 1 — the check that would have caught it.
//
// The IDE consumes ONE export path: `--lint`. `ide-symbol-index` 2.3.4 recorded
// 2087 call edges and 3757 field references and read as delivered — but those
// counts came from a full-build export, which the plugin never runs. On the
// lint path both relations are EMPTY, and have always been: Ctrl-click on a
// method or a field has never worked in any project (spec §1.1, §1.5).
//
// So these are not tests of a feature. They are the acceptance that was
// missing: a relation the IDE reads must not be empty, and the two export
// paths must not disagree. Both are RED until Units 2-4 land, and that is the
// point — 1.3.1 requires them red first, naming the actual relation.
//
// Driven off a REAL export rather than a fixture (plan 1.2.1): a fixture
// cannot go empty, so a fixture could never have caught this.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

#ifdef _WIN32
#  define CAJETA_XCOV_DEVNULL "NUL"
#else
#  define CAJETA_XCOV_DEVNULL "/dev/null"
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
              / ("cajeta_xcov_" + tag + "_" + std::to_string(rng()));
    fs::create_directories(base);
    return base;
}

void writeUnit(const fs::path& root, const std::string& rel,
               const std::string& text) {
    auto file = root / rel;
    fs::create_directories(file.parent_path());
    std::ofstream out(file);
    out << text;
}

// A project whose every method is reachable from the entry point, so a build
// export and a lint export cover the same code. Each relation the plugin reads
// is present by construction: a field read AND write through `this`, a field
// read through another receiver, calls on a receiver, an override, and an
// inheritance edge.
struct Corpus {
    fs::path root;      // source root
    fs::path build;     // build output
    fs::path counter;   // demo/Counter.cajeta — the single-file lint target
};

Corpus makeCorpus(const std::string& tag) {
    auto base = freshTempDir(tag);
    auto src = base / "src";
    auto build = base / "build";
    fs::create_directories(src);
    fs::create_directories(build);

    writeUnit(src, "demo/Counter.cajeta",
        "package demo;\n"                                   // 1
        "public class Counter {\n"                          // 2
        "    int32 v;\n"                                    // 3  field decl
        "    public Counter() {\n"                          // 4
        "        this.v = 0;\n"                             // 5  field write
        "    }\n"                                           // 6
        "    public void bump() {\n"                        // 7
        "        this.v = this.v + 1;\n"                    // 8  write + read
        "    }\n"                                           // 9
        "    public int32 value() {\n"                      // 10
        "        return this.v;\n"                          // 11 field read
        "    }\n"                                           // 12
        "}\n");                                             // 13

    writeUnit(src, "demo/Doubler.cajeta",
        "package demo;\n"
        "public class Doubler extends Counter {\n"          // inheritance
        "    public void bump() {\n"                        // override
        "        super.bump();\n"
        "        super.bump();\n"
        "    }\n"
        "}\n");

    writeUnit(src, "demo/Main.cajeta",
        "package demo;\n"                                   // 1
        "public class Main {\n"                             // 2
        "    public static int32 run() {\n"                 // 3
        "        Counter c = heap Counter();\n"             // 4
        "        c.bump();\n"                               // 5  call edge
        "        Doubler d = heap Doubler();\n"             // 6
        "        d.bump();\n"                               // 7  call edge
        "        return c.value() + d.value();\n"           // 8  two call edges
        "    }\n"                                           // 9
        "}\n");                                             // 10

    return Corpus{src, build, src / "demo" / "Counter.cajeta"};
}

bool haveCompiler() { return fs::exists(compilerBinary()); }

std::string slurp(const fs::path& p) {
    std::ifstream f(p);
    if (!f) return "";
    return std::string((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
}

int runQuiet(const std::string& cmd) {
    return std::system((cmd + " > " CAJETA_XCOV_DEVNULL " 2>&1").c_str());
}

// Whole-root lint export — the path the IDE's cold index runs.
std::string lintExport(const Corpus& c, const std::string& extra = "") {
    auto out = freshTempDir("lintdoc") / "xref.json";
    runQuiet(compilerBinary() + " --lint " + c.root.string()
             + " --emit-xref=" + out.string() + " --diag-format=json " + extra);
    return slurp(out);
}

// Single-file lint — the path the IDE runs PER EDIT. Its records ride the
// NDJSON diagnostic stream on stderr as {"kind":"xref","rel":...,"record":...};
// `--emit-xref=<path>` is a directory-only form and writes nothing here, which
// is why this reads the stream the plugin's annotator actually reads.
std::string lintFileStream(const Corpus& c, const fs::path& file) {
    auto errFile = freshTempDir("lintfile") / "stderr.txt";
    std::string cmd = compilerBinary() + " --lint " + file.string()
                    + " --source-root " + c.root.string()
                    + " --emit-xref --diag-format=json"
                    + " > " CAJETA_XCOV_DEVNULL " 2> " + errFile.string();
    (void) std::system(cmd.c_str());
    return slurp(errFile);
}

// The `record` payloads of one relation, from the NDJSON stream.
std::vector<std::string> streamRelation(const std::string& stream,
                                        const std::string& rel) {
    std::vector<std::string> out;
    const std::string tag = "\"rel\":\"" + rel + "\"";
    std::istringstream in(stream);
    for (std::string line; std::getline(in, line); ) {
        if (line.find("\"kind\":\"xref\"") == std::string::npos) continue;
        if (line.find(tag) == std::string::npos) continue;
        auto at = line.find("\"record\":");
        if (at == std::string::npos) continue;
        std::string rec = line.substr(at + 9);
        if (!rec.empty() && rec.back() == '}') rec.pop_back();   // outer brace
        out.push_back(rec);
    }
    return out;
}

// Full-build export — where the counts in ide-symbol-index 2.3.4 came from.
std::string buildExport(const Corpus& c) {
    auto out = freshTempDir("builddoc") / "xref.json";
    runQuiet(compilerBinary() + " --emit-xref=" + out.string()
             + " --emit=ir demo.Main.run "
             + c.root.string() + " " + c.build.string());
    return slurp(out);
}

// The writer emits one record per line inside each relation's array (its own
// determinism pin, XrefExport 1.1.7, keeps that stable), so the records can be
// read without dragging in a JSON parser. Returns the trimmed record lines of
// the named relation.
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
        if (t.rfind("]", 0) == 0) break;          // end of this array
        if (!t.empty() && t.back() == ',') t.pop_back();
        if (t.rfind("{", 0) == 0) out.push_back(t);
    }
    return out;
}

// References carrying `"kind": "<k>"`.
std::vector<std::string> referencesOfKind(const std::string& doc,
                                          const std::string& k) {
    std::vector<std::string> out;
    const std::string needle = "\"kind\": \"" + k + "\"";
    for (const auto& r : relation(doc, "references"))
        if (r.find(needle) != std::string::npos) out.push_back(r);
    return out;
}

} // namespace

// ── 1.1.1 — every relation the plugin reads is non-empty ──────────────────
//
// Named individually rather than as one aggregate assertion: the failure must
// say WHICH relation is empty, because "the export is wrong" is what everybody
// already believed and nobody could act on.
TEST(XrefRelationCoverage, LintExportCarriesEveryRelationThePluginReads) {
    if (!haveCompiler()) GTEST_SKIP() << "compiler binary not built";
    auto c = makeCorpus("coverage");
    const std::string doc = lintExport(c);
    ASSERT_FALSE(doc.empty()) << "the lint export wrote nothing at all";

    EXPECT_FALSE(relation(doc, "declarations").empty())
        << "lint export carries no DECLARATIONS";
    EXPECT_FALSE(relation(doc, "inheritance").empty())
        << "lint export carries no INHERITANCE edges";
    EXPECT_FALSE(relation(doc, "overrides").empty())
        << "lint export carries no OVERRIDES";
    EXPECT_FALSE(referencesOfKind(doc, "type").empty())
        << "lint export carries no TYPE references";

    // The two the IDE has never had. Ctrl-click on a method needs `calls`;
    // Ctrl-click on a field needs a `field` reference (spec §3.1.1, §3.1.2).
    EXPECT_FALSE(relation(doc, "calls").empty())
        << "lint export carries no CALL edges — Ctrl-click on a method cannot "
           "resolve. The corpus calls Counter.bump, Counter.value and "
           "Doubler.bump from demo.Main.run.";
    EXPECT_FALSE(referencesOfKind(doc, "field").empty())
        << "lint export carries no FIELD references — Ctrl-click on a field "
           "cannot resolve. The corpus reads and writes Counter.v on four "
           "lines of Counter.cajeta alone.";
}

// ── 1.1.2 — the two export paths agree ────────────────────────────────────
//
// Directional on purpose: a build export only covers what its entry point
// reaches, so lint may legitimately carry MORE (unreachable code the IDE still
// indexes). What must never happen is lint carrying LESS — that is exactly
// today's defect. Every edge the build found must appear in the lint export,
// at the same position (spec 2.1.1, 2.1.2).
TEST(XrefRelationCoverage, LintCarriesEveryCallAndFieldEdgeTheBuildFound) {
    if (!haveCompiler()) GTEST_SKIP() << "compiler binary not built";
    auto c = makeCorpus("agreement");
    const std::string built = buildExport(c);
    const std::string linted = lintExport(c);
    ASSERT_FALSE(built.empty()) << "the build export wrote nothing";
    ASSERT_FALSE(linted.empty()) << "the lint export wrote nothing";

    // Spec 2.1.1 is about "the same SOURCE ROOT". A build export additionally
    // carries records for the stdlib code it compiled; lint deliberately does
    // not resolve stdlib bodies — the same policy captureStaticReceivers
    // already applies ("the project's own files are what a developer
    // navigates"), and resolving the whole stdlib per edit is not a cost the
    // per-edit path can take. So the comparison is scoped to the root's own
    // files, which is what the requirement actually says.
    auto ownFilesOnly = [](const std::vector<std::string>& recs) {
        std::vector<std::string> out;
        for (const auto& r : recs)
            if (r.find("\"file\": \"demo/") != std::string::npos) out.push_back(r);
        return out;
    };

    // The build side must itself be non-empty, or the comparison proves
    // nothing (1.3.2: neither check may pass against an empty export).
    ASSERT_FALSE(ownFilesOnly(relation(built, "calls")).empty())
        << "the BUILD export carries no calls — the corpus, not the lint "
           "path, is what is wrong";
    ASSERT_FALSE(ownFilesOnly(referencesOfKind(built, "field")).empty())
        << "the BUILD export carries no field references — the corpus, not "
           "the lint path, is what is wrong";

    auto missingFrom = [](const std::vector<std::string>& want,
                          const std::vector<std::string>& have)
        -> std::vector<std::string> {
        std::set<std::string> h(have.begin(), have.end());
        std::vector<std::string> gone;
        for (const auto& w : want) if (!h.count(w)) gone.push_back(w);
        return gone;
    };

    const auto callsGone = missingFrom(ownFilesOnly(relation(built, "calls")),
                                       relation(linted, "calls"));
    EXPECT_TRUE(callsGone.empty())
        << callsGone.size() << " call edge(s) the build found are absent from "
           "the lint export; first: " << (callsGone.empty() ? "" : callsGone[0]);

    const auto fieldsGone =
        missingFrom(ownFilesOnly(referencesOfKind(built, "field")),
                    referencesOfKind(linted, "field"));
    EXPECT_TRUE(fieldsGone.empty())
        << fieldsGone.size() << " field reference(s) the build found are absent "
           "from the lint export; first: "
        << (fieldsGone.empty() ? "" : fieldsGone[0]);
}

// ── 1.1.3 — a per-edit shard carries what the whole root carries ──────────
//
// The plugin overwrites a file's records with the shard from its per-edit
// lint. A shard carrying less than the whole-root export it replaces silently
// deletes edges from a live index (spec 2.1.4, 2.2.6).
TEST(XrefRelationCoverage, PerFileLintShardCarriesThatFilesCallsAndFields) {
    if (!haveCompiler()) GTEST_SKIP() << "compiler binary not built";
    auto c = makeCorpus("shard");
    const std::string shard = lintFileStream(c, c.counter);
    ASSERT_FALSE(streamRelation(shard, "declarations").empty())
        << "the per-edit lint stream carried no xref records at all";

    // Counter.cajeta reads and writes v on four lines; a shard carrying no
    // field reference deletes them from a live index when it overwrites that
    // file's records.
    std::vector<std::string> fields;
    for (const auto& r : streamRelation(shard, "references"))
        if (r.find("\"kind\": \"field\"") != std::string::npos) fields.push_back(r);
    EXPECT_FALSE(fields.empty())
        << "the per-edit shard for Counter.cajeta carries no field references";

    // Counter.value() has no calls of its own, so `calls` is legitimately
    // empty here — Main.cajeta is where the call edges live. Every record the
    // shard does carry must belong to its own file, or it would overwrite a
    // neighbour's records.
    for (const auto& r : fields) {
        EXPECT_NE(r.find("Counter.cajeta"), std::string::npos)
            << "a per-edit shard carried a record for another file: " << r;
    }
}
