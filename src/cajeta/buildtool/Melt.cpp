#include "cajeta/buildtool/Melt.h"

#include <llvm/Support/Error.h>

#include <set>

namespace cajeta::buildtool {

    namespace {

        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(), msg);
        }

        // Fields the spec marks as exportable inside the `melt` block.
        // Anything else is rejected — matches the "inert inherits,
        // active doesn't" rule from BuildTool.md "What a melt exports".
        const std::set<std::string>& allowedMeltFields() {
            static const std::set<std::string> kFields = {
                "dependencies", "properties", "actions",
                "repositories", "melts",
            };
            return kFields;
        }

    } // namespace

    llvm::Expected<MeltImport> parseMeltImport(const std::string& s) {
        auto at = s.rfind('@');
        if (at == std::string::npos || at == 0 || at + 1 == s.size()) {
            return err("melt import '" + s +
                       "' must have the form name@version");
        }
        MeltImport out;
        out.name    = s.substr(0, at);
        out.version = s.substr(at + 1);
        // Both halves non-empty — sanity check.
        if (out.name.empty() || out.version.empty()) {
            return err("melt import '" + s +
                       "': name and version must both be non-empty");
        }
        return out;
    }

    llvm::Expected<std::vector<MeltImport>> parseSettingsMelts(
        const Manifest& m) {
        std::vector<MeltImport> out;
        const auto* arr = m.settingsRaw.getArray("melts");
        if (!arr) return out;

        for (size_t i = 0; i < arr->size(); ++i) {
            auto s = (*arr)[i].getAsString();
            if (!s) {
                return err("settings.melts[" + std::to_string(i) +
                           "] must be a string 'name@version'");
            }
            auto imp = parseMeltImport(s->str());
            if (!imp) {
                return err("settings.melts[" + std::to_string(i) +
                           "]: " + llvm::toString(imp.takeError()));
            }
            out.push_back(std::move(*imp));
        }
        return out;
    }

    namespace {

        // Parse `melt.repositories` array. Mirrors the
        // `parseRepositories` logic from Dependency.cpp but operates
        // on a sub-object rather than `settings.repositories`.
        // Reused with the cross-checking validation that the public
        // parseRepositories already does.
        llvm::Expected<std::vector<RepositorySpec>> parseMeltRepos(
            const llvm::json::Array& arr) {
            std::vector<RepositorySpec> out;
            out.reserve(arr.size());
            for (size_t i = 0; i < arr.size(); ++i) {
                const auto* obj = arr[i].getAsObject();
                if (!obj) {
                    return err("melt.repositories[" + std::to_string(i) +
                               "] must be an object");
                }
                RepositorySpec r;
                auto name = obj->getString("name");
                if (!name) {
                    return err("melt.repositories[" + std::to_string(i) +
                               "] missing required 'name'");
                }
                r.name = name->str();
                if (auto t = obj->getString("type")) r.type = t->str();
                else if (obj->getString("path")) r.type = "filesystem";
                else if (obj->getString("url"))  r.type = "http";
                else {
                    return err("melt.repositories." + r.name +
                               ": cannot infer 'type'");
                }
                if (auto v = obj->getString("path")) r.path = v->str();
                if (auto v = obj->getString("url"))  r.url  = v->str();
                if (auto v = obj->getInteger("priority")) {
                    r.priority = static_cast<int>(*v);
                }
                if (auto v = obj->getString("ref"))    r.gitRef = v->str();
                if (auto v = obj->getString("tag"))    r.gitRef = v->str();
                if (auto v = obj->getString("branch")) r.gitRef = v->str();
                if (auto v = obj->getString("rev"))    r.gitRef = v->str();
                if (auto v = obj->getString("subdir")) r.gitSubdir = v->str();
                out.push_back(std::move(r));
            }
            return out;
        }

    } // namespace

    llvm::Expected<Melt> parseMelt(const Manifest& m) {
        Melt out;
        if (!m.hasMelt) return out;

        // Unknown fields are rejected so consumers get a clear error
        // if they (e.g.) tried to slip `plugins` or `capabilities`
        // into a melt.
        for (const auto& kv : m.meltRaw) {
            if (!allowedMeltFields().count(kv.first.str())) {
                return err("'melt." + kv.first.str() +
                           "' is not an exportable melt field "
                           "(allowed: dependencies, properties, "
                           "actions, repositories, melts)");
            }
        }

        if (const auto* deps = m.meltRaw.getObject("dependencies")) {
            for (const auto& kv : *deps) {
                auto cs = kv.second.getAsString();
                if (!cs) {
                    return err("melt.dependencies." + kv.first.str() +
                               ": value must be a version constraint "
                               "string");
                }
                out.dependencies[kv.first.str()] = cs->str();
            }
        }

        if (const auto* props = m.meltRaw.getObject("properties")) {
            for (const auto& kv : *props) {
                auto ps = kv.second.getAsString();
                if (!ps) {
                    return err("melt.properties." + kv.first.str() +
                               ": value must be a string (properties "
                               "are inert text substituted at use site)");
                }
                out.properties[kv.first.str()] = ps->str();
            }
        }

        if (const auto* acts = m.meltRaw.getObject("actions")) {
            out.actionsRaw = *acts;
        }

        if (const auto* repos = m.meltRaw.getArray("repositories")) {
            auto parsed = parseMeltRepos(*repos);
            if (!parsed) return parsed.takeError();
            out.repositories = std::move(*parsed);
        }

        if (const auto* melts = m.meltRaw.getArray("melts")) {
            for (size_t i = 0; i < melts->size(); ++i) {
                auto s = (*melts)[i].getAsString();
                if (!s) {
                    return err("melt.melts[" + std::to_string(i) +
                               "] must be a string 'name@version'");
                }
                auto imp = parseMeltImport(s->str());
                if (!imp) {
                    return err("melt.melts[" + std::to_string(i) +
                               "]: " + llvm::toString(imp.takeError()));
                }
                out.melts.push_back(std::move(*imp));
            }
        }

        return out;
    }

} // namespace cajeta::buildtool
