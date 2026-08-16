// lint-server plan Unit 1 (specs/lint-server-spec.md §3): in-process lint
// reuse parity. Prove that lint → restoreBaseline → lint in ONE process
// produces byte-for-byte what a fresh `cajeta --lint` subprocess produces,
// under the lint-specific state the JIT reuse tests never touch (prescan,
// registerLintContext siblings, --shadow, lazy stdlib imports, xref stream).
//
// The oracle is the built compiler binary (constraint 1.4.2: the one-shot
// path is the reference implementation); the subject is
// cajeta::lintservice::warmLint, the factored prime/restore + driver surface
// the --lint-server loop (Unit 2) will sit on.

#include <gtest/gtest.h>

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/LintService.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#  include <io.h>
#  define CAJETA_DUP _dup
#  define CAJETA_DUP2 _dup2
#  define CAJETA_CLOSE _close
#  define CAJETA_FILENO _fileno
#  define CAJETA_LINT_DEVNULL "NUL"
#else
#  include <unistd.h>
#  define CAJETA_DUP dup
#  define CAJETA_DUP2 dup2
#  define CAJETA_CLOSE close
#  define CAJETA_FILENO fileno
#  define CAJETA_LINT_DEVNULL "/dev/null"
#endif

namespace fs = std::filesystem;

namespace {

std::string compilerBinary() {
    const char* envRoot = std::getenv("CAJETA_SOURCE_ROOT");
    std::string r;
    if (envRoot && *envRoot) {
        r = envRoot;
    } else {
#ifdef CAJETA_SOURCE_ROOT_DEFAULT
        r = CAJETA_SOURCE_ROOT_DEFAULT;
#else
        r = ".";
#endif
    }
#ifdef _WIN32
    std::string p = r + "/build/src/cajeta.exe";
    std::replace(p.begin(), p.end(), '/', '\\');
    return p;
#else
    return r + "/build/src/cajeta";
#endif
}

fs::path freshTempDir(const std::string& tag) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = fs::temp_directory_path()
              / ("cajeta_lintreuse_" + tag + "_" + std::to_string(rng()));
    fs::create_directories(base);
    return base;
}

