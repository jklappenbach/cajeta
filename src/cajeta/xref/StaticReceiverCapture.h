#pragma once

#include <memory>

namespace cajeta {
    class CajetaModule;
}

namespace cajeta::xref {

    // Records a type reference for each static call / field-access receiver whose
    // leading identifier is a TYPE, resolved with the compiler's own precedence
    // (local → field → type); a name ever bound as a value is never recorded.
    void captureStaticReceivers(const std::shared_ptr<CajetaModule>& module);

}
