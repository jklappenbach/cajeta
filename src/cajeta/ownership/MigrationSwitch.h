#pragma once

// The warning-first landing of an ownership check: one whose blast radius is
// unknown warns, is migrated against in a pass, then flips back to an error. Off
// by default and keyed to the environment, so no source can opt itself out.

#include <string>

namespace cajeta::ownership {

    class MigrationSwitch {
    public:
        // Warn mode is `<envVar>=warn`; anything else, unset included, is error.
        // `envVar` is read per call rather than cached, so a test that flips the
        // switch mid-process does not depend on which test ran first.
        explicit MigrationSwitch(const char* envVar) : envVar_(envVar) {}

        bool warns() const;

        // Test override, taking precedence over the environment.
        void setWarns(bool on) { override_ = on ? 1 : 0; }
        void clearOverride() { override_ = -1; }

        // Warn-mode reporting on two channels: `note` goes to stderr verbatim as
        // the migration's uncapped, engine-independent enumeration record, while
        // `message` becomes a real warning under `warnCode`, deduped and capped.
        void report(const std::string& note, const std::string& warnCode,
                    const std::string& message, const std::string& file,
                    int line) const;

    private:
        const char* envVar_;
        int override_ = -1;  // -1 = ask the environment, 0 = error, 1 = warn
    };

}  // namespace cajeta::ownership
