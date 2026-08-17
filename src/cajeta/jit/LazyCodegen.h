#pragma once

// lazy-codegen 2.1.5 / spec 5.1 — the emission-mode toggle.
//
// `CAJETA_EAGER_CODEGEN=1` restores eager emission and is a PERMANENT supported
// control, not a migration aid (Julian, 2026-08-16): "is it the lazy path?"
// stays answerable for the life of the feature.
//
// The environment SEEDS the mode; it does not LOCK it. Reading getenv into a
// function-local static — the obvious spelling — fixes the value at first call
// and makes the two paths untestable in one process. `CAJETA_TWO_STAGE_PARSE`
// has that defect today, which is why the SLL fix could not be regression
// -tested by flipping a flag.

namespace cajeta {

    // True when method bodies are emitted on demand. Cheap and side-effect free
    // — the generator consults it on every tryToGenerate, including from ORC's
    // materialization threads.
    bool lazyCodegenEnabled();

    // Set the mode for this process. Tests A/B with it; hosts call it once from
    // their own configuration.
    void setLazyCodegenEnabled(bool enabled);

} // namespace cajeta
