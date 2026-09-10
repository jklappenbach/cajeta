#include "ObligationReplay.h"

#include "../compile/CajetaModule.h"
#include "../method/Method.h"
#include "../type/CajetaArray.h"
#include "../type/CajetaClass.h"
#include "../type/FormalParameter.h"

#include <set>
#include <string>
#include <vector>

namespace cajeta {

    namespace {

        std::string trim(const std::string& s) {
            auto b = s.find_first_not_of(" \t\r\n");
            if (b == std::string::npos) return {};
            auto e = s.find_last_not_of(" \t\r\n");
            return s.substr(b, e - b + 1);
        }

        // Split at top-level commas, angle/paren depth aware.
        std::vector<std::string> splitTopLevel(const std::string& s) {
            std::vector<std::string> out;
            int depth = 0;
            size_t start = 0;
            for (size_t i = 0; i < s.size(); ++i) {
                char c = s[i];
                if (c == '<' || c == '(') depth++;
                else if (c == '>' || c == ')') depth--;
                else if (c == ',' && depth == 0) {
                    out.push_back(s.substr(start, i - start));
                    start = i + 1;
                }
            }
            out.push_back(s.substr(start));
            return out;
        }

        // Parse a `<...>` argument list into types, stripping any stale leading
        // `#`. False + err on an unresolvable argument.
        bool resolveArgList(const std::string& inner,
                            std::vector<CajetaTypePtr>& args,
                            std::string& err) {
            for (auto& piece : splitTopLevel(inner)) {
                std::string arg = trim(piece);
                if (!arg.empty() && arg[0] == '#') arg = trim(arg.substr(1));
                CajetaTypePtr t = resolveCanonicalType(arg, err);
                if (!t) return false;
                args.push_back(std::move(t));
            }
            return true;
        }

    } // namespace

    CajetaTypePtr resolveCanonicalType(const std::string& canonical,
                                       std::string& err) {
        std::string s = trim(canonical);
        if (s.empty()) {
            err = "empty canonical";
            return nullptr;
        }
        auto& canon = CajetaType::getCanonicalMap();
        auto hit = canon.find(s);
        if (hit != canon.end()) return hit->second;

        // Heap-reference form only; fixed-size `T[N]` falls through as unresolvable.
        if (s.size() > 2 && s.compare(s.size() - 2, 2, "[]") == 0) {
            CajetaTypePtr elem = resolveCanonicalType(
                s.substr(0, s.size() - 2), err);
            if (!elem) return nullptr;
            CajetaModulePtr owner;
            if (auto k = std::dynamic_pointer_cast<CajetaClass>(elem))
                owner = k->getModule();
            if (!owner) owner = CajetaModule::getStdlibModule();
            if (!owner) {
                err = "no owning module for array canonical `" + s + "`";
                return nullptr;
            }
            auto arr = std::make_shared<CajetaArray>(owner, elem);
            owner->getStructures()[arr->toCanonical()] =
                std::static_pointer_cast<CajetaClass>(arr);
            return arr;
        }

        auto lt = s.find('<');
        if (lt == std::string::npos || s.back() != '>') {
            err = "unresolvable canonical `" + s + "`";
            return nullptr;
        }
        std::string base = s.substr(0, lt);
        auto baseHit = canon.find(base);
        if (baseHit == canon.end()) {
            err = "unknown template `" + base + "`";
            return nullptr;
        }
        auto klass = std::dynamic_pointer_cast<CajetaClass>(baseHit->second);
        if (!klass) {
            err = "`" + base + "` is not a class template";
            return nullptr;
        }
        std::vector<CajetaTypePtr> args;
        if (!resolveArgList(s.substr(lt + 1, s.size() - lt - 2),
                            args, err))
            return nullptr;
        CajetaClassPtr inst = klass->instantiate(std::move(args));
        if (!inst) err = "instantiation failed for `" + s + "`";
        return inst;
    }

