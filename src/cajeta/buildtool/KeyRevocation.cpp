#include "cajeta/buildtool/KeyRevocation.h"

#include <llvm/Support/JSON.h>

namespace cajeta::buildtool {

    namespace {

        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                           "%s", msg.c_str());
        }

        constexpr const char* kRevocationType = "key-revocation";

    } // namespace

    const RevokedKey* KeyRevocation::find(const std::string& keyId,
                                          const std::string& organization) const {
        for (const auto& r : revoked) {
            if (r.id != keyId) continue;
            // An entry with no organization applies to every document.
            if (r.organization.empty() || r.organization == organization) {
                return &r;
            }
        }
        return nullptr;
    }

    llvm::Expected<KeyRevocation> loadKeyRevocation(
            const std::string& envelopeJson,
            const RepositoryDelegation& delegation,
            const std::string& repositoryName,
            std::time_t now,
            std::time_t seenIssuedAt) {
        // Only the delegated keys verify. Building the verifier set from the
        // delegation rather than taking one as a parameter is what makes a
        // root signature unrepresentable here rather than merely discouraged.
        std::vector<RootKey> verifiers;
        for (const auto* k : delegation.usableKeys(now)) {
            verifiers.push_back(RootKey{k->id, k->publicKeyPem, false});
        }
        if (verifiers.empty()) {
            return err("revocation statement: the repository delegation has "
                       "no key inside its validity window right now, so "
                       "nothing it signs can be verified");
        }

        auto envelope = openSignedEnvelope(envelopeJson, verifiers,
                                           "revocation statement");
        if (!envelope) return envelope.takeError();

        auto body = llvm::json::parse(envelope->payload);
        if (!body) {
            llvm::consumeError(body.takeError());
            return err("revocation statement: payload is not valid JSON");
        }
        auto* obj = body->getAsObject();
        if (!obj) return err("revocation statement: payload is not an object");

        // Before any field is read, as with the delegation: a document of
        // the wrong kind is refused as the wrong kind, not reported as a
        // malformed one.
        auto type = obj->getString("type");
        if (!type || *type != kRevocationType) {
            return err("revocation statement: payload is not of type '"
                       + std::string(kRevocationType) + "'");
        }

        KeyRevocation rev;
        rev.signedByKeyId = envelope->rootKeyId;

        auto repo = obj->getString("repository");
        if (!repo || repo->empty()) {
            return err("revocation statement: no repository");
        }
        rev.repository = repo->str();
        if (rev.repository != repositoryName) {
            return err("revocation statement claims repository '"
                       + rev.repository + "' but was fetched from '"
                       + repositoryName + "'; one repository's statement "
                       "must not be replayable at another");
        }

        auto issued = obj->getString("issued-at");
        if (!issued) return err("revocation statement: no issued-at");
        auto issuedAt = parseUtcTimestamp(issued->str());
        if (!issuedAt) return issuedAt.takeError();
        rev.issuedAt = *issuedAt;

        auto notAfter = obj->getString("not-after");
        if (!notAfter) return err("revocation statement: no not-after");
        auto expiry = parseUtcTimestamp(notAfter->str());
        if (!expiry) return expiry.takeError();
        rev.notAfter = *expiry;

        // An EMPTY array is a valid statement — "nothing is revoked as of
        // issued-at" — and a different assertion from serving nothing. A
        // missing array is not the same thing and is malformed.
        const auto* revoked = obj->getArray("revoked");
        if (!revoked) return err("revocation statement: no revoked list");
        for (const auto& v : *revoked) {
            const auto* e = v.getAsObject();
            if (!e) return err("revocation statement: entry is not an object");
            auto id = e->getString("id");
            if (!id || id->empty()) {
                return err("revocation statement: entry has no id");
            }
            RevokedKey rk;
            rk.id = id->str();
            if (auto s = e->getString("organization")) rk.organization = s->str();
            if (auto s = e->getString("reason")) rk.reason = s->str();
            if (auto s = e->getString("revoked-at")) {
                auto t = parseUtcTimestamp(s->str());
                if (!t) return t.takeError();
                rk.revokedAt = *t;
            }
            rev.revoked.push_back(std::move(rk));
        }

        if (now >= rev.notAfter) {
            return err("revocation statement for '" + rev.repository
                       + "' expired at " + notAfter->str()
                       + "; a revocation an attacker can suppress is not a "
                         "revocation, so a stale one is refused rather than "
                         "read as 'nothing is revoked'");
        }
        if (seenIssuedAt != 0 && rev.issuedAt < seenIssuedAt) {
            return err("revocation statement for '" + rev.repository
                       + "' is older than one already seen; rolling back to "
                         "a previous list is how a revoked key comes back");
        }
        return rev;
    }

    llvm::Expected<std::optional<KeyRevocation>> revocationFor(
            const Repository& repo,
            const RepositoryDelegation* delegation,
            std::time_t now,
            std::time_t seenIssuedAt) {
        auto caps = repo.capabilities();
        if (!caps) return caps.takeError();
        if (!caps->revocation) {
            // Never advertised, so nothing is promised. Every other
            // document degrades this way; this one degrades ONLY here.
            return std::optional<KeyRevocation>{};
        }

        if (!delegation) {
            return err("repository '" + repo.name() + "' advertises "
                       "revocation but serves no delegation. The statement "
                       "is signed by a delegated key, so without one there "
                       "is nothing that could verify it.");
        }

        auto raw = repo.revocations();
        if (!raw) {
            return err("repository '" + repo.name() + "' advertises "
                       "revocation and its statement could not be fetched: "
                       + llvm::toString(raw.takeError())
                       + " Refusing rather than proceeding unrevoked.");
        }
        if (!raw->has_value()) {
            return err("repository '" + repo.name() + "' advertises "
                       "revocation but serves no statement. That is refused "
                       "rather than read as 'nothing is revoked' — otherwise "
                       "blocking one request would un-revoke every key it "
                       "has ever revoked.");
        }

        auto rev = loadKeyRevocation(**raw, *delegation, repo.name(), now,
                                     seenIssuedAt);
        if (!rev) return rev.takeError();
        return std::optional<KeyRevocation>(std::move(*rev));
    }

} // namespace cajeta::buildtool
