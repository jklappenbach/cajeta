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
        // jupyter-kernel 2.1.3a — the class GENERATION this name was bound
        // under, as `CajetaClass::getGenerationSuffix()` read it at bind time
        // ("" for the first declaration, "$g2" for the second, ...). Recorded
        // as a STRING rather than inferred from `boundType` because a
        // redeclaration reuses the same CajetaClass instance: the pointer is
        // the newest generation even for an older value, and the suffix on it
        // is overwritten in place. The string is the only copy of the fact
        // that survives the redefinition.
        std::string generation;
        // True when the title was transferred away and the name not yet
        // rebound — reads in later units are rejected (spec §4.2).
        bool moved = false;
        // Human-readable transfer-site note, appended to the cross-unit
        // use-after-move diagnostic.
        std::string transferSite;
    };

    // One class declaration as this session last saw it (script-units 5.3,
    // 5.4). `shape` is a structural fingerprint — fields in declaration order
    // with their type canonicals, plus the set of method signatures — built
    // the same way for every declaration so two are comparable. It is what
    // distinguishes a BODY-ONLY redefinition (same shape, new bodies: the
    // value keeps its identity and adopts them) from a GENERATIONAL one
    // (different shape: a new type, and old values keep the old one).
    //
    // Recorded rather than derived, because by the time a redeclaration is
    // being prototyped the previous declaration is no longer reachable
    // through the type registry: registration has already replaced it.
    struct DeclaredClass {
        std::string canonical;
        std::string shape;
        std::string suffix;   // "" for the first generation, "$g2", ...
    };

    class SessionState {
        // First-binding order, mirroring the runtime registry's slot order.
        std::vector<SessionBindingFact> facts;
        // The classes this session's cells have DECLARED. A redefinition is a
        // name declared twice IN THIS SESSION — not merely a name whose LLVM
        // struct already exists in the context, because a session shares its
        // context with every earlier session in the process and those leave
        // their structs behind (script-units 5.3).
        std::vector<DeclaredClass> declaredClasses;
        std::vector<std::string> bodyOnlyRedefinitions;
        // Implicit class of each unit compiled into this session, OLDEST
        // first. A later unit's bare call to an earlier unit's top-level
        // method resolves by walking this newest-first (jupyter-kernel
        // 1.2.4): each unit is its own implicit class, so without it the
        // call only ever sees the current unit's members.
        std::vector<std::string> unitClasses;
        // Does this session's units all compile in ONE type world (one
        // LLVMContext, one type registry)? The kernel: yes — the session owns
        // its context for its whole life. `cajeta run` and the test harness:
        // no — each unit gets a fresh world and the previous one is torn down.
        //
        // It decides whether a recorded `boundType` may be TOUCHED at all in a
        // later unit. Across worlds the recorded object outlives its context
        // (it is a shared_ptr) while the `llvm::Type*` inside it dangles, so
        // even asking whether it is usable reads freed memory — the flag has
        // to come from the host, because by the time seeding runs there is no
        // safe question to ask the pointer itself.
        bool sharedTypeWorld = false;

    public:
        void setSharedTypeWorld(bool v) { sharedTypeWorld = v; }
        bool hasSharedTypeWorld() const { return sharedTypeWorld; }

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
        // second declaration of it is a redefinition (generational, or
        // body-only per script-units 5.4).
        bool hasDeclaredClass(const std::string& canonical) const {
            return declaredClass(canonical) != nullptr;
        }
        std::vector<std::string> declaredClassNames() const {
            std::vector<std::string> out;
            out.reserve(declaredClasses.size());
            for (auto& c : declaredClasses) out.push_back(c.canonical);
            return out;
        }
        const DeclaredClass* declaredClass(const std::string& canonical) const {
            for (auto& c : declaredClasses) {
                if (c.canonical == canonical) return &c;
            }
            return nullptr;
        }
        // `shape` is the structural fingerprint of the declaration (fields in
        // order + the signature set) and `suffix` the generation it ended up
        // with. Both are needed by the NEXT declaration of the same name:
        // the shape to decide whether it is body-only, the suffix to reuse
        // the same identity when it is. A redeclaration REPLACES the record —
        // the comparison is always against the most recent declaration, not
        // the first.
        void noteDeclaredClass(const std::string& canonical,
                               const std::string& shape,
                               const std::string& suffix) {
            for (auto& c : declaredClasses) {
                if (c.canonical == canonical) {
                    c.shape = shape;
                    c.suffix = suffix;
                    return;
                }
            }
            declaredClasses.push_back({canonical, shape, suffix});
        }

        // Classes this unit redefined BODY-ONLY (script-units 5.4). The host
        // drains it after delivery: suppressing the generation is only half
        // the job, and the other half — repointing the live vtable at the new
        // bodies — can only happen once the code is materialized and has
        // addresses. Per-UNIT, so it is taken rather than read.
        void noteBodyOnlyRedefinition(const std::string& canonical) {
            for (auto& c : bodyOnlyRedefinitions) {
                if (c == canonical) return;
            }
            bodyOnlyRedefinitions.push_back(canonical);
        }
        std::vector<std::string> takeBodyOnlyRedefinitions() {
            std::vector<std::string> out;
            out.swap(bodyOnlyRedefinitions);
            return out;
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
