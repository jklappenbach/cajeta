// Component scopes across an archive boundary: a scope published in a .cja is
// entered by code compiled in the consumer, and the consumer's components
// resolve inside the archive's anchors (component-scopes plan 8.1).

#include "gtest/gtest.h"

#include "../PortableEnv.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace fs = std::filesystem;

namespace {

std::string sourceRoot() {
    const char* envRoot = std::getenv("CAJETA_SOURCE_ROOT");
    if (envRoot && *envRoot) return envRoot;
#ifdef CAJETA_SOURCE_ROOT_DEFAULT
    return CAJETA_SOURCE_ROOT_DEFAULT;
#else
    return ".";
#endif
}

std::string compilerBinary() { return sourceRoot() + "/build/src/cajeta"; }

int exitOf(int status) {
#ifdef _WIN32
    return status;
#else
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

struct World {
    fs::path root;
    World() {
        static std::mt19937_64 rng(std::random_device{}());
        root = fs::temp_directory_path() / ("cajeta_scope_archive_" + std::to_string(rng()));
        fs::create_directories(root);
    }
    ~World() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    void write(const fs::path& rel, const std::string& text) const {
        fs::create_directories((root / rel).parent_path());
        std::ofstream(root / rel) << text;
    }
    int sh(const std::string& cmd, std::string& out, const std::string& tag) const {
        fs::path log = root / (tag + ".log");
        std::string full = "cd \"" + root.string() + "\" && " + cmd + " > \"" + log.string() + "\" 2>&1";
        int rc = exitOf(std::system(cajeta_shell(full).c_str()));
        out = readFile(log);
        return rc;
    }
};

const char* kLibHandler =
    "package lib;\n"
    "public interface Handler {\n"
    "    @Scope(\"Request\")\n"
    "    int32 handle(int32 n);\n"
    "}\n";

const char* kLibServer =
    "package lib;\n"
    "public final class Server {\n"
    "    public static int32 dispatch(Handler h, int32 n) { return h.handle(n); }\n"
    "    public static void main(String[] args) { }\n"
    "}\n";

const char* kLibSession =
    "package lib;\n"
    "@Scope(\"Session\") public class Session {\n"
    "    public int32 id;\n"
    "    public Session(int32 id) { this.id = id; return; }\n"
    "    public int32 within(() -> int32 body) { return body(); }\n"
    "}\n";

const char* kAppMain =
    "package app;\n"
    "import cajeta.lang.String;\n"
    "import cajeta.lang.System;\n"
    "import lib.Handler;\n"
    "import lib.Server;\n"
    "import lib.Session;\n"
    "@Component(scope = \"Request\") public class Cart {\n"
    "    public int32 v;\n"
    "    public Cart() { v = 0; return; }\n"
    "}\n"
    "@Component(scope = \"Session\") public class Prefs {\n"
    "    public int32 n;\n"
    "    public Prefs() { n = 0; return; }\n"
    "}\n"
    "public class H implements Handler {\n"
    "    public H() { return; }\n"
    "    public int32 handle(int32 n) {\n"
    "        Cart a = Cart.__cajeta_inject();\n"
    "        Cart b = Cart.__cajeta_inject();\n"
    "        a.v = a.v + n;\n"
    "        return b.v;\n"
    "    }\n"
    "}\n"
    "public class Premium extends Session {\n"
    "    public int32 tier;\n"
    "    public Premium(int32 id, int32 tier) { this.id = id; this.tier = tier; return; }\n"
    "    public int32 bump() {\n"
    "        Prefs p = Prefs.__cajeta_inject();\n"
    "        p.n = p.n + 100;\n"
    "        return p.n + this.tier;\n"
    "    }\n"
    "}\n"
    "public class Main {\n"
    "    static int32 addPrefs(int32 k) {\n"
    "        Prefs p = Prefs.__cajeta_inject();\n"
    "        p.n = p.n + k;\n"
    "        return p.n;\n"
    "    }\n"
    "    public static int32 main(String[] args) {\n"
    "        Handler h = heap H();\n"
    "        int32 x = Server.dispatch(h, 5);\n"
    "        int32 y = Server.dispatch(h, 7);\n"
    "        Session s = heap Session(3);\n"
    "        int32 p1 = s.within(() -> Main.addPrefs(2));\n"
    "        int32 p2 = s.within(() -> Main.addPrefs(4));\n"
    "        Premium q = heap Premium(8, 9);\n"
    "        int32 b = q.bump();\n"
    "        System.stdout.println(\"x=\" + x + \" y=\" + y + \" p1=\" + p1 + \" p2=\" + p2\n"
    "            + \" id=\" + s.id + \" qid=\" + q.id + \" bump=\" + b);\n"
    "        return 0;\n"
    "    }\n"
    "}\n";

bool haveCompiler() { return fs::exists(compilerBinary()); }

} // namespace

// plan 8.1.1 to 8.1.3: an archived interface-method scope anchors a consumer's
// implementation, a consumer component resolves inside an archived instance
// scope, and the archived anchor class keeps one layout on both sides.
TEST(ScopeArchive, scopesPublishedInAnArchiveServeTheConsumer) {
    if (!haveCompiler()) GTEST_SKIP() << "compiler binary not built";
    World w;
    w.write("libsrc/lib/Handler.cajeta", kLibHandler);
    w.write("libsrc/lib/Server.cajeta", kLibServer);
    w.write("libsrc/lib/Session.cajeta", kLibSession);
    w.write("appsrc/app/Main.cajeta", kAppMain);
    fs::create_directories(w.root / "libout");
    fs::create_directories(w.root / "appout");
    std::string log;
    ASSERT_EQ(w.sh("\"" + compilerBinary() + "\" --emit=cja -o libout/lib.cja "
                   "lib.Server.main libsrc libout", log, "build-cja"), 0) << log;
    ASSERT_EQ(w.sh("\"" + compilerBinary() + "\" --emit=exe --classpath=libout/lib.cja "
                   "-o appout/prog app.Main.main appsrc appout", log, "build-exe"), 0) << log;
    std::string run;
    ASSERT_EQ(w.sh("./appout/prog", run, "run"), 0) << run;
    EXPECT_NE(run.find("x=5 y=7"), std::string::npos) << run;
    EXPECT_NE(run.find("p1=2 p2=6"), std::string::npos) << run;
    EXPECT_NE(run.find("id=3 qid=8 bump=109"), std::string::npos) << run;
}
