// The `sign` action: a detached ed25519 signature over a file's bytes, by the same
// OpenSSL flow `cajeta archive sign` uses.

#include "cajeta/buildtool/Action.h"

#include <llvm/Support/Error.h>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/sha.h>

#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace cajeta::buildtool {

    namespace {

        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(), msg);
        }

        std::string lastOpenSslError() {
            unsigned long e = ERR_get_error();
            char buf[256];
            ERR_error_string_n(e, buf, sizeof(buf));
            ERR_clear_error();
            return buf;
        }

        struct BioFree { void operator()(BIO* b) const { if (b) BIO_free(b); } };
        struct PkeyFree { void operator()(EVP_PKEY* k) const { if (k) EVP_PKEY_free(k); } };
        struct MdCtxFree { void operator()(EVP_MD_CTX* c) const { if (c) EVP_MD_CTX_free(c); } };

        using BioPtr   = std::unique_ptr<BIO, BioFree>;
        using PkeyPtr  = std::unique_ptr<EVP_PKEY, PkeyFree>;
        using MdCtxPtr = std::unique_ptr<EVP_MD_CTX, MdCtxFree>;

        // Parse a PEM-encoded private key, rejecting anything that is not ed25519.
        llvm::Expected<PkeyPtr> loadPrivateKeyFromPem(const std::string& pemSource) {
            BioPtr bio(BIO_new_mem_buf(pemSource.data(),
                                       static_cast<int>(pemSource.size())));
            if (!bio) return err("sign: BIO_new_mem_buf failed");
            PkeyPtr pkey(PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr));
            if (!pkey) return err("sign: PEM_read_bio_PrivateKey: " +
                                  lastOpenSslError());
            if (EVP_PKEY_id(pkey.get()) != EVP_PKEY_ED25519) {
                return err("sign: key is not ed25519 (expected an "
                           "Ed25519 PRIVATE KEY PEM)");
            }
            return pkey;
        }

        // SHA-256 of `bytes`, rendered as "sha256:<hex>".
        std::string sha256Hex(const std::vector<uint8_t>& bytes) {
            unsigned char digest[SHA256_DIGEST_LENGTH];
            EVP_MD_CTX* ctx = EVP_MD_CTX_new();
            EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
            EVP_DigestUpdate(ctx, bytes.data(), bytes.size());
            unsigned int outLen = 0;
            EVP_DigestFinal_ex(ctx, digest, &outLen);
            EVP_MD_CTX_free(ctx);
            static const char* hexd = "0123456789abcdef";
            std::string s = "sha256:";
            s.reserve(7 + outLen * 2);
            for (unsigned i = 0; i < outLen; ++i) {
                s += hexd[(digest[i] >> 4) & 0xF];
                s += hexd[digest[i] & 0xF];
            }
            return s;
        }

        std::vector<uint8_t> readFile(const std::string& path) {
            std::ifstream in(path, std::ios::binary);
            if (!in) return {};
            std::ostringstream ss; ss << in.rdbuf();
            const auto& s = ss.str();
            return std::vector<uint8_t>(s.begin(), s.end());
        }

    } // namespace

    class SignAction : public Action {
    public:
        std::string name() const override { return "sign"; }

        // Requires `input` and `key-id` plus exactly one of `key-env` / `key-path`;
        // `out` defaults to `<input>.sig`. Reports back the .sig path, its sha256
        // and the key-id, for downstream actions.
        llvm::Expected<ActionResult> run(
            const llvm::json::Object& params,
            TaskContext& /*ctx*/) const override {

            auto input = params.getString("input");
            if (!input) return err("sign: missing required 'input'");
            auto keyId = params.getString("key-id");
            if (!keyId) return err("sign: missing required 'key-id'");

            std::string pem;
            if (auto envName = params.getString("key-env")) {
                const char* v = std::getenv(envName->str().c_str());
                if (!v || !*v) {
                    return err("sign: env var '" + envName->str() +
                               "' is unset or empty");
                }
                pem = v;
            } else if (auto pathStr = params.getString("key-path")) {
                std::ifstream in(pathStr->str());
                if (!in) return err("sign: cannot open key file '" +
                                    pathStr->str() + "'");
                std::ostringstream ss; ss << in.rdbuf();
                pem = ss.str();
            } else {
                return err("sign: one of 'key-env' or 'key-path' "
                           "is required");
            }

            auto pkey = loadPrivateKeyFromPem(pem);
            if (!pkey) return pkey.takeError();

            auto archiveBytes = readFile(input->str());
            if (archiveBytes.empty()) {
                std::ifstream in(input->str(), std::ios::binary);
                if (!in) {
                    return err("sign: cannot open input '" + input->str() + "'");
                }
            }

            MdCtxPtr ctx(EVP_MD_CTX_new());
            if (!ctx) return err("sign: EVP_MD_CTX_new: " +
                                 lastOpenSslError());
            if (EVP_DigestSignInit(ctx.get(), nullptr, nullptr,
                                   nullptr, pkey->get()) != 1) {
                return err("sign: EVP_DigestSignInit: " + lastOpenSslError());
            }
            size_t sigLen = 0;
            if (EVP_DigestSign(ctx.get(), nullptr, &sigLen,
                               archiveBytes.data(),
                               archiveBytes.size()) != 1) {
                return err("sign: probe signature length: " +
                           lastOpenSslError());
            }
            std::vector<uint8_t> sig(sigLen);
            if (EVP_DigestSign(ctx.get(), sig.data(), &sigLen,
                               archiveBytes.data(),
                               archiveBytes.size()) != 1) {
                return err("sign: EVP_DigestSign: " + lastOpenSslError());
            }
            sig.resize(sigLen);

            std::string outPath;
            if (auto v = params.getString("out")) outPath = v->str();
            else outPath = input->str() + ".sig";

            std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
            if (!out) {
                return err("sign: cannot open output '" + outPath + "'");
            }
            out.write(reinterpret_cast<const char*>(sig.data()),
                      static_cast<std::streamsize>(sig.size()));
            if (!out) {
                return err("sign: short write to '" + outPath + "'");
            }

            // The key-id sidecar beside the signature, `<out>.keyid`, is how the
            // launcher resolves the public key without out-of-band metadata.
            {
                std::ofstream kidOut(outPath + ".keyid", std::ios::trunc);
                if (kidOut) kidOut << keyId->str() << "\n";
            }

            ActionResult r;
            r.outputs["path"]   = outPath;
            r.outputs["sha256"] = sha256Hex(sig);
            r.outputs["key-id"] = keyId->str();
            return r;
        }
    };

    std::unique_ptr<Action> makeSignAction() {
        return std::make_unique<SignAction>();
    }

} // namespace cajeta::buildtool
