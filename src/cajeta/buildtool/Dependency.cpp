#include "cajeta/buildtool/Dependency.h"

#include <filesystem>

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
            // `ref`, `tag`, `branch` and `rev` all collapse to one literal gitRef.
            if (auto v = obj->getString("ref"))    r.gitRef = v->str();
            if (auto v = obj->getString("tag"))    r.gitRef = v->str();
            if (auto v = obj->getString("branch")) r.gitRef = v->str();
            if (auto v = obj->getString("rev"))    r.gitRef = v->str();
            if (auto v = obj->getString("subdir")) r.gitSubdir = v->str();

            if (r.type == "filesystem" && r.path.empty()) {
                return err("settings.repositories." + r.name +
                           ": type='filesystem' requires 'path'");
            }
            if ((r.type == "http" || r.type == "maven-compat") &&
                r.url.empty()) {
                return err("settings.repositories." + r.name +
                           ": type='" + r.type + "' requires 'url'");
            }
            if (r.type == "git") {
                if (r.url.empty()) {
                    return err("settings.repositories." + r.name +
                               ": type='git' requires 'url' (clone URL)");
                }
                if (r.gitRef.empty()) {
                    return err("settings.repositories." + r.name +
                               ": type='git' requires one of 'ref' / "
                               "'tag' / 'branch' / 'rev'");
                }
            }

            // Parsed for any repo type; drivers that cannot use it ignore it.
            if (const auto* a = obj->getObject("auth")) {
                if (auto t = a->getString("type")) {
                    r.auth.type = t->str();
                }
                if (auto v = a->getString("token-env")) {
                    r.auth.tokenEnv = v->str();
                }
                if (auto v = a->getString("token")) {
                    r.auth.tokenLiteral = v->str();
                }
                if (auto v = a->getString("client-cert")) {
                    r.auth.clientCertPath = v->str();
                }
                if (auto v = a->getString("client-key")) {
                    r.auth.clientKeyPath = v->str();
                }
                if (auto v = a->getString("ca-cert")) {
                    r.auth.caCertPath = v->str();
                }
                if (r.auth.type == "bearer" &&
                    r.auth.tokenEnv.empty() &&
                    r.auth.tokenLiteral.empty()) {
                    return err("settings.repositories." + r.name +
                               ".auth: 'bearer' requires 'token-env' or "
                               "'token'");
                }
                if (r.auth.type == "mtls" &&
                    (r.auth.clientCertPath.empty() ||
                     r.auth.clientKeyPath.empty())) {
                    return err("settings.repositories." + r.name +
                               ".auth: 'mtls' requires 'client-cert' and "
                               "'client-key'");
                }
                if (!r.auth.type.empty() &&
                    r.auth.type != "bearer" &&
                    r.auth.type != "mtls") {
                    return err("settings.repositories." + r.name +
                               ".auth.type: must be 'bearer' or 'mtls'; "
                               "got '" + r.auth.type + "'");
                }
            }

            out.push_back(std::move(r));
        }

        // A filesystem repository's `path` is relative to the MANIFEST that
        // declares it, never to the process's working directory. Anchoring it here
        // fixes every consumer at once.
        if (!m.sourcePath.empty()) {
            auto base = std::filesystem::path(m.sourcePath).parent_path();
            for (auto& r : out) {
                if (r.type != "filesystem" || r.path.empty()) continue;
                if (std::filesystem::path(r.path).is_absolute()) continue;
                r.path = std::filesystem::weakly_canonical(base / r.path)
                             .string();
            }
        }

        // Priority descending, declaration order among ties.
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
            // Either a bare constraint string, or an object carrying `version`
            // plus `from`, or a `path` / `git` source.
            if (auto s = kv.second.getAsString()) {
                d.versionConstraint = s->str();
            } else if (const auto* obj = kv.second.getAsObject()) {
                if (auto v = obj->getString("version")) {
                    d.versionConstraint = v->str();
                } else if (obj->get("path") || obj->get("git")) {
                    // A path/git source negotiates no version, so the empty
                    // constraint left here makes resolution skip the entry.
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

    llvm::Expected<std::vector<OverrideSpec>> parseOverrides(
        const Manifest& m) {
        std::vector<OverrideSpec> out;
        const auto* settings = &m.settingsRaw;
        const auto* overrides = settings->getObject("overrides");
        if (!overrides) return out;

        for (const auto& kv : *overrides) {
            OverrideSpec o;
            o.name = kv.first.str();
            if (auto s = kv.second.getAsString()) {
                o.versionConstraint = s->str();
            } else if (const auto* obj = kv.second.getAsObject()) {
                if (auto v = obj->getString("version")) {
                    o.versionConstraint = v->str();
                } else if (auto p = obj->getString("path")) {
                    o.path = p->str();
                } else if (auto g = obj->getString("git")) {
                    o.git = g->str();
                    if (auto r = obj->getString("rev")) {
                        o.rev = r->str();
                    }
                } else {
                    return err("settings.overrides." + o.name +
                               ": object form requires 'version', "
                               "'path', or 'git'");
                }
                if (auto a = obj->getBoolean("allow-major-downgrade")) {
                    o.allowMajorDowngrade = *a;
                }
            } else {
                return err("settings.overrides." + o.name +
                           ": value must be a string (version constraint) "
                           "or object");
            }
            out.push_back(std::move(o));
        }
        return out;
    }

} // namespace cajeta::buildtool
