// CirPrinter — human-readable dump of a CirFunction (spec §2.5): a signature, then
// each block as a labelled region of typed SSA instruction lines. Stable enough for
// tests to assert on, and backs the `--emit=cir` debug mode.

#pragma once

#include <string>

#include "Cir.h"

namespace cajeta {
namespace ir {

    class CirPrinter {
    public:
        // Renders `fn` in full: the `fn name<generics>(params) -> ret` signature, then
        // every block in order as `label(params):` followed by its instruction lines
        // and its terminator. Returns the whole text, newline-terminated.
        static std::string print(const CirFunction& fn);
    };

} // namespace ir
} // namespace cajeta