// Write <root>/demo/<Name>.cajeta with the given class body (placed inside
// `package demo;`); returns the file path.
fs::path writeUnit(const fs::path& root, const std::string& name,
                   const std::string& classBody) {
    auto dir = root / "demo";
    fs::create_directories(dir);
    auto file = dir / (name + ".cajeta");
    std::ofstream out(file);
    out << "package demo;\n" << classBody << "\n";
    out.close();
    return file;
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// The fresh-process oracle: run `cajeta --lint <file> <flags>`, return its
// stderr (diagnostics + xref stream — everything parity is judged on) and
// exit code. rc == -1 means the binary isn't built (tests skip).
struct OracleRun {
    int rc = -1;
    std::string err;
};

OracleRun oracleLint(const fs::path& file, const std::string& flags) {
    OracleRun r;
    auto bin = compilerBinary();
    if (!fs::exists(bin)) return r;
    auto errFile = freshTempDir("oracle") / "stderr.txt";
    std::string cmd = bin + " --lint " + file.string() + " " + flags
                    + " > " CAJETA_LINT_DEVNULL " 2> " + errFile.string();
    r.rc = std::system(cmd.c_str());
    r.err = readFile(errFile);
    return r;
}

// The warm in-process subject: run one warmLint request with stderr
// redirected to a file, return exit code + captured bytes. The redirect is
// at the fd level, so it covers both the CRT stderr FILE and std::cerr
// (both sink to fd 2). rc stays -1 if the capture file couldn't be opened.
struct WarmRun {
    int rc = -1;
    std::string err;
};

WarmRun runWarm(const fs::path& file, const std::string& sourceRoot = "",
                const std::string& shadow = "", bool emitXref = false,
                const std::vector<std::string>& classpath = {}) {
    WarmRun r;
    auto errFile = freshTempDir("warm") / "stderr.txt";

    std::fflush(stderr);
    std::cerr.flush();
    int savedFd = CAJETA_DUP(CAJETA_FILENO(stderr));
    FILE* redirect = std::fopen(errFile.string().c_str(), "wb");
    if (!redirect) {
        CAJETA_CLOSE(savedFd);
        return r;
    }
    CAJETA_DUP2(CAJETA_FILENO(redirect), CAJETA_FILENO(stderr));

    cajeta::lintservice::LintRequest req;
    req.file = file.string();
    req.sourceRoot = sourceRoot;
    req.shadow = shadow;
    req.jsonDiagnostics = true;
    req.emitXref = emitXref;
    req.classpath = classpath;
    r.rc = cajeta::lintservice::warmLint(req);

    std::fflush(stderr);
    std::cerr.flush();
    CAJETA_DUP2(savedFd, CAJETA_FILENO(stderr));
    CAJETA_CLOSE(savedFd);
    std::fclose(redirect);

    r.err = readFile(errFile);
    return r;
}

// Like runWarm, but through the SERVED path (sibling-context reuse), so a
// caller can drive cold-then-warm-hit against one persistent context — the
// shape the `--lint-server` loop actually runs.
WarmRun runServed(const fs::path& file, const std::string& sourceRoot,
                  cajeta::lintservice::SiblingContext& ctx,
                  const std::vector<std::string>& classpath = {},
                  bool emitXref = false) {
    WarmRun r;
    auto errFile = freshTempDir("served") / "stderr.txt";

    std::fflush(stderr);
    std::cerr.flush();
    int savedFd = CAJETA_DUP(CAJETA_FILENO(stderr));
    FILE* redirect = std::fopen(errFile.string().c_str(), "wb");
    if (!redirect) {
        CAJETA_CLOSE(savedFd);
        return r;
    }
    CAJETA_DUP2(CAJETA_FILENO(redirect), CAJETA_FILENO(stderr));

    cajeta::lintservice::LintRequest req;
    req.file = file.string();
    req.sourceRoot = sourceRoot;
    req.jsonDiagnostics = true;
    req.emitXref = emitXref;
    req.classpath = classpath;
    r.rc = cajeta::lintservice::warmLintServed(req, ctx).rc;

    std::fflush(stderr);
    std::cerr.flush();
    CAJETA_DUP2(savedFd, CAJETA_FILENO(stderr));
    CAJETA_CLOSE(savedFd);
    std::fclose(redirect);

    r.err = readFile(errFile);
    return r;
}

// Build a one-class library archive and return its `.cja` path (empty on
// failure). The dependency half of the classpath fixtures below. With
// [withDi] the library also carries a DI graph — one `@Component` provider of
// an interface and one `@Component` consumer that `@Inject`s it — the shape
// that exposes a DOUBLE ingest: registering the same provider twice reads as
// two candidate providers and the injection point turns ambiguous.
fs::path buildDepArchive(const std::string& tag, bool withDi = false) {
    auto lib = freshTempDir(tag);
    auto src = lib / "src";
    fs::create_directories(src / "dep");
    {
        std::ofstream out(src / "dep" / "DepType.cajeta");
        out << "package dep;\n"
               "public class DepType {\n"
               "    public int32 answer() { return 42; }\n"
               "}\n";
    }
    if (withDi) {
        std::ofstream out(src / "dep" / "Wiring.cajeta");
        out << "package dep;\n"
               "public interface Sink {\n"
               "    public void write();\n"
               "}\n"
               "@Component\n"
               "public final class ConsoleSink implements Sink {\n"
               "    public ConsoleSink() { }\n"
               "    public void write() { }\n"
               "}\n"
               "@Component\n"
               "public final class Pipeline {\n"
               "    @Inject Sink sink;\n"
               "    public Pipeline() { }\n"
               "}\n";
    }
    auto arc = lib / "arc";
    fs::create_directories(arc);
    std::string cmd = compilerBinary() + " dep.DepType " + src.string() + " "
                    + arc.string() + " --emit=cja > " CAJETA_LINT_DEVNULL " 2>&1";
    if (std::system(cmd.c_str()) != 0) return {};
    for (auto& e : fs::directory_iterator(arc))
        if (e.path().extension() == ".cja") return e.path();
    return {};
}

const char* HEALTHY_ALPHA =
    "public final class Alpha {\n"
    "    public static void main() { int32 x = 1; }\n"
    "}";

const char* ERROR_ALPHA =
    "public final class Alpha {\n"
    "    public static void main() { NoSuchType z = NoSuchType.create(); }\n"
    "}";

const char* HEALTHY_BETA =
    "public final class Beta {\n"
    "    public static void main() { int32 y = 2; }\n"
    "}";

const char* SIBLING =
    "public final class Sibling {\n"
    "    public static Sibling make() { return null; }\n"
    "    public static int32 add(int32 a, int32 b) { return a + b; }\n"
    "}";

#define SKIP_WITHOUT_BINARY() \
    if (!fs::exists(compilerBinary())) \
        GTEST_SKIP() << "compiler binary unavailable"

} // namespace

// 1.1.1 — lint A, restore, lint A again in one process: both runs' output
// identical to a fresh process's.

// 1.1.1 (error shape) — the same holds when the file has semantic errors.

// 1.1.2 — lint A (with errors), restore, lint healthy B: no leakage of A's
// diagnostics, types, or placeholders into B's run.

