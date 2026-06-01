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
#include <unistd.h>

// The global environ pointer. `extern` declared at file scope (not
// inside the cajeta namespace below) so the link references ::environ
// rather than cajeta::buildtool::environ.
extern char** environ;

#ifndef CAJETA_VERSION
#define CAJETA_VERSION "0.0.0-unknown"
#endif

namespace cajeta::buildtool {

    namespace {

        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(), msg);
        }

        // The set of property names cajeta provides as built-ins.
        // User property declarations that collide with any of these are
        // hard errors at manifest-load time.
        //
        // `env.<NAME>` and `details.*` use dotted lookup; the prefix
        // (everything before the first `.`) is what matters for
        // collision detection.
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

        // Resolve a built-in property name to its value. Returns
        // nullopt if `name` isn't a recognized built-in (caller falls
        // through to user-property lookup).
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

            // env.<NAME> — look up the OS env variable. Missing env
            // resolves to empty string with a warning (today we silently
            // resolve to empty; an env-var-warning surface is on the
            // followup list).
            if (name.size() > 4 && name.compare(0, 4, "env.") == 0) {
                std::string envName = name.substr(4);
                const char* v = std::getenv(envName.c_str());
                return std::string(v ? v : "");
            }

            // artifact.* properties only resolve in distribution-action
            // context (Phase 9). For now: present but empty.
            if (name.size() > 9 && name.compare(0, 9, "artifact.") == 0) {
                return std::string("");
            }

            // Not a known built-in.
            (void)error;
            return std::nullopt;
        }

        // Find every `${NAME}` reference in a string. Returns the
        // names in order of appearance (duplicates included). Does
        // not validate that each name resolves — that's the caller's
        // job, since "X references Y" is needed for the dep graph
        // whether or not Y exists.
        //
        // Escape: `$$` is a literal `$` and does not start a reference.
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

        // Substitute all ${NAME} occurrences in `s`. Each lookup goes
        // through `lookup`; if `lookup` returns nullopt the substitution
        // fails with the missing name in `missing`.
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
                        // Unterminated reference — treat as literal so
                        // diagnostics can surface it at a higher layer.
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

        // Try to read a string-valued property from the manifest's
        // properties block. Returns nullopt if the key is absent or
        // not a string; in the latter case sets `typeError` so the
        // caller can produce a useful error message.
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
        // Lazy `env.*` resolution — the open namespace can't be
        // materialized into `values` ahead of time.
        if (name.size() > 4 && name.compare(0, 4, "env.") == 0) {
            std::string envName = name.substr(4);
            const char* v = std::getenv(envName.c_str());
            return std::string(v ? v : "");
        }
        // Lazy `artifact.*` resolution — Phase 9 will populate these
        // when actions publish them; for now they return empty.
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
        for (char** e = ::environ; *e; ++e) {
            std::string entry = *e;
            if (entry.compare(0, prefix.size(), prefix) != 0) continue;
            auto eq = entry.find('=', prefix.size());
            if (eq == std::string::npos) continue;
            std::string envSuffix = entry.substr(prefix.size(),
                                                 eq - prefix.size());
            std::string value = entry.substr(eq + 1);
            // CAJETA_PROPERTY_STACK_VERSION → "stack-version"
            // (underscores become hyphens; case lowered).
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

        // Step 1: validate that no user property collides with a
        // built-in. We do this up front because the collision is a
        // structural error, not a per-evaluation one.
        for (const auto& kv : manifest.propertiesRaw) {
            std::string name = kv.first.str();
            if (isBuiltinName(name)) {
                return err("property '" + name + "' in manifest collides "
                           "with a built-in property name");
            }
        }

        // Step 2: walk the dep graph via DFS, materializing values
        // bottom-up. A property's deps are the ${NAME}s referenced in
        // its value. We use post-order traversal with cycle detection
        // via a "visiting" set.
        ResolvedProperties out;

        std::unordered_set<std::string> visiting;

        // Look up a property's *unresolved* source text. Precedence:
        // CLI override > env override > manifest properties.
        // (Profiles are layered into the manifest before this point —
        // they merge into propertiesRaw during profile activation,
        // which lands in Phase 8. Today the profile slot is reserved
        // but unused.)
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

        // Recursive resolver. Returns the resolved value of `name`,
        // or propagates an error.
        std::function<llvm::Expected<std::string>(const std::string&)> resolve;
        resolve = [&](const std::string& name) -> llvm::Expected<std::string> {
            // Already done?
            if (auto it = out.values.find(name); it != out.values.end()) {
                return it->second;
            }

            // Built-in? Resolve directly; built-ins never reference
            // other properties.
            std::string biErr;
            if (auto bi = resolveBuiltin(name, manifest, overrides, biErr)) {
                out.values[name] = *bi;
                out.resolutionOrder.push_back(name);
                return *bi;
            }

            // Cycle?
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

            // User-defined? Find the source text and recurse on its
            // referenced names.
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

            // Now substitute the dependencies into our raw value.
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

        // Resolve every user-declared property eagerly. This catches
        // cycles and undefined references at load time rather than
        // deferring them to first-use.
        for (const auto& kv : manifest.propertiesRaw) {
            std::string name = kv.first.str();
            if (auto e = resolve(name); !e) {
                return e.takeError();
            }
        }

        // Eagerly materialize the fixed built-ins so substitute() can
        // resolve them without re-deriving on every call. Open-
        // namespace built-ins (env.*, artifact.*) stay lazy — see
        // ResolvedProperties::lookup().
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

} // namespace cajeta::buildtool
