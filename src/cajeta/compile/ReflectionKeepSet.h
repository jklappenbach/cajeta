#pragma once

// DCE Tier-0b keep-set resolution (lean-linker-dce.md §3.2), factored out of
// the Lean-mode link so the lazy kernel can run the same resolution per cell
// (lazy-codegen 4.2.4, spec 2.2.1) — the question "what must survive because
// reflection might want it" has ONE answer, shared by both consumers.

#include <map>
#include <memory>
#include <set>
#include <string>

namespace cajeta {

    // Resolve the accumulated reflection sites (CajetaModule::reflectionKeep)
    // against the canonical map into the set of class canonical names whose
    // registration ctor must be kept. Returns NULL when the accumulator
    // carries a forces-ALL site — null keep-set means keep everything
    // (CajetaModule::keepsClass). `keptBy`, when non-null, receives
    // canon -> first reason kept (provenance for --why-kept / keepset.json).
    std::shared_ptr<const std::set<std::string>>
    resolveReflectionKeepSet(std::map<std::string, std::string>* keptBy = nullptr);

} // namespace cajeta
