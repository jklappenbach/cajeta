//
// CirSpecializationAnalysis — Phase-A specialization analysis (spec §3.5, §3.6).
//
// Analysis-only: it does NOT transform CIR. It interrogates a slice of lowered
// CirFunctions (a caller + the callees it invokes) and, for each call site that
// passes a closure argument, decides SPECIALIZE vs LEAVE-INDIRECT by the binary,
// structural conjunction of §3.6.1:
//   (known target) ∧ (capture-free) ∧ (supplied directly) ∧
//   (complete invocation set) ∧ (escape-safe).
// A SPECIALIZE classification produces a request (§3.6.2) carrying exactly what
// codegen (Unit 4) needs to materialize the fast instance. No cost model, no LTO
// dependence — reproducible across builds.
//

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

        // Deterministic, testable dump of the decisions.
        static std::string print(const CirAnalysisResult& result);
    };

} // namespace ir
} // namespace cajeta
