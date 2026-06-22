//
// CirPrinter — human-readable textual dump of a CirFunction (spec §2.5).
//
// One function per block of text: signature, then each block as a labelled
// region of typed SSA instruction lines. Stable enough for tests to assert on
// (round-trips names, types, ownership kinds, the make.closure/apply.closure
// shape); backs the `--emit=cir` debug mode (Unit 4+). Matches the XpuMirPrinter
// pattern: ostringstream in anonymous helpers, static public string-returning
// entry points.
//

#pragma once

#include <string>

#include "Cir.h"

namespace cajeta {
namespace ir {

    class CirPrinter {
    public:
        static std::string print(const CirFunction& fn);
    };

} // namespace ir
} // namespace cajeta
