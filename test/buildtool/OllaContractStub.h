// A server implementing publisher-trust's serving contract, §3 of
// specs/schemas/publisher-trust-protocol-v1.md.
//
// This exists so the contract is EXECUTABLE rather than prose alone (plan
// item 7.1.1). Everything here is what the document says a server does —
// endpoints, status codes, envelope shape — and `OllaContractTests.cpp`
// runs the shipped client against it. If the document and the client ever
// disagree, this stub is where it shows up.
//
// It covers §3 only. §4 through §6 — upload refusals and administration —
// have no client-observable surface and need their own tests wherever olla
// is built.

#pragma once

#include "OrgKeyFixture.h"
#include "TestHttpServer.h"

#include <filesystem>
#include <sstream>
#include <string>

namespace cajeta::buildtool::testing {

    class OllaContractStub {
    public:
        explicit OllaContractStub(std::filesystem::path scratch,
                                  std::string rootId = "olla-root-1")
            : scratch_(std::move(scratch)), rootId_(std::move(rootId)) {
            rootKey_ = makeKeyPair(scratch_ / "server-keys", "root");
            orgKey_ = makeKeyPair(scratch_ / "server-keys", "org");
            releaseKey_ = makeKeyPair(scratch_ / "server-keys", "release");
            advertiseV2(true);
        }

        std::string baseUrl() const { return srv_.baseUrl(); }

        // What a client fetching from this stub will compare a delegation
        // or revocation against — scheme://host:port, matching
        // HttpRepository::origin(). Documents must name THIS, not a
        // nickname, or they are refused as replays.
        std::string origin() const {
            const std::string u = srv_.baseUrl();
            auto scheme = u.find("://");
            if (scheme == std::string::npos) return u;
            auto slash = u.find('/', scheme + 3);
            return slash == std::string::npos ? u : u.substr(0, slash);
        }

        // The public root a client ships with (§2, spec 3.1).
        RootKey root() const { return rootKeyOf(rootKey_, rootId_); }
        const TestKeyPair& organizationKey() const { return orgKey_; }
        const std::string& rootId() const { return rootId_; }

        // §3.1 — everything else is v2. A server that implements the whole
        // contract and forgets this has disabled verification with no error
        // appearing anywhere, which is why it is a knob here.
        void advertiseV2(bool yes) {
            v2_ = yes;
            writeCapabilities();
        }

        // §3.1 — the `revocation` flag. Separate from advertiseV2 because
        // the two have OPPOSITE failure directions: an unadvertised v2
        // silently degrades, an advertised revocation that then goes
        // missing refuses. Both knobs exist so a test can show each.
        void advertiseRevocation(bool yes) {
            revocation_ = yes;
            writeCapabilities();
        }

        // §3.4 — the delegation naming which keys may sign release metadata
        // and the revocation statement.
        void serveDelegation(const std::string& repository = {}) {
            const std::string repo = repository.empty() ? origin() : repository;
            std::ostringstream p;
            p << "{\"type\":\"repository-delegation\","
              << "\"repository\":\"" << repo << "\","
              << "\"not-after\":\"2030-01-01T00:00:00Z\","
              << "\"keys\":[{\"id\":\"release-1\",\"algorithm\":\"ed25519\","
              << "\"public-key\":\"" << jsonEscapePem(readWholeFile(releaseKey_.pub))
              << "\",\"not-before\":\"2020-01-01T00:00:00Z\","
              << "\"not-after\":\"2030-01-01T00:00:00Z\"}]}";
            srv_.route("/v2/repository-keys", 200,
                       envelopeAround(scratch_, p.str(), rootKey_, rootId_,
                                      "delegation"));
        }

        // §3.8 — the revocation statement, signed by the DELEGATED key.
        // `revokedEntries` is raw JSON array contents; empty is the healthy
        // steady state and asserts that nothing is revoked.
        void serveRevocation(const std::string& revokedEntries,
                             const std::string& repository = {},
                             const std::string& issuedAt = "2026-06-01T00:00:00Z",
                             const std::string& notAfter = "2026-06-01T01:00:00Z") {
            const std::string repo = repository.empty() ? origin() : repository;
            std::ostringstream p;
            p << "{\"type\":\"key-revocation\","
              << "\"repository\":\"" << repo << "\","
              << "\"issued-at\":\"" << issuedAt << "\","
              << "\"not-after\":\"" << notAfter << "\","
              << "\"revoked\":[" << revokedEntries << "]}";
            srv_.route("/v2/revocations", 200,
                       envelopeAround(scratch_, p.str(), releaseKey_,
                                      "release-1", "revocation"));
        }

        const TestKeyPair& releaseKey() const { return releaseKey_; }

        // §3.3 — the organization key document.
        void serveOrganization(const OrgDocumentSpec& spec) {
            srv_.route("/v2/org-keys/" + spec.organization, 200,
                       orgKeyDocument(scratch_, spec, orgKey_, rootKey_,
                                      rootId_, "doc-" + spec.organization));
        }