    bool replayObligation(const std::string& rawKey, std::string& err) {
        std::string key = trim(rawKey);
        if (key.empty()) return true;

        auto sep = key.find("::");
        if (sep == std::string::npos)
            return resolveCanonicalType(key, err) != nullptr;

        // Lambda-specialization clones are codegen artifacts, already frozen into
        // the cached .bc of whichever module's codegen drove them.
        if (key.find("$spec$") != std::string::npos) return true;

        // Resolving the host instantiates it, so obligation order does not matter.
        std::string host = key.substr(0, sep);
        std::string rest = key.substr(sep + 2);
        auto paren = rest.find('(');
        if (paren == std::string::npos) {
            // Static-field form `Owner::field`: force the declaring module to define
            // the global, as a live reference from the skipped module would have.
            CajetaTypePtr hostType = resolveCanonicalType(host, err);
            if (!hostType) return false;
            auto hostClass = std::dynamic_pointer_cast<CajetaClass>(hostType);
            if (!hostClass) {
                err = "static-field obligation host `" + host
                    + "` is not a class";
                return false;
            }
            auto& props = hostClass->getProperties();
            auto pit = props.find(rest);
            if (pit == props.end() || !pit->second
                || !pit->second->isStatic()) {
                err = "no static field `" + rest + "` on `" + host + "`";
                return false;
            }
            if (!hostClass->getOrCreateStaticFieldGlobal(pit->second,
                                                         nullptr)) {
                err = "static-field global creation failed for `" + key + "`";
                return false;
            }
            return true;
        }
        std::string methodName = trim(rest.substr(0, paren));

        // The param count, and on a tie the param types, pick between overloads.
        int depth = 0;
        size_t i = paren;
        for (; i < rest.size(); ++i) {
            if (rest[i] == '(') depth++;
            else if (rest[i] == ')' && --depth == 0) { ++i; break; }
        }
        std::string paramsInner =
            trim(rest.substr(paren + 1, (i - 1) - (paren + 1)));
        std::vector<std::string> keyParams;
        if (!paramsInner.empty()) {
            for (auto& piece : splitTopLevel(paramsInner)) {
                std::string p = trim(piece);
                if (!p.empty() && p[0] == '#') p = trim(p.substr(1));
                keyParams.push_back(std::move(p));
            }
        }
        size_t keyParamCount = keyParams.size();
        std::vector<CajetaTypePtr> targs;
        if (i < rest.size() && rest[i] == '<' && rest.back() == '>') {
            if (!resolveArgList(rest.substr(i + 1, rest.size() - i - 2),
                                targs, err))
                return false;
        }
        if (targs.empty()) {
            err = "method obligation lacks type arguments `" + key + "`";
            return false;
        }

        CajetaTypePtr hostType = resolveCanonicalType(host, err);
        if (!hostType) return false;
        auto hostClass = std::dynamic_pointer_cast<CajetaClass>(hostType);
        if (!hostClass) {
            err = "method obligation host `" + host + "` is not a class";
            return false;
        }

        // One template is reachable under several map keys, so dedupe by identity;
        // same-name overloads then disambiguate by declared value-param count.
        std::set<Method*> seen;
        std::vector<MethodPtr> named;
        for (auto& [mapKey, m] : hostClass->getMethods()) {
            if (m && m->isMethodTemplate() && m->getName() == methodName
                && seen.insert(m.get()).second) {
                named.push_back(m);
            }
        }
        if (named.empty()) {
            err = "no method template `" + methodName + "` on `" + host + "`";
            return false;
        }
        // Break a same-count tie on the value-param types. Instantiating a losing
        // candidate is inert: no register/prototype/body, nothing emitted. `keySkip`
        // and `declSkip` re-align the two lists, either of which may lead with `this`.
        auto signatureMatches = [&](const MethodPtr& cand,
                                    size_t keySkip) -> bool {
            MethodPtr probe = cand->instantiateMethodTemplate(targs);
            if (!probe) return false;
            auto probeParams = probe->getParameterList();
            size_t declSkip = (!probeParams.empty() && probeParams.front()
                               && probeParams.front()->getName() == "this")
                                  ? 1u : 0u;
            if (keyParams.size() < keySkip) return false;
            size_t n = keyParams.size() - keySkip;
            if (probeParams.size() - declSkip != n) return false;
            for (size_t p = 0; p < n; ++p) {
                auto& fp = probeParams[declSkip + p];
                if (!fp || !fp->getType()) return false;
                const std::string& keyParam = keyParams[keySkip + p];
                std::string ignored;
                CajetaTypePtr want = resolveCanonicalType(keyParam, ignored);
                // An unresolvable key param falls back to comparing the spelling.
                if (want) {
                    if (want->toCanonical() != fp->getType()->toCanonical())
                        return false;
                } else if (keyParam != fp->getType()->toCanonical()) {
                    return false;
                }
            }
            return true;
        };

        MethodPtr tmpl;
        if (named.size() == 1) {
            tmpl = named.front();
        } else {
            for (size_t wantOffset = 0; !tmpl && wantOffset <= 1;
                 ++wantOffset) {
                if (keyParamCount < wantOffset) break;
                size_t want = keyParamCount - wantOffset;
                std::vector<MethodPtr> sameCount;
                for (auto& m : named) {
                    if (m->getParameters().size() == want) sameCount.push_back(m);
                }
                if (sameCount.size() == 1) {
                    tmpl = sameCount.front();
                } else if (sameCount.size() > 1) {
                    MethodPtr match;
                    bool tie = false;
                    for (auto& m : sameCount) {
                        if (!signatureMatches(m, wantOffset)) continue;
                        if (match) { tie = true; break; }
                        match = m;
                    }
                    if (tie) {
                        err = "ambiguous method template `" + methodName
                            + "` on `" + host + "` (multiple overloads with "
                            + std::to_string(want) + " params share the same "
                              "value-param signature)";
                        return false;
                    }
                    tmpl = match;
                }
            }
            if (!tmpl) {
                err = "no method template `" + methodName + "` on `" + host
                    + "` matches " + std::to_string(keyParamCount)
                    + " value params";
                return false;
            }
        }
        MethodPtr inst = tmpl->instantiateMethodTemplate(std::move(targs));
        if (!inst) {
            err = "method-template instantiation failed for `" + key + "`";
            return false;
        }
        // Replay has no call site, so the instantiation is brought to life here;
        // otherwise its body never emits and the skipped module's reference dangles.
        hostClass->ensureMethodInstantiationAlive(std::move(inst));
        return true;
    }

} // namespace cajeta
