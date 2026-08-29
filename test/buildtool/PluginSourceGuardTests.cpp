// plugin-output-protocol plan §1.3.2 — no in-tree plugin source spells the
// wire format or writes to stdout.
//
// The defect the whole spec exists for was a plugin writing its own JSON:
//
//     System.stdout.println("{\"kind\":\"log\",\"level\":\"info\",\"message\":\""
//         + message + "\"}");
//
// with no escaping, so a quote, a backslash or a newline in `message` emitted
// malformed JSON — a Windows path or a quoted compiler diagnostic was enough.
// §1 replaced that with `PluginHost` reached through an `ActionContext`, but a
// replacement only holds if the old shape cannot come back. Nothing stops an
// author from writing that line again next week, and it would look fine in
// review: it is short, obvious, and works for every message without a special
// character in it.
//
// So this is a source scan rather than a behavioural test. There is no input
// that makes a hand-written emitter fail loudly enough to catch in a normal
// run — that is precisely why the original went unnoticed — and the property
// wanted here is "this code does not exist", which only a scan can assert.
//
// Modelled on PortableEnvGuardTests, including its negative arm: a scanner
// whose pattern silently matched nothing would report a clean tree forever.

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

    fs::path sourceRoot() {
        const char* env = std::getenv("CAJETA_SOURCE_ROOT");
        std::string r;
        if (env && *env) {
            r = env;
        } else {
#ifdef CAJETA_SOURCE_ROOT_DEFAULT
            r = CAJETA_SOURCE_ROOT_DEFAULT;
#else
            r = ".";
#endif
        }
        return fs::path(r);
    }

    // Every in-tree source that is, or ships as, plugin code.
    std::vector<fs::path> pluginSourceRoots() {
        const fs::path root = sourceRoot();
        return {
            root / "build-tools" / "plugins",
            root / "runtime" / "src" / "cajeta" / "buildtool" / "plugin",
        };
    }

    // Comments are stripped before scanning, so a docstring may quote the bad
    // pattern as the thing it replaced — `PluginHost`'s does — without being
    // reported. Scanning raw text would force a file-level exemption there,
    // and an exempt file is one the guard no longer covers.
    std::string stripComments(const std::string& src) {
        std::string out;
        out.reserve(src.size());
        enum { Code, Line, Block, Str } state = Code;
        for (size_t i = 0; i < src.size(); ++i) {
            const char c = src[i];
            const char n = (i + 1 < src.size()) ? src[i + 1] : '\0';
            switch (state) {
                case Code:
                    if (c == '/' && n == '/') { state = Line;  ++i; continue; }
                    if (c == '/' && n == '*') { state = Block; ++i; continue; }
                    if (c == '"') state = Str;
                    out += c;
                    continue;
                case Str:
                    // A backslash escapes the next byte, so an embedded \" does
                    // not end the literal — the emitter's own records are full
                    // of those.
                    if (c == '\\') { out += c; if (n) { out += n; ++i; } continue; }
                    if (c == '"') state = Code;
                    out += c;
                    continue;
                case Line:
                    if (c == '\n') { state = Code; out += c; }
                    continue;
                case Block:
                    if (c == '*' && n == '/') { state = Code; ++i; }
                    continue;
            }
        }
        return out;
    }

    struct Offence {
        std::string file;
        std::string why;
    };

    // A protocol record spelled by hand: `{"kind":"<a kind the protocol
    // defines>"`. Narrow on purpose — `cajeta.coverage`'s ExcludeParser has a
    // diagnostic reading `form { "kind": "file", ... }` describing its own
    // config schema, which is not the wire format and must not be reported.
    const std::regex& recordLiteralRe() {
        static const std::regex re(
            R"(\{\s*\\"kind\\"\s*:\s*\\"(log|warn|write|output|finding|result)\\")");
        return re;
    }

    // Any write to the process's stdout from plugin code.
    const std::regex& stdoutWriteRe() {
        static const std::regex re(
            R"((System\s*\.\s*stdout|System\s*\.\s*out\b|STDOUT_FILENO))");
        return re;
    }

    std::vector<fs::path> collectPluginSources(int& scanned) {
        std::vector<fs::path> files;
        for (const auto& root : pluginSourceRoots()) {
            if (!fs::is_directory(root)) continue;
            for (const auto& e : fs::recursive_directory_iterator(root)) {
                if (!e.is_regular_file()) continue;
                if (e.path().extension() != ".cajeta") continue;
                files.push_back(e.path());
                ++scanned;
            }
        }
        return files;
    }

}  // namespace

