// lint-server plan Unit 3 (specs/lint-server-spec.md §4): sibling-context
// reuse. The server keeps a project root's sibling signatures warm across
// requests, re-registering only when the on-disk set changes (mtime/size).
// The done record's "siblingsReparsed" reports the sweep each request took:
// 0 on a warm hit, the full count on a resweep (the coarse-invalidation
// model — any sibling change reswics the root).
//
// These require editing files BETWEEN requests within one server process, so
// they drive it interactively over a bidirectional pipe (POSIX only). The
// oracle for the parity check is the one-shot binary, as everywhere else.

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

#ifndef _WIN32
#  include <fcntl.h>
#  include <sys/wait.h>
#  include <unistd.h>

namespace {

std::string compilerBinary() {
    const char* envRoot = std::getenv("CAJETA_SOURCE_ROOT");
    std::string r;
    if (envRoot && *envRoot) r = envRoot;
    else {
#ifdef CAJETA_SOURCE_ROOT_DEFAULT
        r = CAJETA_SOURCE_ROOT_DEFAULT;
#else
        r = ".";
#endif
    }
    return r + "/build/src/cajeta";
}

fs::path freshTempDir(const std::string& tag) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = fs::temp_directory_path()
              / ("cajeta_lsib_" + tag + "_" + std::to_string(rng()));
    fs::create_directories(base);
    return base;
}

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
    std::stringstream ss; ss << in.rdbuf();
    return ss.str();
}

bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// An interactively-driven `cajeta --lint-server`: write a request, read the
// response up to that id's done marker, edit files, repeat. Fork + two pipes;
// the child's stdin/stdout are the pipes, stderr goes to /dev/null.
class InteractiveServer {
public:
    InteractiveServer(const std::string& sourceRoot, bool started) : started_(started) {
        if (!started_) return;
        int inPipe[2], outPipe[2];
        if (pipe(inPipe) != 0 || pipe(outPipe) != 0) { started_ = false; return; }
        pid_ = fork();
        if (pid_ < 0) { started_ = false; return; }
        if (pid_ == 0) {
            dup2(inPipe[0], STDIN_FILENO);
            dup2(outPipe[1], STDOUT_FILENO);
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) dup2(devnull, STDERR_FILENO);
            close(inPipe[0]); close(inPipe[1]);
            close(outPipe[0]); close(outPipe[1]);
            std::string bin = compilerBinary();
            std::string root = sourceRoot;
            std::vector<char*> argv = {
                const_cast<char*>(bin.c_str()),
                const_cast<char*>("--lint-server"),
                const_cast<char*>("--diag-format=json"),
                const_cast<char*>("--source-root"),
                const_cast<char*>(root.c_str()),
                nullptr };
            execv(bin.c_str(), argv.data());
            _exit(127);
        }
        close(inPipe[0]); close(outPipe[1]);
        inFd_ = inPipe[1];
        outFd_ = outPipe[0];
        // Consume the ready record so callers see only per-request output.
        ready_ = readLineBlocking();
    }

    ~InteractiveServer() {
        if (!started_) return;
        if (inFd_ >= 0) close(inFd_);
        if (outFd_ >= 0) close(outFd_);
        if (pid_ > 0) { int st; waitpid(pid_, &st, 0); }
    }

    bool ok() const { return started_ && pid_ > 0; }
    const std::string& ready() const { return ready_; }

    void send(const std::string& line) {
        std::string l = line;
        if (l.empty() || l.back() != '\n') l.push_back('\n');
        ssize_t n = write(inFd_, l.data(), l.size());
        (void) n;
    }

    // Read response lines until the done (or error) marker for `id`; return
    // the concatenated block (payload lines + the marker line).
    std::string readResponse(int id) {
        std::string block;
        const std::string donePrefix = "{\"kind\":\"done\",\"id\":" + std::to_string(id);
        const std::string errPrefix  = "{\"kind\":\"error\",\"id\":" + std::to_string(id);
        for (;;) {
            std::string line = readLineBlocking();
            if (line.empty() && eof_) break;
            block += line + "\n";
            if (line.rfind(donePrefix, 0) == 0 || line.rfind(errPrefix, 0) == 0)
                break;
        }
        return block;
    }

