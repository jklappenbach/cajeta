//
// CompilerMode + per-feature toggle struct.
//
// Spec: docs/CompilerModes.md. The flavor flags (`--debug`, `--release`,
// `--fast`, `--debug-release`, `--minimal`) on the CLI expand into a
// CompilerFlags struct, which holds one field per toggleable feature. Per-
// feature CLI overrides (`--bounds=on`, `--source-tags=on`, etc.) override
// the flavor default after expansion.
//
// Adding a new feature:
//   1. Add an entry to the relevant enum below (or a new bool field).
//   2. Add a default for each CompilerMode in CompilerFlags::defaultsForMode.
//   3. Add CLI parsing in src/main.cpp (mirror the --bounds= pattern).
//   4. Consume from CajetaModule::getFlags() at the codegen site.
//

#pragma once

#include <string>

namespace cajeta {

    enum class CompilerMode {
        Debug,          // newcomer-friendly: source tags, poison-on-free, strict live-set
        DebugRelease,   // release perf + debug visibility (PGO-collection profile)
        Release,        // default-on safety nets, no diagnostic instrumentation
        Fast,           // -O3, drop all checks that aren't load-bearing for correctness
        Minimal,        // smallest binary; you've reasoned about safety yourself
    };

    enum class BoundsCheck {
        On,             // compare+branch, throw IndexOutOfBoundsException
        Off,            // no check; out-of-bounds is UB
        Trap,           // compare+branch to @llvm.trap (SIGILL, no unwind)
    };

    enum class LiveSet {
        Strict,         // unbounded growth + rehash; assert on duplicates (debug)
        Bounded,        // fixed capacity, warn-and-leak past load cap (release)
        Off,            // no tracking; aliased fields will double-free (minimal)
    };

    enum class OverflowChecks {
        On,             // branch + trap on signed overflow
        Off,            // UB; compiler may assume no overflow
        Wrapping,       // well-defined two's-complement modular arithmetic
    };

    enum class NullChecks {
        On,             // compare+branch, throw NullPointerException
        Off,            // no check; null-deref is UB
        Trap,           // compare+branch to @llvm.trap
    };

    enum class DiagVerbosity {
        Terse,          // one line per diagnostic, CI-friendly
        Normal,
        Verbose,        // code samples + suggested fixes + doc URLs
    };

    // Compiler-diagnostic output format (the `--diag-format` flag; docs/
    // CompilerModes.md § --diag-format). Text: human-readable stderr (the
    // unchanged default). Json: one NDJSON diagnostic object per line on stderr
    // for machine consumers (the IntelliJ plugin, the build tool) so they parse
    // structured diagnostics instead of regex-scraping free text.
    enum class DiagFormat { Text, Json };

    // LLVM IR optimization level for generated user code (the `--opt` flag).
    // Cajeta historically ran NO IR optimization on user code (only codegen);
    // O0 preserves that. O2/O3 run the full per-module pipeline (incl.
    // LoopVectorize + SLP). Applies to --emit=obj/exe; see compile/Optimizer.h.
    enum class OptLevel { O0, O1, O2, O3 };

    // Link-time optimization policy (the `--lto` flag). Off: each module is
    // compiled to a native object and linked — no cross-module inlining (a
    // stdlib-hot-path method can never inline into user code). Thin: each module
    // is emitted as ThinLTO bitcode + summary and the cross-module import +
    // backend runs at link, so e.g. `@Inline` `ArrayList.add` folds into a user
    // append loop. Full reserved (monolithic LTO) — not yet wired; treated as
    // Thin. ThinLTO uses the fork's version-matched lld (system lld may differ).
    enum class LtoMode { Off, Thin, Full };

