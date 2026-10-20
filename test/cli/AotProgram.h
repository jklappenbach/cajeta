// Builds and runs small AOT programs for the CLI tests: sources on disk, the real
// compiler binary, an environment scoped to one command, and the dumped
// post-optimization IR (CAJETA_DUMP_CODEGEN_BC) for golden checks.
#ifndef CAJETA_TEST_CLI_AOT_PROGRAM_H
#define CAJETA_TEST_CLI_AOT_PROGRAM_H

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <utility>
#include <vector>
#include "../PortableEnv.h"

namespace cajeta_test_aot {

namespace fs = std::filesystem;

inline std::string compilerBinary() {
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

inline int exitOf(int status) {
#ifdef _WIN32
    return status;
#else
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
#endif
}

inline std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

/** A scratch tree under the temp directory, removed on destruction. */
struct Workspace {
    fs::path root;
    Workspace() {
        static std::mt19937_64 rng(std::random_device{}());
        root = fs::temp_directory_path() / ("cajeta_aot_" + std::to_string(rng()));
        fs::create_directories(root);
    }
    ~Workspace() { std::error_code ec; fs::remove_all(root, ec); }
    void write(const std::string& rel, const std::string& text) {
        fs::create_directories((root / rel).parent_path());
        std::ofstream(root / rel) << text;
    }
};

struct Built { bool ok = false; std::string log; fs::path exe; fs::path dump; };

/** Runs the compiler as `cajeta <args> <entry> <src> <out>` in `w`, with `env` scoped to it. */
inline Built compile(Workspace& w, const std::string& tag, const std::string& args,
                     const std::string& entry, const std::string& src,
                     std::initializer_list<std::pair<const char*, std::string>> env = {}) {
    Built b;
    fs::path out = w.root / (tag + "-out");
    b.dump = w.root / (tag + "-dump");
    fs::create_directories(out);
    fs::create_directories(b.dump);
    b.exe = out / "prog";
    std::string prefix = cajeta_env_prefix({{"CAJETA_DUMP_CODEGEN_BC", b.dump.string()}});
    for (const auto& kv : env) prefix += cajeta_env_prefix({{kv.first, kv.second}});
    fs::path log = w.root / (tag + ".log");
    std::string outFlag = args.find("-o ") == std::string::npos
        ? " -o \"" + b.exe.string() + "\"" : "";
    std::string cmd = prefix + "\"" + compilerBinary() + "\" " + args + outFlag + " " + entry + " \""
        + (w.root / src).string() + "\" \"" + out.string() + "\" > \"" + log.string() + "\" 2>&1";
    b.ok = exitOf(std::system(cajeta_shell(cmd).c_str())) == 0;
    b.log = slurp(log);
    return b;
}

struct Ran { int rc = -1; std::string out; std::string err; };

inline Ran run(const Built& b, const std::string& envPrefix = "") {
    Ran r;
    fs::path o = b.exe.string() + ".stdout", e = b.exe.string() + ".stderr";
    std::string cmd = envPrefix + "\"" + cajeta_exe_path(b.exe).string() + "\" > \"" + o.string()
        + "\" 2> \"" + e.string() + "\"";
    r.rc = exitOf(std::system(cajeta_shell(cmd).c_str()));
    r.out = slurp(o);
    r.err = slurp(e);
    return r;
}

/** Concatenated post-optimization IR of every module the build emitted. */
inline std::string dumpedIr(const Built& b) {
    std::string all;
    for (auto& f : fs::directory_iterator(b.dump))
        if (f.path().extension() == ".ll") all += slurp(f.path());
    return all;
}

/** Bodies of the defined functions whose header contains `needle`. */
inline std::string bodiesOf(const std::string& ir, const std::string& needle) {
    std::string out;
    size_t p = 0;
    while ((p = ir.find("\ndefine ", p)) != std::string::npos) {
        size_t nl = ir.find('\n', p + 1);
        size_t end = ir.find("\n}", nl);
        if (ir.substr(p, nl - p).find(needle) != std::string::npos)
            out += ir.substr(p, (end == std::string::npos ? ir.size() : end) - p);
        p = nl;
    }
    return out;
}

}  // namespace cajeta_test_aot

#endif