private:
    std::string readLineBlocking() {
        std::string line;
        for (;;) {
            auto nl = pending_.find('\n');
            if (nl != std::string::npos) {
                line = pending_.substr(0, nl);
                pending_.erase(0, nl + 1);
                return line;
            }
            char buf[4096];
            ssize_t n = read(outFd_, buf, sizeof buf);
            if (n <= 0) { eof_ = true; line = pending_; pending_.clear(); return line; }
            pending_.append(buf, n);
        }
    }

    bool started_ = false;
    bool eof_ = false;
    pid_t pid_ = -1;
    int inFd_ = -1, outFd_ = -1;
    std::string pending_, ready_;
};

int siblingsReparsed(const std::string& doneBlock) {
    // Last line is the done marker; parse "siblingsReparsed":<n>.
    auto pos = doneBlock.rfind("\"siblingsReparsed\":");
    if (pos == std::string::npos) return -1;
    pos += std::string("\"siblingsReparsed\":").size();
    return std::atoi(doneBlock.c_str() + pos);
}

std::string lintReq(int id, const fs::path& file, bool emitXref = false) {
    std::string r = "{\"kind\":\"lint\",\"id\":" + std::to_string(id)
                  + ",\"file\":\"" + file.string() + "\"";
    if (emitXref) r += ",\"emitXref\":true";
    return r + "}";
}

// The payload slice of a done block: everything before the final marker line.
std::string payloadOf(const std::string& doneBlock) {
    auto nl = doneBlock.rfind('\n', doneBlock.size() >= 2 ? doneBlock.size() - 2 : 0);
    // The block ends with "<payload...>\n<marker>\n"; strip the marker line.
    auto lastMarker = doneBlock.rfind("{\"kind\":\"done\"");
    if (lastMarker == std::string::npos) lastMarker = doneBlock.rfind("{\"kind\":\"error\"");
    if (lastMarker == std::string::npos) return doneBlock;
    (void) nl;
    return doneBlock.substr(0, lastMarker);
}

// One-shot oracle stderr for `cajeta --lint <target> --source-root <root>`
// (optionally --emit-xref) — the parity reference (spec 1.4.1 / §4).
std::string oneShotStderr(const fs::path& target, const fs::path& root,
                          bool emitXref) {
    auto bin = compilerBinary();
    auto errFile = freshTempDir("oracle") / "stderr.txt";
    std::string cmd = bin + " --lint " + target.string()
                    + " --source-root " + root.string()
                    + " --diag-format=json" + (emitXref ? " --emit-xref" : "")
                    + " > /dev/null 2> " + errFile.string();
    int rc = std::system(cmd.c_str());
    (void) rc;
    return readFile(errFile);
}

const char* SIBLING_ADD2 =
    "public final class Sibling {\n"
    "    public static int32 add(int32 a, int32 b) { return a + b; }\n"
    "}";

#define SKIP_WITHOUT_BINARY() \
    if (!fs::exists(compilerBinary())) GTEST_SKIP() << "compiler binary unavailable"

} // namespace

// 3.1.1 — a second request against an unchanged root re-registers no siblings.
TEST(LintServerSibling, UnchangedRootReparsesNoSiblings) {
    SKIP_WITHOUT_BINARY();
    auto root = freshTempDir("unchanged") / "src";
    writeUnit(root, "Sibling", SIBLING_ADD2);
    auto target = writeUnit(root, "Target",
        "public final class Target {\n"
        "    public static void main() { int32 x = Sibling.add(1, 2); }\n"
        "}");

    InteractiveServer srv(root.string(), true);
    ASSERT_TRUE(srv.ok());

    srv.send(lintReq(1, target));
    auto r1 = srv.readResponse(1);
    srv.send(lintReq(2, target));
    auto r2 = srv.readResponse(2);
    srv.send("{\"kind\":\"shutdown\"}");

    EXPECT_GT(siblingsReparsed(r1), 0) << "first request must sweep the siblings";
    EXPECT_EQ(siblingsReparsed(r2), 0)
        << "an unchanged root must re-register no siblings on the 2nd request";
}

