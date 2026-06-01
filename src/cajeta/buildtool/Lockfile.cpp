#include "cajeta/buildtool/Lockfile.h"

#include "cajeta/buildtool/Dependency.h"
#include "cajeta/buildtool/Melt.h"
#include "cajeta/buildtool/Plugin.h"

#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>

#include <openssl/evp.h>
#include <openssl/sha.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#ifndef CAJETA_VERSION
#define CAJETA_VERSION "0.0.0-unknown"
#endif

namespace cajeta::buildtool {

    namespace {

        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(), msg);
        }

        std::string toHex(const unsigned char* data, size_t len) {
            static const char* digits = "0123456789abcdef";
            std::string s;
            s.reserve(len * 2);
            for (size_t i = 0; i < len; ++i) {
                s += digits[(data[i] >> 4) & 0xF];
                s += digits[data[i] & 0xF];
            }
            return s;
        }

    } // namespace

    std::string sha256Hex(const std::string& bytes) {
        unsigned char digest[SHA256_DIGEST_LENGTH];
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
        EVP_DigestUpdate(ctx, bytes.data(), bytes.size());
        unsigned int outLen = 0;
        EVP_DigestFinal_ex(ctx, digest, &outLen);
        EVP_MD_CTX_free(ctx);
        return "sha256:" + toHex(digest, outLen);
    }

    std::string nowIsoUtc() {
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
#ifdef _WIN32
        gmtime_s(&tm, &t);
#else
        gmtime_r(&t, &tm);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
        return std::string(buf);
    }

    Lockfile composeLockfile(
        const Manifest& manifest,
        const std::string& manifestSource,
        const ResolvedProperties& props,
        const std::string& nowIso) {
        (void)manifest;
        Lockfile lf;
        lf.lockfileVersion = 1;
        lf.manifestChecksum = sha256Hex(manifestSource);
        lf.generatorTool = "cajeta";
        lf.generatorVersion = CAJETA_VERSION;
        lf.resolvedAt = nowIso;
        lf.properties = props.values;
        // packages/melts/plugins/overrides stay empty until callers
        // either populate the typed slots (composeLockfileWithResolution)
        // or fill the raw escape hatches.
        return lf;
    }

    Lockfile composeLockfileWithResolution(
        const Manifest& manifest,
        const std::string& manifestSource,
        const ResolvedProperties& props,
        const std::vector<ResolvedDependency>& resolvedDeps,
        const MeltResolution& melts,
        const std::map<std::string, std::string>& meltProvidedBy,
        const std::vector<ResolvedPlugin>& resolvedPlugins,
        const std::string& nowIso) {
        auto lf = composeLockfile(manifest, manifestSource, props, nowIso);
        lf.packagesTyped.reserve(resolvedDeps.size());
        for (const auto& d : resolvedDeps) {
            ResolvedPackageEntry p;
            p.name = d.name;
            p.version = d.version;
            p.resolvedFromRepo = d.resolvedFromRepo;
            p.checksum = d.sha256;
            auto it = meltProvidedBy.find(d.name);
            p.providedBy = (it != meltProvidedBy.end())
                           ? it->second
                           : std::string("explicit");
            lf.packagesTyped.push_back(std::move(p));
        }
        lf.meltsTyped.reserve(melts.resolvedMelts.size());
        for (const auto& m : melts.resolvedMelts) {
            ResolvedMeltEntry me;
            me.name = m.name;
            me.version = m.version;
            me.resolvedFromRepo = m.resolvedFromRepo;
            me.checksum = m.sha256;
            for (const auto& t : m.transitiveMelts) {
                me.transitiveMelts.push_back(t.name + "@" + t.version);
            }
            lf.meltsTyped.push_back(std::move(me));
        }
        lf.pluginsTyped.reserve(resolvedPlugins.size());
        for (const auto& rp : resolvedPlugins) {
            ResolvedPluginEntry pe;
            pe.name = rp.name;
            pe.version = rp.version;
            pe.resolvedFromRepo = rp.resolvedFromRepo;
            pe.checksum = rp.sha256;
            // Capability list lands sorted so the lockfile is stable
            // across hash-set iteration order.
            pe.capabilities.assign(rp.capabilities.begin(),
                                   rp.capabilities.end());
            std::sort(pe.capabilities.begin(), pe.capabilities.end());
            lf.pluginsTyped.push_back(std::move(pe));
        }
        return lf;
    }

    DriftReport checkDrift(const Lockfile& lf,
                           const std::string& currentSource) {
        DriftReport rep;
        rep.oldChecksum = lf.manifestChecksum;
        rep.newChecksum = sha256Hex(currentSource);
        rep.changed = (rep.oldChecksum != rep.newChecksum);
        return rep;
    }

    llvm::Error writeLockfile(const std::string& path, const Lockfile& lf) {
        // Build the JSON document with deterministic key order so
        // round-tripping a lockfile produces byte-identical output —
        // useful for VCS-friendly diffs and reproducibility checks.
        llvm::json::Object root;
        root["lockfile-version"] = lf.lockfileVersion;
        root["manifest-checksum"] = lf.manifestChecksum;
        root["generator"] = llvm::json::Object{
            {"tool", lf.generatorTool},
            {"version", lf.generatorVersion},
        };
        root["resolved-at"] = lf.resolvedAt;

        // Properties — emit in sorted key order so the on-disk shape
        // is stable across runs that resolve the same property set in
        // different evaluation orders.
        llvm::json::Object propsObj;
        for (const auto& kv : lf.properties) {
            propsObj[kv.first] = kv.second;
        }
        root["properties"] = std::move(propsObj);

        // Packages array: typed entries get a deterministic shape
        // (name, version, resolved-from, checksum, provided-by). When
        // no typed entries are present, fall back to whatever was in
        // packagesRaw (Phase 2 compatibility path — composeLockfile
        // before melt resolution left it empty).
        {
            llvm::json::Array a;
            if (!lf.packagesTyped.empty()) {
                for (const auto& p : lf.packagesTyped) {
                    a.push_back(llvm::json::Object{
                        {"name",          p.name},
                        {"version",       p.version},
                        {"resolved-from", p.resolvedFromRepo},
                        {"checksum",      p.checksum},
                        {"provided-by",   p.providedBy.empty()
                                          ? std::string("explicit")
                                          : p.providedBy},
                    });
                }
            } else {
                a = lf.packagesRaw;
            }
            root["packages"] = llvm::json::Value(std::move(a));
        }
        // Melts array: typed entries get their own shape per spec
        // (name, version, resolved-from, checksum, transitive-melts).
        {
            llvm::json::Array a;
            if (!lf.meltsTyped.empty()) {
                for (const auto& mlt : lf.meltsTyped) {
                    llvm::json::Array transit;
                    for (const auto& t : mlt.transitiveMelts) {
                        transit.push_back(t);
                    }
                    a.push_back(llvm::json::Object{
                        {"name",             mlt.name},
                        {"version",          mlt.version},
                        {"resolved-from",    mlt.resolvedFromRepo},
                        {"checksum",         mlt.checksum},
                        {"transitive-melts", llvm::json::Value(
                                                std::move(transit))},
                    });
                }
            } else {
                a = lf.meltsRaw;
            }
            root["melts"] = llvm::json::Value(std::move(a));
        }
        // Plugins array: typed entries when populated, raw fallback
        // otherwise (Phase 2 schema slot stayed an empty array).
        {
            llvm::json::Array a;
            if (!lf.pluginsTyped.empty()) {
                for (const auto& p : lf.pluginsTyped) {
                    llvm::json::Array caps;
                    for (const auto& c : p.capabilities) {
                        caps.push_back(c);
                    }
                    a.push_back(llvm::json::Object{
                        {"name",          p.name},
                        {"version",       p.version},
                        {"resolved-from", p.resolvedFromRepo},
                        {"checksum",      p.checksum},
                        {"capabilities",  llvm::json::Value(
                                              std::move(caps))},
                    });
                }
            } else {
                a = lf.pluginsRaw;
            }
            root["plugins"] = llvm::json::Value(std::move(a));
        }
        {
            llvm::json::Array a = lf.overrides;
            root["overrides"] = llvm::json::Value(std::move(a));
        }

        std::string text;
        {
            llvm::raw_string_ostream os(text);
            os << llvm::formatv("{0:2}", llvm::json::Value(std::move(root)));
        }
        if (!text.empty() && text.back() != '\n') text += '\n';

        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            return err("cannot open lockfile for write: '" + path + "'");
        }
        out.write(text.data(), text.size());
        if (!out) {
            return err("error writing lockfile '" + path + "'");
        }
        return llvm::Error::success();
    }

    llvm::Expected<Lockfile> readLockfile(const std::string& path) {
        auto buf = llvm::MemoryBuffer::getFile(path);
        if (!buf) {
            return err("cannot open lockfile '" + path + "': " +
                       buf.getError().message());
        }
        // Strict JSON — no JSONC preprocessing here. The lockfile is
        // machine-only; comments would create merge conflicts.
        auto val = llvm::json::parse((*buf)->getBuffer());
        if (!val) {
            std::string msg;
            llvm::raw_string_ostream os(msg);
            os << "lockfile parse error in '" << path << "': "
               << val.takeError();
            return err(msg);
        }
        const auto* root = val->getAsObject();
        if (!root) {
            return err("lockfile '" + path + "' must be a JSON object");
        }

        Lockfile lf;
        if (auto v = root->getInteger("lockfile-version")) {
            lf.lockfileVersion = static_cast<int>(*v);
        } else {
            return err("lockfile missing 'lockfile-version' field");
        }
        if (auto v = root->getString("manifest-checksum")) {
            lf.manifestChecksum = v->str();
        } else {
            return err("lockfile missing 'manifest-checksum' field");
        }
        if (const auto* gen = root->getObject("generator")) {
            if (auto v = gen->getString("tool")) lf.generatorTool = v->str();
            if (auto v = gen->getString("version")) lf.generatorVersion = v->str();
        }
        if (auto v = root->getString("resolved-at")) {
            lf.resolvedAt = v->str();
        }
        if (const auto* p = root->getObject("properties")) {
            for (const auto& kv : *p) {
                auto s = kv.second.getAsString();
                if (s) lf.properties[kv.first.str()] = s->str();
            }
        }
        if (const auto* a = root->getArray("packages")) {
            lf.packagesRaw = *a;
            // Also parse into typed entries when the shape matches.
            // Unknown fields are preserved via the raw fallback so
            // round-trips don't drop schema additions.
            for (size_t i = 0; i < a->size(); ++i) {
                const auto* o = (*a)[i].getAsObject();
                if (!o) continue;
                ResolvedPackageEntry p;
                if (auto v = o->getString("name"))          p.name = v->str();
                if (auto v = o->getString("version"))       p.version = v->str();
                if (auto v = o->getString("resolved-from")) p.resolvedFromRepo = v->str();
                if (auto v = o->getString("checksum"))      p.checksum = v->str();
                if (auto v = o->getString("provided-by"))   p.providedBy = v->str();
                if (!p.name.empty() && !p.version.empty()) {
                    lf.packagesTyped.push_back(std::move(p));
                }
            }
        }
        if (const auto* a = root->getArray("melts")) {
            lf.meltsRaw = *a;
            for (size_t i = 0; i < a->size(); ++i) {
                const auto* o = (*a)[i].getAsObject();
                if (!o) continue;
                ResolvedMeltEntry me;
                if (auto v = o->getString("name"))          me.name = v->str();
                if (auto v = o->getString("version"))       me.version = v->str();
                if (auto v = o->getString("resolved-from")) me.resolvedFromRepo = v->str();
                if (auto v = o->getString("checksum"))      me.checksum = v->str();
                if (const auto* t = o->getArray("transitive-melts")) {
                    for (size_t j = 0; j < t->size(); ++j) {
                        if (auto s = (*t)[j].getAsString()) {
                            me.transitiveMelts.push_back(s->str());
                        }
                    }
                }
                if (!me.name.empty() && !me.version.empty()) {
                    lf.meltsTyped.push_back(std::move(me));
                }
            }
        }
        if (const auto* a = root->getArray("plugins")) {
            lf.pluginsRaw = *a;
            for (size_t i = 0; i < a->size(); ++i) {
                const auto* o = (*a)[i].getAsObject();
                if (!o) continue;
                ResolvedPluginEntry p;
                if (auto v = o->getString("name"))          p.name = v->str();
                if (auto v = o->getString("version"))       p.version = v->str();
                if (auto v = o->getString("resolved-from")) p.resolvedFromRepo = v->str();
                if (auto v = o->getString("checksum"))      p.checksum = v->str();
                if (const auto* cs = o->getArray("capabilities")) {
                    for (size_t j = 0; j < cs->size(); ++j) {
                        if (auto s = (*cs)[j].getAsString()) {
                            p.capabilities.push_back(s->str());
                        }
                    }
                }
                if (!p.name.empty() && !p.version.empty()) {
                    lf.pluginsTyped.push_back(std::move(p));
                }
            }
        }
        if (const auto* a = root->getArray("overrides")) {
            lf.overrides = *a;
        }
        return lf;
    }

} // namespace cajeta::buildtool
