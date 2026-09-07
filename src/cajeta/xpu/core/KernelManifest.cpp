//
// KernelManifest — record, JSON codec, occupancy fill, spill warning and the
// registration-ctor emission. See the header.
//

#include "KernelManifest.h"
#include "DeviceProfile.h"

#include "cajeta/method/Method.h"
#include "cajeta/type/CajetaClass.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"

#include <cstdio>

#ifndef CAJETA_VERSION
#define CAJETA_VERSION "0.0.0-unknown"
#endif

namespace cajeta {
namespace xpu {

    std::string KernelManifest::simpleName() const {
        auto dot = kernel.rfind('.');
        return dot == std::string::npos ? kernel : kernel.substr(dot + 1);
    }

    std::string qualifiedKernelName(const MethodPtr& kernel) {
        if (!kernel) return {};
        if (auto parent = kernel->getParent())
            return parent->toCanonical() + "." + kernel->getName();
        return kernel->getName();
    }

    std::string sha256Hex(const uint8_t* data, std::size_t len) {
        llvm::SHA256 h;
        h.update(llvm::ArrayRef<uint8_t>(data, len));
        std::array<uint8_t, 32> digest = h.final();
        return "sha256:" + llvm::toHex(llvm::ArrayRef<uint8_t>(digest), /*LowerCase=*/true);
    }

    std::string compilerVersionString() { return CAJETA_VERSION; }

    namespace {
        void putU(llvm::json::Object& o, const char* key,
                  const std::optional<unsigned>& v) {
            if (v) o[key] = (int64_t) *v;
        }
        void putS(llvm::json::Object& o, const char* key,
                  const std::optional<std::string>& v) {
            if (v) o[key] = *v;
        }
        std::optional<unsigned> getU(const llvm::json::Object& o, const char* key) {
            if (auto v = o.getInteger(key)) {
                if (*v >= 0) return (unsigned) *v;
            }
            return std::nullopt;
        }
        std::optional<std::string> getS(const llvm::json::Object& o, const char* key) {
            if (auto v = o.getString(key)) return v->str();
            return std::nullopt;
        }
    } // namespace

    std::string toJson(const KernelManifest& m) {
        llvm::json::Object identity;
        identity["kernel"] = m.kernel;
        identity["target"] = m.target;
        identity["codeHash"] = m.codeHash;
        identity["compilerVersion"] = m.compilerVersion;
        identity["xpuAbiVersion"] = (int64_t) m.xpuAbiVersion;
        identity["instrumented"] = m.instrumented;

        llvm::json::Object footprint;
        putU(footprint, "waveWidth", m.waveWidth);
        putU(footprint, "vgpr", m.vgpr);
        putU(footprint, "sgpr", m.sgpr);
        putU(footprint, "spillBytes", m.spillBytes);
        putU(footprint, "ldsStaticBytes", m.ldsStaticBytes);
        putS(footprint, "ldsDynamicParam", m.ldsDynamicParam);
        putU(footprint, "threadsPerGroup", m.threadsPerGroup);
        putU(footprint, "residentGroupsPerCu", m.residentGroupsPerCu);
        if (!m.feasibleBlocks.empty()) {
            llvm::json::Array fb;
            for (unsigned b : m.feasibleBlocks) fb.push_back((int64_t) b);
            footprint["feasibleBlocks"] = std::move(fb);
        }
        putS(footprint, "occupancyLimiter", m.occupancyLimiter);

        llvm::json::Object root;
        root["schemaVersion"] = (int64_t) KernelManifest::kSchemaVersion;
        root["identity"] = std::move(identity);
        root["footprint"] = std::move(footprint);
        // Unit 3 fills the access list from the tile body; the empty list is
        // "nothing derived yet", which the schema accepts.
        root["access"] = llvm::json::Array();
        root["restartable"] = m.restartable;
        root["captureSafe"] = m.captureSafe;

        std::string out;
        llvm::raw_string_ostream os(out);
        os << llvm::formatv("{0:2}", llvm::json::Value(std::move(root)));
        os << "\n";
        return out;
    }

    bool fromJson(const std::string& text, KernelManifest& out, std::string* error) {
        auto parsed = llvm::json::parse(text);
        if (!parsed) {
            if (error) *error = "manifest is not JSON: " + llvm::toString(parsed.takeError());
            else llvm::consumeError(parsed.takeError());
            return false;
        }
        const llvm::json::Object* root = parsed->getAsObject();
        if (!root) { if (error) *error = "manifest root is not an object"; return false; }
        auto sv = root->getInteger("schemaVersion");
        if (!sv || *sv != KernelManifest::kSchemaVersion) {
            if (error) *error = "unsupported manifest schemaVersion";
            return false;
        }
        const llvm::json::Object* id = root->getObject("identity");
        if (!id) { if (error) *error = "manifest has no identity"; return false; }
        KernelManifest m;
        m.kernel = id->getString("kernel").value_or("").str();
        m.target = id->getString("target").value_or("").str();
        m.codeHash = id->getString("codeHash").value_or("").str();
        m.compilerVersion = id->getString("compilerVersion").value_or("").str();
        m.xpuAbiVersion = (int) id->getInteger("xpuAbiVersion").value_or(0);
        m.instrumented = id->getBoolean("instrumented").value_or(false);
        if (m.kernel.empty() || m.target.empty() || m.codeHash.empty()) {
            if (error) *error = "manifest identity is incomplete";
            return false;
        }
        if (const llvm::json::Object* fp = root->getObject("footprint")) {
            m.waveWidth = getU(*fp, "waveWidth");
            m.vgpr = getU(*fp, "vgpr");
            m.sgpr = getU(*fp, "sgpr");
            m.spillBytes = getU(*fp, "spillBytes");
            m.ldsStaticBytes = getU(*fp, "ldsStaticBytes");
            m.ldsDynamicParam = getS(*fp, "ldsDynamicParam");
            m.threadsPerGroup = getU(*fp, "threadsPerGroup");
            m.residentGroupsPerCu = getU(*fp, "residentGroupsPerCu");
            if (const llvm::json::Array* fb = fp->getArray("feasibleBlocks"))
                for (const auto& v : *fb)
                    if (auto n = v.getAsInteger()) m.feasibleBlocks.push_back((unsigned) *n);
            m.occupancyLimiter = getS(*fp, "occupancyLimiter");
        }
        m.restartable = root->getBoolean("restartable").value_or(false);
        m.captureSafe = root->getBoolean("captureSafe").value_or(false);
        out = std::move(m);
        return true;
    }

