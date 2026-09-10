// The compiler-side session table: one SessionState IS a session, kept alive by
// the host across unit compiles. Every fact is carried IN its slot, never
// re-resolved, since a later unit may compile in a different type world.
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace cajeta {

    class CajetaType;
    typedef std::shared_ptr<CajetaType> CajetaTypePtr;

    struct SessionBindingFact {
        std::string name;
        // Resolved against the CURRENT unit's world; one that fails seeds name-only.
        std::string typeCanonical;
        // The exact type bound to, while still live — the honest answer, because
        // after a redefinition the canonical resolves to the NEW generation and
        // would reinterpret an old value under the new layout.
        CajetaTypePtr boundType;
        // The generation bound under ("", "$g2", ...), kept as a STRING because a
        // redeclaration reuses the CajetaClass instance and overwrites its suffix
        // in place: this is the only copy that survives the redefinition.
        std::string generation;
        // Title transferred away and not yet rebound; later reads are rejected.
        bool moved = false;
        // Appended to the cross-unit use-after-move diagnostic.
        std::string transferSite;
    };

    // One class declaration as this session last saw it. `shape` is a structural
    // fingerprint separating a BODY-ONLY redefinition from a GENERATIONAL one;
    // it is recorded because registration has already replaced the predecessor.
    struct DeclaredClass {
        std::string canonical;
        std::string shape;
        std::string suffix;   // "" for the first generation, "$g2", ...
    };

    class SessionState {
        std::vector<SessionBindingFact> facts;
        // A redefinition is a name declared twice IN THIS SESSION, not merely one
        // whose LLVM struct exists: the context is shared with earlier sessions.
        std::vector<DeclaredClass> declaredClasses;
        std::vector<std::string> bodyOnlyRedefinitions;
        // Implicit class of each unit, OLDEST first: a bare call to an earlier
        // unit's top-level method resolves by walking this newest-first.
        std::vector<std::string> unitClasses;
        // Whether every unit compiles in ONE type world. It decides whether a
        // recorded `boundType` may be TOUCHED later: across worlds its inner
        // llvm::Type* dangles, so even testing it reads freed memory.
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

        // True when a second declaration of `canonical` would be a redefinition.
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
        // Records this declaration's fingerprint and generation for the NEXT
        // declaration of the name: shape decides body-only, suffix reuses the
        // identity. A redeclaration REPLACES the record, so it is always latest.
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

        // Classes this unit redefined body-only. Per-UNIT and drained by the host,
        // which must still repoint the live vtable once the code has addresses.
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

        // Inserts, or updates in place: the name keeps its first-binding slot.
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