    // Lean linker / DCE policy (the `--link-mode` flag; plans/compiler/
    // lean-linker-dce.md). Full keeps every class's reflection registration
    // ctor (today's behavior — keep-everything, defeats --gc-sections); Lean
    // emits a class's registration ctor only when the class is in the generated
    // keep-set, so unkept classes lose their llvm.global_ctors anchor and
    // section-GC strips them. Lean is the intended default for --emit=exe (set
    // in Compiler from emitMode, not from CompilerMode); Full is the default
    // everywhere else (JIT/obj/cja) and the `--link-mode=full` / `--keep-all`
    // opt-out. Step 0a wires the toggle + the gate with a keep-all keep-set
    // (inert); 0b narrows it via the reachability BFS.
    enum class LinkMode { Full, Lean };

    // --tree-shake (Tier-1 RTA; plans/compiler/stdlib-tree-shaking.md). Report is
    // Phase A (analysis only); On is Phase B (prune unreachable cajeta method
    // bodies so the linker drops their native deps, e.g. OpenSSL). Default Off.
    enum class TreeShake { Off, Report, On };

    // --debug-info=off|line|full (specs/external-debug-spec.md §2). One switch
    // over the two independent codegen toggles below it:
    //   Off  — nothing: no shadow stack, no safepoints, no local records.
    //   Line — the shadow stack + #FrameDesc only, so a captured trace still
    //          resolves to Type.method(File.cajeta:NN). Cheapest useful level,
    //          and the default.
    //   Full — adds __cajeta_dbg_safepoint / __cajeta_dbg_local, the embedded
    //          location table, and forced RTTI retention. What an external
    //          debugger needs.
    enum class DebugInfo { Off, Line, Full };

    struct CompilerFlags {
        // ----- safety nets (runtime checks) -----
        BoundsCheck     bounds              = BoundsCheck::On;
        NullChecks      nullChecks          = NullChecks::On;
        OverflowChecks  overflowChecks      = OverflowChecks::On;

        // ----- diagnostic instrumentation -----
        bool            sourceTags          = true;   // CompilerModes.md § Source-tagged drop-chain entries
        bool            poisonFree          = true;   // memset freed bytes with sentinel
        LiveSet         liveSet             = LiveSet::Strict;
        bool            dropChainValidate   = true;   // per-push/pop integrity checks
        bool            ubTraps             = true;   // explicit trap before would-be UB
        bool            useAfterMoveRt      = true;   // runtime backup for the static checker
        bool            stackTraceCapture   = true;   // backtrace(3) at throw site

        // ----- compiler diagnostic surface -----
        DiagVerbosity   diagVerbosity       = DiagVerbosity::Verbose;
        bool            diagHints           = true;   // "did you mean...", etc.
        DiagFormat      diagFormat          = DiagFormat::Text;  // --diag-format (machine-readable diagnostics; mode-independent)

        // ----- profiling -----
        bool            profileCounters     = false;  // PGO-collection instrumentation

        // ----- optimization -----
        OptLevel        opt                 = OptLevel::O0;  // IR opt for --emit=obj/exe
        LtoMode         lto                 = LtoMode::Off;  // cross-module LTO for --emit=exe

        // ----- lean linker / DCE -----
        // Class-registration link policy (the --link-mode flag). Defaults Full
        // (keep every class's registration ctor — today's behavior); Compiler
        // flips it to Lean for --emit=exe unless the user opts out. Mode-
        // independent, so defaultsForMode leaves it Full for every CompilerMode.
        LinkMode        linkMode            = LinkMode::Full;
        // --why-kept=<canonical class>: print which reflection site/root kept the
        // named class in the lean keep-set (empty = off). Diagnostic only.
        std::string     whyKept             = "";
        // --keepset-json=<path>: write the generated keep-set + provenance to this
        // JSON file (empty = off; lean builds only).
        std::string     keepsetJson         = "";
        // --emit-xref=<path>: write the compiler's RESOLVED cross-reference index
        // (declarations, inheritance, references, overrides, calls) to this JSON
        // file (empty = off). Opt-in: a build that does not ask for it pays nothing.
        // The IDE consumes this instead of reimplementing Cajeta's resolution in
        // Kotlin — see specs/ide-symbol-index-spec.md §2 and
        // specs/schemas/cajeta-xref-v1.schema.json.
        std::string     emitXref            = "";

