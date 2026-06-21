#include "cajeta/buildtool/Flavor.h"

#include <llvm/Support/Error.h>

#include <set>
#include <string>
#include <vector>

namespace cajeta::buildtool {

    namespace {

        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(), msg);
        }

        // Join allowed values for citation, e.g. "O0 / O1 / O2 / O3 / Oz".
        std::string joinAllowed(const std::vector<std::string>& xs) {
            std::string out;
            for (size_t i = 0; i < xs.size(); ++i) {
                if (i) out += " / ";
                out += "\"" + xs[i] + "\"";
            }
            return out;
        }

        // Build the citation listing every known property key. Used
        // in the unknown-key error so the user sees the full vocab.
        std::string allowedPropertyKeyList() {
            std::string out;
            for (const auto& spec : flavorPropertyVocab()) {
                if (!out.empty()) out += ", ";
                out += spec.key;
            }
            return out;
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

    const std::vector<FlavorPropertySpec>& flavorPropertyVocab() {
        using K = FlavorPropertySpec::Kind;
        // 4th field = the compiler frontend flag this property lowers to
        // (empty = build-flavor intent with no frontend flag; see
        // FlavorPropertySpec::compilerFlag and BuildTool.md "Property
        // vocabulary"). `opt`'s `Oz` is flavor-only — the frontend's --opt
        // accepts O0..O3 — so a flavor using `Oz` is rejected at lowering.
        static const std::vector<FlavorPropertySpec> v = {
            {"opt",             K::EnumString, {"O0", "O1", "O2", "O3", "Oz"}, "opt"},
            {"cpu",             K::EnumString, {"native", "generic"},          "cpu"},
            {"lto",             K::EnumString, {"off", "thin", "full"},        "lto"},
            {"debug-info",      K::EnumString, {"off", "line", "full"},        ""},
            {"strip-symbols",   K::Boolean,    {},                            ""},
            {"bounds-check",    K::EnumString, {"on", "off", "trap"},          "bounds"},
            {"null-checks",     K::EnumString, {"on", "off", "trap"},          "null-checks"},
            {"overflow-checks", K::EnumString, {"on", "off", "wrapping"},      "overflow-checks"},
            {"asan",            K::Boolean,    {},                            ""},
            {"tsan",            K::Boolean,    {},                            ""},
            {"msan",            K::Boolean,    {},                            ""},
            {"ubsan",           K::Boolean,    {},                            ""},
            {"analytics",       K::Boolean,    {},                            ""},
            {"source-tags",     K::Boolean,    {},                            "source-tags"},
        };
        return v;
    }

    const FlavorPropertySpec* findFlavorPropertySpec(llvm::StringRef key) {
        for (const auto& spec : flavorPropertyVocab()) {
            if (spec.key == key) return &spec;
        }
        return nullptr;
    }

    llvm::Expected<llvm::json::Object> builtinFlavorProperties(
        llvm::StringRef name) {
        // Source of truth: BuildTool.md "Built-in flavors" table.
        if (name == "release") {
            return llvm::json::Object{
                {"opt",           "O2"},
                {"lto",           "thin"},
                {"strip-symbols", true},
                {"debug-info",    "line"},
                {"bounds-check",  "off"},
            };
        }
        if (name == "debug") {
            return llvm::json::Object{
                {"opt",           "O0"},
                {"lto",           "off"},
                {"strip-symbols", false},
                {"debug-info",    "full"},
                {"bounds-check",  "on"},
            };
        }
        return err("flavor '" + name.str() +
                   "' is not a built-in (allowed: release, debug)");
    }

    llvm::Error validateFlavorProperty(
        llvm::StringRef key,
        const llvm::json::Value& value,
        llvm::StringRef where) {
        const auto* spec = findFlavorPropertySpec(key);
        if (!spec) {
            return err(where.str() + ": unknown property key '" +
                       key.str() + "' (allowed: " +
                       allowedPropertyKeyList() + ")");
        }
        switch (spec->kind) {
        case FlavorPropertySpec::Kind::Boolean: {
            if (!value.getAsBoolean()) {
                return err(where.str() + ": property '" + key.str() +
                           "' must be a boolean (true / false), got " +
                           llvm::formatv("{0}", value).str());
            }
            return llvm::Error::success();
        }
        case FlavorPropertySpec::Kind::EnumString: {
            auto s = value.getAsString();
            if (!s) {
                return err(where.str() + ": property '" + key.str() +
                           "' must be a string (one of " +
                           joinAllowed(spec->allowed) + ")");
            }
            std::string sv = s->str();
            for (const auto& a : spec->allowed) {
                if (a == sv) return llvm::Error::success();
            }
            return err(where.str() + ": property '" + key.str() +
                       "' value '" + sv + "' not allowed (one of " +
                       joinAllowed(spec->allowed) + ")");
        }
        }
        return llvm::Error::success();
    }

    llvm::Error validateCustomFlavors(
        const llvm::json::Object& customFlavors) {
        // First pass: every override key/value validates against the
        // vocabulary. The base chain check follows.
        for (const auto& kv : customFlavors) {
            std::string name = kv.first.str();
            const auto* spec = kv.second.getAsObject();
            if (!spec) {
                return err("settings.build.custom-flavors." + name +
                           ": entry must be an object");
            }
            for (const auto& propKv : *spec) {
                std::string pk = propKv.first.str();
                if (pk == "base") {
                    auto bs = propKv.second.getAsString();
                    if (!bs) {
                        return err("settings.build.custom-flavors." +
                                   name + ".base: must be a string");
                    }
                    continue;
                }
                if (auto e = validateFlavorProperty(
                        pk, propKv.second,
                        "settings.build.custom-flavors." + name)) {
                    return e;
                }
            }
        }
        // Second pass: chain traversal for cycle + missing-base
        // detection. We re-use inlineChain by feeding a dry-run
        // base + overrides accumulator we discard.
        for (const auto& kv : customFlavors) {
            std::string name = kv.first.str();
            std::set<std::string> chain;
            std::string dryBase;
            llvm::json::Object dryOverrides;
            if (auto e = inlineChain(name, customFlavors, chain,
                                     dryBase, dryOverrides)) {
                return e;
            }
        }
        return llvm::Error::success();
    }

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
                std::string k = kv.first.str();
                if (k == "base") continue;
                // Inline overrides also go through the vocab check,
                // so a typo in a one-shot composition surfaces at the
                // build action's resolve step (it would have surfaced
                // at load too if the same composition lived in
                // custom-flavors).
                if (auto e = validateFlavorProperty(
                        k, kv.second, "inline flavor")) {
                    return std::move(e);
                }
                out.overrides[kv.first] = kv.second;
            }
            return out;
        }

        return err("flavor: value must be a string or an object "
                   "(string names a built-in or custom flavor; object "
                   "is an inline {base, ...overrides} composition)");
    }

    llvm::Expected<llvm::json::Object> effectiveProperties(
        const ResolvedFlavor& r) {
        auto base = builtinFlavorProperties(r.base);
        if (!base) return base.takeError();
        for (const auto& kv : r.overrides) {
            (*base)[kv.first] = kv.second;
        }
        return std::move(*base);
    }

    std::vector<std::string> toCompilerFlags(
        const llvm::json::Object& props) {
        // Vocabulary order keeps argv deterministic regardless of
        // how the JSON object stores keys.
        std::vector<std::string> out;
        for (const auto& spec : flavorPropertyVocab()) {
            // Only properties that map to a compiler frontend flag are
            // lowered to argv; the rest (lto, debug-info, strip-symbols,
            // sanitizers, analytics) are build-flavor intent honored at the
            // emit/link stage, not understood by the frontend.
            if (spec.compilerFlag.empty()) continue;
            const auto* v = props.get(spec.key);
            if (!v) continue;
            std::string rendered;
            switch (spec.kind) {
            case FlavorPropertySpec::Kind::Boolean: {
                auto b = v->getAsBoolean();
                if (!b) continue; // skip malformed silently — validator catches.
                rendered = *b ? "on" : "off";  // frontend bools accept on/off
                break;
            }
            case FlavorPropertySpec::Kind::EnumString: {
                auto s = v->getAsString();
                if (!s) continue;
                rendered = s->str();
                break;
            }
            }
            out.push_back("--" + spec.compilerFlag + "=" + rendered);
        }
        return out;
    }

} // namespace cajeta::buildtool
