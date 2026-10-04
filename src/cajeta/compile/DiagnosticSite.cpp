#include "DiagnosticSite.h"

#include "CajetaModule.h"
#include "StdlibEmbedded.h"
#include "cajeta/type/CajetaClass.h"

#include <algorithm>
#include <set>

namespace cajeta {

    namespace {
        const std::set<std::string>& stdlibPaths() {
            static const std::set<std::string> paths = [] {
                std::set<std::string> s;
                for (size_t i = 0; i < stdlib::g_fileCount; ++i) {
                    s.insert(stdlib::g_files[i].relativePath);
                }
                return s;
            }();
            return paths;
        }

        const std::set<std::string>& stdlibPackages() {
            static const std::set<std::string> pkgs = [] {
                std::set<std::string> s;
                for (const auto& rel : stdlibPaths()) {
                    auto slash = rel.find_last_of('/');
                    std::string pkg = slash == std::string::npos ? std::string() : rel.substr(0, slash);
                    std::replace(pkg.begin(), pkg.end(), '/', '.');
                    s.insert(pkg);
                }
                return s;
            }();
            return pkgs;
        }
    }

    DiagnosticSite diagnosticSiteOf(CajetaModule& module) {
        std::string pkg;
        std::string top;
        auto& stack = module.getStructureStack();
        if (!stack.empty() && stack.back() && stack.back()->getQName()) {
            auto qn = stack.back()->getQName();
            pkg = qn->getPackageName();
            top = qn->getTypeName();
            top = top.substr(0, top.find_first_of("<."));
        }
        bool stdlibClass = module.getStdlibModule().get() == &module
            || (!pkg.empty() && stdlibPackages().count(pkg) > 0);
        if (stdlibClass && module.getSourcePath().empty()) {
            std::string rel = pkg;
            std::replace(rel.begin(), rel.end(), '.', '/');
            rel += "/" + top + ".cajeta";
            return {"<stdlib>/" + rel, "stdlib"};
        }
        if (module.isClasspathOrigin()) {
            return {module.getSourcePath().empty() ? module.getArchivePath() : module.getSourcePath(),
                    "dependency"};
        }
        return {module.getSourcePath(), stdlibClass ? "stdlib" : "project"};
    }

} // namespace cajeta
