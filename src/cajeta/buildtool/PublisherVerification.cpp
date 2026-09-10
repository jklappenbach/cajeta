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

        // Appended to every refusal: a contact nothing surfaces is worse than none.
        std::string contactSuffix(const OrgKeyDocument& doc) {
            if (doc.securityContact.uri.empty()) return {};
            std::string out = " Report a problem with '" + doc.organization
                            + "' to " + doc.securityContact.uri;
            if (!doc.securityContact.label.empty()) {
                out += " (" + doc.securityContact.label + ")";
            }
            return out + ".";
        }

    } // namespace

    bool namespaceOwns(const std::string& nameSpace, const std::string& name) {
        if (nameSpace.empty() || name.empty()) return false;
        if (name == nameSpace) return true;
        // The separator is the whole point: without it `dev.cajeta` would own
        // `dev.cajetaevil`, and an attacker picks the name.
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

        // Namespace first: cheapest, and the most informative failure.
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
                        "good." + contactSuffix(doc);
            return v;
        }

        auto usable = doc.usableKeys(now);
        if (usable.empty()) {
            // An empty set is legitimate and reads "cannot verify", not "verified".
            v.check = PublisherCheck::NoUsableKey;
            v.message = "'" + doc.organization + "' has no signing key "
                        "inside its validity window right now, so nothing it "
                        "published can be verified. The organization needs a "
                        "current key published before this installs." + contactSuffix(doc);
            return v;
        }

        bool unreadable = false;
        // Held, not returned: a good key may sit beside this revoked one.
        const RevokedKey* blockedBy = nullptr;
        for (const auto* key : usable) {
            if (revocation) {
                if (const auto* r = revocation->find(key->id, doc.organization)) {
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
                // Remembered, so a document whose keys are ALL unusable does not
                // report as a clean mismatch.
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
                        "the only thing that installs." + contactSuffix(doc);
            return v;
        }

        if (unreadable) {
            v.check = PublisherCheck::Unreadable;
            v.message = "the signature for '" + artifactName + "' could not "
                        "be checked: no key in '" + doc.organization
                      + "'s key document could be read as an ed25519 public "
                        "key. This is 'we could not check', not 'it is fine'." + contactSuffix(doc);
            return v;
        }

        v.check = PublisherCheck::Signature;
        v.message = "the signature for '" + artifactName + "' does not match "
                    "any of the " + std::to_string(usable.size())
                  + " key(s) '" + doc.organization + "' has valid right now. "
                    "The bytes are signed, but not by the organization that "
                    "owns this name." + contactSuffix(doc);
        return v;
    }

} // namespace cajeta::buildtool
