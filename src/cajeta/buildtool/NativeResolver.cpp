// Native-dependency resolver — unit 4 (collection). See NativeResolver.h.

#include "NativeResolver.h"

#include <llvm/Support/JSON.h>

namespace cajeta::buildtool {

    llvm::Expected<NativeMeta> parseNativeMeta(
            llvm::StringRef json, const std::string& src) {
        auto fail = [&](const std::string& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(), src + ": " + msg);
        };
        auto parsed = llvm::json::parse(json);
        if (!parsed) return parsed.takeError();
        const auto* root = parsed->getAsObject();
        if (!root) return fail("native metadata must be a JSON object");

        NativeMeta meta;
        if (const auto* req = root->getArray("requires")) {
            for (const auto& r : *req) {
                if (auto s = r.getAsString()) meta.requiredLibs.insert(s->str());
                else return fail("'requires' entries must be strings");
            }
        }
        if (const auto* libs = root->getObject("libraries")) {
            for (const auto& kv : *libs) {
                const std::string id = kv.first.str();
                const auto* o = kv.second.getAsObject();
                if (!o) return fail("libraries." + id + " must be an object");
                auto lib = parseNativeLibraryEntry(
                    id, *o, src + ": libraries." + id);
                if (!lib) return lib.takeError();
                meta.libraries[id] = std::move(*lib);
            }
        }
        return meta;
    }

    llvm::Expected<NativeRequirementSet> collectNativeRequirements(
            const std::vector<const cajeta::CajetaArchive*>& archives) {
        NativeRequirementSet out;
        for (const auto* arc : archives) {
            const auto* metaEntry = arc->nativeLibrariesMeta();
            if (!metaEntry) continue;  // slim archive — no native metadata
            llvm::StringRef json(
                reinterpret_cast<const char*>(metaEntry->data.data()),
                metaEntry->data.size());
            auto meta = parseNativeMeta(json, arc->getName());
            if (!meta) return meta.takeError();
            for (const auto& id : meta->requiredLibs) out.required.insert(id);
            for (auto& kv : meta->libraries) {
                out.versionConstraints[kv.first].push_back(kv.second.version);
                // First declaration's metadata wins the payload slot; unit 5
                // resolves the concrete version across versionConstraints.
                out.libraries.emplace(kv.first, kv.second);
            }
        }
        for (const auto& id : out.required) {
            if (out.libraries.find(id) == out.libraries.end())
                out.unsatisfied.insert(id);
        }
        return out;
    }

} // namespace cajeta::buildtool
