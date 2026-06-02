#include "cajeta/cli/SignatureVerify.h"

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/sha.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace cajeta::cli {

    namespace {

        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(), msg);
        }

        std::string readBinary(const std::string& path) {
            std::ifstream in(path, std::ios::binary);
            if (!in) return {};
            std::ostringstream ss; ss << in.rdbuf();
            return ss.str();
        }

        std::string sha256Hex(const std::string& bytes) {
            unsigned char digest[SHA256_DIGEST_LENGTH];
            EVP_MD_CTX* ctx = EVP_MD_CTX_new();
            EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
            EVP_DigestUpdate(ctx, bytes.data(), bytes.size());
            unsigned int dlen = 0;
            EVP_DigestFinal_ex(ctx, digest, &dlen);
            EVP_MD_CTX_free(ctx);
            static const char* hexd = "0123456789abcdef";
            std::string s;
            s.reserve(dlen * 2);
            for (unsigned i = 0; i < dlen; ++i) {
                s += hexd[(digest[i] >> 4) & 0xF];
                s += hexd[digest[i] & 0xF];
            }
            return s;
        }

        std::string strip(const std::string& s) {
            auto begin = s.find_first_not_of(" \t\r\n");
            if (begin == std::string::npos) return {};
            auto end = s.find_last_not_of(" \t\r\n");
            return s.substr(begin, end - begin + 1);
        }

    } // namespace

    std::string readKeyIdSidecar(const std::string& archivePath) {
        std::ifstream in(archivePath + ".sig.keyid");
        if (!in) return {};
        std::string line;
        std::getline(in, line);
        return strip(line);
    }

    llvm::Error writeKeyIdSidecar(const std::string& archivePath,
                                  const std::string& keyId) {
        std::ofstream out(archivePath + ".sig.keyid",
                          std::ios::trunc);
        if (!out) {
            return err("writeKeyIdSidecar: cannot open '" +
                       archivePath + ".sig.keyid'");
        }
        out << keyId << "\n";
        return llvm::Error::success();
    }

    llvm::Expected<VerifyResult> verifyArchiveSignature(
        const TrustStoreLayout& layout,
        const std::string& archivePath,
        const VerifyOptions& opts) {
        namespace fs = std::filesystem;
        std::error_code ec;
        if (!fs::exists(archivePath, ec)) {
            return err("verify: archive '" + archivePath + "' not found");
        }
        std::string sigPath = opts.signaturePathOverride
            ? *opts.signaturePathOverride : (archivePath + ".sig");
        if (!fs::exists(sigPath, ec)) {
            return err("verify: signature '" + sigPath + "' not found "
                       "(`cajeta archive sign` writes <archive>.sig)");
        }
        std::string keyId = opts.keyIdOverride
            ? *opts.keyIdOverride : readKeyIdSidecar(archivePath);
        if (keyId.empty()) {
            return err("verify: no key-id available "
                       "(`cajeta archive sign` writes "
                       "<archive>.sig.keyid; pass --key-id to override)");
        }
        auto entry = lookupTrustedKey(layout, keyId);
        if (!entry) {
            return err("verify: key-id '" + keyId +
                       "' not found in any trust-store tier "
                       "(env / user / system) — run "
                       "`cajeta trust add " + keyId + " <pem-path>`");
        }
        // Load the PEM public key.
        std::ifstream pemIn(entry->path);
        std::ostringstream pemSS; pemSS << pemIn.rdbuf();
        std::string pem = pemSS.str();
        BIO* bio = BIO_new_mem_buf(pem.data(),
                                   static_cast<int>(pem.size()));
        EVP_PKEY* pk = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);
        if (!pk) {
            return err("verify: trust-store key '" + entry->path +
                       "' is not a valid PEM public key");
        }
        if (EVP_PKEY_id(pk) != EVP_PKEY_ED25519) {
            EVP_PKEY_free(pk);
            return err("verify: trust-store key '" + entry->path +
                       "' is not ed25519");
        }
        std::string archiveBytes = readBinary(archivePath);
        std::string sigBytes = readBinary(sigPath);
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        if (EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, pk) != 1) {
            EVP_MD_CTX_free(ctx);
            EVP_PKEY_free(pk);
            return err("verify: EVP_DigestVerifyInit failed");
        }
        int rc = EVP_DigestVerify(
            ctx,
            reinterpret_cast<const unsigned char*>(sigBytes.data()),
            sigBytes.size(),
            reinterpret_cast<const unsigned char*>(archiveBytes.data()),
            archiveBytes.size());
        EVP_MD_CTX_free(ctx);
        EVP_PKEY_free(pk);
        if (rc != 1) {
            std::string computed = sha256Hex(archiveBytes);
            return err("verify: signature does NOT match key-id '" +
                       keyId + "' (computed archive digest sha256:" +
                       computed + " — archive bytes were tampered or "
                       "the wrong key is recorded)");
        }
        VerifyResult out;
        out.keyId = keyId;
        out.fingerprint = entry->fingerprint;
        out.archiveSha256 = sha256Hex(archiveBytes);
        return out;
    }

} // namespace cajeta::cli
