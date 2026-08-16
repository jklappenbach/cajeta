#pragma once

// Spec §5.5 — the warning-first landing of an ownership check.
//
// A check whose blast radius is unknown lands as a WARNING, is migrated
// against in one pass, then flips to an error. The reason is structural: a
// thrown diagnostic stops the build at the FIRST offending site, so seeing the
// second costs a fix plus a full rebuild and the total is never visible.
// CAPTURED_BORROW_PARAM landed error-first against an audit's estimate of 10
// sites; the gate found 76 failures over 30 real sites, and the unit's
// acceptance stayed open for weeks. Under warn mode the same enumeration took
// one pass over five libraries.
//
// This is the mechanism, factored out of the second use rather than the first:
// two hand-rolled copies of "read an env var, report on two channels" is how
// the CAPTURED_BORROW and OWNED_RESULT switches would drift apart, and their
// divergence would be invisible until one of them silently stopped demoting.
//
// A switch is a MIGRATION INSTRUMENT with a defined end, not a severity knob:
// off by default, switched by ENVIRONMENT rather than by source annotation so
// no code can opt itself out permanently, and the plan item that opens it is
// closed only by flipping back.

#include <string>

namespace cajeta::ownership {

    class MigrationSwitch {
    public:
        // `envVar` is read per call, not cached: a test that flips the switch
        // mid-process must not be at the mercy of which test ran first.
        // Warn mode is `<envVar>=warn`; anything else, including unset, is the
        // default error.
        explicit MigrationSwitch(const char* envVar) : envVar_(envVar) {}

        bool warns() const;

        // Test override, taking precedence over the environment.
        void setWarns(bool on) { override_ = on ? 1 : 0; }
        void clearOverride() { override_ = -1; }

        // Warn-mode reporting, on two channels deliberately:
        //
        //   - `note` goes to stderr verbatim as a grep-able record. This is
        //     the migration's enumeration channel: uncapped, and independent
        //     of whether a DiagnosticEngine is installed. The 8.1.1 harvest
        //     established it survives every entry point, including the build
        //     tool's.
        //   - `message` goes to the active DiagnosticEngine as a warning under
        //     `warnCode`, so the demotion is a real warning in the compiler's
        //     own stream. That stream dedups and CAPS at 100 — fine for a
        //     developer reading their build, useless for a migration that
        //     needs the whole list, which is why both exist.
        void report(const std::string& note, const std::string& warnCode,
                    const std::string& message, const std::string& file,
                    int line) const;

    private:
        const char* envVar_;
        int override_ = -1;  // -1 = ask the environment, 0 = error, 1 = warn
    };

}  // namespace cajeta::ownership
