#include "cajeta/buildtool/Dependency.h"

#include <llvm/Support/Error.h>

#include <algorithm>
#include <string>

namespace cajeta::buildtool {

    namespace {

        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(), msg);
        }

    } // namespace

    llvm::Expected<std::vector<RepositorySpec>> parseRepositories(
        const Manifest& m) {
        std::vector<RepositorySpec> out;
        const auto* settings = &m.settingsRaw;
        const auto* arr = settings->getArray("repositories");
        if (!arr) return out;  // no settings.repositories → empty list

        for (size_t i = 0; i < arr->size(); ++i) {
            const auto* obj = (*arr)[i].getAsObject();
            if (!obj) {
                return err("settings.repositories[" + std::to_string(i) +
                           "] must be an object");
            }
            RepositorySpec r;
            auto name = obj->getString("name");
            if (!name) {
                return err("settings.repositories[" + std::to_string(i) +
                           "] missing required 'name'");
            }
            r.name = name->str();

            // Type: default "filesystem" when `path` is present,
            // "http" when `url` is present. Explicit `type` field
            // wins. Spec says explicit type is required for
            // maven-compat; we don't infer that one.
            if (auto t = obj->getString("type")) {
                r.type = t->str();
            } else if (obj->getString("path")) {
                r.type = "filesystem";
            } else if (obj->getString("url")) {
                r.type = "http";
            } else {
                return err("settings.repositories." + r.name +
                           ": cannot infer 'type'; supply one of "
                           "filesystem / http / maven-compat / git");
            }

            if (auto v = obj->getString("path")) r.path = v->str();
            if (auto v = obj->getString("url"))  r.url  = v->str();
            if (auto v = obj->getInteger("priority")) {
                r.priority = static_cast<int>(*v);
            }

            // Validate the type / fields combination.
            if (r.type == "filesystem" && r.path.empty()) {
                return err("settings.repositories." + r.name +
                           ": type='filesystem' requires 'path'");
            }
            if ((r.type == "http" || r.type == "maven-compat") &&
                r.url.empty()) {
                return err("settings.repositories." + r.name +
                           ": type='" + r.type + "' requires 'url'");
            }

            out.push_back(std::move(r));
        }

        // Sort by priority descending; preserve declaration order
        // among ties (stable_sort).
        std::stable_sort(out.begin(), out.end(),
            [](const RepositorySpec& a, const RepositorySpec& b) {
                return a.priority > b.priority;
            });
        return out;
    }

    llvm::Expected<std::vector<DependencySpec>> parseDependencies(
        const Manifest& m) {
        std::vector<DependencySpec> out;
        const auto* settings = &m.settingsRaw;
        const auto* deps = settings->getObject("dependencies");
        if (!deps) return out;  // none declared → empty list

        for (const auto& kv : *deps) {
            DependencySpec d;
            d.name = kv.first.str();
            // Two recognized forms:
            //   1) string constraint: "1.2.*"
            //   2) object: { version: "1.2.*", from: "repo" }
            //              or { path: "..." } / { git: "..." }
            if (auto s = kv.second.getAsString()) {
                d.versionConstraint = s->str();
            } else if (const auto* obj = kv.second.getAsObject()) {
                if (auto v = obj->getString("version")) {
                    d.versionConstraint = v->str();
                } else if (obj->get("path") || obj->get("git")) {
                    // 6c parses these into a richer DependencySpec
                    // (with source kind). For 6a we leave the
                    // version constraint empty and continue —
                    // downstream resolution skips entries with no
                    // constraint (path/git sources don't need
                    // version negotiation against a repo).
                } else {
                    return err("settings.dependencies." + d.name +
                               ": object form requires 'version', "
                               "'path', or 'git'");
                }
                if (auto v = obj->getString("from")) {
                    d.fromRepo = v->str();
                }
            } else {
                return err("settings.dependencies." + d.name +
                           ": value must be a string (version constraint) "
                           "or object");
            }
            out.push_back(std::move(d));
        }
        return out;
    }

} // namespace cajeta::buildtool