// 3.1.2 — touching a sibling reparses (resweeps), and the change is visible to
// the next lint of a referencing file. Lint resolves TYPE references (not
// method-call arity, which is codegen-phase), so the observable change is a
// referenced type appearing/disappearing as the sibling's content changes.
TEST(LintServerSibling, TouchedSiblingResweepsAndChangeVisible) {
    SKIP_WITHOUT_BINARY();
    auto root = freshTempDir("touch") / "src";
    writeUnit(root, "Sibling", "public final class Sibling { }");
    // Target uses Sibling as a field type — a lint-resolved reference.
    auto target = writeUnit(root, "Target",
        "public final class Target {\n"
        "    Sibling aide;\n"
        "    public static void main() { }\n"
        "}");

    InteractiveServer srv(root.string(), true);
    ASSERT_TRUE(srv.ok());

    srv.send(lintReq(1, target));
    auto r1 = srv.readResponse(1);
    EXPECT_EQ(r1.find("\"severity\":\"error\""), std::string::npos)
        << "baseline target must resolve Sibling:\n" << r1;

    // Rewrite Sibling.cajeta to declare a DIFFERENT class — the `Sibling` type
    // disappears, so Target's field type no longer resolves.
    writeUnit(root, "Sibling", "public final class SiblingRenamed { }");

    srv.send(lintReq(2, target));
    auto r2 = srv.readResponse(2);
    srv.send("{\"kind\":\"shutdown\"}");

    EXPECT_GT(siblingsReparsed(r2), 0)
        << "a changed sibling must trigger a resweep";
    EXPECT_NE(r2.find("\"severity\":\"error\""), std::string::npos)
        << "the sibling change must be visible to the target:\n" << r2;
}

// 3.1.3 — an added sibling joins; a deleted sibling's signatures drop.
TEST(LintServerSibling, AddedSiblingJoinsDeletedSiblingDrops) {
    SKIP_WITHOUT_BINARY();
    auto root = freshTempDir("addel") / "src";
    writeUnit(root, "Sibling", SIBLING_ADD2);
    // Target references a Helper TYPE (field) that does not exist yet.
    auto target = writeUnit(root, "Target",
        "public final class Target {\n"
        "    Helper aide;\n"
        "    public static void main() { }\n"
        "}");

    InteractiveServer srv(root.string(), true);
    ASSERT_TRUE(srv.ok());

    srv.send(lintReq(1, target));
    auto r1 = srv.readResponse(1);
    EXPECT_NE(r1.find("\"severity\":\"error\""), std::string::npos)
        << "Helper is undefined, so the target must error:\n" << r1;

    // Add Helper: the type now resolves.
    auto helper = writeUnit(root, "Helper", "public final class Helper { }");
    srv.send(lintReq(2, target));
    auto r2 = srv.readResponse(2);
    EXPECT_GT(siblingsReparsed(r2), 0) << "an added sibling must resweep";
    EXPECT_EQ(r2.find("\"severity\":\"error\""), std::string::npos)
        << "the added Helper must let the target resolve:\n" << r2;

    // Delete Helper: the type is unresolved again.
    fs::remove(helper);
    srv.send(lintReq(3, target));
    auto r3 = srv.readResponse(3);
    srv.send("{\"kind\":\"shutdown\"}");
    EXPECT_NE(r3.find("\"severity\":\"error\""), std::string::npos)
        << "a deleted sibling's signatures must drop:\n" << r3;
}

