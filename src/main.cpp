#include <iostream>
#include <string>
#include <vector>
#include <llvm/Support/InitLLVM.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/TargetParser/Host.h>
#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CompilerMode.h"
#include "cajeta/error/Exception.h"
#include "cajeta/cli/ArchiveCommands.h"

// CAJETA_VERSION and CAJETA_GIT_HASH are stamped at configure time by the
// top-level CMakeLists.txt. Fall back to a placeholder if a non-CMake build
// somehow gets here, so `cajeta --version` always prints something rather
// than failing to compile.
#ifndef CAJETA_VERSION
#define CAJETA_VERSION "0.0.0-unknown"
#endif
#ifndef CAJETA_GIT_HASH
#define CAJETA_GIT_HASH "unknown"
#endif

using namespace std;
using namespace antlr4;
using namespace cajeta;

namespace {

// Single-line + multi-line version output. `--version` short form prints
// the one-liner; the longer block goes to stdout for `--version --verbose`
// or anything else that wants the full build provenance. Kept here in
// main.cpp rather than buried in Compiler because (a) version reporting
// shouldn't drag in the whole codegen pipeline, and (b) the build-info
// macros above are file-scope to this TU.
void printVersion(bool verbose) {
    std::cout << "cajeta " << CAJETA_VERSION
              << " (" << CAJETA_GIT_HASH << ")" << std::endl;
    if (verbose) {
        std::cout << "LLVM:   " << LLVM_VERSION_STRING << std::endl;
        std::cout << "host:   " << llvm::sys::getDefaultTargetTriple() << std::endl;
    }
}

void printUsage(const char* progname) {
    std::cerr << "Usage: " << progname
              << " [options] <entry-method> <source-root-path> <archive-root-path>\n"
              << "\n"
              << "Mode (cajeta-docs/CompilerModes.md):\n"
              << "  --mode=debug|debug-release|release|fast|minimal\n"
              << "  --debug, --debug-release, --release, --fast, --minimal   (flavor aliases)\n"
              << "\n"
              << "Per-feature overrides (apply after mode):\n"
              << "  --bounds=on|off|trap                 Array bounds checking.\n"
              << "  --null-checks=on|off|trap            Null-receiver checks.\n"
              << "  --overflow-checks=on|off|wrapping    Integer overflow behavior.\n"
              << "  --source-tags=on|off                 Carry alloc/drop source positions on chain entries.\n"
              << "  --poison-free=on|off                 Sentinel-fill freed bytes.\n"
              << "  --live-set=strict|bounded|off        Live-allocation set discipline.\n"
              << "  --drop-chain-validate=on|off         Per-push/pop integrity checks.\n"
              << "  --ub-traps=on|off                    Trap before would-be UB.\n"
              << "  --use-after-move-rt=on|off           Runtime backup for the static use-after-move check.\n"
              << "  --stack-trace-capture=on|off         backtrace(3) at throw site.\n"
              << "  --diag-verbosity=terse|normal|verbose  Compile-time diagnostic detail.\n"
              << "  --diag-hints=on|off                  \"Did you mean...\" suggestions.\n"
              << "  --profile-counters=on|off            Per-method PGO-collection instrumentation.\n"
              << "\n"
              << "Output:\n"
              << "  --emit=ir|obj|cja|uber|exe           Output mode. Default ir.\n"
              << "  --classpath=a.cja,b.cja              Cajeta archives to ingest as dependencies\n"
              << "                                       (repeatable; comma-separates inside each occurrence).\n"
              << "  --prune-uber=on|off                  When --emit=uber, only bundle classpath entries\n"
              << "                                       transitively referenced by user / stdlib bitcode.\n"
              << "                                       Default on; --prune-uber=off bundles everything.\n"
              << "  --target=<triple>                    LLVM target triple. Default: host.\n"
              << "  --cpu=<name>                         Target CPU. Default: generic.\n"
              << "  --features=<list>                    Comma-separated target features (e.g. +neon).\n"
              << "\n"
              << "XPU (GPU compute, cajeta-docs/CajetaXPU.md):\n"
              << "  --xpu-backend=<list>                 Device backend(s) for @Kernel methods, comma-separated\n"
              << "                                       (none|nvptx|amdgpu|vulkan|cpu). Default none (host-only).\n"
              << "                                       Each embeds its kernel + registration ctor so kernel.launch(...)\n"
              << "                                       resolves at runtime; e.g. vulkan,cpu bundles a GPU target with a\n"
              << "                                       CPU fallback (the runtime picks the best available at launch).\n"
              << "  --xpu-arch=<arch>                    Device arch: nvptx SM target (e.g. sm_89, default),\n"
              << "                                       amdgpu GFX target (e.g. gfx1151), or vulkan SPIR-V env\n"
              << "                                       (e.g. vulkan1.3, the vulkan default). cpu has no arch.\n"
              << "  --xpu-emit=none|ptx|cubin|isa|hsaco|spirv|spvasm|obj  Also drop a per-kernel device artifact for\n"
              << "                                       inspection (ptx/cubin nvptx; isa/hsaco amdgpu; spirv/spvasm vulkan; obj cpu).\n"
              << "                                       Default none (registration only).\n"
              << "  -o <path>                            Output path for the final artifact.\n"
              << "  --help, -h                           This message.\n"
              << "  --version, -V                        Print version + build provenance and exit.\n"
              << "                                       Pair with --verbose for LLVM + host-triple lines.\n";
}

bool startsWith(const std::string& s, const std::string& prefix) {
    return s.rfind(prefix, 0) == 0;
}

// Returns true if `arg` matched `--<name>=...`; in that case `value` is set.
bool match(const std::string& arg, const std::string& name, std::string& value) {
    std::string prefix = "--" + name + "=";
    if (!startsWith(arg, prefix)) return false;
    value = arg.substr(prefix.size());
    return true;
}

// Tri-state setter. Reports + returns false on unrecognized value so the caller
// can print usage and exit. Verbose by design — `--bounds=tarp` should not
// silently fall through.
template <typename T>
bool setEnumFlag(const char* flagName, const std::string& value,
                 std::initializer_list<std::pair<const char*, T>> choices,
                 T& out) {
    for (auto& choice : choices) {
        if (value == choice.first) {
            out = choice.second;
            return true;
        }
    }
    std::cerr << "cajeta: unrecognized value for --" << flagName << ": " << value
              << " (expected ";
    bool first = true;
    for (auto& choice : choices) {
        if (!first) std::cerr << "|";
        std::cerr << choice.first;
        first = false;
    }
    std::cerr << ")\n";
    return false;
}

bool setBoolFlag(const char* flagName, const std::string& value, bool& out) {
    if (value == "on" || value == "true" || value == "1") { out = true; return true; }
    if (value == "off" || value == "false" || value == "0") { out = false; return true; }
    std::cerr << "cajeta: unrecognized value for --" << flagName << ": " << value
              << " (expected on|off)\n";
    return false;
}

} // namespace

