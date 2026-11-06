//
// A --release executable links the stack drop of a template instantiation
// that only a classpath archive's code drops. Optional<Item> reached through
// Channel<Item>.receive() inside a library gets its drop functions in the
// stdlib module, where nothing calls the stack drop; the optimizer used to
// discard that linkonce_odr definition and the link failed with an undefined
// __cajeta_stack_cajeta_lang_Optional_..._drop. Found building cabra against
// dev.cajeta.http 0.9.x (DispatchPool.worker) on v0.51.0, 2026-11-05.
//

#include "gtest/gtest.h"

#include <array>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include "../PortableEnv.h"

namespace {

namespace fs = std::filesystem;

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
#ifdef _WIN32
    if (r.size() >= 3 && r[0] == '/' && std::isalpha((unsigned char) r[1]) && r[2] == '/')
        r = std::string(1, r[1]) + ":" + r.substr(2);
    std::string p = r + "/build/src/cajeta.exe";
    std::replace(p.begin(), p.end(), '/', '\\');
    return p;
#else
    return r + "/build/src/cajeta";
#endif
}

std::string capture(const std::string& cmd) {
    std::string out;
#ifdef _WIN32
    FILE* p = _popen((cmd + " 2>NUL").c_str(), "r");
#else
    FILE* p = popen((cmd + " 2>" CAJETA_PORTABLE_DEVNULL "").c_str(), "r");
#endif
    if (!p) return out;
    std::array<char, 512> buf;
    while (fgets(buf.data(), (int) buf.size(), p)) out += buf.data();
#ifdef _WIN32
    _pclose(p);
    out.erase(std::remove(out.begin(), out.end(), '\r'), out.end());
#else
    pclose(p);
#endif
    return out;
}

void write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p);
    out << text;
}

// The library: Pool.drain receives from a Channel<Item> and drops each
// Optional<Item> on the stack. Only this archive's code calls the stack drop.
const char* kItem =
    "package lib;\n"
    "public class Item {\n"
    "    public int32 v;\n"
    "    public Item(int32 v) { this.v = v; }\n"
    "}\n";

const char* kPool =
    "package lib;\n"
    "import cajeta.lang.Optional;\n"
    "import cajeta.concurrent.Channel;\n"
    "public class Pool {\n"
    "    Channel<Item> queue;\n"
    "    public Pool() { this.queue = heap Channel<Item>(4); }\n"
    "    public void offer(#Item d) { this.queue.send(#d); }\n"
    "    public void close() { this.queue.close(); }\n"
    "    public static int32 drain(Pool p) {\n"
    "        int32 sum = 0;\n"
    "        boolean go = true;\n"
    "        while (go) {\n"
    "            Optional<Item> o = p.queue.receive();\n"
    "            if (!o.isPresent()) {\n"
    "                go = false;\n"
    "            } else {\n"
    "                Item d #= o.take();\n"
    "                sum = sum + d.v;\n"
    "            }\n"
    "        }\n"
    "        return sum;\n"
    "    }\n"
    "}\n";

const char* kMain =
    "package app;\n"
    "import lib.Item;\n"
    "import lib.Pool;\n"
    "public class Main {\n"
    "    public static void main(String[] args) {\n"
    "        Pool p = heap Pool();\n"
    "        p.offer(#heap Item(3));\n"
    "        p.offer(#heap Item(4));\n"
    "        p.close();\n"
    "        System.stdout.println(Pool.drain(p));\n"
    "    }\n"
    "}\n";

// Builds lib.cja, then the app with `mode`; returns the run's output, or
// "build failed" naming the step.
std::string buildAndRun(const std::string& tag, const std::string& mode) {
    static std::mt19937_64 rng(std::random_device{}());
    fs::path base = fs::temp_directory_path()
        / ("cajeta_reldroplink_" + tag + "_" + std::to_string(rng()));
    write(base / "lib" / "lib" / "Item.cajeta", kItem);
    write(base / "lib" / "lib" / "Pool.cajeta", kPool);
    write(base / "app" / "app" / "Main.cajeta", kMain);
    fs::path cja = base / "lib.cja";
    fs::path bin = base / "app.bin";
    std::string out = "lib build failed";
    std::string lib = compilerBinary() + " --emit=cja -o " + cja.string()
        + " lib.Pool.drain " + (base / "lib").string() + " "
        + (base / "out-lib").string() + " > " CAJETA_PORTABLE_DEVNULL " 2>&1";
    if (std::system(lib.c_str()) == 0) {
        out = "app build failed";
        std::string app = compilerBinary() + " --emit=exe " + mode
            + " --classpath=" + cja.string() + " -o " + bin.string()
            + " app.Main.main " + (base / "app").string() + " "
            + (base / "out-app").string() + " > " CAJETA_PORTABLE_DEVNULL " 2>&1";
        if (std::system(app.c_str()) == 0) {
            if (!fs::exists(bin)) bin += ".exe";
            if (fs::exists(bin)) out = capture(bin.string());
        }
    }
    fs::remove_all(base);
    return out;
}

} // namespace

// FIRES: under --release the stack drop only the archive calls still links.
TEST(ReleaseClasspathDropLinkTests, releaseLinksAStackDropOnlyAnArchiveCalls) {
    EXPECT_EQ("7\n", buildAndRun("release", "--release"));
}

// Control: the default mode never lost the definition.
TEST(ReleaseClasspathDropLinkTests, defaultModeLinksTheSameProgram) {
    EXPECT_EQ("7\n", buildAndRun("default", ""));
}