        // ----- tree-shaking (Tier-1 RTA; plans/compiler/stdlib-tree-shaking.md) -----
        // --tree-shake=off|report|on. On (Phase B/C + Tier-1.5) prunes unreachable
        // method bodies + dead clinits; Report (Phase A) just prints the analysis.
        // This field defaults Off, but main.cpp flips it to On for --emit=exe unless
        // the user passed --tree-shake (mirrors the lean-linker default). Sound by
        // construction (conservative reachability; Class<Object> keeps every class).
        TreeShake       treeShake           = TreeShake::Off;

        // ----- debugging -----
        // The requested level. `debugInfo` and `lineInfo` below are the derived
        // bools the codegen guards read; applyDebugInfo() keeps all three in
        // step. They remain independently settable (--line-info=off after a
        // --debug-info=line, say) — last flag on the command line wins.
        DebugInfo       debugInfoLevel      = DebugInfo::Line;

        // Emit __cajeta_dbg_safepoint(loc_id) at each statement boundary so the
        // in-process debugger (`cajeta dap`) can park the executing fiber at a
        // breakpoint. Opt-in via --debug-info=full / -g; OFF for every mode by
        // default (it changes codegen and only matters under a debugger), so
        // ordinary builds and the existing test suite are unaffected.
        bool            debugInfo           = false;

        // The SAFEPOINT half of the above, on its own. `--debug-info=full`
        // turns both on (applyDebugInfo keeps them in step), but a consumer
        // that wants only statement boundaries can ask for just this.
        //
        // Split out for jupyter-kernel U6: the notebook interrupt is taken at
        // a safepoint, so kernel cells need these — but `debugInfo` also
        // calls `noteForceAll("--debug-info=full")`, which retains the entire
        // class registry, and that dragged every stdlib class into a cell's
        // compile. The first cell then died on `unknown field type
        // 'bfloat16'` from a stdlib class that does not compile from source
        // in that world. The kernel wants a place to stop, not a debugger's
        // worth of metadata, and now it can say so.
        bool            safepoints          = false;

        // A would-be-UB trap (divide by zero, shift past the width, signed
        // overflow) UNWINDS to the session guard instead of executing
        // `llvm.trap`. Off everywhere but a Jupyter cell, and never implied
        // by a mode: a program that divides by zero should stop at the trap,
        // which is the whole point of `ubTraps`.
        //
        // A notebook is the case where that is the wrong answer. `4 / 0` is
        // among the most ordinary things a person types by accident, and a
        // `ud2` takes the kernel, every binding and every earlier cell with
        // it — the same hole Unit 4 closed for throws, reached by a route
        // that goes around the exception machinery entirely. The trap site
        // calls the runtime first and traps only if that RETURNS, so nothing
        // changes for a module compiled outside a session.
        bool            trapsUnwind         = false;

        // Emit the line-info shadow-stack calls (__cajeta_line_enter/mark/leave)
        // + a per-method #FrameDesc so a captured stack trace resolves to
        // Package.Class.method(File.cajeta:NN) with NO debug info (diagnostic-
        // exceptions §5/§8). Default ON in every flavor (semantic traces are the
        // ergonomic default); --line-info=off drops all of it for zero cost.
        // Perf: v1 marks each statement with a call — measure before relying on
        // default-on in release (see the plan's Unit 3 perf note).
        bool            lineInfo            = true;

        // ----- experimental perf -----
        // Skip the per-method prologue __cajeta_scope_enter() (the implicit
        // function-body structured-concurrency frame). That frame heap-allocs
        // on EVERY call but is only needed when the body has a bare `spawn`.
        // PROTOTYPE/UNSAFE: this prototype simply omits it, so it is correct
        // only for spawn-free code (measures the alloc win; the safe version
        // lazily pushes the frame at spawn sites). OFF by default.
        bool            lazyScope           = false;

