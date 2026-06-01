// `sign` — produce a detached ed25519 signature over an
// archive's bytes. Mirrors the OpenSSL flow that
// `cajeta archive sign` uses (cli/ArchiveCommands.cpp). See
// ArchiveManagement.md §8 for the on-disk signature format.
//
// Params:
//   input       (required) path to the file to sign
//   key-env     OR
//   key-path    (one required) PEM-encoded ed25519 private key
//   key-id      (required) opaque identifier stored alongside the
//               signature; the launcher uses it to look up the
//               matching public key in the trust store
//   out         (optional) signature file path; default <input>.sig
//
// Outputs:
//   path        the .sig file
//   sha256      SHA-256 of the .sig contents (the 64 ed25519 bytes)
//   key-id      echoed for downstream actions

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

        llvm::Expected<ActionResult> run(
            const llvm::json::Object& params,
            TaskContext& /*ctx*/) const override {

            auto input = params.getString("input");
            if (!input) return err("sign: missing required 'input'");
            auto keyId = params.getString("key-id");
            if (!keyId) return err("sign: missing required 'key-id'");

            // Key source: either key-env (read from env var) or
            // key-path (read from disk).
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
                // Distinguish empty file from unreadable; check
                // existence-and-size.
                std::ifstream in(input->str(), std::ios::binary);
                if (!in) {
                    return err("sign: cannot open input '" + input->str() + "'");
                }
                // Empty file is legal; sign produces a signature
                // over the empty byte string.
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

            // Output path: explicit `out` or default to <input>.sig.
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
