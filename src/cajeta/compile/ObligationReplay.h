// Incremental compilation Phase 3 — obligation replay: a codegen-skipped
// module's sidecar-recorded template instantiations are re-driven BEFORE the
// codegen loop, idempotently, so their bodies are generated this build.

#pragma once

#include "../type/CajetaType.h"

#include <string>

namespace cajeta {

    // Resolves a canonical type string (`int32`, `ArrayStream<int32>`, `#`-owning
    // args) to a type, instantiating templates; null + `err` if unresolvable.
    CajetaTypePtr resolveCanonicalType(const std::string& canonical,
                                       std::string& err);

    // Replays one obligation sidecar line — class `Pkg.Class<args>` or method
    // `Pkg.Class<args>::name(params)<targs>`; blank succeeds, false + `err` else.
    bool replayObligation(const std::string& key, std::string& err);

} // namespace cajeta
