// The shared parse-and-inject core behind the @Logged member injector and the
// codec body synthesizers.
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace cajeta {
    class CajetaModule;
    using CajetaModulePtr = std::shared_ptr<CajetaModule>;
}

namespace cajeta::synth {

    // Inject `import <packageName>.<shortName>` ONLY when `shortName` is unbound:
    // synthesized source spells types short, so the import must exist, but a
    // user's own same-short-name import must always win.
    void injectImportIfUnbound(const CajetaModulePtr& module,
                               const std::string& shortName,
                               const std::string& packageName);

    // A deterministic identifier for a synthesized wrapper, derived from the
    // trigger's canonical and its type-argument canonicals, so output is
    // byte-identical across compiles. Sanitizing to [A-Za-z0-9_] is NOT injective.
    std::string deriveSynthName(const std::string& prefix,
                                const std::string& triggerCanonical,
                                const std::vector<std::string>& argCanonicals);

}
