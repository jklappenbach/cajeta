#pragma once

#include "cajeta/compile/CompilerMode.h"

namespace cajeta::buildtool {

    // Process-wide diagnostic output format for the compiler subprocesses that
    // build actions spawn (the `--diag-format` flag; json-diagnostics-spec §2).
    // Set once at CLI dispatch, read by BuildAction when it builds the compiler
    // argv — a scoped global like NativeProvision's g_nativePhase, so it applies
    // uniformly to nested and parallel build actions in one invocation without
    // threading through the task runner (and stays out of the reproducibility /
    // property hashes, since diagnostic format never changes the built artifact).
    void setDiagnosticFormat(DiagFormat format);
    DiagFormat diagnosticFormat();

} // namespace cajeta::buildtool
