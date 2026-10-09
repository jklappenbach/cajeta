#include "LaunchBlocks.h"
#include "KernelManifest.h"
#include "XpuAttributes.h"
#include "../mir/XpuMirBuilder.h"
#include "../../asn/expression/LiteralExpression.h"
#include "../../compile/CajetaModule.h"
#include "../../method/Method.h"

#include <algorithm>
#include <optional>
#include <unordered_set>

namespace cajeta {
namespace xpu {

    namespace {
        std::optional<std::array<unsigned, 3>> constBlock(const std::vector<ExpressionPtr>& dims) {
            if (dims.empty() || dims.size() > 3) return std::nullopt;
            std::array<unsigned, 3> b{1, 1, 1};
            for (size_t i = 0; i < dims.size(); i++) {
                auto lit = std::dynamic_pointer_cast<IntegerLiteralExpression>(dims[i]);
                if (!lit) return std::nullopt;
                unsigned long v = 0;
                try { v = std::stoul(lit->getRawValue()); } catch (...) { return std::nullopt; }
                if (v == 0) return std::nullopt;
                b[i] = static_cast<unsigned>(v);
            }
            return b;
        }

        std::string simpleName(const std::string& qualified) {
            auto dot = qualified.rfind('.');
            return dot == std::string::npos ? qualified : qualified.substr(dot + 1);
        }
    } // namespace

    LaunchBlocks scanLaunchBlocks(const std::vector<CajetaModulePtr>& modules) {
        LaunchBlocks out;
        std::unordered_map<std::string, std::optional<std::array<unsigned, 3>>> agreed;
        std::unordered_set<std::string> broken;
        for (auto& module : modules) {
            if (!module) continue;
            bool hasKernel = false;
            for (auto& method : module->getAllMethods())
                if (method && isKernel(*method)) { hasKernel = true; break; }
            if (!hasKernel) continue;
            auto mir = mir::XpuMirBuilder::buildForModule(module);
            if (!mir) continue;
            for (auto& site : mir->launchSites) {
                if (!site) continue;
                const std::string& qualified = site->kernelCanonicalName;
                std::string simple = simpleName(qualified);
                auto b = constBlock(site->block);
                if (!b) {
                    ++out.unboundedSites[simple];
                    broken.insert(qualified);
                    continue;
                }
                unsigned& cur = out.maxThreads[simple];
                cur = std::max(cur, (*b)[0] * (*b)[1] * (*b)[2]);
                auto it = agreed.find(qualified);
                if (it == agreed.end()) agreed.emplace(qualified, b);
                else if (it->second != b) broken.insert(qualified);
            }
        }
        for (const auto& k : out.unboundedSites) out.maxThreads.erase(k.first);
        for (const auto& [qualified, b] : agreed)
            if (b && !broken.count(qualified)) out.pinned.emplace(qualified, *b);
        for (auto& module : modules) {
            if (!module) continue;
            for (auto& method : module->getAllMethods()) {
                if (!method || !isKernel(*method)) continue;
                auto it = out.pinned.find(qualifiedKernelName(method));
                method->setPinnedLaunchBlock(it == out.pinned.end()
                    ? std::nullopt : std::optional<std::array<unsigned, 3>>(it->second));
            }
        }
        return out;
    }

} // namespace xpu
} // namespace cajeta
