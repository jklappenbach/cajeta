#include "cajeta/buildtool/OrgKeyDocument.h"

#include "cajeta/buildtool/Signature.h"

#include <string>

#include <llvm/Support/Base64.h>
#include <llvm/Support/JSON.h>


namespace cajeta::buildtool {

    namespace {

        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                           "%s", msg.c_str());
        }

        // The envelope version this build understands. A document declaring
        // anything else is refused rather than interpreted: a future format
        // may move a security-relevant field, and guessing at it is how a
        // check silently stops applying.
        constexpr int kSupportedFormat = 1;

        // Days before month m (0-based) in a non-leap year.
        constexpr int kDaysBeforeMonth[12] = {
            0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
        };

        bool isLeap(int y) {
            return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
        }

        // Days from 1970-01-01 to y-m-d. Computed rather than taken from
        // timegm, which is absent on some of the platforms this ships to and
        // timezone-sensitive on others; a validity window must not depend on
        // the host's TZ.
        long long daysFromEpoch(int y, int m, int d) {
            long long days = 0;
            for (int year = 1970; year < y; ++year) days += isLeap(year) ? 366 : 365;
            days += kDaysBeforeMonth[m - 1];
            if (m > 2 && isLeap(y)) days += 1;
            return days + (d - 1);
        }

    } // namespace

    llvm::Expected<std::time_t> parseUtcTimestamp(const std::string& text) {
        // Exactly YYYY-MM-DDTHH:MM:SSZ. Length is checked first so a longer
        // string carrying an offset or fractional seconds cannot pass by
        // matching a prefix.
        if (text.size() != 20 || text[4] != '-' || text[7] != '-'
            || text[10] != 'T' || text[13] != ':' || text[16] != ':'
            || text[19] != 'Z') {
            return err("'" + text + "' is not an RFC 3339 UTC timestamp "
                       "(YYYY-MM-DDTHH:MM:SSZ)");
        }
        for (size_t i : {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u,
                         11u, 12u, 14u, 15u, 17u, 18u}) {
            if (text[i] < '0' || text[i] > '9') {
                return err("'" + text + "' is not an RFC 3339 UTC timestamp");
            }
        }
        auto num = [&](size_t at, size_t len) {
            int v = 0;
            for (size_t i = at; i < at + len; ++i) v = v * 10 + (text[i] - '0');
            return v;
        };
        int year = num(0, 4), mon = num(5, 2), day = num(8, 2);
        int hh = num(11, 2), mm = num(14, 2), ss = num(17, 2);
        if (year < 1970 || mon < 1 || mon > 12 || day < 1 || day > 31
            || hh > 23 || mm > 59 || ss > 60) {
            return err("'" + text + "' is out of range");
        }
        long long secs = daysFromEpoch(year, mon, day) * 86400LL
                       + hh * 3600LL + mm * 60LL + ss;
        return static_cast<std::time_t>(secs);
    }

    std::vector<const OrgSigningKey*>
    OrgKeyDocument::usableKeys(std::time_t now) const {
        std::vector<const OrgSigningKey*> out;
        for (const auto& k : keys) {
            if (k.usableAt(now)) out.push_back(&k);
        }
        return out;
    }

    llvm::Expected<OrgKeyDocument> loadOrgKeyDocument(
            const std::string& envelopeJson,
            const std::vector<RootKey>& roots,
            std::time_t now) {
        auto parsed = llvm::json::parse(envelopeJson);
        if (!parsed) {
            llvm::consumeError(parsed.takeError());
            return err("organization key document: envelope is not valid JSON");
        }
        auto* env = parsed->getAsObject();
        if (!env) return err("organization key document: envelope is not an object");

        auto format = env->getInteger("format");
        if (!format) return err("organization key document: no format version");
        if (*format != kSupportedFormat) {
            return err("organization key document: format version "
                       + std::to_string(*format) + " is not understood by this "
                       "toolchain (expected " + std::to_string(kSupportedFormat)
                       + ")");
        }

        auto rootKeyId = env->getString("root-key-id");
        auto payloadB64 = env->getString("payload");
        auto signatureB64 = env->getString("signature");
        if (!rootKeyId || !payloadB64 || !signatureB64) {
            return err("organization key document: envelope is missing "
                       "root-key-id, payload or signature");
        }

        std::vector<char> payloadBytes;
        if (auto e = llvm::decodeBase64(*payloadB64, payloadBytes)) {
            llvm::consumeError(std::move(e));
            return err("organization key document: payload is not base64");
        }
        std::vector<char> sigBytes;
        if (auto e = llvm::decodeBase64(*signatureB64, sigBytes)) {
            llvm::consumeError(std::move(e));
            return err("organization key document: signature is not base64");
        }
        std::string payload(payloadBytes.begin(), payloadBytes.end());
        std::string signature(sigBytes.begin(), sigBytes.end());

        // Verify BEFORE parsing the payload. Nothing inside an unverified
        // document should influence anything, including which errors are
        // reported about it.
        bool verified = false;
        std::string verifiedBy;
        for (const auto& root : roots) {
            auto ok = verifyDetachedEd25519PemBytes(payload, signature, root.pem);
            if (!ok) {                    // unusable root, not an answer
                llvm::consumeError(ok.takeError());
                continue;
            }
            if (*ok) { verified = true; verifiedBy = root.id; break; }
        }
        if (!verified) {
            return err("organization key document: signature does not match "
                       "any trusted root key (" + std::to_string(roots.size())
                       + " checked)");
        }

        auto body = llvm::json::parse(payload);
        if (!body) {
            llvm::consumeError(body.takeError());
            return err("organization key document: payload is not valid JSON");
        }
        auto* obj = body->getAsObject();
        if (!obj) return err("organization key document: payload is not an object");

        OrgKeyDocument doc;
        // The id that VERIFIED, not the one the envelope claimed. The
        // envelope's `root-key-id` is a hint for picking a candidate;
        // reporting it as fact would let a document name a root that never
        // signed it (spec §6.3 wants the answer, not the assertion).
        doc.rootKeyId = verifiedBy;

        auto org = obj->getString("organization");
        if (!org || org->empty()) {
            return err("organization key document: no organization");
        }
        doc.organization = org->str();

        auto* namespaces = obj->getArray("namespaces");
        if (!namespaces || namespaces->empty()) {
            return err("organization key document: '" + doc.organization
                       + "' claims no namespaces — a document that owns "
                         "nothing can authorise nothing");
        }
        for (auto& n : *namespaces) {
            auto s = n.getAsString();
            if (!s || s->empty()) {
                return err("organization key document: a namespace entry is "
                           "not a non-empty string");
            }
            doc.namespaces.push_back(s->str());
        }

        auto notAfter = obj->getString("not-after");
        if (!notAfter) return err("organization key document: no not-after");
        auto docExpiry = parseUtcTimestamp(notAfter->str());
        if (!docExpiry) return docExpiry.takeError();
        doc.notAfter = *docExpiry;

        auto* keys = obj->getArray("keys");
        if (!keys || keys->empty()) {
            return err("organization key document: '" + doc.organization
                       + "' lists no keys");
        }
        for (auto& entry : *keys) {
            auto* k = entry.getAsObject();
            if (!k) return err("organization key document: a key is not an object");
            auto id = k->getString("id");
            auto alg = k->getString("algorithm");
            auto pem = k->getString("public-key");
            auto nb = k->getString("not-before");
            auto na = k->getString("not-after");
            if (!id || !alg || !pem || !nb || !na) {
                return err("organization key document: a key is missing id, "
                           "algorithm, public-key, not-before or not-after");
            }
            if (*alg != "ed25519") {
                return err("organization key document: key '" + id->str()
                           + "' uses unsupported algorithm '" + alg->str() + "'");
            }
            auto from = parseUtcTimestamp(nb->str());
            if (!from) return from.takeError();
            auto until = parseUtcTimestamp(na->str());
            if (!until) return until.takeError();
            if (*until <= *from) {
                return err("organization key document: key '" + id->str()
                           + "' has a not-after at or before its not-before, "
                             "so it is never usable");
            }
            OrgSigningKey key;
            key.id = id->str();
            key.algorithm = alg->str();
            key.publicKeyPem = pem->str();
            key.notBefore = *from;
            key.notAfter = *until;
            doc.keys.push_back(std::move(key));
        }

        // Expiry last, so a malformed document reports what is wrong with it
        // rather than only that it is old. An expired document is an ERROR,
        // not a parsed value with a flag: nothing downstream can then hold
        // one and forget to check (spec 2.5).
        if (now >= doc.notAfter) {
            return err("organization key document for '" + doc.organization
                       + "' expired at " + notAfter->str()
                       + "; it is validly signed but out of date, and a stale "
                         "document is how revocation-by-expiry gets bypassed");
        }
        return doc;
    }

} // namespace cajeta::buildtool