// 1.1.3 — with --emit-xref: the second run's xref stream is byte-identical
// to a fresh process's (records, order, version line).
TEST(LintReuse, XrefStreamByteIdenticalAfterRestore) {
    SKIP_WITHOUT_BINARY();
    auto root = freshTempDir("xref") / "src";
    writeUnit(root, "Sibling", SIBLING);
    auto file = writeUnit(root, "Target",
        "public final class Target {\n"
        "    public static void main() { int32 x = Sibling.add(1, 2); }\n"
        "}");

    std::string flags = "--diag-format=json --emit-xref --source-root " + root.string();
    auto oracle = oracleLint(file, flags);
    auto warm1 = runWarm(file, root.string(), "", /*emitXref=*/true);
    auto warm2 = runWarm(file, root.string(), "", /*emitXref=*/true);

    EXPECT_EQ(warm1.err, oracle.err) << "first warm xref stream diverges";
    EXPECT_EQ(warm2.err, oracle.err) << "second warm xref stream diverges";
}

// 1.1.4 — a syntax-broken buffer, restored, then the healthy twin: the
// healthy twin's output is unaffected by the broken run before it.

// 1.1.5 — lazy stdlib: first lint imports cajeta.math; after restore, a lint
// NOT importing it resolves as fresh (no ghost package), and a lint importing
// it again re-parses it correctly.
TEST(LintReuse, LazyStdlibPackageDoesNotGhostAcrossRestore) {
    SKIP_WITHOUT_BINARY();
    auto rootImport = freshTempDir("mathimp") / "src";
    auto importer = writeUnit(rootImport, "MathUser",
        "import cajeta.math.MathInfo;\n"
        "public final class MathUser {\n"
        "    public static void main() { int32 v = MathInfo.version(); }\n"
        "}");
    auto rootBare = freshTempDir("mathbare") / "src";
    auto bare = writeUnit(rootBare, "MathFree",
        "public final class MathFree {\n"
        "    public static void main() { int32 v = MathInfo.version(); }\n"
        "}");

    auto oracleImporter = oracleLint(importer, "--diag-format=json");
    auto oracleBare = oracleLint(bare, "--diag-format=json");

    // Run 1: imports cajeta.math — parses the lazy package into the warm stdlib.
    auto warmImport1 = runWarm(importer);
    EXPECT_EQ(warmImport1.err, oracleImporter.err)
        << "math-importing warm run diverges from one-shot";

    // Run 2: no import. The per-request restore must roll the lazy package
    // back — the un-imported reference resolves (or fails) exactly as a
    // fresh process, with no ghost of run 1's parse.
    auto warmBare = runWarm(bare);
    EXPECT_EQ(warmBare.rc == 0, oracleBare.rc == 0)
        << "ghost cajeta.math changed the un-imported run's outcome";
    EXPECT_EQ(warmBare.err, oracleBare.err)
        << "un-imported reference after restore diverges from one-shot";

    // Run 3: imports again — the package re-parses cleanly, same as fresh.
    auto warmImport2 = runWarm(importer);
    EXPECT_EQ(warmImport2.err, oracleImporter.err)
        << "re-imported cajeta.math after restore diverges from one-shot";
}

// 1.1.6 — --source-root context in consecutive runs: sibling signatures
// resolve after restore identically to fresh.

// A consumer that declares a field of the dependency's type. The reference
// resolves only if the `.cja` on the classpath was ingested; without it the
// lint reports CAJETA_ERROR_UNRESOLVED_TYPE.
const char* DEP_CONSUMER =
    "public final class Consumer {\n"
    "    dep.DepType d;\n"
    "    public static void main() { }\n"
    "}";

// The warm path must honor the classpath exactly as one-shot `--lint
// --classpath=...` does. Julian, 2026-07-31: `Logger` from the
// dev.cajeta.logging dep stayed red-underlined in CLion while one-shot lint
// of the same buffer was clean — the warm server never received the
// classpath, so every dependency type read as unresolved.

// The served path reuses a sibling context across requests. The second
// request is a warm HIT (nothing changed), and it must still resolve the
// dependency — a re-ingest against restored registries must neither drop the
// dep nor double-register it.

