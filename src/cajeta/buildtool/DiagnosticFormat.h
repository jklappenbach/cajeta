#pragma once

#include "cajeta/compile/CompilerMode.h"

namespace cajeta::buildtool {

    // Process-wide diagnostic output format (`--diag-format`) for the compiler
    // subprocesses build actions spawn. Set once at CLI dispatch, read when
    // BuildAction builds the argv; deliberately outside the reproducibility hashes.
    void setDiagnosticFormat(DiagFormat format);
    DiagFormat diagnosticFormat();

} // namespace cajeta::buildtool