int main(int argc, const char* argv[]) {
    // Top-level subcommand dispatch. `cajeta archive ...` routes to
    // the archive-management surface (cajeta-docs/ArchiveManagement.md);
    // anything else falls through to the legacy compile flow below.
    // When BuildTool.md ships, more subcommands ("build", "test",
    // "run", ...) land alongside `archive`.
    if (argc >= 2 && std::string(argv[1]) == "archive") {
        return cajeta::dispatchArchive(argc, argv);
    }

    // --version / -V short-circuit. Handled before Compiler construction
    // (which initializes LLVM targets, runs codegen-irrelevant work)
    // because `cajeta --version` should be sub-millisecond cheap. Verbose
    // form prints the LLVM version + host triple too — useful for bug
    // reports when something codegen-specific goes wrong on a particular
    // host CPU or LLVM build.
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--version" || arg == "-V") {
            bool verbose = false;
            for (int j = 1; j < argc; j++) {
                std::string a = argv[j];
                if (a == "--verbose" || a == "-v") { verbose = true; break; }
            }
            printVersion(verbose);
            return 0;
        }
    }

    Compiler compiler(argc, argv);
    std::vector<std::string> positional;
    // Track whether --xpu-arch was given explicitly so the amdgpu backend can
    // default its arch to gfx1151 (vs the nvptx sm_89 default) only when the
    // user didn't pin one. The two backends share a single xpuArch field.
    bool xpuArchExplicit = false;

    auto parseModeName = [&](const std::string& name) -> bool {
        if (name == "debug")            { compiler.setMode(CompilerMode::Debug); return true; }
        if (name == "debug-release")    { compiler.setMode(CompilerMode::DebugRelease); return true; }
        if (name == "release")          { compiler.setMode(CompilerMode::Release); return true; }
        if (name == "fast")             { compiler.setMode(CompilerMode::Fast); return true; }
        if (name == "minimal")          { compiler.setMode(CompilerMode::Minimal); return true; }
        std::cerr << "cajeta: unrecognized mode: " << name
                  << " (expected debug|debug-release|release|fast|minimal)\n";
        return false;
    };

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        std::string value;

        // Mode + flavor aliases. Process before per-feature flags so flavor
        // defaults are applied before any per-feature override.
        if (match(arg, "mode", value)) {
            if (!parseModeName(value)) { printUsage(argv[0]); return 1; }
        } else if (arg == "--debug")          { compiler.setMode(CompilerMode::Debug); }
          else if (arg == "--debug-release")  { compiler.setMode(CompilerMode::DebugRelease); }
          else if (arg == "--release")        { compiler.setMode(CompilerMode::Release); }
          else if (arg == "--fast")           { compiler.setMode(CompilerMode::Fast); }
          else if (arg == "--minimal")        { compiler.setMode(CompilerMode::Minimal); }

        // Per-feature toggles. Mutate the compiler's flags struct in place.
        else if (match(arg, "bounds", value)) {
            if (!setEnumFlag<BoundsCheck>("bounds", value,
                    { {"on", BoundsCheck::On}, {"off", BoundsCheck::Off}, {"trap", BoundsCheck::Trap} },
                    compiler.getMutableFlags().bounds)) {
                printUsage(argv[0]); return 1;
            }
        } else if (match(arg, "null-checks", value)) {
            if (!setEnumFlag<NullChecks>("null-checks", value,
                    { {"on", NullChecks::On}, {"off", NullChecks::Off}, {"trap", NullChecks::Trap} },
                    compiler.getMutableFlags().nullChecks)) {
                printUsage(argv[0]); return 1;
            }
        } else if (match(arg, "overflow-checks", value)) {
            if (!setEnumFlag<OverflowChecks>("overflow-checks", value,
                    { {"on", OverflowChecks::On}, {"off", OverflowChecks::Off}, {"wrapping", OverflowChecks::Wrapping} },
                    compiler.getMutableFlags().overflowChecks)) {
                printUsage(argv[0]); return 1;
            }
        } else if (match(arg, "live-set", value)) {
            if (!setEnumFlag<LiveSet>("live-set", value,
                    { {"strict", LiveSet::Strict}, {"bounded", LiveSet::Bounded}, {"off", LiveSet::Off} },
                    compiler.getMutableFlags().liveSet)) {
                printUsage(argv[0]); return 1;
            }
        } else if (match(arg, "diag-verbosity", value)) {
            if (!setEnumFlag<DiagVerbosity>("diag-verbosity", value,
                    { {"terse", DiagVerbosity::Terse}, {"normal", DiagVerbosity::Normal}, {"verbose", DiagVerbosity::Verbose} },
                    compiler.getMutableFlags().diagVerbosity)) {
                printUsage(argv[0]); return 1;
            }
        } else if (match(arg, "source-tags",         value)) { if (!setBoolFlag("source-tags",         value, compiler.getMutableFlags().sourceTags))         { printUsage(argv[0]); return 1; } }
          else if (match(arg, "poison-free",         value)) { if (!setBoolFlag("poison-free",         value, compiler.getMutableFlags().poisonFree))         { printUsage(argv[0]); return 1; } }
          else if (match(arg, "drop-chain-validate", value)) { if (!setBoolFlag("drop-chain-validate", value, compiler.getMutableFlags().dropChainValidate))  { printUsage(argv[0]); return 1; } }
          else if (match(arg, "ub-traps",            value)) { if (!setBoolFlag("ub-traps",            value, compiler.getMutableFlags().ubTraps))            { printUsage(argv[0]); return 1; } }
          else if (match(arg, "use-after-move-rt",   value)) { if (!setBoolFlag("use-after-move-rt",   value, compiler.getMutableFlags().useAfterMoveRt))     { printUsage(argv[0]); return 1; } }
          else if (match(arg, "stack-trace-capture", value)) { if (!setBoolFlag("stack-trace-capture", value, compiler.getMutableFlags().stackTraceCapture))  { printUsage(argv[0]); return 1; } }
          else if (match(arg, "diag-hints",          value)) { if (!setBoolFlag("diag-hints",          value, compiler.getMutableFlags().diagHints))          { printUsage(argv[0]); return 1; } }
          else if (match(arg, "profile-counters",    value)) { if (!setBoolFlag("profile-counters",    value, compiler.getMutableFlags().profileCounters))    { printUsage(argv[0]); return 1; } }

        // Output / target.
        else if (match(arg, "emit", value)) {
            if (value == "ir") {
                compiler.setEmitMode(EmitMode::IR);
            } else if (value == "obj") {
                compiler.setEmitMode(EmitMode::Obj);
            } else if (value == "cja") {
                compiler.setEmitMode(EmitMode::Cja);
            } else if (value == "uber") {
                compiler.setEmitMode(EmitMode::Uber);
            } else if (value == "exe") {
                compiler.setEmitMode(EmitMode::Exe);
            } else {
                std::cerr << "cajeta: unrecognized value for --emit: " << value
                          << " (expected ir|obj|cja|uber|exe)\n";
                printUsage(argv[0]);
                return 1;
            }
        } else if (match(arg, "prune-uber", value)) {
            // Local bool because setPruneUber lives on Compiler (not on
            // CompilerFlags). The setBoolFlag helper writes through a
            // reference, so we route through a temporary.
            bool tmp = true;
            if (!setBoolFlag("prune-uber", value, tmp)) {
                printUsage(argv[0]);
                return 1;
            }
            compiler.setPruneUber(tmp);
        } else if (match(arg, "classpath", value)) {
            // Java-style classpath — comma-separated `.cja` paths.
            // Repeatable; commas split within each occurrence. Empty
            // entries are skipped (so trailing/duplicate commas don't
            // explode).
            size_t start = 0;
            while (start <= value.size()) {
                size_t comma = value.find(',', start);
                std::string piece = (comma == std::string::npos)
                    ? value.substr(start)
                    : value.substr(start, comma - start);
                if (!piece.empty()) {
                    compiler.addClasspath(piece);
                }
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        } else if (match(arg, "target", value)) {
            compiler.setTargetTriple(value);
        } else if (match(arg, "cpu", value)) {
            compiler.setCpu(value);
        } else if (match(arg, "features", value)) {
            compiler.setFeatures(value);
        } else if (match(arg, "xpu-backend", value)) {
            // Comma-separated list — a binary can bundle several targets
            // (e.g. vulkan,cpu); the runtime dispatcher picks the best
            // available at launch. `none` clears the list.
            compiler.setXpuBackend(XpuBackend::None);
            size_t start = 0;
            for (;;) {
                size_t comma = value.find(',', start);
                std::string tok = (comma == std::string::npos)
                    ? value.substr(start) : value.substr(start, comma - start);
                XpuBackend b;
                if (!setEnumFlag<XpuBackend>("xpu-backend", tok,
                        { {"none", XpuBackend::None}, {"nvptx", XpuBackend::Nvptx},
                          {"amdgpu", XpuBackend::Amdgpu}, {"vulkan", XpuBackend::Vulkan},
                          {"cpu", XpuBackend::Cpu} }, b)) {
                    printUsage(argv[0]); return 1;
                }
                compiler.addXpuBackend(b);  // None is a no-op
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        } else if (match(arg, "xpu-emit", value)) {
            XpuEmit e;
            if (!setEnumFlag<XpuEmit>("xpu-emit", value,
                    { {"none", XpuEmit::None}, {"ptx", XpuEmit::Ptx}, {"cubin", XpuEmit::Cubin},
                      {"isa", XpuEmit::Isa}, {"hsaco", XpuEmit::Hsaco},
                      {"spirv", XpuEmit::Spirv}, {"spvasm", XpuEmit::Spvasm},
                      {"obj", XpuEmit::Object} }, e)) {
                printUsage(argv[0]); return 1;
            }
            compiler.setXpuEmit(e);
        } else if (match(arg, "xpu-arch", value)) {
            compiler.setXpuArch(value);
            xpuArchExplicit = true;
        } else if (arg == "-o") {
            if (i + 1 >= argc) {
                std::cerr << "cajeta: -o requires a path argument\n";
                return 1;
            }
            compiler.setOutputPath(argv[++i]);
        } else if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        } else if (startsWith(arg, "--")) {
            std::cerr << "cajeta: unknown option: " << arg << "\n";
            printUsage(argv[0]);
            return 1;
        } else {
            positional.push_back(arg);
        }
    }

    if (positional.size() < 3) {
        printUsage(argc > 0 ? argv[0] : "cajeta");
        return 1;
    }

    // The xpuArch default ("sm_89") is NVPTX-shaped; for the amdgpu backend
    // default to a GFX target instead, and for vulkan a SPIR-V target env,
    // unless the user pinned --xpu-arch.
    if (compiler.usesXpuBackend(XpuBackend::Vulkan) && !xpuArchExplicit) {
        compiler.setXpuArch("vulkan1.3");
    }
    if (compiler.usesXpuBackend(XpuBackend::Amdgpu) && !xpuArchExplicit) {
        compiler.setXpuArch("gfx1151");
    }

    try {
        compiler.compile(positional[0], positional[1], positional[2]);
    } catch (cajeta::Exception& e) {
        std::cerr << "cajeta: " << e.getErrorId() << ": " << e.getMessage() << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "cajeta: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