// The warm hit restores a context baseline that ALREADY carries the
// dependency's declarations. Ingesting the classpath again on top of it
// registers every dependency `@Component` a second time, and the dependency's
// own `@Inject` sites then see two candidate providers. Julian, 2026-07-31:
// the second lint of an unchanged tour buffer reported
// CAJETA_ERROR_DI_AMBIGUOUS for dev.cajeta.logging's Appender, which the first
// lint and the one-shot binary both resolved cleanly.
TEST(LintReuse, ClasspathDiProvidersAreNotDoubleRegisteredOnWarmHit) {
    SKIP_WITHOUT_BINARY();
    auto cja = buildDepArchive("dicp", /*withDi=*/true);
    ASSERT_FALSE(cja.empty()) << "dep .cja build failed";

    auto root = freshTempDir("dicpuser") / "src";
    writeUnit(root, "Sibling", SIBLING);
    auto target = writeUnit(root, "Consumer", DEP_CONSUMER);

    std::string flags = "--diag-format=json --source-root " + root.string()
                      + " --classpath=" + cja.string();
    auto oracle = oracleLint(target, flags);
    ASSERT_EQ(oracle.err.find("DI_AMBIGUOUS"), std::string::npos)
        << "fixture is ambiguous even one-shot; test would be vacuous:\n"
        << oracle.err;

    cajeta::lintservice::SiblingContext ctx;
    auto cold = runServed(target, root.string(), ctx, {cja.string()});
    auto hot  = runServed(target, root.string(), ctx, {cja.string()});

    EXPECT_EQ(cold.err, oracle.err) << "cold served run diverges from one-shot";
    EXPECT_EQ(hot.err, oracle.err)
        << "dependency providers double-registered on the warm hit:\n" << hot.err;
}

// 1.3.1 — parity across the existing lint fixture corpus. The entries
// mirror the fixture shapes LintModeTests / XrefLintTests drive through the
// one-shot binary (clean, syntax-broken, semantic error, unknown field
// type, sibling project, broken buffer under a root, annotation
// declaration) — the corpus, not hand-picked samples. Every entry is
// linted warm twice, interleaved across the whole corpus, so each input
// runs against state every other input has dirtied.
TEST(LintReuse, ParityAcrossLintFixtureCorpus) {
    SKIP_WITHOUT_BINARY();

    struct Entry {
        const char* tag;
        const char* unitName;      // class file under demo/
        const char* body;          // full class body (inside package demo;)
        bool withSibling;          // add the Helper sibling + --source-root
        bool emitXref;
    };
    const Entry corpus[] = {
        {"clean", "Clean",
         "public final class Clean {\n"
         "    public static void main() { int32 x = 1; }\n"
         "}", false, false},
        {"syntax", "Syn",
         "public final class Syn {\n"
         "    public static void main() { return 1 + ; }\n"
         "}", false, false},
        {"semantic", "Sem",
         "public final class Sem {\n"
         "    public static void main() { NoSuchType z = NoSuchType.create(); }\n"
         "}", false, false},
        {"unknown-field-type", "Bad",
         "public class Bad {\n"
         "    UnknownAbc u;\n"
         "}", false, true},
        {"sibling-project", "Target",
         "public class Target {\n"
         "    Helper aide;\n"
         "    public Target() {\n"
         "        this.aide = heap Helper();\n"
         "    }\n"
         "    public int32 answer() {\n"
         "        return 2;\n"
         "    }\n"
         "}", true, true},
        {"broken-under-root", "Target",
         "public class Target {\n"
         "    public int32 answer( {\n"
         "        return ;;;\n"
         "    }\n"
         "}", true, true},
        {"annotation", "Retry",
         "annotation Retry {\n"
         "    int32 attempts() default 3;\n"
         "}", true, true},
    };

    const char* HELPER =
        "public class Helper {\n"
        "    public int32 assist() {\n"
        "        return 40;\n"
        "    }\n"
        "}";

    struct Prepared {
        const Entry* entry;
        fs::path file;
        std::string root;
        std::string oracleErr;
    };
    std::vector<Prepared> prepared;
    for (const Entry& e : corpus) {
        auto root = freshTempDir(std::string("corpus_") + e.tag) / "src";
        if (e.withSibling) writeUnit(root, "Helper", HELPER);
        Prepared p;
        p.entry = &e;
        p.file = writeUnit(root, e.unitName, e.body);
        p.root = e.withSibling ? root.string() : std::string();
        std::string flags = "--diag-format=json";
        if (e.emitXref) flags += " --emit-xref";
        if (e.withSibling) flags += " --source-root " + p.root;
        p.oracleErr = oracleLint(p.file, flags).err;
        prepared.push_back(std::move(p));
    }

    for (int pass = 1; pass <= 2; ++pass) {
        for (const Prepared& p : prepared) {
            auto warm = runWarm(p.file, p.root, "", p.entry->emitXref);
            EXPECT_EQ(warm.err, p.oracleErr)
                << "corpus entry '" << p.entry->tag << "' diverged from the "
                << "one-shot oracle on warm pass " << pass;
        }
    }
}

// 1.1.6 (shadow) — a staged buffer (--shadow) replacing its on-disk twin
// behaves identically warm and fresh, across a restore.
