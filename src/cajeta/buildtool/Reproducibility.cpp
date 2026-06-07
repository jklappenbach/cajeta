#include "cajeta/buildtool/Reproducibility.h"

#include <openssl/evp.h>
#include <openssl/sha.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace cajeta::buildtool {

    namespace {

        std::string envOrEmpty(const std::string& key) {
            const char* v = std::getenv(key.c_str());
            return v ? v : "";
        }

        std::string firstNonEmpty(
            const std::map<std::string, std::string>& envOverrides,
            const std::string& key) {
            auto it = envOverrides.find(key);
            if (it != envOverrides.end() && !it->second.empty()) {
                return it->second;
            }
            return envOrEmpty(key);
        }

    } // namespace

    std::string resolveSourceDateEpoch(
        const ResolvedProperties& props,
        const std::map<std::string, std::string>& envOverrides) {
        // 1. CAJETA_SOURCE_DATE_EPOCH (CI knob).
        auto ci = firstNonEmpty(envOverrides, "CAJETA_SOURCE_DATE_EPOCH");
        if (!ci.empty()) return ci;
        // 2. SOURCE_DATE_EPOCH (the standard).
        auto std = firstNonEmpty(envOverrides, "SOURCE_DATE_EPOCH");
        if (!std.empty()) return std;
        // 3. Manifest property.
        auto it = props.values.find("cajeta.source-date-epoch");
        if (it != props.values.end() && !it->second.empty()) {
            return it->second;
        }
        // 4. Hard default.
        return "0";
    }

    std::string composeDebugPrefixMap(
        const std::string& projectRoot,
        const std::string& virtualPrefix) {
        if (projectRoot.empty()) return {};
        return projectRoot + "=" + virtualPrefix;
    }

    uint64_t deterministicSeed(const std::string& contentHash) {
        std::string blob = contentHash + "cajeta/seed-v1";
        unsigned char digest[SHA256_DIGEST_LENGTH];
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
        EVP_DigestUpdate(ctx, blob.data(), blob.size());
        unsigned int outLen = 0;
        EVP_DigestFinal_ex(ctx, digest, &outLen);
        EVP_MD_CTX_free(ctx);
        uint64_t seed = 0;
        for (int i = 0; i < 8; ++i) {
            seed |= static_cast<uint64_t>(digest[i]) << (8 * i);
        }
        return seed;
    }

    std::vector<std::string> reproducibilityFlags(
        const ResolvedProperties& props,
        const std::string& projectRoot,
        const std::string& contentHash) {
        std::vector<std::string> out;
        out.push_back("--source-date-epoch=" +
                      resolveSourceDateEpoch(props));
        auto map = composeDebugPrefixMap(projectRoot);
        if (!map.empty()) {
            out.push_back("--debug-prefix-map=" + map);
        }
        if (!contentHash.empty()) {
            uint64_t seed = deterministicSeed(contentHash);
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%016llx",
                          static_cast<unsigned long long>(seed));
            out.push_back("--seed=" + std::string(buf));
        }
        return out;
    }

    std::string byteCompareFiles(const std::string& a,
                                 const std::string& b) {
        std::ifstream fa(a, std::ios::binary);
        std::ifstream fb(b, std::ios::binary);
        if (!fa) return "cannot open '" + a + "'";
        if (!fb) return "cannot open '" + b + "'";
        std::ostringstream sa, sb;
        sa << fa.rdbuf();
        sb << fb.rdbuf();
        std::string ba = sa.str();
        std::string bb = sb.str();
        if (ba.size() != bb.size()) {
            return "size A=" + std::to_string(ba.size()) +
                   ", B=" + std::to_string(bb.size());
        }
        for (size_t i = 0; i < ba.size(); ++i) {
            if (ba[i] != bb[i]) {
                char buf[128];
                std::snprintf(buf, sizeof(buf),
                              "first-differing byte at offset %zu: "
                              "A=0x%02x, B=0x%02x",
                              i,
                              static_cast<unsigned>(
                                  static_cast<unsigned char>(ba[i])),
                              static_cast<unsigned>(
                                  static_cast<unsigned char>(bb[i])));
                return buf;
            }
        }
        return "";  // identical
    }

    ReproducibilityVerifyResult verifyReproducibleArchive(
        const std::string& archiveA,
        const std::string& archiveB) {
        ReproducibilityVerifyResult r;
        std::ifstream fa(archiveA, std::ios::binary | std::ios::ate);
        std::ifstream fb(archiveB, std::ios::binary | std::ios::ate);
        if (fa) r.sizeA = static_cast<size_t>(fa.tellg());
        if (fb) r.sizeB = static_cast<size_t>(fb.tellg());
        r.diff = byteCompareFiles(archiveA, archiveB);
        r.identical = r.diff.empty();
        return r;
    }

} // namespace cajeta::buildtool
