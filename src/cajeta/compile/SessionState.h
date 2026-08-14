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

#include <memory>
#include <string>
#include <vector>

namespace cajeta {

    class CajetaType;
    typedef std::shared_ptr<CajetaType> CajetaTypePtr;

    struct SessionBindingFact {
        std::string name;
        // Canonical type name at (re)binding time. Resolved against the
        // CURRENT unit's type world when seeding; a canonical that no longer
        // resolves seeds name-only (ownership checks still apply).
        std::string typeCanonical;
        // The exact type the name was bound to, when it is still live. A
        // session's type world PERSISTS across units, so this is the honest
        // answer and the canonical is the fallback for hosts that discard the
        // world between units. It matters for generational redefinition
        // (script-units 5.3): after a later cell redefines `Point`, the
        // canonical resolves to the NEW generation, but a value bound from
        // the old one is still the old type — re-resolving by name would
        // reinterpret it under the new layout and dispatch into new bodies.
        CajetaTypePtr boundType;
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
        // Canonical names of the classes this session's cells have DECLARED.
        // A redefinition is a name declared twice IN THIS SESSION — not merely
        // a name whose LLVM struct already exists in the context, because a
        // session shares its context with every earlier session in the process
        // and those leave their structs behind (script-units 5.3).
        std::vector<std::string> declaredClasses;
        // Implicit class of each unit compiled into this session, OLDEST
        // first. A later unit's bare call to an earlier unit's top-level
        // method resolves by walking this newest-first (jupyter-kernel
        // 1.2.4): each unit is its own implicit class, so without it the
        // call only ever sees the current unit's members.
        std::vector<std::string> unitClasses;

    public:
        void addUnitClass(const std::string& canonical) {
            for (auto& c : unitClasses) {
                if (c == canonical) return;      // redefinition keeps its slot
            }
            unitClasses.push_back(canonical);
        }
        const std::vector<std::string>& getUnitClasses() const {
            return unitClasses;
        }

        // True when this session has already declared `canonical` — i.e. a
        // second declaration of it is a generational redefinition.
        bool hasDeclaredClass(const std::string& canonical) const {
            for (auto& c : declaredClasses) {
                if (c == canonical) return true;
            }
            return false;
        }
        void noteDeclaredClass(const std::string& canonical) {
            if (!hasDeclaredClass(canonical)) declaredClasses.push_back(canonical);
        }

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
