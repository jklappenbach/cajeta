#include "cajeta/buildtool/OciImage.h"

#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>

#include <openssl/evp.h>
#include <openssl/sha.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace cajeta::buildtool {

    namespace {

        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(), msg);
        }

        std::string sha256OfBytes(const std::string& bytes) {
            unsigned char digest[SHA256_DIGEST_LENGTH];
            EVP_MD_CTX* ctx = EVP_MD_CTX_new();
            EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
            EVP_DigestUpdate(ctx, bytes.data(), bytes.size());
            unsigned int outLen = 0;
            EVP_DigestFinal_ex(ctx, digest, &outLen);
            EVP_MD_CTX_free(ctx);
            static const char* hexd = "0123456789abcdef";
            std::string s;
            s.reserve(outLen * 2);
            for (unsigned i = 0; i < outLen; ++i) {
                s += hexd[(digest[i] >> 4) & 0xF];
                s += hexd[digest[i] & 0xF];
            }
            return s;
        }

        std::string readBinary(const std::filesystem::path& p) {
            std::ifstream in(p, std::ios::binary);
            std::ostringstream ss; ss << in.rdbuf();
            return ss.str();
        }

        llvm::Error writeBinary(const std::filesystem::path& p,
                                const std::string& contents) {
            std::error_code ec;
            std::filesystem::create_directories(p.parent_path(), ec);
            if (ec) {
                return err("write '" + p.string() + "': " + ec.message());
            }
            std::ofstream out(p, std::ios::binary | std::ios::trunc);
            if (!out) return err("cannot open '" + p.string() + "'");
            out.write(contents.data(),
                      static_cast<std::streamsize>(contents.size()));
            if (!out) return err("short write to '" + p.string() + "'");
            return llvm::Error::success();
        }

        // POSIX ustar header (512 bytes). One regular-file entry into
        // a tar buffer. Mirrors the writer in repo/TarZstd.cpp; the
        // OCI layer tar lives in a different namespace so we keep
        // its own copy (no cross-file coupling).
        void appendTarEntry(std::string& out,
                            const std::string& name,
                            const std::string& data,
                            int mode = 0755) {
            char header[512] = {};
            std::strncpy(header, name.c_str(), 99);
            std::snprintf(header + 100, 8, "%07o", mode);
            std::snprintf(header + 108, 8, "%07o", 0);   // uid
            std::snprintf(header + 116, 8, "%07o", 0);   // gid
            std::snprintf(header + 124, 12, "%011llo",
                          static_cast<unsigned long long>(data.size()));
            std::snprintf(header + 136, 12, "%011o", 0);  // mtime
            std::memset(header + 148, ' ', 8);            // chksum prep
            header[156] = '0';                            // regular file
            std::strncpy(header + 257, "ustar", 5);
            header[263] = '0'; header[264] = '0';         // version
            // Checksum.
            unsigned chk = 0;
            for (int i = 0; i < 512; ++i) {
                chk += static_cast<unsigned char>(header[i]);
            }
            std::snprintf(header + 148, 8, "%06o", chk);
            header[155] = ' ';
            out.append(header, 512);
            out.append(data);
            // 512-byte padding.
            if (size_t pad = (512 - data.size() % 512) % 512; pad > 0) {
                out.append(pad, '\0');
            }
        }

        // gzip-compress `raw` using zlib's deflate (system zlib).
        // OCI layers default to `application/vnd.oci.image.layer.v1.tar+gzip`.
        std::string gzipBytes(const std::string& raw);

        std::string serializeJson(const llvm::json::Value& v) {
            std::string out;
            llvm::raw_string_ostream os(out);
            os << llvm::formatv("{0:2}", v);
            return out;
        }

    } // namespace

} // namespace cajeta::buildtool

// ───── gzip via zlib (linked transitively through libcurl). ─────
#include <zlib.h>

namespace cajeta::buildtool {

    namespace {

        std::string gzipBytes(const std::string& raw) {
            // gzip wrap: windowBits 15 | 16 = gzip framing.
            z_stream zs{};
            deflateInit2(&zs, Z_BEST_SPEED, Z_DEFLATED,
                         15 | 16, 8, Z_DEFAULT_STRATEGY);
            zs.next_in = reinterpret_cast<Bytef*>(
                const_cast<char*>(raw.data()));
            zs.avail_in = static_cast<uInt>(raw.size());
            std::string out;
            out.reserve(raw.size() / 2 + 64);
            char buf[16384];
            int rc;
            do {
                zs.next_out = reinterpret_cast<Bytef*>(buf);
                zs.avail_out = sizeof(buf);
                rc = deflate(&zs, Z_FINISH);
                out.append(buf, sizeof(buf) - zs.avail_out);
            } while (rc == Z_OK);
            deflateEnd(&zs);
            return out;
        }

    } // namespace

