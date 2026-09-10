#pragma once

// The emission-mode toggle (lazy-codegen 2.1.5 / spec 5.1). `CAJETA_EAGER_CODEGEN=1`
// is a permanent supported control, and it SEEDS the mode without locking it: a
// function-local getenv would fix it at first call and make both paths untestable.

namespace cajeta {

    // True when method bodies are emitted on demand; cheap and side-effect free, as
    // ORC's materialization threads consult it on every tryToGenerate.
    bool lazyCodegenEnabled();

    // Sets the mode for this process; tests A/B with it, hosts call it once.
    void setLazyCodegenEnabled(bool enabled);

} // namespace cajeta
