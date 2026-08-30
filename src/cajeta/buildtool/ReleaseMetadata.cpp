#include "cajeta/buildtool/ReleaseMetadata.h"

#include <llvm/Support/JSON.h>

namespace cajeta::buildtool {

    namespace {

        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                           "%s", msg.c_str());
        }

        // The comparison side (ArtifactCache::sha256OfFile) always produces
        // the prefixed form, so an un-normalised value here would fail every
        // verification it touched.
        std::string normaliseDigest(std::string sha) {
            if (!sha.empty() && sha.rfind("sha256:", 0) != 0) {
                sha = "sha256:" + sha;
            }
            return sha;
        }

        void readFields(const llvm::json::Object& obj, ReleaseMetadata& md) {
            if (auto s = obj.getString("name")) md.name = s->str();
            if (auto s = obj.getString("version")) md.version = s->str();
            if (auto s = obj.getString("sha256")) {
                md.sha256 = normaliseDigest(s->str());
            }
            if (auto s = obj.getString("organization")) {
                md.organization = s->str();
            }
        }

    } // namespace

    llvm::Expected<ReleaseMetadata> loadReleaseMetadata(
            const std::string& json,
            const std::vector<RootKey>& roots) {
        auto parsed = llvm::json::parse(json);
        if (!parsed) {
            llvm::consumeError(parsed.takeError());
            return err("release metadata: not valid JSON");
        }
        auto* obj = parsed->getAsObject();
        if (!obj) return err("release metadata: not a JSON object");

        // A bare envelope, or one carried beside the plain body. Either way
        // the envelope is authoritative and the plain fields are ignored —
        // merging them is how an unsigned value ends up trusted.
        const llvm::json::Object* envelopeObj = nullptr;
        if (obj->get("payload") && obj->get("signature")) {
            envelopeObj = obj;
        } else if (const auto* nested = obj->getObject("signed")) {
            envelopeObj = nested;
        }

        ReleaseMetadata md;
        if (envelopeObj) {
            // Re-serialising the envelope is safe precisely because the
            // payload is opaque base64: round-tripping the wrapper cannot
            // disturb the bytes the signature covers.
            llvm::json::Object copy = *envelopeObj;
            llvm::json::Value value(std::move(copy));
            std::string envelopeJson;
            llvm::raw_string_ostream os(envelopeJson);
            os << value;
            os.flush();
            auto envelope = openSignedEnvelope(envelopeJson, roots,
                                               "release metadata");
            if (!envelope) return envelope.takeError();

            auto body = llvm::json::parse(envelope->payload);
            if (!body) {
                llvm::consumeError(body.takeError());
                return err("release metadata: signed payload is not valid JSON");
            }
            auto* inner = body->getAsObject();
            if (!inner) {
                return err("release metadata: signed payload is not an object");
            }
            readFields(*inner, md);
            md.signedByRoot = true;
            md.rootKeyId = envelope->rootKeyId;
        } else {
            readFields(*obj, md);
        }

        if (md.sha256.empty()) {
            return err("release metadata: no sha256");
        }
        return md;
    }

} // namespace cajeta::buildtool
