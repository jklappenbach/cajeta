#include "cajeta/buildtool/Signature.h"

#include <openssl/evp.h>
#include <openssl/pem.h>

#include <fstream>
#include <memory>
#include <sstream>

namespace cajeta::buildtool {

    namespace {

        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                           "%s", msg.c_str());
        }

        struct BioDeleter {
            void operator()(BIO* b) const { if (b) BIO_free(b); }
        };
        struct PkeyDeleter {
            void operator()(EVP_PKEY* p) const { if (p) EVP_PKEY_free(p); }
        };
        struct MdCtxDeleter {
            void operator()(EVP_MD_CTX* c) const { if (c) EVP_MD_CTX_free(c); }
        };

        llvm::Expected<std::string> readAll(const std::string& path) {
            std::ifstream in(path, std::ios::binary);
            if (!in) return err("cannot read '" + path + "'");
            std::ostringstream buf;
            buf << in.rdbuf();
            return buf.str();
        }

        // nullptr with no error = "this PEM is not a usable ed25519 public
        // key", which verifyAgainstAnyKey treats as skip-this-key.
        std::unique_ptr<EVP_PKEY, PkeyDeleter> loadEd25519Pub(
                const std::string& pemPath) {
            std::unique_ptr<BIO, BioDeleter> bio(
                BIO_new_file(pemPath.c_str(), "r"));
            if (!bio) return nullptr;
            std::unique_ptr<EVP_PKEY, PkeyDeleter> key(
                PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr));
            if (!key) return nullptr;
            if (EVP_PKEY_id(key.get()) != EVP_PKEY_ED25519) return nullptr;
            return key;
        }

    } // namespace

    llvm::Expected<std::string> readSignatureFile(const std::string& path) {
        return readAll(path);
    }

    llvm::Expected<bool> verifyDetachedEd25519(const std::string& dataPath,
                                               const std::string& signature,
                                               const std::string& pemPath) {
        auto data = readAll(dataPath);
        if (!data) return data.takeError();

        auto key = loadEd25519Pub(pemPath);
        if (!key) {
            return err("'" + pemPath + "' is not a readable ed25519 public "
                       "key");
        }

        std::unique_ptr<EVP_MD_CTX, MdCtxDeleter> ctx(EVP_MD_CTX_new());
        if (!ctx) return err("openssl: EVP_MD_CTX_new failed");
        if (EVP_DigestVerifyInit(ctx.get(), nullptr, nullptr, nullptr,
                                 key.get()) != 1) {
            return err("openssl: DigestVerifyInit failed for '" + pemPath
                       + "'");
        }
        // ed25519 is single-shot: no streaming update, the whole message
        // goes in at once (same call shape `archive verify-sig` uses).
        int rv = EVP_DigestVerify(
            ctx.get(),
            reinterpret_cast<const unsigned char*>(signature.data()),
            signature.size(),
            reinterpret_cast<const unsigned char*>(data->data()),
            data->size());
        return rv == 1;
    }

    llvm::Expected<std::optional<std::string>> verifyAgainstAnyKey(
            const std::string& dataPath,
            const std::string& signature,
            const std::vector<std::string>& pemPaths) {
        for (const auto& pem : pemPaths) {
            auto ok = verifyDetachedEd25519(dataPath, signature, pem);
            if (!ok) {
                // An unusable key is not an answer about the signature.
                llvm::consumeError(ok.takeError());
                continue;
            }
            if (*ok) return std::optional<std::string>{pem};
        }
        return std::optional<std::string>{};
    }

} // namespace cajeta::buildtool
