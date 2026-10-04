// Where a diagnostic reported from a module points, and which origin it carries.
#pragma once

#include <string>

namespace cajeta {

    class CajetaModule;

    struct DiagnosticSite {
        std::string file;
        std::string origin;   // "project" | "dependency" | "stdlib"
    };

    // The file and origin for a diagnostic reported while `module` compiles its innermost
    // class: an embedded stdlib class names `<stdlib>/<package path>/<Class>.cajeta`.
    DiagnosticSite diagnosticSiteOf(CajetaModule& module);

} // namespace cajeta
