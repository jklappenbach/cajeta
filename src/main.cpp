#include <filesystem>
#include <iostream>
#include <string>
#include <vector>
#include <llvm/Support/InitLLVM.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/TargetParser/Host.h>
#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CompilerMode.h"
#include "cajeta/error/Exception.h"
#include "cajeta/error/Diagnostics.h"
#include "cajeta/error/DiagnosticEngine.h"
#include "cajeta/cli/ArchiveCommands.h"
#include "cajeta/cli/DocCommand.h"
#include "cajeta/cli/IdeCommands.h"
#include "cajeta/cli/NativeCommands.h"
#include "cajeta/cli/StdlibCommands.h"
#include "cajeta/cli/XpuProfileCommand.h"
#include "cajeta/jit/CajetaJitHost.h"
#include "cajeta/dap/DapServer.h"
#include "cajeta/buildtool/BuildToolCommands.h"

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
              << "       " << progname << " <subcommand> [args...]\n"
              << "\n"
              << "Subcommands (build tool):\n"
              << "  init [type] [dir]  Scaffold a project (init --list shows archetypes).\n"
              << "  build              Build the current project (task from cajeta.json).\n"
              << "  test               Run the project's tests.\n"
              << "  tasks              List the project's tasks with descriptions.\n"
              << "  add | remove | upgrade | pin   Manage manifest dependencies.\n"
              << "  install            Publish this project's library into the local repository.\n"
              << "  publish            Publish to a remote repository.\n"
              << "  info | show        Inspect the project / a dependency.\n"
              << "  doc <root>         Generate API documentation (doc --help).\n"
              << "  search-skill | list-skills | get-skills   Skill discovery in dependencies.\n"
              << "  coverage | verify | verify-reproducible | trust   Quality and provenance.\n"
              << "  workspace | members | toolchain | which | sandbox-info   Environment.\n"
              << "  Any other name runs the matching cajeta.json task (e.g. `cajeta run`\n"
              << "  when the manifest defines a run task).\n"
              << "\n"
              << "Subcommands (tooling):\n"
              << "  archive <cmd>      Create / inspect / sign .cja archives (archive --help).\n"
              << "  ide <cmd>          Manage the bundled IntelliJ IDEA plugin\n"
              << "                     (ide install | uninstall | list).\n"
              << "  jit-run <root> <package.Class.method>   Compile + run an entry point via the JIT.\n"
              << "  dap                Debug Adapter Protocol server over stdio (for IDE debugging).\n"
              << "  stdlib <cmd>       Embedded stdlib source access (stdlib list |\n"
              << "                     stdlib extract <dir>) — extraction preserves package\n"
              << "                     paths and writes a .cajeta-stdlib.json identity marker.\n"
              << "\n"
              << "Mode (docs/specification/buildtool/CompilerModes.md):\n"
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
              << "  --opt=O0|O1|O2|O3                    IR optimization for --emit=obj/exe (default O0;\n"
              << "                                       --release/--fast imply O2/O3).\n"
              << "  --lto=off|thin|full                  Cross-module LTO for --emit=exe (default off).\n"
              << "                                       thin: inline across module boundaries (stdlib\n"
              << "                                       hot paths into user code). Uses the fork lld.\n"
              << "  --drop-chain-validate=on|off         Per-push/pop integrity checks.\n"
              << "  --ub-traps=on|off                    Trap before would-be UB.\n"
              << "  --use-after-move-rt=on|off           Runtime backup for the static use-after-move check.\n"
              << "  --stack-trace-capture=on|off         backtrace(3) at throw site.\n"
              << "  --diag-verbosity=terse|normal|verbose  Compile-time diagnostic detail.\n"
              << "  --diag-hints=on|off                  \"Did you mean...\" suggestions.\n"
              << "  --diag-format=text|json              Diagnostic output format. json = one NDJSON\n"
              << "                                       object per line on stderr for tools/IDEs (default text).\n"
              << "  --profile-counters=on|off            Per-method PGO-collection instrumentation.\n"
              << "  --debug-info=off|line|full           Debug records in the binary (default line).\n"
              << "                                       line = shadow stack, so traces resolve to\n"
              << "                                       Type.method(File.cajeta:NN). full = adds\n"
              << "                                       safepoints + locals for an external debugger.\n"
              << "\n"
              << "Output:\n"
              << "  --emit=ir|obj|cja|uber|exe           Output mode. Default ir.\n"
              << "  --link-mode=lean|full                Linker/DCE policy. lean (default for --emit=exe)\n"
              << "                                       strips classes outside the keep-set; full keeps\n"
              << "                                       every class (default elsewhere; --keep-all alias).\n"
              << "  --why-kept=<class>                   Lean DCE: report which reflection site kept the\n"
              << "                                       named (canonical) class in the keep-set.\n"
              << "  --keepset-json=<path>                Lean DCE: write the generated keep-set + provenance\n"
              << "                                       to <path> as JSON.\n"
              << "  --emit-xref=<path>                   Write the resolved cross-reference index\n"
              << "                                       (declarations, inheritance, references, overrides,\n"
              << "                                       calls) to <path> as JSON, for IDE symbol lookup.\n"
              << "                                       With --lint <root>: whole-root export, one document.\n"
              << "  --emit-xref                          (bare, with --lint <file>) Emit the linted file's\n"
              << "                                       xref records as NDJSON on the diagnostic channel,\n"
              << "                                       kind:\"xref\" — one subprocess returns diagnostics\n"
              << "                                       and xref per edit.\n"
              << "  --tree-shake=off|report|on           Tier-1 RTA (--emit=exe). on (DEFAULT): prune\n"
              << "                                       unreachable method bodies + dead clinits (drops e.g.\n"
              << "                                       OpenSSL). report: print the strip analysis. off: opt out.\n"
              << "  --classpath=a.cja,b.cja              Cajeta archives to ingest as dependencies\n"
              << "                                       (repeatable; comma-separates inside each occurrence).\n"
              << "  --skill-root=<dir>                   Package root holding skills/ to embed in the .cja\n"
              << "                                       (defaults to the source root; build tool passes project root).\n"
              << "  --cache-manifest=<path>              Incremental-compilation manifest (cache-manifest-v1):\n"
              << "                                       per-source clean/dirty + .bc/obligation cache slots.\n"
              << "                                       Generated by `cajeta build`, not hand-authored.\n"
              << "  --prune-uber=on|off                  When --emit=uber, only bundle classpath entries\n"
              << "                                       transitively referenced by user / stdlib bitcode.\n"
              << "                                       Default on; --prune-uber=off bundles everything.\n"
              << "  --target=<triple>                    LLVM target triple. Default: host.\n"
              << "  --cpu=<name>                         Target CPU. Default: generic.\n"
              << "  --features=<list>                    Comma-separated target features (e.g. +neon).\n"
              << "  --profile=<name>                     Active @Profile for component gating\n"
              << "                                       (dev/test/release/...; default none).\n"
              << "\n"
              << "Reproducible builds (docs/BuildTool.md):\n"
              << "  --source-date-epoch=<unix-ts>        Fixed build timestamp (SOURCE_DATE_EPOCH).\n"
              << "  --debug-prefix-map=<from>=<to>       Remap source paths in debug info.\n"
              << "  --seed=<hex>                         Deterministic salt for any build RNG.\n"
              << "\n"
              << "XPU (GPU compute, docs/CajetaXPU.md):\n"
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

