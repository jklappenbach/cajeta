#include "cajeta/buildtool/Lockfile.h"

#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>

#include <openssl/evp.h>
#include <openssl/sha.h>

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
        // packages/plugins/overrides stay empty until Phases 6/7.
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

        // json::Value only accepts Array via move; copy then move.
        {
            llvm::json::Array a = lf.packages;
            root["packages"] = llvm::json::Value(std::move(a));
        }
        {
            llvm::json::Array a = lf.plugins;
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
            lf.packages = *a;
        }
        if (const auto* a = root->getArray("plugins")) {
            lf.plugins = *a;
        }
        if (const auto* a = root->getArray("overrides")) {
            lf.overrides = *a;
        }
        return lf;
    }

} // namespace cajeta::buildtool
