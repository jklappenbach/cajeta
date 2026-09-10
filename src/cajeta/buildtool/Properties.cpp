#include "cajeta/buildtool/Properties.h"

#include <llvm/Support/Error.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <optional>
#include <set>
#include <sstream>
#include <unordered_set>
#include <vector>

// Declared at file scope so it resolves to ::environ, never a namespaced one.
// Windows has it as `_environ`, `environ` there being an unredeclarable macro.
#if !defined(_WIN32)
extern char** environ;
#endif

#ifndef CAJETA_VERSION
#define CAJETA_VERSION "0.0.0-unknown"
#endif

namespace cajeta::buildtool {

    namespace {

        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(), msg);
        }

        // The built-in names; a user property colliding with one is an error at
        // manifest load. Dotted names collide on their prefix alone.
        const std::set<std::string> kBuiltinPrefixes = {
            "details", "flavor", "profile", "target",
            "env", "artifact", "workspace", "cajeta",
        };

        bool isBuiltinName(const std::string& name) {
            auto dot = name.find('.');
            std::string prefix = (dot == std::string::npos)
                ? name
                : name.substr(0, dot);
            return kBuiltinPrefixes.count(prefix) > 0;
        }

        // A built-in's value, or nullopt when `name` is not one of them.
        std::optional<std::string> resolveBuiltin(
            const std::string& name,
            const Manifest& manifest,
            const PropertyOverrides& ov,
            std::string& error) {
            if (name == "details.name") return manifest.details.name;
            if (name == "details.version") return manifest.details.version;
            if (name == "details.group") return manifest.details.group();
            if (name == "details.library") return manifest.details.library();
            if (name == "flavor") return ov.flavor.value_or("");
            if (name == "profile") return ov.profile.value_or("");
            if (name == "target") return ov.target.value_or("");
            if (name == "workspace.root") return ov.workspaceRoot.value_or("");
            if (name == "cajeta.version") return std::string(CAJETA_VERSION);

            // A missing env variable resolves to the empty string, silently.
            if (name.size() > 4 && name.compare(0, 4, "env.") == 0) {
                std::string envName = name.substr(4);
                const char* v = std::getenv(envName.c_str());
                return std::string(v ? v : "");
            }

            // artifact.* is present but empty outside a distribution action.
            if (name.size() > 9 && name.compare(0, 9, "artifact.") == 0) {
                return std::string("");
            }

            (void)error;
            return std::nullopt;
        }

        // Every `${NAME}` in `s`, in order, duplicates included and none checked
        // for resolvability — the dep graph needs them either way. `$$` escapes.
        std::vector<std::string> referencedNames(const std::string& s) {
            std::vector<std::string> out;
            for (size_t i = 0; i + 1 < s.size(); ) {
                if (s[i] == '$' && s[i + 1] == '$') {
                    i += 2;
                    continue;
                }
                if (s[i] == '$' && s[i + 1] == '{') {
                    size_t close = s.find('}', i + 2);
                    if (close == std::string::npos) break;
                    out.push_back(s.substr(i + 2, close - (i + 2)));
                    i = close + 1;
                    continue;
                }
                ++i;
            }
            return out;
        }

        // Substitutes every `${NAME}` in `s` through `lookup`; false when one
        // returns nullopt, naming it in `missing`.
        bool substituteOnce(
            const std::string& s,
            const std::function<std::optional<std::string>(const std::string&)>& lookup,
            std::string& out,
            std::string& missing) {
            std::string r;
            r.reserve(s.size());
            for (size_t i = 0; i < s.size(); ) {
                if (i + 1 < s.size() && s[i] == '$' && s[i + 1] == '$') {
                    r += '$';
                    i += 2;
                    continue;
                }
                if (i + 1 < s.size() && s[i] == '$' && s[i + 1] == '{') {
                    size_t close = s.find('}', i + 2);
                    if (close == std::string::npos) {
                        // Unterminated: kept literal for a higher layer to flag.
                        r += s.substr(i);
                        i = s.size();
                        continue;
                    }
                    std::string name = s.substr(i + 2, close - (i + 2));
                    auto v = lookup(name);
                    if (!v) {
                        missing = name;
                        return false;
                    }
                    r += *v;
                    i = close + 1;
                    continue;
                }
                r += s[i++];
            }
            out = std::move(r);
            return true;
        }

        // A string property from the manifest's properties block; nullopt when
        // absent or non-string, the latter also setting `typeError`.
        std::optional<std::string> manifestProperty(
            const Manifest& m,
            const std::string& name,
            bool& typeError) {
            const auto* v = m.propertiesRaw.get(name);
            if (!v) return std::nullopt;
            auto s = v->getAsString();
            if (!s) {
                typeError = true;
                return std::nullopt;
            }
            return s->str();
        }

    } // namespace

    std::optional<std::string> ResolvedProperties::lookup(
        const std::string& name) const {
        auto it = values.find(name);
        if (it != values.end()) return it->second;
        // env.* stays lazy: an open namespace cannot be materialized ahead.
        if (name.size() > 4 && name.compare(0, 4, "env.") == 0) {
            std::string envName = name.substr(4);
            const char* v = std::getenv(envName.c_str());
            return std::string(v ? v : "");
        }
        // artifact.* likewise, and empty until an action publishes one.
        if (name.size() > 9 && name.compare(0, 9, "artifact.") == 0) {
            return std::string("");
        }
        return std::nullopt;
    }

    llvm::Expected<std::pair<std::string, std::string>> parseCliOverride(
        const std::string& token) {
        auto eq = token.find('=');
        if (eq == std::string::npos || eq == 0) {
            return err("malformed property override '" + token +
                       "': expected NAME=VALUE");
        }
        return std::make_pair(token.substr(0, eq), token.substr(eq + 1));
    }

    void loadEnvOverrides(PropertyOverrides& dst) {
        const std::string prefix = "CAJETA_PROPERTY_";
#if defined(_WIN32)
        char** envp = _environ;
#else
        char** envp = environ;
#endif
        for (char** e = envp; e && *e; ++e) {
            std::string entry = *e;
            if (entry.compare(0, prefix.size(), prefix) != 0) continue;
            auto eq = entry.find('=', prefix.size());
            if (eq == std::string::npos) continue;
            std::string envSuffix = entry.substr(prefix.size(),
                                                 eq - prefix.size());
            std::string value = entry.substr(eq + 1);
            // CAJETA_PROPERTY_STACK_VERSION becomes "stack-version".
            std::string propName;
            propName.reserve(envSuffix.size());
            for (char c : envSuffix) {
                if (c == '_') propName += '-';
                else propName += static_cast<char>(std::tolower((unsigned char)c));
            }
            dst.env[propName] = value;
        }
    }

    llvm::Expected<ResolvedProperties> resolveProperties(
        const Manifest& manifest,
        const PropertyOverrides& overrides) {

        // A collision with a built-in is structural, so it is caught up front.
        for (const auto& kv : manifest.propertiesRaw) {
            std::string name = kv.first.str();
            if (isBuiltinName(name)) {
                return err("property '" + name + "' in manifest collides "
                           "with a built-in property name");
            }
        }

        // Values materialize bottom-up, post-order over the ${NAME} references.
        ResolvedProperties out;

        std::unordered_set<std::string> visiting;

        // A property's UNRESOLVED source text, by precedence: CLI override, then
        // env override, then the manifest (profiles merge in before this point).
        auto sourceFor = [&](const std::string& name,
                             bool* foundOut) -> std::string {
            if (auto it = overrides.cli.find(name); it != overrides.cli.end()) {
                if (foundOut) *foundOut = true;
                return it->second;
            }
            if (auto it = overrides.env.find(name); it != overrides.env.end()) {
                if (foundOut) *foundOut = true;
                return it->second;
            }
            bool typeError = false;
            if (auto v = manifestProperty(manifest, name, typeError)) {
                if (foundOut) *foundOut = true;
                return *v;
            }
            if (typeError) {
                if (foundOut) *foundOut = false;
                return "<type-error>";
            }
            if (foundOut) *foundOut = false;
            return "";
        };

        // Resolves `name`, recursing through its references.
        std::function<llvm::Expected<std::string>(const std::string&)> resolve;
        resolve = [&](const std::string& name) -> llvm::Expected<std::string> {
            if (auto it = out.values.find(name); it != out.values.end()) {
                return it->second;
            }

            // Built-ins never reference other properties, so no recursion.
            std::string biErr;
            if (auto bi = resolveBuiltin(name, manifest, overrides, biErr)) {
                out.values[name] = *bi;
                out.resolutionOrder.push_back(name);
                return *bi;
            }

            if (visiting.count(name)) {
                std::string cycle;
                for (const auto& v : visiting) {
                    if (!cycle.empty()) cycle += " → ";
                    cycle += v;
                }
                if (!cycle.empty()) cycle += " → ";
                cycle += name;
                return err("cyclic property reference: " + cycle);
            }

            bool found = false;
            std::string raw = sourceFor(name, &found);
            if (!found) {
                if (raw == "<type-error>") {
                    return err("property '" + name + "' must be a string");
                }
                return err("undefined property '" + name + "'");
            }

            visiting.insert(name);
            for (const auto& dep : referencedNames(raw)) {
                if (auto e = resolve(dep)) {
                    // OK
                } else {
                    visiting.erase(name);
                    return e.takeError();
                }
            }
            visiting.erase(name);

            std::string resolved;
            std::string missing;
            bool ok = substituteOnce(raw,
                [&](const std::string& n) -> std::optional<std::string> {
                    auto it = out.values.find(n);
                    if (it == out.values.end()) return std::nullopt;
                    return it->second;
                }, resolved, missing);
            if (!ok) {
                return err("property '" + name +
                           "' references undefined property '" + missing + "'");
            }

            out.values[name] = resolved;
            out.resolutionOrder.push_back(name);
            return resolved;
        };

        // Eager, so a cycle or undefined reference fails at load, not at use.
        for (const auto& kv : manifest.propertiesRaw) {
            std::string name = kv.first.str();
            if (auto e = resolve(name); !e) {
                return e.takeError();
            }
        }

        // The fixed built-ins materialize too, so substitute() need not
        // re-derive them; the open namespaces stay lazy in lookup().
        auto putBuiltin = [&](const std::string& name) {
            if (out.values.count(name)) return;
            std::string biErr;
            auto v = resolveBuiltin(name, manifest, overrides, biErr);
            if (v) out.values[name] = *v;
        };
        putBuiltin("details.name");
        putBuiltin("details.version");
        putBuiltin("details.group");
        putBuiltin("details.library");
        putBuiltin("flavor");
        putBuiltin("profile");
        putBuiltin("target");
        putBuiltin("workspace.root");
        putBuiltin("cajeta.version");

        return out;
    }

    llvm::Expected<std::string> substitute(
        const std::string& s,
        const ResolvedProperties& props,
        const std::string& whereContext) {
        std::string result;
        std::string missing;
        bool ok = substituteOnce(s,
            [&](const std::string& name) -> std::optional<std::string> {
                return props.lookup(name);
            }, result, missing);
        if (!ok) {
            return err(whereContext + ": references undefined property '" +
                       missing + "'");
        }
        return result;
    }

    namespace {

        llvm::Error substituteJsonObject(llvm::json::Object& obj,
                                         const ResolvedProperties& props,
                                         const std::string& where);

        // Rewrites `${...}` in every string reachable from one JSON value.
        llvm::Error substituteJsonValue(llvm::json::Value& v,
                                        const ResolvedProperties& props,
                                        const std::string& where) {
            if (auto* obj = v.getAsObject()) {
                return substituteJsonObject(*obj, props, where);
            }
            if (auto* arr = v.getAsArray()) {
                for (size_t i = 0; i < arr->size(); ++i) {
                    if (auto e = substituteJsonValue(
                            (*arr)[i], props,
                            where + "[" + std::to_string(i) + "]")) {
                        return e;
                    }
                }
                return llvm::Error::success();
            }
            if (auto str = v.getAsString()) {
                auto out = substitute(str->str(), props, where);
                if (!out) return out.takeError();
                v = llvm::json::Value(*out);
            }
            return llvm::Error::success();
        }

        llvm::Error substituteJsonObject(llvm::json::Object& obj,
                                         const ResolvedProperties& props,
                                         const std::string& where) {
            for (auto& kv : obj) {
                if (auto e = substituteJsonValue(
                        kv.second, props, where + "." + kv.first.str())) {
                    return e;
                }
            }
            return llvm::Error::success();
        }

    } // namespace

    llvm::Error substituteManifestProperties(
        Manifest& m, const ResolvedProperties& props) {
        // `settings` and `plugins` ONLY: `tasks` carries late-bound references
        // that resolve at run time against TaskContext, and `details` is what
        // the built-ins derive from, so substituting it would be circular.
        if (auto e = substituteJsonObject(m.settingsRaw, props, "settings")) {
            return e;
        }
        return substituteJsonObject(m.pluginsRaw, props, "plugins");
    }

} // namespace cajeta::buildtool
