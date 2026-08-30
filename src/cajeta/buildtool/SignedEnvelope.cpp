#include "cajeta/buildtool/SignedEnvelope.h"

#include "cajeta/buildtool/Signature.h"

#include <llvm/Support/Base64.h>
#include <llvm/Support/JSON.h>

namespace cajeta::buildtool {

    namespace {

        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                           "%s", msg.c_str());
        }

        // The envelope version this build understands. Anything else is
        // refused rather than interpreted: a future format may move a
        // security-relevant field, and guessing at it is how a check
        // silently stops applying.
        constexpr int kSupportedFormat = 1;

    } // namespace

    bool looksLikeSignedEnvelope(const std::string& envelopeJson) {
        auto parsed = llvm::json::parse(envelopeJson);
        if (!parsed) {
            llvm::consumeError(parsed.takeError());
            return false;
        }
        auto* obj = parsed->getAsObject();
        if (!obj) return false;
        return obj->get("payload") && obj->get("signature");
    }

    llvm::Expected<SignedEnvelope> openSignedEnvelope(
            const std::string& envelopeJson,
            const std::vector<RootKey>& roots,
            const std::string& what) {
        auto parsed = llvm::json::parse(envelopeJson);
        if (!parsed) {
            llvm::consumeError(parsed.takeError());
            return err(what + ": envelope is not valid JSON");
        }
        auto* env = parsed->getAsObject();
        if (!env) return err(what + ": envelope is not an object");

        auto format = env->getInteger("format");
        if (!format) return err(what + ": no format version");
        if (*format != kSupportedFormat) {
            return err(what + ": format version " + std::to_string(*format)
                       + " is not understood by this toolchain (expected "
                       + std::to_string(kSupportedFormat) + ")");
        }

        auto rootKeyId = env->getString("root-key-id");
        auto payloadB64 = env->getString("payload");
        auto signatureB64 = env->getString("signature");
        if (!rootKeyId || !payloadB64 || !signatureB64) {
            return err(what + ": envelope is missing root-key-id, payload or "
                              "signature");
        }

        std::vector<char> payloadBytes;
        if (auto e = llvm::decodeBase64(*payloadB64, payloadBytes)) {
            llvm::consumeError(std::move(e));
            return err(what + ": payload is not base64");
        }
        std::vector<char> sigBytes;
        if (auto e = llvm::decodeBase64(*signatureB64, sigBytes)) {
            llvm::consumeError(std::move(e));
            return err(what + ": signature is not base64");
        }

        SignedEnvelope out;
        out.payload.assign(payloadBytes.begin(), payloadBytes.end());
        std::string signature(sigBytes.begin(), sigBytes.end());

        for (const auto& root : roots) {
            auto ok = verifyDetachedEd25519PemBytes(out.payload, signature,
                                                    root.pem);
            if (!ok) {                    // unusable root, not an answer
                llvm::consumeError(ok.takeError());
                continue;
            }
            if (*ok) {
                out.rootKeyId = root.id;
                return out;
            }
        }
        return err(what + ": signature does not match any trusted root key ("
                   + std::to_string(roots.size()) + " checked)");
    }

} // namespace cajeta::buildtool
