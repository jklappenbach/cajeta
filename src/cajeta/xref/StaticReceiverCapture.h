#pragma once

#include <memory>

namespace cajeta {
    class CajetaModule;
}

namespace cajeta::xref {

    // Record a type reference for each static method-call / field-access
    // receiver (`Gzip.decompress(...)`, `Gzip.CONST`) in this module's method
    // bodies whose leading identifier is a TYPE — not a local, parameter, or
    // field. Runs during the lint / whole-root export (no codegen), so
    // static-receiver navigation lands in the fast index the IDE rebuilds.
    //
    // Conservative by construction: the receiver is resolved against the same
    // precedence the compiler uses (local → field → type), and a name that is
    // EVER a local/parameter/field anywhere in the method is never recorded —
    // so the pass can only emit an edge when the identifier is definitely a
    // type, never producing a wrong Ctrl-click jump. No-op unless xref capture
    // is on.
    void captureStaticReceivers(const std::shared_ptr<CajetaModule>& module);

}
