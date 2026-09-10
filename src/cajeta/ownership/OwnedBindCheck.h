#pragma once

// stdlib-ownership-convention 8.2.7 (spec §4.6), CAJETA_ERROR_OWNED_RESULT_NEEDS_TRANSFER:
// a `#T` result must be received with `#=`, and binding it with plain `=` is an error
// naming the transfer. `CAJETA_OWNED_BIND=warn` (spec §5.5) demotes it for a sweep.

#include <string>

namespace cajeta::ownership {

    // `calleeKey` names the `#`-returning method, `lvalue` the local being declared, and
    // `inMethod` the receiver. Throws in the default mode, reports in warn mode; a
    // `classpathOrigin` site always demotes to a note, since that archive is not editable.
    void rejectPlainOwnedBind(const std::string& calleeKey,
                              const std::string& lvalue,
                              const std::string& file, int line,
                              const std::string& inMethod,
                              bool classpathOrigin = false);

    // Test control over the §5.5 switch. Off (error) by default.
    bool ownedBindWarns();
    void setOwnedBindWarns(bool on);
    void clearOwnedBindWarnsOverride();

}  // namespace cajeta::ownership
