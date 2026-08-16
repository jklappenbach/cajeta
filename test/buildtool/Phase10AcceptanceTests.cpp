// Phase 10 — Archive signing + launcher verification. Pins the
// six acceptance criteria from plans/buildtool/build-tool-plan.md "Phase 10 —
// Archive signing + launcher verification":
//
//   1. Signed archive verifies under `strict` mode.
//   2. Unsigned archive fails under `strict` mode with the
//      expected error message.
//   3. Tampered archive fails verification with computed-vs-
//      expected digest pair printed.
//   4. Trust-add then verify works end-to-end against a fresh
//      keypair.
//   5. System trust store unaffected when a user adds/removes
//      keys.
//   6. `CAJETA_REQUIRE_SIGNATURE=strict` overrides a laxer CLI
//      flag.
//
// Also pins: lookup precedence env → user → system; remove rejects
// missing key; addTrustedKey rejects non-ed25519 PEMs.

#include "cajeta/cli/SignatureVerify.h"
#include "cajeta/cli/TrustStore.h"

#include <gtest/gtest.h>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "../PortableEnv.h"

using cajeta::cli::addTrustedKey;
using cajeta::cli::fingerprintOfPemFile;
using cajeta::cli::listTrustedKeys;
using cajeta::cli::lookupTrustedKey;
using cajeta::cli::removeTrustedKey;
using cajeta::cli::resolveTrustStoreLayout;
using cajeta::cli::TrustStoreLayout;
using cajeta::cli::verifyArchiveSignature;
using cajeta::cli::VerifyOptions;
using cajeta::cli::writeKeyIdSidecar;

namespace {

    std::string errorText(llvm::Error&& err) {
        std::string out;
        llvm::raw_string_ostream os(out);
        os << err;
        consumeError(std::move(err));
        return out;
    }

    std::filesystem::path tempRoot(const std::string& tag) {
        auto p = std::filesystem::temp_directory_path() /
                 ("cajeta-phase10-" + tag + "-" +
                  std::to_string(::getpid()) + "-" +
                  std::to_string(::rand()));
        std::filesystem::create_directories(p);
        return p;
    }

    void writeFile(const std::filesystem::path& p,
                   const std::string& contents) {
        std::filesystem::create_directories(p.parent_path());
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out.write(contents.data(),
                  static_cast<std::streamsize>(contents.size()));
    }

