// `verify-sig` — verify a detached ed25519 signature against an
// input file and a public key. Mirrors `cajeta archive verify-sig`.
//
// Params:
//   input         (required) the signed file
//   pubkey-env    OR
//   pubkey-path   (one required) PEM-encoded ed25519 public key
//   sig           (optional) signature file; default <input>.sig
//
// Outputs:
//   valid         "true" / "false"

#include "cajeta/buildtool/Action.h"

#include <llvm/Support/Error.h>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

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

        llvm::Expected<PkeyPtr> loadPublicKeyFromPem(const std::string& pemSource) {
            BioPtr bio(BIO_new_mem_buf(pemSource.data(),
                                       static_cast<int>(pemSource.size())));
            if (!bio) return err("verify-sig: BIO_new_mem_buf failed");
            PkeyPtr pkey(PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr));
            if (!pkey) return err("verify-sig: PEM_read_bio_PUBKEY: " +
                                  lastOpenSslError());
            if (EVP_PKEY_id(pkey.get()) != EVP_PKEY_ED25519) {
                return err("verify-sig: key is not ed25519");
            }
            return pkey;
        }

        std::vector<uint8_t> readFile(const std::string& path) {
            std::ifstream in(path, std::ios::binary);
            if (!in) return {};
            std::ostringstream ss; ss << in.rdbuf();
            const auto& s = ss.str();
            return std::vector<uint8_t>(s.begin(), s.end());
        }

    } // namespace

    class VerifySigAction : public Action {
    public:
        std::string name() const override { return "verify-sig"; }

        llvm::Expected<ActionResult> run(
            const llvm::json::Object& params,
            TaskContext& /*ctx*/) const override {

            auto input = params.getString("input");
            if (!input) return err("verify-sig: missing required 'input'");

            std::string sigPath;
            if (auto v = params.getString("sig")) sigPath = v->str();
            else sigPath = input->str() + ".sig";

            std::string pem;
            if (auto envName = params.getString("pubkey-env")) {
                const char* v = std::getenv(envName->str().c_str());
                if (!v || !*v) {
                    return err("verify-sig: env var '" + envName->str() +
                               "' is unset or empty");
                }
                pem = v;
            } else if (auto pathStr = params.getString("pubkey-path")) {
                std::ifstream in(pathStr->str());
                if (!in) return err("verify-sig: cannot open pubkey file '" +
                                    pathStr->str() + "'");
                std::ostringstream ss; ss << in.rdbuf();
                pem = ss.str();
            } else {
                return err("verify-sig: one of 'pubkey-env' or "
                           "'pubkey-path' is required");
            }

            auto pkey = loadPublicKeyFromPem(pem);
            if (!pkey) return pkey.takeError();

            std::ifstream inFile(input->str(), std::ios::binary);
            if (!inFile) return err("verify-sig: cannot open input '" +
                                    input->str() + "'");
            auto inputBytes = readFile(input->str());

            auto sigBytes = readFile(sigPath);
            if (sigBytes.empty()) {
                std::ifstream sigStat(sigPath, std::ios::binary);
                if (!sigStat) return err("verify-sig: cannot open signature '" +
                                         sigPath + "'");
            }

            MdCtxPtr ctx(EVP_MD_CTX_new());
            if (!ctx) return err("verify-sig: EVP_MD_CTX_new: " +
                                 lastOpenSslError());
            if (EVP_DigestVerifyInit(ctx.get(), nullptr, nullptr,
                                     nullptr, pkey->get()) != 1) {
                return err("verify-sig: EVP_DigestVerifyInit: " +
                           lastOpenSslError());
            }
            int rv = EVP_DigestVerify(ctx.get(), sigBytes.data(),
                                      sigBytes.size(),
                                      inputBytes.data(), inputBytes.size());

            ActionResult r;
            r.outputs["valid"] = (rv == 1) ? "true" : "false";
            if (rv != 1) {
                // Don't treat a bad signature as a hard error from
                // the action — the consumer's task decides what to
                // do (compare valid="false" against an expectation).
                // OpenSSL clears its error queue on negative result.
                ERR_clear_error();
            }
            return r;
        }
    };

    std::unique_ptr<Action> makeVerifySigAction() {
        return std::make_unique<VerifySigAction>();
    }

} // namespace cajeta::buildtool