// 3.1.4 — a sibling made syntax-broken mid-session is skipped (one-shot
// behavior: broken siblings contribute nothing) and recovers when fixed.
TEST(LintServerSibling, BrokenSiblingSkippedThenRecovers) {
    SKIP_WITHOUT_BINARY();
    auto root = freshTempDir("broken") / "src";
    writeUnit(root, "Sibling", SIBLING_ADD2);
    // Target does NOT reference Sibling, so a broken Sibling must not fail it.
    auto target = writeUnit(root, "Target",
        "public final class Target {\n"
        "    public static void main() { int32 x = 1; }\n"
        "}");

    InteractiveServer srv(root.string(), true);
    ASSERT_TRUE(srv.ok());

    srv.send(lintReq(1, target));
    auto r1 = srv.readResponse(1);
    EXPECT_EQ(r1.find("\"severity\":\"error\""), std::string::npos) << r1;

    // Break Sibling syntactically.
    writeUnit(root, "Sibling",
        "public final class Sibling { public static int32 add(int32 a, { }");
    srv.send(lintReq(2, target));
    auto r2 = srv.readResponse(2);
    EXPECT_EQ(r2.find("\"severity\":\"error\""), std::string::npos)
        << "a broken sibling must not fail an unrelated target:\n" << r2;

    // Fix Sibling: still clean.
    writeUnit(root, "Sibling", SIBLING_ADD2);
    srv.send(lintReq(3, target));
    auto r3 = srv.readResponse(3);
    srv.send("{\"kind\":\"shutdown\"}");
    EXPECT_EQ(r3.find("\"severity\":\"error\""), std::string::npos)
        << "target must remain clean after the sibling recovers:\n" << r3;
}

// 3.3.1 — parity on samples/tour after a touch/edit/delete sequence: each warm
// or reswept response's payload (diagnostics + xref) is byte-identical to a
// fresh one-shot of the SAME target against the SAME on-disk root state. This
// is the safety net for the warm-baseline design — it exercises a target that
// imports both a sibling (tour.DemoClass) and lazy stdlib (cajeta.math.*), so
// a divergence in the sibling context OR the lazy-package snapshot shows up.
TEST(LintServerSibling, ParityOnTourAcrossTouchAndDelete) {
    SKIP_WITHOUT_BINARY();
    const char* envRoot = std::getenv("CAJETA_SOURCE_ROOT");
    fs::path repo = (envRoot && *envRoot) ? fs::path(envRoot)
#ifdef CAJETA_SOURCE_ROOT_DEFAULT
        : fs::path(CAJETA_SOURCE_ROOT_DEFAULT);
#else
        : fs::path(".");
#endif
    fs::path tourSrc = repo / "samples/tour/src";
    if (!fs::exists(tourSrc)) GTEST_SKIP() << "samples/tour not present";

    // Work on a COPY so touch/delete never mutate the checkout.
    auto work = freshTempDir("tour");
    std::error_code ec;
    fs::copy(tourSrc, work / "src",
             fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec) << "copy failed: " << ec.message();

    fs::path root = work / "src/main/cajeta";
    fs::path target = root / "tour/math/StatsDemo.cajeta";
    fs::path touchable = root / "tour/DemoClass.cajeta";           // a real sibling
    fs::path deletable = root / "tour/collection/HeapDemo.cajeta"; // unrelated to target
    ASSERT_TRUE(fs::exists(target));
    ASSERT_TRUE(fs::exists(touchable));

    InteractiveServer srv(root.string(), true);
    ASSERT_TRUE(srv.ok());

    auto expectParity = [&](int id, const char* label) {
        srv.send(lintReq(id, target, /*emitXref=*/true));
        std::string block = srv.readResponse(id);
        std::string oracle = oneShotStderr(target, root, /*emitXref=*/true);
        EXPECT_EQ(payloadOf(block), oracle)
            << "warm/reswept payload diverges from one-shot at: " << label;
    };

    expectParity(1, "initial (resweep)");
    expectParity(2, "repeat (warm)");

    // Touch a referenced sibling (append a no-op comment: content + mtime change).
    { std::ofstream a(touchable, std::ios::app); a << "\n// touched\n"; }
    expectParity(3, "after touch (resweep)");

    // Delete an unrelated sibling.
    if (fs::exists(deletable)) fs::remove(deletable);
    expectParity(4, "after delete (resweep)");

    srv.send("{\"kind\":\"shutdown\"}");
}

#endif // !_WIN32
