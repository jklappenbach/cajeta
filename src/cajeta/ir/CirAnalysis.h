// CirSpecializationAnalysis — Phase-A specialization analysis (spec §3.5, §3.6).
// Analysis only, never a transform: for each call site passing a closure it decides
// SPECIALIZE vs LEAVE-INDIRECT by the §3.6.1 conjunction, with no cost model.

#pragma once

#include <string>
#include <vector>

#include "Cir.h"

namespace cajeta {
namespace ir {

    // §3.6.2 — emitted for a SPECIALIZE site.
    struct CirSpecRequest {
        std::string callerFn;                // function holding the call site
        std::string callee;                  // F — the called function
        std::vector<std::string> typeArgs;   // concrete type args (empty when non-generic)
        std::string closureParam;            // P — the function-typed parameter
        std::string targetFn;                // fn — the make.closure target the arg is bound to
        int invocationSites = 0;             // # of apply.closure %P sites inside F (complete set)
    };

    // A site left on the existing indirect closure path, with the failing probe.
    struct CirLeaveIndirect {
        std::string callerFn;
        std::string callee;
        std::string reason;
    };

    struct CirAnalysisResult {
        std::vector<CirSpecRequest> specialize;
        std::vector<CirLeaveIndirect> leaveIndirect;
    };

    class CirSpecializationAnalysis {
    public:
        // Analyze a closed slice (functions referenced by name resolve within it).
        static CirAnalysisResult analyze(const std::vector<CirFunctionPtr>& slice);

        // Renders `result` as one line per decision — every SPECIALIZE request first,
        // then every LEAVE-INDIRECT with its failing probe. Diagnostic text only; it
        // follows the vectors' order, so equal results print identically.
        static std::string print(const CirAnalysisResult& result);
    };

} // namespace ir
} // namespace cajeta
