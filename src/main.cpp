#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <llvm/Support/InitLLVM.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/TargetParser/Host.h>
#include "cajeta/compile/IrTools.h"
#include "cajeta/compile/Compiler.h"
#include "cajeta/prof/TraceSummary.h"
#include "cajeta/compile/CompilerMode.h"
#include "cajeta/compile/LintService.h"
#include "cajeta/prof/ProfileSelection.h"
#include "cajeta/error/Exception.h"
#include "cajeta/error/Diagnostics.h"
#include "cajeta/error/DiagnosticEngine.h"
#include "cajeta/buildtool/mcp/CompilerMcpServer.h"
#include "cajeta/cli/ArchiveCommands.h"
#include "cajeta/cli/DocCommand.h"
#include "cajeta/cli/IdeCommands.h"
#include "cajeta/cli/NativeCommands.h"
#include "cajeta/cli/StdlibCommands.h"
#include "cajeta/cli/XpuProfileCommand.h"
#include "cajeta/jit/CajetaJitHost.h"
#include "cajeta/dap/DapServer.h"
#include "cajeta/kernel/KernelMain.h"
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
              << "  deps               Print the dependency tree (--format=text|json|csv).\n"
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
              << "  kernel [-f <file>] Jupyter kernel over ZeroMQ. Started by a notebook frontend;\n"
              << "                     install its kernelspec with `cajeta init --kernel`.\n"
              << "  compiler-mcp       Model Context Protocol server over stdio, serving skills\n"
              << "                     (searchSkills | listSkills | getSkills) to a coding agent.\n"
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
              << "  --profiler=off|instrument            Exact call counts + inclusive time (default off).\n"
              << "                                       A separate tier from sampling: probes are emitted\n"
              << "                                       into the build, so it needs a rebuild and costs\n"
              << "                                       nothing when off.\n"
              << "  --profiler-select=<file>             Limit instrumentation to the classes this file\n"
              << "                                       selects. One `include <pattern>` or\n"
              << "                                       `exclude <pattern>` per line; `**` crosses package\n"
              << "                                       boundaries, `*` does not. Include defines the\n"
              << "                                       universe, exclude subtracts. Unselected code carries\n"
              << "                                       NO probe, so a narrow selection is an overhead cut.\n"
              << "\n"
              << "Lint / IDE (compiler-lint-mode-spec, lint-server-spec):\n"
              << "  --lint <file>                        Diagnostics-only: run the semantic passes over one\n"
              << "                                       file and stop before codegen. Pair with --emit-xref\n"
              << "                                       (bare) for xref records on the diagnostic channel.\n"
              << "  --lint <dir> --emit-xref=<path>      Whole-root xref export: one document over every file.\n"
              << "  --lint <dir> --list-profiles         Report the DI profiles the project declares (@Profile)\n"
              << "                                       as {\"profiles\":[...]} on stdout. Front-end only.\n"
              << "  --source-root <dir>                  Project context for --lint: sibling files are parsed\n"
              << "                                       for their signatures so cross-file references resolve.\n"
              << "  --shadow <path>                      On-disk file the linted (staged) buffer stands in for;\n"
              << "                                       records report against this original path.\n"
              << "  --lint-server                        Warm lint daemon: prime the stdlib once, then read\n"
              << "                                       NDJSON lint requests on stdin and answer warm on stdout\n"
              << "                                       (proto 1.0). Honors --source-root / --diag-format;\n"
              << "                                       emitXref is a per-request field. See lint-server-spec.\n"
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
              << "  --cpu=<name>                         Target CPU. Default: native (host cpu + detected features).\n"
              << "                                       Pass a named cpu (or `generic`) for a portable binary; a .cja\n"
              << "                                       carries bitcode, so libraries stay portable either way.\n"
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
    // Resolve --diag-format BEFORE any verb dispatches (compiler-jsonl 5.1.2).
    // `jit-run` and `dap` return from this function long before the flag-parse
    // loop below ever runs, which is precisely why the same flag used to mean
    // different things depending on the verb: a compile got the full stream,
    // jit-run got only a CAJETA_DIAG_FORMAT export for the runtime's
    // uncaught-throw emitter, and nothing else got anything. Resolving here
    // makes it one flag with one meaning; the loop below still parses it for
    // the compile path's own flags struct, and agrees by construction.
    cajeta::resolveDiagFormatFromArgv(argc, argv);

    // Top-level subcommand dispatch. `cajeta archive ...` routes to
    // the archive-management surface (docs/ArchiveManagement.md);
    // `cajeta info` (and eventually `build`, `test`, ...) routes to
    // the build-tool surface (docs/BuildTool.md). Anything not
    // claimed by either falls through to the legacy compile flow below.
    if (argc >= 2 && std::string(argv[1]) == "archive") {
        return cajeta::dispatchArchive(argc, argv);
    }

    // `cajeta lower` / `cajeta disasm` — the LLVM tool surface tools built on
    // cajeta's IR need, served by the compiler's own linked-in LLVM.
    //
    // Dispatched BEFORE the build-tool task probe, so a manifest task of
    // either name cannot shadow the verb — the same reason `run` sits above
    // it. See IrTools.h for why these exist rather than telling callers to
    // install a matching llc.
    if (argc >= 2 && std::string(argv[1]) == "lower") {
        return cajeta::irLowerCommand(argc, argv);
    }
    if (argc >= 2 && std::string(argv[1]) == "disasm") {
        return cajeta::irDisasmCommand(argc, argv);
    }

    // `cajeta run <file>.cajeta` — compile the file as a script unit and
    // execute it as a one-unit session under the JIT host (script-units
    // spec §7). Dispatched BEFORE the build-tool task probe (and excluded
    // from it), so a manifest task named "run" can never shadow the verb.
    if (argc >= 2 && std::string(argv[1]) == "run") {
        return cajeta::jit::dispatchRun(argc, argv);
    }

    // `cajeta profile summary <trace>` — per-kernel totals from a .pftrace
    // (cajeta-profiler 14.4). Above the build-tool task probe for the same
    // reason `run` is: a manifest task named "profile" must not shadow the
    // verb, and samples/profile is exactly such a project.
    if (argc >= 2 && std::string(argv[1]) == "profile") {
        return cajeta::prof::dispatchProfile(argc, argv);
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
        // Announce the stream before the server says anything, so its
        // stderr is a well-formed stream and not records with no version.
        cajeta::emitStreamRecordOnce();
        cajeta::dap::DapServer server;
        return server.runOverStdio();
    }

    // `cajeta kernel [-f <connection-file>]` — Jupyter v5.3 kernel over
    // ZeroMQ (specs/jupyter-kernel-spec.md §3). Jupyter Lab starts this from
    // the kernelspec `cajeta init --kernel` installs. Its stdout belongs to
    // the CELLS being run, which the session captures per cell, so nothing
    // here may print to stdout after the connection banner.
    if (argc >= 2 && std::string(argv[1]) == "kernel") {
        return cajeta::kernel::dispatchKernel(argc, argv);
    }

    // `cajeta compiler-mcp` — MCP stdio server for skill discovery
    // (specs/archive/compiler-mcp-spec.md). stdout is the protocol channel;
    // diagnostics go to stderr.
    if (argc >= 2 && std::string(argv[1]) == "compiler-mcp") {
        auto server = cajeta::buildtool::mcp::CompilerMcpServer::create(
            CAJETA_VERSION, ".");
        if (!server) {
            std::cerr << "cajeta compiler-mcp: "
                      << llvm::toString(server.takeError()) << "\n";
            return 1;
        }
        return server->run(std::cin, std::cout);
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
    bool lintServerMode = false; // --lint-server: warm NDJSON lint daemon (lint-server-spec)
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

    // --list-profiles: report declared DI profiles and exit (IDE support).
    bool listProfiles = false;
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
        } else if (match(arg, "profiler", value)) {
            std::string pErr;
            if (!cajeta::applyProfiler(value, compiler.getMutableFlags(), &pErr)) {
                std::cerr << "cajeta: " << pErr << "\n";
                printUsage(argv[0]); return 1;
            }
        } else if (match(arg, "profiler-select", value)) {
            // The FILE's CONTENTS land in the flags, not its path
            // (cajeta-profiler §3.10). The path is kept only for the trace
            // record and diagnostics, so two build roots that read the same
            // selection from different paths still share cached objects, and
            // an in-place edit of one cannot alias the previous probe set.
            std::ifstream selIn(value, std::ios::binary);
            if (!selIn) {
                std::cerr << "cajeta: cannot read --profiler-select file: "
                          << value << "\n";
                return 1;
            }
            std::ostringstream selBuf;
            selBuf << selIn.rdbuf();
            compiler.getMutableFlags().profilerSelect = selBuf.str();
            compiler.getMutableFlags().profilerSelectOrigin = value;
            std::vector<std::string> selErrors;
            cajeta::prof::ProfileSelection::parse(
                compiler.getFlags().profilerSelect, &selErrors);
            if (!selErrors.empty()) {
                for (const auto& e : selErrors) std::cerr << "cajeta: " << e << "\n";
                return 1;
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
        } else if (arg == "--list-profiles") {
            // Report the DI profiles this project declares (@Profile), for an
            // IDE to offer instead of making the developer remember them.
            listProfiles = true;
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
        } else if (arg == "--lint-server") {
            lintServerMode = true;
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

    // --lint-server: the warm NDJSON lint daemon (lint-server-spec §2).
    // Distinct mode — reads requests on stdin, never a positional file; the
    // xref stream is a per-REQUEST toggle, so a CLI --emit-xref here is a
    // category error. Refuse the nonsensical combos before priming so the
    // plugin never sees a half-started server.
    if (lintServerMode) {
        bool jsonDiag = compiler.getFlags().diagFormat == DiagFormat::Json;
        if (!compiler.getFlags().emitXref.empty()) {
            if (jsonDiag)
                cajeta::emitJsonDiagnostic("error", "xref-server",
                    "--emit-xref is a per-request field in server mode "
                    "(\"emitXref\":true), not a CLI flag");
            else
                std::cerr << "cajeta: --lint-server takes emitXref as a "
                             "per-request field, not a CLI --emit-xref flag\n";
            return 1;
        }
        if (!positional.empty()) {
            if (jsonDiag)
                cajeta::emitJsonDiagnostic("error", "server-positional",
                    "--lint-server reads requests on stdin; it takes no "
                    "positional file", positional[0]);
            else
                std::cerr << "cajeta: --lint-server takes no positional file "
                             "(requests arrive on stdin)\n";
            return 1;
        }
        if (!lintSourceRoot.empty()
                && !std::filesystem::is_directory(lintSourceRoot)) {
            if (jsonDiag)
                cajeta::emitJsonDiagnostic("error", "source-root",
                                           "not a directory: " + lintSourceRoot);
            else
                std::cerr << "cajeta: --source-root not a directory: "
                          << lintSourceRoot << "\n";
            return 1;
        }
        cajeta::lintservice::ServerOptions opts;
        opts.sourceRoot = lintSourceRoot;
        // --classpath is start-time context here, like --source-root: the
        // server builds a fresh Compiler per request and each one needs the
        // dependency archives, or every reference into a dependency lints as
        // an unknown type (Julian, 2026-07-31 — `Logger` red-underlined while
        // one-shot lint of the same buffer was clean).
        opts.classpath = compiler.getClasspath();
        opts.jsonDiagnostics = jsonDiag;
        opts.classpath = compiler.getClasspath();
        return cajeta::lintservice::runLintServer(opts);
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
        if (listProfiles && std::filesystem::is_directory(lintFile)) {
            // Front-end parse only — profiles are an annotation fact, so no
            // codegen is needed to know them. Reports what the COMPILER sees,
            // which is the point: a plugin-side text scan would have to
            // re-implement the three @Profile spellings and would still miss
            // profiles declared inside a dependency archive.
            compiler.lintRoot(lintFile);
            std::set<std::string> profiles;
            auto modules = compiler.getModules();   // by value (see the header)
            for (auto& module : modules) {
                if (!module) continue;
                for (auto& [canonical, klass] : module->getStructures()) {
                    if (!klass) continue;
                    for (auto& inst : klass->getAnnotationInstances()) {
                        if (!inst || !inst->getName()) continue;
                        if (inst->getName()->getTypeName() != "Profile") continue;
                        // Three spellings, all handled by the same accessors
                        // the DI selection uses: @Profile("dev"),
                        // @Profile({"dev","test"}), and repeated @Profile.
                        const std::vector<std::string>& list = inst->getStringList();
                        if (!list.empty()) {
                            for (auto& one : list)
                                if (!one.empty()) profiles.insert(one);
                        } else {
                            const std::string& one = inst->getString();
                            if (!one.empty()) profiles.insert(one);
                        }
                    }
                }
            }
            // A JSON document on stdout, like --emit-xref writes one to a
            // path: this is a query answering with data, not a diagnostic
            // stream. Sorted so the IDE's list is stable between runs.
            std::cout << "{\"profiles\":[";
            bool first = true;
            for (const auto& p : profiles) {
                if (!first) std::cout << ",";
                std::cout << "\"" << p << "\"";
                first = false;
            }
            std::cout << "]}" << std::endl;
            return 0;
        }

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
        // Engine lifecycle, exception folding, and emit order live in
        // runLintDriver, shared verbatim with the warm-lint path
        // (lint-server spec 1.4.1: parity by construction).
        return cajeta::lintservice::runLintDriver(
            compiler, lintFile, lintSourceRoot, lintShadow);
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

    // ...and the HIGH side, which went unguarded until 2026-08-27. Only the
    // first three positionals are ever read, so a fourth was discarded in
    // silence -- and the damage was not the discard but the SHIFT: someone
    // reaching for a two-source-root build (`... Main.main srcA srcB out`) got
    // srcB bound to the output directory. Exit 0, `out` left empty, object
    // files written into a source tree, and not one of srcB's types compiled.
    // It reads as a type-resolution failure, and cost a day of chasing one.
    //
    // A compile has ONE source root by construction; a second tree is a
    // dependency, so the remedy is --classpath, and the diagnostic has to say
    // so or the user's next move is another guess.
    if (positional.size() > 3) {
        static const char* kSlot[3] = {"entry.method", "source-root ",
                                       "output-dir  "};
        std::cerr << "cajeta: the compile verb takes exactly 3 positional "
                     "arguments, got " << positional.size() << ".\n";
        for (size_t i = 0; i < positional.size(); ++i) {
            if (i < 3) {
                std::cerr << "  " << kSlot[i] << " : " << positional[i]
                          << (i == 2 ? "   <-- read as the OUTPUT DIRECTORY\n"
                                     : "\n");
            } else {
                std::cerr << "  ignored      : " << positional[i] << "\n";
            }
        }
        std::cerr << "  A compile reads ONE source root. To build across two "
                     "source trees, emit the first\n"
                     "  as an archive (--emit=cja) and compile the second "
                     "against it with --classpath=<a>.cja.\n";
        return 1;
    }

    // build-output-layout §4.1: generated files never land in a source tree.
    //
    // The arity check above closes one way in. This closes the rest: whatever
    // path arrives at the output position, if it holds cajeta SOURCES it is a
    // source tree and we refuse it. That is the precise hazard — 180 object
    // files landed in cajeta-cabra/src and 75 in cajeta-llm/src on
    // 2026-08-27, at exit 0, and were then committed by a routine `git add`.
    //
    // Keyed on "contains sources" rather than on containment against the
    // source root, which over-fires: source root `.` with output `./build` is
    // ordinary and correct, and a containment rule rejects it. A build
    // directory holds no sources, so it can never trip this.
    //
    // §3.4 settled that output destinations belong to the BUILD TOOL and the
    // compiler keeps its bare positional, so for every script that invokes
    // `cajeta` directly this guard is the whole of the protection.
    {
        const std::string& outDir = positional[2];
        std::error_code ec;
        if (std::filesystem::is_directory(outDir, ec)) {
            // RECURSIVE: a source root normally holds package directories,
            // not loose files — `src/main/cajeta` contains `dev/`, and the
            // .cajeta files sit several levels down. An immediate-children
            // scan sees only directories and waves the source tree through.
            // Bounded so a large pre-existing output directory cannot make
            // this expensive; a source tree hits a .cajeta long before the
            // cap.
            std::string offender;
            int examined = 0;
            constexpr int kMaxExamined = 20000;
            std::filesystem::recursive_directory_iterator it(
                outDir,
                std::filesystem::directory_options::skip_permission_denied,
                ec), end;
            for (; !ec && it != end && offender.empty(); it.increment(ec)) {
                if (++examined > kMaxExamined) break;
                if (it->path().extension() == ".cajeta"
                        && it->is_regular_file(ec)) {
                    offender = std::filesystem::relative(it->path(), outDir, ec)
                        .string();
                    if (offender.empty()) {
                        offender = it->path().filename().string();
                    }
                }
            }
            if (!offender.empty()) {
                std::cerr
                    << "cajeta: refusing to write build output into a source "
                       "tree.\n"
                       "  output-dir : " << outDir << "\n"
                       "  it holds   : " << offender << " (and possibly more "
                       ".cajeta sources)\n"
                       "  Generated files do not belong beside sources. Point "
                       "the output directory at\n"
                       "  a build location (e.g. build/), which is what the "
                       "build tool does by default.\n";
                return 1;
            }
        }
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
    // Announce the stream before the compile says anything (compiler-jsonl
    // 2.1.3). Emitted here rather than at the flag parse because the earlier
    // verbs return before this point and each needs its own treatment:
    // `--lint` and `--lint-server` must stay byte-identical to EACH OTHER
    // (lint-server-spec 1.4.1), so their envelope is emitted by the shared
    // driver instead.
    cajeta::emitStreamRecordOnce();

    // Collect-and-continue for full compile too: migrated resolution sites report
    // to the engine (recovering) instead of aborting; the codegen loop + emit are
    // gated on the engine having no errors (Compiler::compile). All diagnostics
    // are emitted here at the end (collect-continue-compile-spec).
    cajeta::DiagnosticEngine engine;
    cajeta::DiagnosticEngine::setActive(&engine);
    try {
        compiler.compile(positional[0], positional[1], positional[2]);
    } catch (cajeta::SyntaxErrorException&) {
        cajeta::DiagnosticEngine::setActive(nullptr);
        cajeta::emitJsonResult("error", "syntax errors");
        return 1;  // syntax diagnostics already emitted during parsing
    } catch (cajeta::Exception& e) {
        engine.report("error", e.getErrorId(), e.getMessage(),
                      e.getFile(), e.getLine(), e.getColumn());
    } catch (const std::exception& e) {
        engine.report("error", "", e.what());
    }
    cajeta::DiagnosticEngine::setActive(nullptr);
    engine.emit(jsonDiag);
    // One terminal record, last, on every exit path (compiler-jsonl 9.4): "did
    // it work" must be answerable from the stream alone, never inferred from an
    // exit code plus silence. No-op in text mode. Emitted AFTER engine.emit so
    // it is genuinely the last record, and keyed on the engine's verdict —
    // collect-and-continue means a failure can arrive without an exception.
    const bool failed = engine.hasErrors();
    cajeta::emitJsonResult(failed ? "error" : "ok");
    return failed ? 1 : 0;
}
