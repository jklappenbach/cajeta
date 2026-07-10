//
// Source-synthesis facility (núcleo Layer-1a) — the shared parse-and-inject core
// that unifies the @Logged member injector and the codec body synthesizers.
// Plan: agents/cajeta/nucleo/source-synthesis-plan.md. Spec:
// docs/specification/nucleo/source-synthesis-spec.md.
//
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace cajeta {
    class CajetaModule;
    using CajetaModulePtr = std::shared_ptr<CajetaModule>;
}

namespace cajeta::synth {

    // Inject `import <packageName>.<shortName>` into the module ONLY when
    // `shortName` is otherwise unbound. Synthesized source uses short type
    // names (`Logger`, `Log`); a fully-qualified static call resolves to null
    // in expression position, so the matching import must exist — but a user's
    // own same-short-name import must always win, hence only-when-unbound.
    // (spec §3.2; the @Logged respect-user-import rule.)
    void injectImportIfUnbound(const CajetaModulePtr& module,
                               const std::string& shortName,
                               const std::string& packageName);

    // Deterministic, collision-resistant identifier for a synthesized
    // wrapper/instance, derived from the trigger's canonical identity and its
    // monomorphized type-argument canonicals. Replaces the pointer-based
    // `(size_t)this` naming in MethodTemplateInstantiator so synthesized output
    // is byte-identical across compiles (reproducible builds; spec §8.3).
    //
    // The result is always a valid identifier: every character outside
    // [A-Za-z0-9_] is replaced by '_'. Distinct (prefix, trigger, args) inputs
    // yield distinct names (arity included), so two specializations never
    // collide in the visitor's structure stack / canonical map.
    std::string deriveSynthName(const std::string& prefix,
                                const std::string& triggerCanonical,
                                const std::vector<std::string>& argCanonicals);

}