        // ----- reproducible builds -----
        // Accepted from the build tool's reproducibility flag set
        // (Reproducibility.cpp). Stored so the emit stage can honor them where
        // it embeds timestamps / source paths / RNG salt; harmless when empty.
        //   --source-date-epoch=<unix-ts>   fixed build timestamp (SOURCE_DATE_EPOCH).
        //   --debug-prefix-map=<from>=<to>  remap source paths in debug info.
        //   --seed=<hex>                    deterministic salt for any build RNG.
        std::string     sourceDateEpoch;
        std::string     debugPrefixMap;
        std::string     seed;

        // Compute the default flag set for a given mode. CLI per-feature
        // flags override after this expansion.
        static CompilerFlags defaultsForMode(CompilerMode mode) {
            CompilerFlags f;
            switch (mode) {
                case CompilerMode::Debug:
                    // Already matches struct defaults above.
                    break;
                case CompilerMode::DebugRelease:
                    f.poisonFree         = false;
                    f.profileCounters    = true;   // canonical PGO-collection build
                    f.opt                = OptLevel::O2;
                    break;
                case CompilerMode::Release:
                    f.sourceTags         = false;
                    f.poisonFree         = false;
                    f.liveSet            = LiveSet::Bounded;
                    f.dropChainValidate  = false;
                    f.ubTraps            = false;
                    f.useAfterMoveRt     = false;
                    f.overflowChecks     = OverflowChecks::Wrapping;
                    f.stackTraceCapture  = false;
                    f.diagVerbosity      = DiagVerbosity::Normal;
                    f.diagHints          = false;
                    f.opt                = OptLevel::O2;
                    break;
                case CompilerMode::Fast:
                    f.bounds             = BoundsCheck::Off;
                    f.sourceTags         = false;
                    f.poisonFree         = false;
                    f.liveSet            = LiveSet::Bounded;
                    f.dropChainValidate  = false;
                    f.ubTraps            = false;
                    f.useAfterMoveRt     = false;
                    f.overflowChecks     = OverflowChecks::Wrapping;
                    f.stackTraceCapture  = false;
                    f.diagVerbosity      = DiagVerbosity::Normal;
                    f.diagHints          = false;
                    f.opt                = OptLevel::O3;
                    break;
                case CompilerMode::Minimal:
                    f.bounds             = BoundsCheck::Off;
                    f.nullChecks         = NullChecks::Off;
                    f.sourceTags         = false;
                    f.poisonFree         = false;
                    f.liveSet            = LiveSet::Off;
                    f.dropChainValidate  = false;
                    f.ubTraps            = false;
                    f.useAfterMoveRt     = false;
                    f.overflowChecks     = OverflowChecks::Wrapping;
                    f.stackTraceCapture  = false;
                    f.diagVerbosity      = DiagVerbosity::Terse;
                    f.diagHints          = false;
                    break;
            }
            return f;
        }
    };

    // --debug-info=off|line|full → the level plus the two derived bools. Returns
    // false on an unknown value, leaving `f` untouched and (when given) filling
    // `error` with a message naming the accepted set.
    inline bool applyDebugInfo(const std::string& value, CompilerFlags& f,
                               std::string* error) {
        DebugInfo level;
        if      (value == "off")  level = DebugInfo::Off;
        else if (value == "line") level = DebugInfo::Line;
        else if (value == "full") level = DebugInfo::Full;
        else {
            if (error)
                *error = "unrecognized value for --debug-info: " + value +
                         " (expected off|line|full)";
            return false;
        }
        f.debugInfoLevel = level;
        f.debugInfo      = (level == DebugInfo::Full);
        // Full debug info implies safepoints — that is what a debugger stops
        // at. Kept in step here so `--debug-info=full` behaves exactly as it
        // did before the two were separable.
        f.safepoints     = f.debugInfo;
        f.lineInfo       = (level != DebugInfo::Off);
        return true;
    }

    inline const char* debugInfoName(DebugInfo level) {
        switch (level) {
            case DebugInfo::Off:  return "off";
            case DebugInfo::Line: return "line";
            case DebugInfo::Full: return "full";
        }
        return "line";
    }

}
