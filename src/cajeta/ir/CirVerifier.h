//
// CirVerifier — well-formedness checks for a CirFunction (spec §2.1.4).
//
// Returns a list of human-readable error messages; an empty list means the
// function is well-formed. Checks:
//   - every block ends in exactly one terminator;
//   - SSA single-definition (no value defined twice as a param/block-param/result);
//   - branch targets pass the right block-parameter arity (and type spelling).
// Analysis-only and codegen-independent (Unit 1).
//

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
