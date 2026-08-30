#include "cajeta/buildtool/RepositoryDelegation.h"

#include <llvm/Support/JSON.h>

namespace cajeta::buildtool {

    namespace {

        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                           "%s", msg.c_str());
        }

        // The signed discriminator. An organization key document does not
        // carry it, so one can never be read as a delegation — which would
        // let any organization's key sign release metadata for everybody.
        constexpr const char* kDelegationType = "repository-delegation";

    } // namespace

    std::vector<const OrgSigningKey*>
    RepositoryDelegation::usableKeys(std::time_t now) const {
        std::vector<const OrgSigningKey*> out;
        for (const auto& k : keys) {
            if (k.usableAt(now)) out.push_back(&k);
        }
        return out;
    }

    llvm::Expected<RepositoryDelegation> loadRepositoryDelegation(
            const std::string& envelopeJson,
            const std::vector<RootKey>& roots,
            std::time_t now) {
        // Verify before parsing, as everywhere else: nothing inside an
        // unverified document influences anything, including which errors
        // are reported about it.
        auto envelope = openSignedEnvelope(envelopeJson, roots,
                                           "repository delegation");
        if (!envelope) return envelope.takeError();

        auto body = llvm::json::parse(envelope->payload);
        if (!body) {
            llvm::consumeError(body.takeError());
            return err("repository delegation: payload is not valid JSON");
        }
        auto* obj = body->getAsObject();
        if (!obj) return err("repository delegation: payload is not an object");

        // The type check comes FIRST, before any field is read. A document of
        // the wrong kind must be refused as the wrong kind, not reported as a
        // malformed one — the latter invites someone to "fix" it by relaxing
        // a field requirement.
        auto type = obj->getString("type");
        if (!type || *type != kDelegationType) {
            return err("repository delegation: payload is not of type '"
                       + std::string(kDelegationType) + "'. A document that "
                       "does not say what it is must never be read as a "
                       "delegation — an organization key document read as one "
                       "would let that organization sign release metadata for "
                       "every other organization.");
        }

        RepositoryDelegation del;
        del.rootKeyId = envelope->rootKeyId;

        auto repo = obj->getString("repository");
        if (!repo || repo->empty()) {
            return err("repository delegation: no repository");
        }
        del.repository = repo->str();

        auto notAfter = obj->getString("not-after");
        if (!notAfter) return err("repository delegation: no not-after");
        auto expiry = parseUtcTimestamp(notAfter->str());
        if (!expiry) return expiry.takeError();
        del.notAfter = *expiry;

        auto* keys = obj->getArray("keys");
        if (!keys || keys->empty()) {
            return err("repository delegation for '" + del.repository
                       + "' lists no keys, so it delegates nothing");
        }
        auto parsed = parseSigningKeys(*keys, "repository delegation");
        if (!parsed) return parsed.takeError();
        del.keys = std::move(*parsed);

        if (now >= del.notAfter) {
            return err("repository delegation for '" + del.repository
                       + "' expired at " + notAfter->str()
                       + "; it is validly signed but out of date, and a stale "
                         "delegation is how a revoked signing key keeps "
                         "working");
        }
        return del;
    }

} // namespace cajeta::buildtool