// Emit a caught cajeta::Exception as an error diagnostic, carrying its source
// span (located-semantic-diagnostics) when the throw site supplied one.
static void emitException(cajeta::Exception& e, bool jsonDiag) {
    if (jsonDiag) {
        cajeta::emitJsonDiagnostic("error", e.getErrorId(), e.getMessage(),
                                   e.getFile(), e.getLine(), e.getColumn());
    } else if (e.hasLocation()) {
        std::cerr << "cajeta: " << e.getFile() << ":" << e.getLine() << ":"
                  << e.getColumn() << ": " << e.getErrorId() << ": "
                  << e.getMessage() << "\n";
    } else {
        std::cerr << "cajeta: " << e.getErrorId() << ": " << e.getMessage() << "\n";
    }
}

int main(int argc, const char* argv[]) {
    // Top-level subcommand dispatch. `cajeta archive ...` routes to
    // the archive-management surface (docs/ArchiveManagement.md);
    // `cajeta info` (and eventually `build`, `test`, ...) routes to
    // the build-tool surface (docs/BuildTool.md). Anything not
    // claimed by either falls through to the legacy compile flow below.
    if (argc >= 2 && std::string(argv[1]) == "archive") {
        return cajeta::dispatchArchive(argc, argv);
    }
    {
        int btExit = 0;
        if (cajeta::buildtool::dispatchBuildTool(argc, argv, &btExit)) {
            return btExit;
        }
    }

    // `cajeta jit-run <sourceRoot> <package.Class.method>` — in-process JIT
    // host (CP1 of the debugger plan; the `cajeta dap` server reuses it). A
    // developer/diagnostic verb for now.
    if (argc >= 2 && std::string(argv[1]) == "jit-run") {
        return cajeta::jit::dispatchJitRun(argc, argv);
    }

    // `cajeta fetch` / `cajeta vendor` — native-dependency provisioning
    // (native-deps unit 14): bring a native artifact into the ~/.cajeta/native
    // cache (verified) or vendor it into the project native/ dir.
    if (argc >= 2 && (std::string(argv[1]) == "fetch"
                   || std::string(argv[1]) == "vendor")) {
        return cajeta::dispatchNative(argc, argv);
    }

    // `cajeta gpu-profile` — interrogate the active GPU + print its DeviceProfile
    // as JSON (xpu-device-profile); the profile suite's env-capture consumes it.
    if (argc >= 2 && std::string(argv[1]) == "gpu-profile") {
        return cajeta::dispatchXpuProfile(argc, argv);
    }

    // `cajeta dap` — Debug Adapter Protocol server over stdio (docs/
    // Debugging.md). The IDE plugin spawns this and drives the debug session.
    if (argc >= 2 && std::string(argv[1]) == "dap") {
        cajeta::dap::DapServer server;
        return server.run(std::cin, std::cout);
    }


    // `cajeta doc <source-root> [...]` — documentation generator
    // (docs/Documentation.md). Forwards to the shared cajetadoc engine
    // (tools/cajetadoc/), the same code as the standalone `cajetadoc` binary.
    if (argc >= 2 && std::string(argv[1]) == "doc") {
        return cajeta::doc::dispatchDoc(argc, argv);
    }

    // `cajeta ide <install|uninstall|list>` — manage the bundled IntelliJ IDEA
    // plugin embedded in this binary (cross-platform install path, D8).
    if (argc >= 2 && std::string(argv[1]) == "ide") {
        return cajeta::dispatchIde(argc, argv);
    }

    // `cajeta stdlib <list|extract <dir>>` — embedded stdlib source access
    // (ide-symbol-index §6.0.3): the IDE plugin extracts + mounts the stdlib
    // source so navigation and debug stops land in real files.
    if (argc >= 2 && std::string(argv[1]) == "stdlib") {
        return cajeta::dispatchStdlib(argc, argv);
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
    // --lint <file>: single-file diagnostics mode (no codegen/emit/entry). The
    // file is the sole positional; handled after the arg loop, before the
    // three-positional compile path (compiler-lint-mode-spec §2).
    bool lintMode = false;
    std::string lintSourceRoot;  // --source-root: project context (lint-source-root-spec)
    std::string lintShadow;      // --shadow: on-disk twin the linted buffer replaces
    // Track whether --xpu-arch was given explicitly so the amdgpu backend can
    // default its arch to gfx1151 (vs the nvptx sm_89 default) only when the
    // user didn't pin one. The two backends share a single xpuArch field.
    bool xpuArchExplicit = false;
    // Track an explicit --link-mode / --keep-all so the default flip to Lean
    // for --emit=exe (below) only applies when the user didn't pin a mode.
    bool linkModeExplicit = false;
    bool treeShakeExplicit = false;
    // --print-cache-discriminator: print the incremental-compilation cache
    // discriminator for exactly this flag set and exit (no compile). The
    // build tool probes this before authoring a cache manifest so flag
    // resolution stays single-sourced here.
    bool printCacheDiscriminator = false;

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
        } else if (match(arg, "opt", value)) {
            if (!setEnumFlag<OptLevel>("opt", value,
                    { {"O0", OptLevel::O0}, {"O1", OptLevel::O1},
                      {"O2", OptLevel::O2}, {"O3", OptLevel::O3} },
                    compiler.getMutableFlags().opt)) {
                printUsage(argv[0]); return 1;
            }
        } else if (match(arg, "lto", value)) {
            if (!setEnumFlag<LtoMode>("lto", value,
                    { {"off", LtoMode::Off}, {"thin", LtoMode::Thin},
                      {"full", LtoMode::Full} },
                    compiler.getMutableFlags().lto)) {
                printUsage(argv[0]); return 1;
            }
        } else if (match(arg, "diag-verbosity", value)) {
            if (!setEnumFlag<DiagVerbosity>("diag-verbosity", value,
                    { {"terse", DiagVerbosity::Terse}, {"normal", DiagVerbosity::Normal}, {"verbose", DiagVerbosity::Verbose} },
                    compiler.getMutableFlags().diagVerbosity)) {
                printUsage(argv[0]); return 1;
            }
        } else if (match(arg, "debug-info", value)) {
            std::string diErr;
            if (!cajeta::applyDebugInfo(value, compiler.getMutableFlags(), &diErr)) {
                std::cerr << "cajeta: " << diErr << "\n";
                printUsage(argv[0]); return 1;
            }
        } else if (match(arg, "diag-format", value)) {
            if (!setEnumFlag<DiagFormat>("diag-format", value,
                    { {"text", DiagFormat::Text}, {"json", DiagFormat::Json} },
                    compiler.getMutableFlags().diagFormat)) {
                printUsage(argv[0]); return 1;
            }
            // Phase-progress records ride the same NDJSON stream as the
            // diagnostics, so they turn on with it (and only with it — text
            // mode stays byte-for-byte as it was).
            cajeta::setJsonProgressEnabled(
                compiler.getFlags().diagFormat == DiagFormat::Json);
        } else if (match(arg, "source-tags",         value)) { if (!setBoolFlag("source-tags",         value, compiler.getMutableFlags().sourceTags))         { printUsage(argv[0]); return 1; } }
          else if (match(arg, "poison-free",         value)) { if (!setBoolFlag("poison-free",         value, compiler.getMutableFlags().poisonFree))         { printUsage(argv[0]); return 1; } }
          else if (match(arg, "drop-chain-validate", value)) { if (!setBoolFlag("drop-chain-validate", value, compiler.getMutableFlags().dropChainValidate))  { printUsage(argv[0]); return 1; } }
          else if (match(arg, "ub-traps",            value)) { if (!setBoolFlag("ub-traps",            value, compiler.getMutableFlags().ubTraps))            { printUsage(argv[0]); return 1; } }
          else if (match(arg, "use-after-move-rt",   value)) { if (!setBoolFlag("use-after-move-rt",   value, compiler.getMutableFlags().useAfterMoveRt))     { printUsage(argv[0]); return 1; } }
          else if (match(arg, "stack-trace-capture", value)) { if (!setBoolFlag("stack-trace-capture", value, compiler.getMutableFlags().stackTraceCapture))  { printUsage(argv[0]); return 1; } }
          else if (match(arg, "diag-hints",          value)) { if (!setBoolFlag("diag-hints",          value, compiler.getMutableFlags().diagHints))          { printUsage(argv[0]); return 1; } }
          else if (match(arg, "profile-counters",    value)) { if (!setBoolFlag("profile-counters",    value, compiler.getMutableFlags().profileCounters))    { printUsage(argv[0]); return 1; } }
          else if (match(arg, "lazy-scope",          value)) { if (!setBoolFlag("lazy-scope",          value, compiler.getMutableFlags().lazyScope))          { printUsage(argv[0]); return 1; } }
          else if (match(arg, "line-info",           value)) { if (!setBoolFlag("line-info",           value, compiler.getMutableFlags().lineInfo))           { printUsage(argv[0]); return 1; } }

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
        } else if (match(arg, "cache-manifest", value)) {
            // Incremental compilation: the build tool's clean/dirty
            // designation + cache slots (cache-manifest-v1). See
            // docs/specification/buildtool/IncrementalCompilation.md.
            compiler.setCacheManifestPath(value);
        } else if (arg == "--print-cache-discriminator") {
            printCacheDiscriminator = true;
        } else if (match(arg, "skill-root", value)) {
            // Where the package's hand-authored skills/ dir lives (skill-
            // discovery D.3). The build tool passes the PROJECT root here so
            // skills/ next to cajeta.json is embedded — the positional source
            // root is the deeper src/main/cajeta. Without it, skill embedding
            // falls back to the source root.
            compiler.setSkillRootOverride(value);
        } else if (match(arg, "target", value)) {
            compiler.setTargetTriple(value);
        } else if (match(arg, "cpu", value)) {
            compiler.setCpu(value);
        } else if (match(arg, "features", value)) {
            compiler.setFeatures(value);
        } else if (match(arg, "profile", value)) {
            // Active @Profile for component gating: CajetaModule includes a
            // @Profile-annotated component only when it matches the active
            // profile (profile-neutral components are always included). The
            // build tool forwards each task's `profile` here (dev/test/release/…).
            CajetaModule::setActiveProfile(value);
        } else if (match(arg, "source-date-epoch", value)) {
            compiler.getMutableFlags().sourceDateEpoch = value;
        } else if (match(arg, "debug-prefix-map", value)) {
            compiler.getMutableFlags().debugPrefixMap = value;
        } else if (match(arg, "seed", value)) {
            compiler.getMutableFlags().seed = value;
        } else if (match(arg, "link-mode", value)) {
            // Lean linker / DCE policy (plans/compiler/lean-linker-dce.md).
            // full = keep every class's reflection registration ctor (today's
            // behavior); lean = emit registration only for keep-set classes so
            // --gc-sections strips the rest. Intended default for --emit=exe is
            // lean (set from emitMode in 0b); `--link-mode=full` / `--keep-all`
            // is the opt-out.
            LinkMode lm;
            if (!setEnumFlag<LinkMode>("link-mode", value,
                    { {"full", LinkMode::Full},
                      {"lean", LinkMode::Lean} }, lm)) {
                printUsage(argv[0]); return 1;
            }
            compiler.getMutableFlags().linkMode = lm;
            linkModeExplicit = true;
        } else if (arg == "--keep-all") {
            // Alias for --link-mode=full (keep every class; no stripping).
            compiler.getMutableFlags().linkMode = LinkMode::Full;
            linkModeExplicit = true;
        } else if (match(arg, "why-kept", value)) {
            // Lean DCE diagnostic: report which reflection site/root kept the
            // named class (canonical name) in the keep-set.
            compiler.getMutableFlags().whyKept = value;
        } else if (match(arg, "keepset-json", value)) {
            // Lean DCE diagnostic: write the generated keep-set + provenance to
            // this path as JSON (lean builds only).
            compiler.getMutableFlags().keepsetJson = value;
        } else if (match(arg, "emit-xref", value)) {
            // Write the RESOLVED cross-reference index to this path as JSON, for
            // an IDE to consume rather than reimplementing Cajeta's name
            // resolution (specs/ide-symbol-index-spec.md §2).
            compiler.getMutableFlags().emitXref = value;
        } else if (arg == "--emit-xref") {
            // Bare form, lint mode only: emit xref records as NDJSON on the
            // diagnostic channel — one subprocess per edit returns diagnostics
            // AND xref (ide-symbol-index §1.5.2). "-" is the stream sentinel.
            compiler.getMutableFlags().emitXref = "-";
        } else if (match(arg, "tree-shake", value)) {
            // Tier-1 RTA (plans/compiler/stdlib-tree-shaking.md). `report`
            // (Phase A) computes IR reachability from the entry + roots and
            // prints what would be stripped — analysis only, no emission change.
            TreeShake ts;
            if (!setEnumFlag<TreeShake>("tree-shake", value,
                    { {"off", TreeShake::Off},
                      {"report", TreeShake::Report},
                      {"on", TreeShake::On} }, ts)) {
                printUsage(argv[0]); return 1;
            }
            compiler.getMutableFlags().treeShake = ts;
            treeShakeExplicit = true;
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
        } else if (arg == "--lint") {
            lintMode = true;
        } else if (arg == "--source-root") {
            if (i + 1 >= argc) {
                std::cerr << "cajeta: --source-root requires a path\n";
                return 1;
            }
            lintSourceRoot = argv[++i];
        } else if (arg == "--shadow") {
            if (i + 1 >= argc) {
                std::cerr << "cajeta: --shadow requires a path\n";
                return 1;
            }
            lintShadow = argv[++i];
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

    // --lint <file>: run the diagnostic passes over one file and stop before
    // codegen (compiler-lint-mode-spec). Distinct mode — takes the single
    // positional as the file, never the three-positional compile path.
    if (lintMode) {
        bool jsonDiag = compiler.getFlags().diagFormat == DiagFormat::Json;
        if (positional.empty()) {
            std::cerr << "cajeta: --lint requires a <file>\n";
            return 1;
        }
        const std::string& lintFile = positional[0];

        // --lint <directory> is the whole-root xref export (ide-symbol-index
        // §2.0.3): parse every file under the root, write one document. It
        // REQUIRES --emit-xref=<path> — "lint a directory, diagnostics only" is
        // not a mode, and guessing one file to lint would be a wrong answer
        // dressed as a right one.
        if (std::filesystem::is_directory(lintFile)) {
            const std::string& xrefOut = compiler.getFlags().emitXref;
            if (xrefOut.empty() || xrefOut == "-") {
                if (jsonDiag)
                    cajeta::emitJsonDiagnostic("error", "xref-root",
                        "linting a directory is the whole-root xref export; "
                        "it requires --emit-xref=<path>", lintFile);
                else
                    std::cerr << "cajeta: --lint <directory> requires "
                                 "--emit-xref=<path> (whole-root xref export)\n";
                return 1;
            }
            return compiler.lintRoot(lintFile) > 0 ? 1 : 0;
        }

        // Single-file lint emits xref on the DIAGNOSTIC channel (bare
        // --emit-xref). A path here would silently write a document scoped to
        // one file — refusing beats an expectation silently half-met.
        if (!compiler.getFlags().emitXref.empty()
                && compiler.getFlags().emitXref != "-") {
            if (jsonDiag)
                cajeta::emitJsonDiagnostic("error", "xref-lint",
                    "single-file lint emits xref on the diagnostic channel; "
                    "use bare --emit-xref, or --lint <root> --emit-xref=<path>",
                    lintFile);
            else
                std::cerr << "cajeta: with --lint <file>, use bare --emit-xref "
                             "(records ride the diagnostic stream); "
                             "--emit-xref=<path> is for --lint <root>\n";
            return 1;
        }

        if (!std::filesystem::exists(lintFile)) {
            if (jsonDiag)
                cajeta::emitJsonDiagnostic("error", "file-not-found",
                                           "no such file: " + lintFile, lintFile);
            else
                std::cerr << "cajeta: no such file: " << lintFile << "\n";
            return 1;
        }
        if (!lintSourceRoot.empty() && !std::filesystem::is_directory(lintSourceRoot)) {
            if (jsonDiag)
                cajeta::emitJsonDiagnostic("error", "source-root",
                                           "not a directory: " + lintSourceRoot);
            else
                std::cerr << "cajeta: --source-root not a directory: "
                          << lintSourceRoot << "\n";
            return 1;
        }
        // Collect-and-continue: recoverable semantic errors report to the engine
        // (multiple, located) instead of aborting; un-migrated throw sites are
        // caught and folded in. All emitted at the end (diagnostic-engine-spec).
        cajeta::DiagnosticEngine engine;
        cajeta::DiagnosticEngine::setActive(&engine);
        try {
            compiler.lint(lintFile, lintSourceRoot, lintShadow);
        } catch (cajeta::SyntaxErrorException&) {
            cajeta::DiagnosticEngine::setActive(nullptr);
            // The buffer did not parse, so nothing was captured for it — the
            // stream is a version line and records for nothing, which is the
            // signal for the plugin to KEEP its previous index for this file
            // (a stale answer beats a wrong one; ide-symbol-index §7).
            compiler.emitLintXrefStream(lintFile, lintSourceRoot, lintShadow);
            return 1;  // syntax diagnostics already emitted during parsing
        } catch (cajeta::Exception& e) {
            engine.report("error", e.getErrorId(), e.getMessage(),
                          e.getFile(), e.getLine(), e.getColumn());
        } catch (const std::exception& e) {
            engine.report("error", "", e.what());
        }
        cajeta::DiagnosticEngine::setActive(nullptr);
        engine.emit(jsonDiag);
        compiler.emitLintXrefStream(lintFile, lintSourceRoot, lintShadow);
        return engine.hasErrors() ? 1 : 0;
    }

    // Manifest builds force tree-shake off + link-mode full
    // (Compiler::setupCacheManifest); mirror that so the probed value keys
    // the same cache tree the actual manifest build will use.
    if (printCacheDiscriminator) {
        compiler.getMutableFlags().treeShake = TreeShake::Off;
        compiler.getMutableFlags().linkMode = LinkMode::Full;
        std::cout << compiler.computeOwnCacheDiscriminator() << "\n";
        return 0;
    }

    if (compiler.getFlags().emitXref == "-") {
        std::cerr << "cajeta: bare --emit-xref is lint-mode only "
                     "(records ride the diagnostic stream); a compile takes "
                     "--emit-xref=<path>\n";
        return 1;
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

    // Lean linking is the default for --emit=exe (bounded reflection makes it
    // sound; unbounded reflection degrades to a conservative keep). Other emit
    // modes (ir/obj/cja/uber) and an explicit --link-mode/--keep-all keep Full.
    if (compiler.getEmitMode() == EmitMode::Exe && !linkModeExplicit) {
        compiler.getMutableFlags().linkMode = LinkMode::Lean;
    }

    // Tree-shaking is ON by default for --emit=exe (Tier-1 RTA). Sound by
    // construction: the reachability set is a conservative over-approximation
    // (it only ever keeps too much; a wrong strip fails loud at link, never a
    // silent miscompile), and reflection keeps a class's bound's subtype closure
    // — `Class<Object>` keeps every class. Devs get the optimized binary without
    // opting in; `--tree-shake=off` is the escape hatch.
    if (compiler.getEmitMode() == EmitMode::Exe && !treeShakeExplicit) {
        compiler.getMutableFlags().treeShake = TreeShake::On;
    }

    bool jsonDiag = compiler.getFlags().diagFormat == DiagFormat::Json;
    try {
        compiler.compile(positional[0], positional[1], positional[2]);
    } catch (cajeta::SyntaxErrorException&) {
        // Per-error syntax diagnostics were already emitted during parsing
        // (NDJSON or console); fail the compile without re-reporting.
        return 1;
    } catch (cajeta::Exception& e) {
        emitException(e, jsonDiag);
        return 1;
    } catch (const std::exception& e) {
        if (jsonDiag) cajeta::emitJsonDiagnostic("error", "", e.what());
        else std::cerr << "cajeta: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
