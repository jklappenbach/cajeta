// CirVerifier — well-formedness checks for a CirFunction (spec §2.1.4): exactly one
// terminator per block, SSA single-definition, and branch targets matching their
// block-parameter arity and type spelling. Analysis-only and codegen-independent.

#pragma once

#include <string>
#include <vector>

#include "Cir.h"

namespace cajeta {
namespace ir {

    class CirVerifier {
    public:
        // Empty result == well-formed.
        static std::vector<std::string> verify(const CirFunction& fn);
    };

} // namespace ir
} // namespace cajeta
