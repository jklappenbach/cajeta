// CompilerMode and the per-feature toggle struct (docs/CompilerModes.md): a flavor flag
// expands into CompilerFlags, one field per feature, and the per-feature CLI overrides
// (`--bounds=on`, ...) are applied after that expansion.

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

    // --diag-format. Text is human-readable stderr; Json is one NDJSON object per line
    // for the plugin and build tool to parse instead of scraping free text.
    enum class DiagFormat { Text, Json };

    // --opt, the IR optimization level for user code on --emit=obj/exe: O0 runs none, as
    // cajeta always did; O2 and O3 run the full per-module pipeline.
    enum class OptLevel { O0, O1, O2, O3 };

    // --lto. Off links native objects with no cross-module inlining; Thin emits ThinLTO
    // bitcode so an `@Inline` stdlib method folds into user code. Full is treated as Thin.
    enum class LtoMode { Off, Thin, Full };

    // --link-mode. Full keeps every class's registration ctor, defeating --gc-sections;
    // Lean emits one only for a kept class, so section-GC strips the rest.
    enum class LinkMode { Full, Lean };

    // --tree-shake. Report analyses only; On prunes unreachable method bodies so the
    // linker can drop their native dependencies.
    enum class TreeShake { Off, Report, On };

    // --debug-info, one switch over the two toggles below. Line, the default, resolves a
    // trace to Type.method(File.cajeta); Full adds what a debugger needs; Off emits none.
    enum class DebugInfo { Off, Line, Full };

    // --profiler. Instrument emits a per-method enter/exit probe pair for exact counts,
    // a separate tier from sampling and from `profileCounters`, which is PGO collection.
    enum class Profiler { Off, Instrument };

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

        // Off in every mode: an un-asked-for build stays byte-identical to an older one.
        Profiler        profiler            = Profiler::Off;

        // --profiler-select: the CONTENTS, never the path, because the cache key hashes
        // these bytes; on a path it would serve objects probed to an edited selection.
        std::string     profilerSelect      = "";

        // Where the selection came from, deliberately OUT of the cache key.
        std::string     profilerSelectOrigin = "";

        // ----- optimization -----
        OptLevel        opt                 = OptLevel::O0;  // IR opt for --emit=obj/exe
        LtoMode         lto                 = LtoMode::Off;  // cross-module LTO for --emit=exe

        // ----- lean linker / DCE -----
        // Mode-independent, so defaultsForMode leaves it Full: Compiler flips it to Lean
        // for --emit=exe unless the user opts out.
        LinkMode        linkMode            = LinkMode::Full;
        // --why-kept: print what kept this class in the lean keep-set (empty = off).
        std::string     whyKept             = "";
        // --keepset-json: write the keep-set and its provenance here (empty = off).
        std::string     keepsetJson         = "";
        // --emit-xref: write the resolved cross-reference index the IDE reads (off = "").
        std::string     emitXref            = "";

        // ----- tree-shaking (Tier-1 RTA; plans/compiler/stdlib-tree-shaking.md) -----
        // Defaults Off here, but main.cpp flips it to On for --emit=exe unless the user
        // passed --tree-shake, mirroring the lean-linker default.
        TreeShake       treeShake           = TreeShake::Off;

        // ----- debugging -----
        // The requested level; `debugInfo` and `lineInfo` below are the derived bools the
        // codegen guards read. Each stays settable on its own, last flag winning.
        DebugInfo       debugInfoLevel      = DebugInfo::Line;

        // The debugger's whole apparatus: safepoints, local records, RTTI retention.
        bool            debugInfo           = false;

        // The SAFEPOINT half of the above alone, for a consumer that wants a place to
        // stop without the class-registry retention `debugInfo` forces.
        bool            safepoints          = false;

        // A would-be-UB trap unwinds to the session guard rather than executing
        // `llvm.trap`, so a notebook's `4 / 0` does not take the kernel with it.
        bool            trapsUnwind         = false;

        // The per-call shadow-stack enter/leave and #FrameDesc that resolve a trace with
        // no debug info. On everywhere: measured at parity with an uninstrumented build.
        bool            lineInfo            = true;

        // ----- experimental perf -----
        // Skips the prologue __cajeta_scope_enter(), which heap-allocs on every call but
        // is needed only for a bare `spawn`. UNSAFE: correct only for spawn-free code.
        bool            lazyScope           = false;

        // ----- reproducible builds -----
        // --source-date-epoch, --debug-prefix-map and --seed, honored where emit embeds
        // a timestamp, a source path or a build-RNG salt.
        std::string     sourceDateEpoch;
        std::string     debugPrefixMap;
        std::string     seed;

        // The default flag set for a mode; per-feature CLI flags override it after.
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

    // Sets the level and its two derived bools. False on an unknown value, leaving `f`
    // untouched and filling `error`, when given, with the accepted set.
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
        // Full implies safepoints: they are what a debugger stops at.
        f.safepoints     = f.debugInfo;
        f.lineInfo       = (level != DebugInfo::Off);
        return true;
    }

    // False on an unknown value, leaving `f` untouched and filling `error` when given.
    inline bool applyProfiler(const std::string& value, CompilerFlags& f,
                              std::string* error) {
        if      (value == "off")        f.profiler = Profiler::Off;
        else if (value == "instrument") f.profiler = Profiler::Instrument;
        else {
            if (error)
                *error = "unrecognized value for --profiler: " + value +
                         " (expected off|instrument)";
            return false;
        }
        return true;
    }

    inline const char* profilerName(Profiler p) {
        return p == Profiler::Instrument ? "instrument" : "off";
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
