#include "cajeta/buildtool/PublisherVerification.h"

#include "cajeta/buildtool/Signature.h"

#include <sstream>

namespace cajeta::buildtool {

    namespace {

        std::string joinNamespaces(const std::vector<std::string>& ns) {
            std::ostringstream out;
            for (size_t i = 0; i < ns.size(); ++i) {
                if (i) out << ", ";
                out << ns[i];
            }
            return out.str();
        }

    } // namespace

    bool namespaceOwns(const std::string& nameSpace, const std::string& name) {
        if (nameSpace.empty() || name.empty()) return false;
        if (name == nameSpace) return true;
        // The separator is the whole point: without it `dev.cajeta` would
        // own `dev.cajetaevil`, and an attacker picks the name.
        return name.size() > nameSpace.size()
            && name.compare(0, nameSpace.size(), nameSpace) == 0
            && name[nameSpace.size()] == '.';
    }

    PublisherVerdict verifyAgainstOrgDocument(const OrgKeyDocument& doc,
                                              const std::string& artifactName,
                                              const std::string& artifactPath,
                                              const std::string& signature,
                                              std::time_t now,
                                              const KeyRevocation* revocation) {
        PublisherVerdict v;
        v.organization = doc.organization;

        // Namespace first. It is the cheapest check and the most
        // informative failure: "this org does not own that name" tells a
        // reader something a signature mismatch does not.
        bool owned = false;
        for (const auto& ns : doc.namespaces) {
            if (namespaceOwns(ns, artifactName)) { owned = true; break; }
        }
        if (!owned) {
            v.check = PublisherCheck::Namespace;
            v.message = "'" + artifactName + "' is outside the namespaces '"
                      + doc.organization + "' owns ("
                      + joinNamespaces(doc.namespaces) + "). A key valid for "
                        "one organization must not sign another's name, so "
                        "this is refused whether or not the signature is "
                        "good.";
            return v;
        }

        auto usable = doc.usableKeys(now);
        if (usable.empty()) {
            // An empty set is a legitimate parse result — every key out of
            // its window — and must read as "cannot verify", never as
            // "verified". Reporting it separately keeps that explicit.
            v.check = PublisherCheck::NoUsableKey;
            v.message = "'" + doc.organization + "' has no signing key "
                        "inside its validity window right now, so nothing it "
                        "published can be verified. The organization needs a "
                        "current key published before this installs.";
            return v;
        }

        bool unreadable = false;
        // A key that WOULD have verified but is revoked. Held rather than
        // returned immediately: a document may list a revoked key beside a
        // good one, and the good one still verifies.
        const RevokedKey* blockedBy = nullptr;
        for (const auto* key : usable) {
            if (revocation) {
                if (const auto* r = revocation->find(key->id, doc.organization)) {
                    // Check the signature anyway, so the verdict can say
                    // "the key that signed this is revoked" rather than the
                    // much weaker "some revoked key exists".
                    auto match = verifyDetachedEd25519File(
                        artifactPath, signature, key->publicKeyPem);
                    if (!match) {
                        llvm::consumeError(match.takeError());
                    } else if (*match) {
                        blockedBy = r;
                    }
                    continue;
                }
            }
            auto ok = verifyDetachedEd25519File(artifactPath, signature,
                                                key->publicKeyPem);
            if (!ok) {
                // An unusable key is not an answer about the signature.
                // Remember it, so a document whose keys are ALL unusable
                // does not report as a clean mismatch.
                llvm::consumeError(ok.takeError());
                unreadable = true;
                continue;
            }
            if (*ok) {
                v.check = PublisherCheck::Verified;
                v.keyId = key->id;
                return v;
            }
        }

        // Before the generic mismatch: a revoked key that matches is a
        // different fact from no key matching, and the two send an operator
        // to different places.
        if (blockedBy) {
            v.check = PublisherCheck::Revoked;
            v.keyId = blockedBy->id;
            v.message = "'" + artifactName + "' is signed by key '"
                      + blockedBy->id + "' of '" + doc.organization
                      + "', which has been REVOKED"
                      + (blockedBy->reason.empty()
                             ? std::string(".")
                             : ": " + blockedBy->reason)
                      + " The signature is genuine; the key is not trusted "
                        "any more. A new release signed by a current key is "
                        "the only thing that installs.";
            return v;
        }

        if (unreadable) {
            v.check = PublisherCheck::Unreadable;
            v.message = "the signature for '" + artifactName + "' could not "
                        "be checked: no key in '" + doc.organization
                      + "'s key document could be read as an ed25519 public "
                        "key. This is 'we could not check', not 'it is fine'.";
            return v;
        }

        v.check = PublisherCheck::Signature;
        v.message = "the signature for '" + artifactName + "' does not match "
                    "any of the " + std::to_string(usable.size())
                  + " key(s) '" + doc.organization + "' has valid right now. "
                    "The bytes are signed, but not by the organization that "
                    "owns this name.";
        return v;
    }

} // namespace cajeta::buildtool