// ---- the guard ------------------------------------------------------------

TEST(PluginSourceGuardTests, noPluginSourceSpellsTheWireFormatOrWritesToStdout) {
    int scanned = 0;
    const auto files = collectPluginSources(scanned);

    // A scan that found nothing would pass forever. This is the check the
    // whole test rests on: a wrong path is the failure mode that makes a
    // source guard worthless.
    ASSERT_GT(scanned, 0)
        << "found no plugin sources under " << sourceRoot()
        << " — set CAJETA_SOURCE_ROOT";

    std::vector<Offence> offences;
    for (const auto& path : files) {
        std::ifstream in(path);
        std::stringstream ss;
        ss << in.rdbuf();
        const std::string code = stripComments(ss.str());
        const std::string rel = fs::relative(path, sourceRoot()).string();

        if (std::regex_search(code, recordLiteralRe())) {
            offences.push_back({rel, "spells a protocol record as a literal"});
        }

        // StdoutActionContext is the transport. Writing records to stdout is
        // its entire job, and it is the channel every other plugin source is
        // required to go through instead.
        if (path.filename() == "StdoutActionContext.cajeta") continue;

        if (std::regex_search(code, stdoutWriteRe())) {
            offences.push_back({rel, "writes to stdout instead of through ActionContext"});
        }
    }

    std::string joined;
    for (const auto& o : offences) joined += "\n  " + o.file + " — " + o.why;
    EXPECT_TRUE(offences.empty())
        << "plugin sources must emit through ActionContext, which escapes"
           " through PluginHost:" << joined
        << "\n\nFix: take an ActionContext and call log/warn/write/output/finding.";
}

// ---- the negative arm: the guard can actually fire -------------------------

TEST(PluginSourceGuardTests, theScanMatchesTheShapeItExistsToCatch) {
    // The exact line the spec was written about.
    const std::string bad =
        "System.stdout.println(\"{\\\"kind\\\":\\\"log\\\",\\\"level\\\":\\\"info\\\"}\");";
    EXPECT_TRUE(std::regex_search(bad, recordLiteralRe()));
    EXPECT_TRUE(std::regex_search(bad, stdoutWriteRe()));

    // Spacing a hand-written record differently must not evade it.
    const std::string spaced = "String r = \"{ \\\"kind\\\" : \\\"finding\\\" }\";";
    EXPECT_TRUE(std::regex_search(spaced, recordLiteralRe()));

    // And must NOT fire on what is legitimate.
    EXPECT_FALSE(std::regex_search(std::string("ctx.log(\"hello\");"), recordLiteralRe()));
    EXPECT_FALSE(std::regex_search(std::string("ctx.log(\"hello\");"), stdoutWriteRe()));
    // cajeta.coverage's exclude-entry diagnostic — a config schema, not a record.
    const std::string schema = "\"form { \\\"kind\\\": \\\"file\\\", \\\"pattern\\\": \\\"...\\\" }\"";
    EXPECT_FALSE(std::regex_search(schema, recordLiteralRe()));
}

TEST(PluginSourceGuardTests, commentStrippingKeepsCodeAndDropsDocumentation) {
    // The docstring case: PluginHost documents the defect it replaced.
    const std::string doc =
        "/**\n * System.stdout.println(\"{\\\"kind\\\":\\\"log\\\"}\");\n */\n"
        "public static int32 f() { return 0; }\n";
    const std::string stripped = stripComments(doc);
    EXPECT_EQ(stripped.find("stdout"), std::string::npos);
    EXPECT_NE(stripped.find("return 0;"), std::string::npos);

    // A line comment goes; the code beside it stays.
    EXPECT_EQ(stripComments("int32 a = 1; // System.stdout\n"), "int32 a = 1; \n");

    // A `//` inside a string literal is not a comment — stripping it would let
    // a URL or a path swallow the rest of a line of real code.
    EXPECT_EQ(stripComments("f(\"http://x\"); g();"), "f(\"http://x\"); g();");

    // An escaped quote does not end the literal, which is what every record
    // spelled by hand is made of.
    EXPECT_NE(stripComments("f(\"a\\\"b\"); // c\n").find("a\\\"b"), std::string::npos);
}
