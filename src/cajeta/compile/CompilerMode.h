//
// CompilerMode + per-feature toggle struct.
//
// Spec: cajeta-docs/CompilerModes.md. The flavor flags (`--debug`, `--release`,
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

        // ----- profiling -----
        bool            profileCounters     = false;  // PGO-collection instrumentation

        // ----- debugging -----
        // Emit __cajeta_dbg_safepoint(loc_id) at each statement boundary so the
        // in-process debugger (`cajeta dap`) can park the executing fiber at a
        // breakpoint. Opt-in via --debug-info / -g; OFF for every mode by
        // default (it changes codegen and only matters under a debugger), so
        // ordinary builds and the existing test suite are unaffected.
        bool            debugInfo           = false;

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

}
