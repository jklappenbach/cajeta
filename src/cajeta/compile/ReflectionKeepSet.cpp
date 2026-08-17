#include "cajeta/compile/ReflectionKeepSet.h"

#include "cajeta/compile/CajetaModule.h"
#include "cajeta/method/Method.h"
#include "cajeta/type/CajetaClass.h"

#include <functional>
#include <vector>

namespace cajeta {

    std::shared_ptr<const std::set<std::string>>
    resolveReflectionKeepSet(std::map<std::string, std::string>* keptBy) {
        auto& rk = CajetaModule::reflectionKeep();
        if (rk.forcesAll) return nullptr;

        auto keep = std::make_shared<std::set<std::string>>();
        auto addKeep = [&](const std::string& canon,
                           const std::string& reason) {
            keep->insert(canon);
            if (keptBy) keptBy->emplace(canon, reason);
        };
        // Key by toCanonical(): the canonicalMap reaches a class via both
        // its short-name alias and its FQ name, but keepsClass() (and the
        // reg-ctor) only ever uses toCanonical() — so dedup to that, or the
        // keep-set/keepset.json over-report aliases that never register.
        std::vector<std::pair<std::string, CajetaClassPtr>> classes;
        for (auto& [mapKey, type] : CajetaType::getCanonicalMap()) {
            if (auto k = std::dynamic_pointer_cast<CajetaClass>(type)) {
                std::string canon = k->toCanonical();
                classes.emplace_back(canon, k);
                if (k->getModifiers().count(REFLECT_RETAINED) > 0) {
                    addKeep(canon, "@Retained keep-pin");
                }
            }
        }
        std::function<bool(const CajetaClassPtr&, const std::string&)>
            derivesFrom = [&](const CajetaClassPtr& c,
                              const std::string& t) -> bool {
                if (!c) return false;
                if (c->toCanonical() == t) return true;
                for (auto& p : c->getSuperClasses()) {
                    if (derivesFrom(p, t)) return true;
                }
                return false;
            };
        for (auto& site : rk.sites) {
            using RS = CajetaModule::ReflSite;
            switch (site.kind) {
                case RS::BoundClosure:
                    for (auto& [canon, k] : classes) {
                        if (derivesFrom(k, site.selector))
                            addKeep(canon,
                                "subtype closure of " + site.selector);
                    }
                    break;
                case RS::ForNameLiteral:
                    if (CajetaType::getCanonicalMap().count(site.selector))
                        addKeep(site.selector,
                            "forName(\"" + site.selector + "\")");
                    break;
                case RS::PackageLiteral:
                    for (auto& [canon, k] : classes) {
                        auto p = canon.rfind('.');
                        if (p != std::string::npos
                                && canon.substr(0, p) == site.selector) {
                            addKeep(canon,
                                "classesInPackage(\"" + site.selector + "\")");
                        }
                    }
                    break;
                case RS::Annotated:
                    for (auto& [canon, k] : classes) {
                        if (k->findAnnotation(site.selector)) {
                            addKeep(canon,
                                "classesAnnotated(@" + site.selector + ")");
                        }
                    }
                    break;
                case RS::MethodAnnotated:
                    // Any METHOD carrying the annotation keeps the
                    // declaring class — the bounded form of the
                    // allClasses() + per-method-filter discovery
                    // idiom (cajeta-unit's @Test runner).
                    for (auto& [canon, k] : classes) {
                        bool hit = false;
                        for (auto& [mk, m] : k->getMethods()) {
                            if (m && m->findAnnotation(site.selector)) {
                                hit = true;
                                break;
                            }
                        }
                        if (hit) {
                            addKeep(canon,
                                "classesWithMethodAnnotated(@"
                                    + site.selector + ")");
                        }
                    }
                    break;
            }
        }
        return keep;
    }

} // namespace cajeta
