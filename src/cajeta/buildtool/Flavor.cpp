#include "cajeta/buildtool/Flavor.h"

#include <llvm/Support/Error.h>

#include <set>
#include <string>

namespace cajeta::buildtool {

    namespace {

        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(), msg);
        }

        // Inline a custom-flavor map's overrides into `out`, walking
        // the `base` chain until a built-in is hit. `chain` is the
        // visiting-set for cycle detection.
        llvm::Error inlineChain(
            const std::string& name,
            const llvm::json::Object& customFlavors,
            std::set<std::string>& chain,
            std::string& outBase,
            llvm::json::Object& outOverrides) {
            if (chain.count(name)) {
                std::string cyclePath;
                for (const auto& n : chain) {
                    if (!cyclePath.empty()) cyclePath += " → ";
                    cyclePath += n;
                }
                cyclePath += " → " + name;
                return err("custom-flavor cycle: " + cyclePath);
            }
            chain.insert(name);

            const auto* spec = customFlavors.getObject(name);
            if (!spec) {
                return err("flavor '" + name +
                           "' is not a built-in (release/debug) and "
                           "isn't declared in settings.build.custom-flavors");
            }
            auto baseV = spec->getString("base");
            if (!baseV) {
                return err("settings.build.custom-flavors." + name +
                           ": missing required 'base' field");
            }
            std::string base = baseV->str();

            // Recurse first so deeper overrides take precedence
            // for keys NOT overridden by the current level. Then
            // apply the current level's overrides on top.
            if (builtinFlavors().count(base)) {
                outBase = base;
            } else {
                if (auto e = inlineChain(base, customFlavors, chain,
                                         outBase, outOverrides)) {
                    return e;
                }
            }
            for (const auto& kv : *spec) {
                if (kv.first.str() == "base") continue;
                outOverrides[kv.first] = kv.second;
            }
            chain.erase(name);
            return llvm::Error::success();
        }

    } // namespace

    llvm::Expected<ResolvedFlavor> resolveFlavor(
        const llvm::json::Value& flavorRef,
        const llvm::json::Object& customFlavors) {
        ResolvedFlavor out;

        // String form.
        if (auto s = flavorRef.getAsString()) {
            std::string name = s->str();
            if (builtinFlavors().count(name)) {
                out.base = name;
                return out;
            }
            std::set<std::string> chain;
            if (auto e = inlineChain(name, customFlavors, chain,
                                     out.base, out.overrides)) {
                return std::move(e);
            }
            return out;
        }

        // Object form: inline composition.
        if (const auto* obj = flavorRef.getAsObject()) {
            auto baseV = obj->getString("base");
            if (!baseV) {
                return err("inline flavor: missing required 'base' field");
            }
            std::string base = baseV->str();
            if (builtinFlavors().count(base)) {
                out.base = base;
            } else {
                std::set<std::string> chain;
                if (auto e = inlineChain(base, customFlavors, chain,
                                         out.base, out.overrides)) {
                    return std::move(e);
                }
            }
            for (const auto& kv : *obj) {
                if (kv.first.str() == "base") continue;
                out.overrides[kv.first] = kv.second;
            }
            return out;
        }

        return err("flavor: value must be a string or an object "
                   "(string names a built-in or custom flavor; object "
                   "is an inline {base, ...overrides} composition)");
    }

} // namespace cajeta::buildtool