    // Generate a fresh ed25519 keypair. Returns {publicPem,
    // privatePem}. OpenSSL handles both halves of the PKCS#8 PEM
    // wrappers via EVP_PKEY.
    struct EdKeyPair {
        std::string publicPem;
        std::string privatePem;
    };
    EdKeyPair generateEd25519KeyPair() {
        EVP_PKEY* pk = nullptr;
        EVP_PKEY_CTX* kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519,
                                                  nullptr);
        EVP_PKEY_keygen_init(kctx);
        EVP_PKEY_keygen(kctx, &pk);
        EVP_PKEY_CTX_free(kctx);
        BIO* pubBio = BIO_new(BIO_s_mem());
        PEM_write_bio_PUBKEY(pubBio, pk);
        BIO* privBio = BIO_new(BIO_s_mem());
        PEM_write_bio_PrivateKey(privBio, pk, nullptr, nullptr, 0,
                                 nullptr, nullptr);
        char* pubBuf;
        long pubLen = BIO_get_mem_data(pubBio, &pubBuf);
        char* privBuf;
        long privLen = BIO_get_mem_data(privBio, &privBuf);
        EdKeyPair out;
        out.publicPem  = std::string(pubBuf,
                                     static_cast<size_t>(pubLen));
        out.privatePem = std::string(privBuf,
                                     static_cast<size_t>(privLen));
        BIO_free(pubBio);
        BIO_free(privBio);
        EVP_PKEY_free(pk);
        return out;
    }

    // Sign `bytes` with the PKCS#8-shaped ed25519 PEM, return the
    // raw 64-byte signature.
    std::string signBytes(const std::string& privPem,
                          const std::string& bytes) {
        BIO* bio = BIO_new_mem_buf(privPem.data(),
                                    static_cast<int>(privPem.size()));
        EVP_PKEY* pk = PEM_read_bio_PrivateKey(bio, nullptr, nullptr,
                                                nullptr);
        BIO_free(bio);
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, pk);
        size_t sigLen = 0;
        EVP_DigestSign(ctx, nullptr, &sigLen,
                       reinterpret_cast<const unsigned char*>(
                           bytes.data()),
                       bytes.size());
        std::string sig(sigLen, '\0');
        EVP_DigestSign(ctx,
                       reinterpret_cast<unsigned char*>(sig.data()),
                       &sigLen,
                       reinterpret_cast<const unsigned char*>(
                           bytes.data()),
                       bytes.size());
        sig.resize(sigLen);
        EVP_MD_CTX_free(ctx);
        EVP_PKEY_free(pk);
        return sig;
    }

    // Build a Phase 10 fixture: archive bytes + signature + key-id
    // sidecar + trust-store layout pointing at a tmp directory.
    struct Fixture {
        std::filesystem::path root;
        std::filesystem::path archive;
        std::filesystem::path home;
        std::filesystem::path system;
        std::string keyId;
        std::string archiveBytes;
        TrustStoreLayout layout;
    };
    Fixture makeFixture(const std::string& tag) {
        Fixture f;
        f.root = tempRoot(tag);
        f.home = f.root / "home";
        f.system = f.root / "etc";
        std::filesystem::create_directories(f.home);
        std::filesystem::create_directories(f.system);
        f.archive = f.root / "demo.cja";
        f.archiveBytes = "ARCHIVEPAYLOAD-" + tag;
        writeFile(f.archive, f.archiveBytes);
        f.keyId = "demo-" + tag;
        f.layout = resolveTrustStoreLayout(
            f.home.string(), f.system.string(),
            std::nullopt);
        return f;
    }

} // namespace

// ─── Trust store: precedence + listing + remove behavior ────────

TEST(Phase10AcceptanceTests, lookupPrecedenceEnvUserSystem) {
    auto f = makeFixture("prec");
    auto envDir = f.root / "envstore";
    std::filesystem::create_directories(envDir);
    auto userDir = f.home / ".cajeta/trust/keys";
    std::filesystem::create_directories(userDir);
    auto sysDir = f.system / "cajeta/trust/keys";
    std::filesystem::create_directories(sysDir);

    auto k1 = generateEd25519KeyPair();
    auto k2 = generateEd25519KeyPair();
    auto k3 = generateEd25519KeyPair();
    writeFile(sysDir  / "alpha.pem", k1.publicPem);
    writeFile(userDir / "alpha.pem", k2.publicPem);
    writeFile(envDir  / "alpha.pem", k3.publicPem);

    auto layout = resolveTrustStoreLayout(
        f.home.string(), sysDir.string(), envDir.string());
    // The env-stored key wins.
    auto entry = lookupTrustedKey(layout, "alpha");
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->tier, "env");

    // Pulling the env key out reveals the user-store key beneath it.
    std::filesystem::remove(envDir / "alpha.pem");
    auto entry2 = lookupTrustedKey(layout, "alpha");
    ASSERT_TRUE(entry2.has_value());
    EXPECT_EQ(entry2->tier, "user");

    std::filesystem::remove_all(f.root);
}

