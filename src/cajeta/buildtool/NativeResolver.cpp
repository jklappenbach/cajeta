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

    // --- Unit 5: resolution ----------------------------------------------

    namespace {
        // Minimal semver: -1 component = unspecified / wildcard (sorts low).
        struct SemVer { int major = -1, minor = -1, patch = -1; };

        SemVer parseSemVer(const std::string& s) {
            SemVer v;
            int* slots[3] = {&v.major, &v.minor, &v.patch};
            size_t start = 0, idx = 0;
            while (idx < 3 && start <= s.size()) {
                size_t dot = s.find('.', start);
                std::string comp = s.substr(start, dot == std::string::npos
                    ? std::string::npos : dot - start);
                if (comp == "*" || comp.empty()) {
                    *slots[idx] = -1;
                } else {
                    int n = 0; bool ok = !comp.empty();
                    for (char c : comp) {
                        if (c < '0' || c > '9') { ok = false; break; }
                        n = n * 10 + (c - '0');
                    }
                    *slots[idx] = ok ? n : -1;
                }
                ++idx;
                if (dot == std::string::npos) break;
                start = dot + 1;
            }
            return v;
        }

        // Order by (major, minor, patch); -1 sorts below any concrete value.
        bool higher(const SemVer& a, const SemVer& b) {
            if (a.major != b.major) return a.major > b.major;
            if (a.minor != b.minor) return a.minor > b.minor;
            return a.patch > b.patch;
        }
    } // namespace

    llvm::Expected<std::string> selectNativeVersion(
            const std::vector<std::string>& constraints) {
        if (constraints.empty())
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(), "no version constraints");
        // Incompatible majors → conflict.
        int major = -2;
        for (const auto& c : constraints) {
            int m = parseSemVer(c).major;
            if (major == -2) major = m;
            else if (m != major)
                return llvm::createStringError(
                    llvm::inconvertibleErrorCode(),
                    "incompatible native version constraints (major mismatch)");
        }
        // Pick the highest (most specific) constraint.
        const std::string* best = &constraints.front();
        SemVer bestV = parseSemVer(*best);
        for (const auto& c : constraints) {
            SemVer v = parseSemVer(c);
            if (higher(v, bestV)) { bestV = v; best = &c; }
        }
        return *best;
    }

    NativeResolution resolveNatives(
            const NativeRequirementSet& reqs,
            const std::map<std::string, NativeOverride>& overrides,
            const std::string& platform,
            const std::vector<NativeProvider>& providers) {
        NativeResolution out;
        for (const auto& id : reqs.required) {
            auto ovIt = overrides.find(id);
            const NativeOverride* ov = ovIt != overrides.end()
                ? &ovIt->second : nullptr;

            // Version: override wins; else select from constraints.
            std::string version;
            if (ov && ov->version) {
                version = *ov->version;
            } else {
                auto vcIt = reqs.versionConstraints.find(id);
                if (vcIt == reqs.versionConstraints.end()
                        || vcIt->second.empty()) {
                    // No metadata. An override path can still satisfy it.
                    if (!(ov && ov->path)) {
                        out.unresolved[id] =
                            "no resolution metadata for '" + id +
                            "' (unsatisfied native requirement)";
                        continue;
                    }
                } else {
                    auto v = selectNativeVersion(vcIt->second);
                    if (!v) {
                        out.unresolved[id] = llvm::toString(v.takeError());
                        continue;
                    }
                    version = *v;
                }
            }

            std::string link = "static";
            auto libIt = reqs.libraries.find(id);
            if (libIt != reqs.libraries.end()) link = libIt->second.link;

            // Override path short-circuits the provider chain.
            if (ov && ov->path) {
                out.resolved[id] = ResolvedNative{
                    id, version, platform, *ov->path, link, "override-path"};
                continue;
            }

            bool done = false;
            for (const auto& p : providers) {
                if (!p.supply) continue;
                auto path = p.supply(id, version, platform);
                if (path) {
                    out.resolved[id] = ResolvedNative{
                        id, version, platform, *path, link, p.name};
                    done = true;
                    break;
                }
            }
            if (!done) {
                out.unresolved[id] = "no provider supplied '" + id + "' " +
                    version + " for " + platform;
            }
        }
        return out;
    }

} // namespace cajeta::buildtool
