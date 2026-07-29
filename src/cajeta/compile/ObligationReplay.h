// Incremental compilation Phase 3 — obligation replay.
//
// A clean (codegen-skipped) module's cached `.bc` references the template
// instantiations its original codegen drove into OTHER modules (stdlib,
// typically). Those sidecar-recorded obligations are replayed BEFORE the
// codegen loop so the instantiations exist and their bodies are generated
// normally this build. Replay is idempotent: an obligation a live module
// also drives resolves to the same cached instantiation.
//
// v1 scope (plan Phase 3 decision D2): a method obligation's value-param
// signature is NOT compared — host + name + type-args must be unambiguous,
// else replay fails loud and the caller degrades the module to dirty.

#pragma once

#include "../type/CajetaType.h"

#include <string>

namespace cajeta {

    // Resolve a canonical type string — `int32`, `cajeta.lang.String`,
    // `cajeta.lang.stream.ArrayStream<int32>`, nested/owning (`#`) template
    // args — to a type, instantiating templates on the way. Null + `err` on
    // anything unresolvable. Requires all declarations parsed (post-parse,
    // pre-codegen).
    CajetaTypePtr resolveCanonicalType(const std::string& canonical,
                                       std::string& err);

    // Replay one obligation sidecar line. Class form (`Pkg.Class<args>`)
    // instantiates the class template; method form
    // (`Pkg.Class<args>::name(params)<targs>`) instantiates the host's
    // method template. Blank lines succeed. False + `err` on failure.
    bool replayObligation(const std::string& key, std::string& err);

} // namespace cajeta