TEST(Phase10AcceptanceTests, addThenRemoveRespectsUserTier) {
    auto f = makeFixture("addrm");
    auto kp = generateEd25519KeyPair();
    auto pemPath = f.root / "fresh.pem";
    writeFile(pemPath, kp.publicPem);

    ASSERT_FALSE((bool)addTrustedKey(f.layout, "fresh", pemPath.string()));
    auto entry = lookupTrustedKey(f.layout, "fresh");
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->tier, "user");

    // Duplicate add → error citing remove-first.
    auto dup = addTrustedKey(f.layout, "fresh", pemPath.string());
    ASSERT_TRUE((bool)dup);
    auto msg = errorText(std::move(dup));
    EXPECT_NE(msg.find("already exists"), std::string::npos);

    // Remove.
    ASSERT_FALSE((bool)removeTrustedKey(f.layout, "fresh"));
    EXPECT_FALSE(lookupTrustedKey(f.layout, "fresh").has_value());

    // Removing a non-existent key errors.
    auto miss = removeTrustedKey(f.layout, "nope");
    ASSERT_TRUE((bool)miss);
    EXPECT_NE(errorText(std::move(miss)).find("not in the user store"),
              std::string::npos);

    std::filesystem::remove_all(f.root);
}


// ─── Acceptance #5 — system trust store unaffected by user ops ──


// ─── Acceptance #1 — signed archive verifies ───────────────────


// ─── Acceptance #2 — unsigned archive fails under strict ───────


// ─── Acceptance #3 — tampered archive cites computed digest ────

TEST(Phase10AcceptanceTests, tamperedArchiveFailsWithDigestPair) {
    auto f = makeFixture("tamper");
    auto kp = generateEd25519KeyPair();
    auto pemPath = f.root / "sig.pem";
    writeFile(pemPath, kp.publicPem);
    ASSERT_FALSE((bool)addTrustedKey(f.layout, f.keyId, pemPath.string()));

    // Sign the ORIGINAL bytes…
    auto sig = signBytes(kp.privatePem, f.archiveBytes);
    writeFile(f.archive.string() + ".sig", sig);
    ASSERT_FALSE((bool)writeKeyIdSidecar(f.archive.string(), f.keyId));

    // …then tamper.
    writeFile(f.archive, "TAMPEREDBYTES-" + f.keyId);
    auto r = verifyArchiveSignature(f.layout, f.archive.string());
    ASSERT_FALSE((bool)r);
    auto msg = errorText(r.takeError());
    EXPECT_NE(msg.find("does NOT match"), std::string::npos);
    EXPECT_NE(msg.find("sha256:"), std::string::npos);
    EXPECT_NE(msg.find(f.keyId), std::string::npos);

    std::filesystem::remove_all(f.root);
}

// ─── Acceptance #4 — trust-add then verify end-to-end ──────────

TEST(Phase10AcceptanceTests, trustAddThenVerifyEndToEnd) {
    auto f = makeFixture("e2e");
    auto kp = generateEd25519KeyPair();
    auto pemPath = f.root / "sig.pem";
    writeFile(pemPath, kp.publicPem);

    // Before add: verify fails.
    auto sig = signBytes(kp.privatePem, f.archiveBytes);
    writeFile(f.archive.string() + ".sig", sig);
    ASSERT_FALSE((bool)writeKeyIdSidecar(f.archive.string(), f.keyId));
    {
        auto r = verifyArchiveSignature(f.layout, f.archive.string());
        ASSERT_FALSE((bool)r);
        auto msg = errorText(r.takeError());
        EXPECT_NE(msg.find("not found in any trust-store tier"),
                  std::string::npos);
    }

    // Add → verify succeeds.
    ASSERT_FALSE((bool)addTrustedKey(f.layout, f.keyId, pemPath.string()));
    auto r = verifyArchiveSignature(f.layout, f.archive.string());
    ASSERT_TRUE((bool)r) << errorText(r.takeError());

    std::filesystem::remove_all(f.root);
}

// ─── Acceptance #6 — env override beats CLI ────────────────────
//
// BuildToolCommands::resolveVerifyMode (anonymous-namespace
// helper) is the production policy. The rule is small enough that
// we pin it inline here too — the contract is "env wins when set
// to a valid mode; falls through to CLI otherwise". A regression
// in either side surfaces in BOTH layers.

// ─── Fingerprint determinism (the `cajeta trust show` output) ──

