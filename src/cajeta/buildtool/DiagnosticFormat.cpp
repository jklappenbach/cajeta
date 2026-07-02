#include "cajeta/buildtool/DiagnosticFormat.h"

namespace cajeta::buildtool {

    namespace {
        // Written once at CLI dispatch (before any action thread starts), then
        // only read — so plain global access is race-free across parallel groups.
        DiagFormat g_diagFormat = DiagFormat::Text;
    }

    void setDiagnosticFormat(DiagFormat format) { g_diagFormat = format; }
    DiagFormat diagnosticFormat() { return g_diagFormat; }

} // namespace cajeta::buildtool