    namespace {
        std::string sanitizedTarget(const KernelManifest& m) {
            std::string target = m.target;
            for (char& c : target)
                if (c == '/' || c == ':' || c == ' ' || c == ',') c = '-';
            return target;
        }
    } // namespace

    std::string manifestFileName(const KernelManifest& m) {
        return m.simpleName() + "." + sanitizedTarget(m) + ".manifest.json";
    }

    std::string manifestArchiveMemberName(const KernelManifest& m) {
        return "xpu/manifests/" + m.kernel + "." + sanitizedTarget(m) + ".manifest.json";
    }

    const char* manifestSectionName(const llvm::Module& host) {
        const llvm::Triple& t = host.getTargetTriple();
        if (t.isOSBinFormatMachO()) return "__DATA,__cajeta_mf";
        if (t.isOSBinFormatCOFF())  return ".cajmf";
        return ".cajeta.manifest";
    }

    void fillOccupancy(KernelManifest& m, const std::string& archName,
                       std::optional<unsigned> pinnedThreads, unsigned clamp) {
        DeviceModel model;
        if (!lookupArch(archName, model)) return;   // unknown arch: absent, not guessed
        if (!m.vgpr) return;                          // no footprint: nothing to pack
        const unsigned lds = m.ldsStaticBytes.value_or(0);
        if (pinnedThreads) {
            const unsigned block = *pinnedThreads;
            m.threadsPerGroup = block;
            unsigned waves = occupancy(model, block, *m.vgpr, lds);
            unsigned wavesPerBlock = model.waveSize
                ? (block + model.waveSize - 1) / model.waveSize : 0;
            m.residentGroupsPerCu = wavesPerBlock ? waves / wavesPerBlock : 0;
            m.occupancyLimiter = occupancyLimiterName(model, block, *m.vgpr, lds);
            return;
        }
        m.feasibleBlocks = candidateBlocks(model, *m.vgpr, lds, clamp);
        if (!m.feasibleBlocks.empty())
            m.occupancyLimiter =
                occupancyLimiterName(model, m.feasibleBlocks.front(), *m.vgpr, lds);
    }

    bool warnIfSpilling(const KernelManifest& m) {
        if (!m.spillBytes || *m.spillBytes == 0) return false;
        std::fprintf(stderr,
                     "cajeta: warning: [xpu-kernel-spill] %s on %s: %u bytes of "
                     "scratch per work-item (vgpr=%u); a spilling kernel is not "
                     "tuned — cut live registers or pin a smaller block\n",
                     m.kernel.c_str(), m.target.c_str(), *m.spillBytes,
                     m.vgpr.value_or(0));
        return true;
    }

    void emitManifestRegistration(
            llvm::Module& host,
            llvm::IRBuilder<llvm::ConstantFolder, llvm::IRBuilderDefaultInserter>& b,
            llvm::Value* nameStr, int backendId, const std::string& arch,
            const KernelManifest& m) {
        llvm::LLVMContext& ctx = host.getContext();
        llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
        llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
        llvm::Type* voidTy = llvm::Type::getVoidTy(ctx);
        llvm::PointerType* ptrTy = llvm::PointerType::get(ctx, 0);

        const std::string json = toJson(m);
        std::string tag = m.simpleName() + "." + arch;
        llvm::Constant* init =
            llvm::ConstantDataArray::getString(ctx, json, /*AddNull=*/true);
        auto* gv = new llvm::GlobalVariable(
            host, init->getType(), /*isConstant=*/true,
            llvm::GlobalValue::PrivateLinkage, init, "xpu.manifest." + tag);
        gv->setAlignment(llvm::MaybeAlign(1));
        // §12.4: a NAMED data section, so the artifact carries the manifest
        // where a tool can find it (`llvm-readelf -p .cajeta.manifest prog`)
        // and the runtime reads it from the loaded image — no file, no
        // storage. The ctor below references the global, so it is never
        // dropped by the linker's dead-stripping.
        gv->setSection(manifestSectionName(host));
        llvm::Value* archStr = b.CreateGlobalString(arch, "xpu.march." + tag);

        // void __cajeta_xpu_register_kernel_manifest(i8* name, i32 backend,
        //                                            i8* arch, i8* json, i64 len)
        llvm::FunctionCallee fn = host.getOrInsertFunction(
            "__cajeta_xpu_register_kernel_manifest",
            llvm::FunctionType::get(voidTy, {ptrTy, i32Ty, ptrTy, ptrTy, i64Ty},
                                    false));
        b.CreateCall(fn, {nameStr, llvm::ConstantInt::get(i32Ty, backendId), archStr,
                          gv, llvm::ConstantInt::get(i64Ty, json.size())});
    }

} // namespace xpu
} // namespace cajeta
