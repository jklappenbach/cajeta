#include "cajeta/cli/TrustStore.h"

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/sha.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

namespace cajeta::cli {

    namespace {

        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(), msg);
        }

        std::string envOrEmpty(const char* name) {
            const char* v = std::getenv(name);
            return v ? std::string(v) : std::string{};
        }

        std::string hexOf(const unsigned char* bytes, size_t n) {
            static const char* hexd = "0123456789abcdef";
            std::string s;
            s.reserve(n * 2);
            for (size_t i = 0; i < n; ++i) {
                s += hexd[(bytes[i] >> 4) & 0xF];
                s += hexd[bytes[i] & 0xF];
            }
            return s;
        }

    } // namespace

    TrustStoreLayout resolveTrustStoreLayout(
        std::optional<std::string> homeOverride,
        std::optional<std::string> systemOverride,
        std::optional<std::string> envOverride) {
        namespace fs = std::filesystem;
        TrustStoreLayout out;

        std::string envDir = envOverride
            ? *envOverride : envOrEmpty("CAJETA_TRUST_KEYS_DIR");
        if (!envDir.empty()) {
            out.envRoot = envDir;
            out.roots.push_back(envDir);
        }

        std::string home = homeOverride
            ? *homeOverride : envOrEmpty("HOME");
        if (!home.empty()) {
            out.userRoot = (fs::path(home) /
                            ".cajeta" / "trust" / "keys").string();
            out.roots.push_back(out.userRoot);
        }

        std::string sys = systemOverride
            ? *systemOverride
#ifdef _WIN32
            : (envOrEmpty("ProgramData").empty()
                 ? std::string("C:\\ProgramData\\cajeta\\trust\\keys")
                 : (fs::path(envOrEmpty("ProgramData")) /
                    "cajeta" / "trust" / "keys").string());
#else
            : std::string("/etc/cajeta/trust/keys");
#endif
        if (!sys.empty()) {
            out.systemRoot = sys;
            out.roots.push_back(sys);
        }
        return out;
    }

    // Walk one tier; populate keyId/path/fingerprint for every .pem.
    static std::vector<TrustStoreEntry> listOneTier(
        const std::string& root, const std::string& tier) {
        namespace fs = std::filesystem;
        std::vector<TrustStoreEntry> out;
        std::error_code ec;
        if (root.empty() || !fs::is_directory(root, ec)) return out;
        for (auto& it : fs::directory_iterator(root, ec)) {
            if (ec) break;
            if (!it.is_regular_file()) continue;
            auto p = it.path();
            if (p.extension() != ".pem") continue;
            TrustStoreEntry e;
            e.keyId = p.stem().string();
            e.path = p.string();
            e.tier = tier;
            auto fp = fingerprintOfPemFile(p.string());
            if (fp) e.fingerprint = *fp;
            else consumeError(fp.takeError());
            out.push_back(std::move(e));
        }
        return out;
    }

    std::vector<TrustStoreEntry> listTrustedKeys(
        const TrustStoreLayout& layout) {
        std::vector<TrustStoreEntry> out;
        std::set<std::string> seen;
        // Tier order matches roots vector — but list each only once,
        // attributing it to the winning tier.
        for (size_t i = 0; i < layout.roots.size(); ++i) {
            std::string tier;
            if (layout.roots[i] == layout.envRoot)         tier = "env";
            else if (layout.roots[i] == layout.userRoot)   tier = "user";
            else if (layout.roots[i] == layout.systemRoot) tier = "system";
            else                                           tier = "other";
            for (auto& e : listOneTier(layout.roots[i], tier)) {
                if (seen.insert(e.keyId).second) {
                    out.push_back(std::move(e));
                }
            }
        }
        return out;
    }

    std::optional<TrustStoreEntry> lookupTrustedKey(
        const TrustStoreLayout& layout,
        const std::string& keyId) {
        namespace fs = std::filesystem;
        for (size_t i = 0; i < layout.roots.size(); ++i) {
            if (layout.roots[i].empty()) continue;
            fs::path p = fs::path(layout.roots[i]) / (keyId + ".pem");
            std::error_code ec;
            if (!fs::is_regular_file(p, ec)) continue;
            TrustStoreEntry e;
            e.keyId = keyId;
            e.path = p.string();
            if (layout.roots[i] == layout.envRoot)        e.tier = "env";
            else if (layout.roots[i] == layout.userRoot)  e.tier = "user";
            else if (layout.roots[i] == layout.systemRoot) e.tier = "system";
            else                                          e.tier = "other";
            auto fp = fingerprintOfPemFile(p.string());
            if (fp) e.fingerprint = *fp;
            else consumeError(fp.takeError());
            return e;
        }
        return std::nullopt;
    }

    llvm::Error addTrustedKey(const TrustStoreLayout& layout,
                              const std::string& keyId,
                              const std::string& pemPath) {
        namespace fs = std::filesystem;
        if (layout.userRoot.empty()) {
            return err("trust add: no user trust-store root (set HOME)");
        }
        std::error_code ec;
        fs::create_directories(layout.userRoot, ec);
        if (ec) {
            return err("trust add: cannot create '" +
                       layout.userRoot + "': " + ec.message());
        }
        fs::path dest = fs::path(layout.userRoot) / (keyId + ".pem");
        if (fs::exists(dest, ec)) {
            return err("trust add: key-id '" + keyId +
                       "' already exists in the user store at '" +
                       dest.string() + "' (use `trust remove " +
                       keyId + "` first)");
        }
        // Validate the input is an Ed25519 public key PEM.
        std::ifstream in(pemPath);
        if (!in) {
            return err("trust add: cannot read '" + pemPath + "'");
        }
        std::ostringstream ss; ss << in.rdbuf();
        std::string pem = ss.str();
        if (pem.find("PUBLIC KEY") == std::string::npos) {
            return err("trust add: '" + pemPath +
                       "' doesn't look like a PUBLIC KEY PEM "
                       "(expected `BEGIN PUBLIC KEY` block)");
        }
        BIO* bio = BIO_new_mem_buf(pem.data(),
                                   static_cast<int>(pem.size()));
        EVP_PKEY* pk = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);
        if (!pk) {
            return err("trust add: '" + pemPath +
                       "' is not a valid PEM public key");
        }
        int id = EVP_PKEY_id(pk);
        EVP_PKEY_free(pk);
        if (id != EVP_PKEY_ED25519) {
            return err("trust add: '" + pemPath +
                       "' is not an ed25519 public key");
        }
        // Copy bytes.
        std::ofstream out(dest, std::ios::binary | std::ios::trunc);
        if (!out) {
            return err("trust add: cannot write '" + dest.string() + "'");
        }
        out << pem;
        return llvm::Error::success();
    }

    llvm::Error removeTrustedKey(const TrustStoreLayout& layout,
                                 const std::string& keyId) {
        namespace fs = std::filesystem;
        if (layout.userRoot.empty()) {
            return err("trust remove: no user trust-store root");
        }
        fs::path dest = fs::path(layout.userRoot) / (keyId + ".pem");
        std::error_code ec;
        if (!fs::exists(dest, ec)) {
            return err("trust remove: '" + keyId +
                       "' is not in the user store (system + env "
                       "tiers are read-only at this level)");
        }
        fs::remove(dest, ec);
        if (ec) {
            return err("trust remove: cannot remove '" +
                       dest.string() + "': " + ec.message());
        }
        return llvm::Error::success();
    }

    llvm::Expected<std::string> fingerprintOfPemFile(
        const std::string& pemPath) {
        std::ifstream in(pemPath);
        if (!in) return err("fingerprint: cannot open '" + pemPath + "'");
        std::ostringstream ss; ss << in.rdbuf();
        std::string pem = ss.str();
        BIO* bio = BIO_new_mem_buf(pem.data(),
                                   static_cast<int>(pem.size()));
        EVP_PKEY* pk = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);
        if (!pk) {
            return err("fingerprint: '" + pemPath +
                       "' is not a valid PEM public key");
        }
        unsigned char* der = nullptr;
        int len = i2d_PUBKEY(pk, &der);
        EVP_PKEY_free(pk);
        if (len <= 0 || !der) {
            return err("fingerprint: cannot serialize key DER");
        }
        unsigned char digest[SHA256_DIGEST_LENGTH];
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
        EVP_DigestUpdate(ctx, der, static_cast<size_t>(len));
        unsigned int dlen = 0;
        EVP_DigestFinal_ex(ctx, digest, &dlen);
        EVP_MD_CTX_free(ctx);
        OPENSSL_free(der);
        return hexOf(digest, dlen);
    }

} // namespace cajeta::cli
