//
// script-units U4 (spec §4.2, §4.6) — the compiler-side session table.
//
// One SessionState instance IS a session from the compiler's point of view:
// the host (`cajeta run`, the Jupyter kernel) keeps it alive across unit
// compiles, and each script unit's entry codegen seeds its root scope from
// this table before analysis and writes ownership facts back after. Facts
// are carried IN the slot — canonical type name, moved flag, transfer site —
// and never re-resolved at read time; a later unit's compile may run in a
// different type world than the one that recorded the fact.
//
// Single-threaded by contract, like the runtime session registry: units of
// one session compile sequentially on the session's compile thread.
//
#pragma once

#include <string>
#include <vector>

namespace cajeta {

    struct SessionBindingFact {
        std::string name;
        // Canonical type name at (re)binding time. Resolved against the
        // CURRENT unit's type world when seeding; a canonical that no longer
        // resolves seeds name-only (ownership checks still apply).
        std::string typeCanonical;
        // True when the title was transferred away and the name not yet
        // rebound — reads in later units are rejected (spec §4.2).
        bool moved = false;
        // Human-readable transfer-site note, appended to the cross-unit
        // use-after-move diagnostic.
        std::string transferSite;
    };

    class SessionState {
        // First-binding order, mirroring the runtime registry's slot order.
        std::vector<SessionBindingFact> facts;

    public:
        SessionBindingFact* find(const std::string& name) {
            for (auto& f : facts) {
                if (f.name == name) return &f;
            }
            return nullptr;
        }

        // Insert, or update in place (the name keeps its first-binding
        // position — same rule as the runtime registry).
        void put(SessionBindingFact fact) {
            if (SessionBindingFact* existing = find(fact.name)) {
                *existing = std::move(fact);
                return;
            }
            facts.push_back(std::move(fact));
        }

        const std::vector<SessionBindingFact>& all() const { return facts; }
        std::vector<SessionBindingFact>& all() { return facts; }
    };

}  // namespace cajeta