        // §3.6 — a transient fault is a 5xx, never a 404. Serving 404 here
        // would convert an outage into a verification bypass.
        void failOrganization(const std::string& org, int status = 503) {
            srv_.route("/v2/org-keys/" + org, status, "unavailable");
        }

        // §3.4 — the resolve body: the plain v2 fields a non-verifying
        // client reads, plus the authoritative `signed` envelope.
        //
        // `plainSha256` and `plainOrganization` are what the UNSIGNED half
        // says. They exist as parameters so a test can make the two halves
        // disagree, which is the only way to show which one the client
        // reads.
        void serveRelease(const std::string& name, const std::string& version,
                          const std::string& sha256,
                          const std::string& organization,
                          const std::string& plainSha256 = {},
                          const std::string& plainOrganization = {}) {
            std::string envelope = envelopeAround(
                scratch_, releasePayload(name, version, sha256, organization),
                rootKey_, rootId_, "rel-" + name + "-" + version);
            routeRelease(name, version, plainSha256.empty() ? sha256 : plainSha256,
                         plainOrganization.empty() ? organization
                                                   : plainOrganization,
                         ",\"signed\":" + envelope);
        }

        // §3.4, the pre-signing server: plain fields only, no envelope. The
        // client must read this as binding nothing.
        void serveUnsignedRelease(const std::string& name,
                                  const std::string& version,
                                  const std::string& sha256,
                                  const std::string& organization) {
            routeRelease(name, version, sha256, organization, "");
        }

        // A release whose signed payload has been altered after signing —
        // a mirror editing what it was handed.
        void serveTamperedRelease(const std::string& name,
                                  const std::string& version,
                                  const std::string& sha256,
                                  const std::string& organization) {
            // Sign one payload, ship a different one.
            std::string signedFor =
                releasePayload(name, version, sha256, organization);
            std::string shipped =
                releasePayload(name, version, sha256, "com.attacker");
            std::string sig = signWithKey(scratch_, signedFor, rootKey_.priv,
                                          "tamper-" + name);
            std::ostringstream env;
            env << "{\"format\":1,\"root-key-id\":\"" << rootId_ << "\","
                << "\"payload\":\"" << llvm::encodeBase64(shipped) << "\","
                << "\"signature\":\"" << llvm::encodeBase64(sig) << "\"}";
            routeRelease(name, version, sha256, organization,
                         ",\"signed\":" + env.str());
        }

        // §5.2 — a RETRACTED release, with the plain half disagreeing.
        //
        // The signed payload says retracted; the plain `retracted` says
        // false, which is what a mirror clearing the flag looks like on the
        // wire. Serving them in agreement would prove nothing.
        void serveRetractedRelease(const std::string& name,
                                   const std::string& version,
                                   const std::string& sha256,
                                   const std::string& organization,
                                   const std::string& reason) {
            std::string envelope = envelopeAround(
                scratch_,
                retractedReleasePayload(name, version, sha256, organization,
                                        true, reason),
                rootKey_, rootId_, "retr-" + name + "-" + version);
            routeRelease(name, version, sha256, organization,
                         ",\"signed\":" + envelope);
        }

        // §3.6 — the bytes.
        void serveBlob(const std::string& bareSha256, const std::string& body) {
            srv_.route("/v2/blob/" + bareSha256, 200, body,
                       "application/octet-stream");
        }

    private:
        void routeRelease(const std::string& name, const std::string& version,
                          const std::string& plainSha,
                          const std::string& plainOrg,
                          const std::string& signedMember) {
            std::ostringstream body;
            body << "{\"sha256\":\"" << plainSha << "\",\"size\":0,"
                 << "\"organization\":\"" << plainOrg << "\","
                 // Always FALSE in the plain half. serveRetractedRelease
                 // relies on it: the signed payload says retracted and this
                 // says otherwise, so a test can tell which one was read.
                 << "\"retracted\":false" << signedMember << "}";
            srv_.route("/v2/resolve?name=" + name + "&version=" + version,
                       200, body.str());
        }

        void writeCapabilities() {
            std::ostringstream c;
            c << "{\"protocol-versions\":"
              << (v2_ ? R"(["v1","v2"])" : R"(["v1"])")
              << ",\"revocation\":" << (revocation_ ? "true" : "false")
              << "}";
            srv_.route("/.well-known/cajeta-capabilities.json", 200, c.str());
        }

        std::filesystem::path scratch_;
        std::string rootId_;
        TestKeyPair rootKey_;
        TestKeyPair orgKey_;
        TestKeyPair releaseKey_;
        bool v2_ = true;
        bool revocation_ = false;
        TestHttpServer srv_;
    };

} // namespace cajeta::buildtool::testing