    llvm::Expected<OciImageResult> writeOciImage(
        const std::string& outDir,
        const OciImageSpec& spec) {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories(fs::path(outDir) / "blobs" / "sha256", ec);
        if (ec) return err("cannot create '" + outDir + "': " + ec.message());

        // Read the executable bytes; this lives in the layer.
        if (!fs::exists(spec.executablePath, ec)) {
            return err("package(container): executable '" +
                       spec.executablePath + "' not found");
        }
        std::string exeBytes = readBinary(spec.executablePath);
        std::string exeName = spec.entrypointName.empty()
            ? fs::path(spec.executablePath).filename().string()
            : spec.entrypointName;

        // Build the rootfs tar: usr/local/bin/<name>.
        std::string rootfsTar;
        appendTarEntry(rootfsTar, "usr/local/bin/" + exeName, exeBytes);
        // Two zero blocks end of tar.
        rootfsTar.append(1024, '\0');
        // Gzip the layer per OCI media type.
        std::string layerBlob = gzipBytes(rootfsTar);
        std::string layerDigest = sha256OfBytes(layerBlob);
        std::string diffIdDigest = sha256OfBytes(rootfsTar);

        // Config block: minimal but spec-compliant.
        llvm::json::Object envArr;
        std::vector<llvm::json::Value> envList;
        for (const auto& kv : spec.env) {
            envList.emplace_back(kv.first + "=" + kv.second);
        }
        llvm::json::Object labelMap;
        for (const auto& kv : spec.labels) {
            labelMap[kv.first] = kv.second;
        }
        llvm::json::Object exposedPorts;
        for (const auto& p : spec.expose) {
            exposedPorts[p] = llvm::json::Object{};
        }
        llvm::json::Object configBlock{
            {"User",        ""},
            {"Env",         llvm::json::Array(envList)},
            {"Entrypoint",  llvm::json::Array{
                "/usr/local/bin/" + exeName}},
            {"Cmd",         llvm::json::Array{}},
            {"WorkingDir",  "/"},
            {"Labels",      std::move(labelMap)},
            {"ExposedPorts", std::move(exposedPorts)},
        };
        llvm::json::Object rootfs{
            {"type",     "layers"},
            {"diff_ids", llvm::json::Array{
                "sha256:" + diffIdDigest}},
        };
        llvm::json::Object history{
            {"created_by",
             "cajeta package container; base=" + spec.baseHint},
        };
        llvm::json::Object configJson{
            {"created",      "1970-01-01T00:00:00Z"},
            {"architecture", spec.arch},
            {"os",           spec.os},
            {"config",       std::move(configBlock)},
            {"rootfs",       std::move(rootfs)},
            {"history",      llvm::json::Array{std::move(history)}},
        };
        std::string configBytes = serializeJson(
            llvm::json::Value(std::move(configJson)));
        std::string configDigest = sha256OfBytes(configBytes);

        // Manifest descriptor.
        llvm::json::Object manifestJson{
            {"schemaVersion", 2},
            {"mediaType",
             "application/vnd.oci.image.manifest.v1+json"},
            {"config", llvm::json::Object{
                {"mediaType",
                 "application/vnd.oci.image.config.v1+json"},
                {"digest", "sha256:" + configDigest},
                {"size",   static_cast<int64_t>(configBytes.size())},
            }},
            {"layers", llvm::json::Array{
                llvm::json::Object{
                    {"mediaType",
                     "application/vnd.oci.image.layer.v1.tar+gzip"},
                    {"digest", "sha256:" + layerDigest},
                    {"size",   static_cast<int64_t>(layerBlob.size())},
                },
            }},
        };
        std::string manifestBytes = serializeJson(
            llvm::json::Value(std::move(manifestJson)));
        std::string manifestDigest = sha256OfBytes(manifestBytes);

        // Index: one manifest descriptor.
        llvm::json::Object indexJson{
            {"schemaVersion", 2},
            {"manifests", llvm::json::Array{
                llvm::json::Object{
                    {"mediaType",
                     "application/vnd.oci.image.manifest.v1+json"},
                    {"digest", "sha256:" + manifestDigest},
                    {"size", static_cast<int64_t>(manifestBytes.size())},
                    {"annotations", llvm::json::Object{
                        {"org.opencontainers.image.ref.name", spec.tag},
                    }},
                },
            }},
        };
        std::string indexBytes = serializeJson(
            llvm::json::Value(std::move(indexJson)));

        // oci-layout marker.
        std::string layout = R"({"imageLayoutVersion":"1.0.0"})";

        // Write everything content-addressed.
        auto base = fs::path(outDir);
        if (auto e = writeBinary(base / "oci-layout", layout))
            return std::move(e);
        if (auto e = writeBinary(base / "index.json", indexBytes))
            return std::move(e);
        if (auto e = writeBinary(
                base / "blobs" / "sha256" / configDigest, configBytes))
            return std::move(e);
        if (auto e = writeBinary(
                base / "blobs" / "sha256" / layerDigest, layerBlob))
            return std::move(e);
        if (auto e = writeBinary(
                base / "blobs" / "sha256" / manifestDigest, manifestBytes))
            return std::move(e);

        OciImageResult out;
        out.layoutDir = outDir;
        out.manifestDigest = "sha256:" + manifestDigest;
        out.configDigest = "sha256:" + configDigest;
        out.layerDigest = "sha256:" + layerDigest;
        out.manifestSize = static_cast<long long>(manifestBytes.size());
        out.configSize = static_cast<long long>(configBytes.size());
        out.layerSize = static_cast<long long>(layerBlob.size());
        return out;
    }

} // namespace cajeta::buildtool
