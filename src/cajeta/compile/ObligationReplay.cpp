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

        // Split at top-level commas — angle/paren depth aware, so
        // `int32,HashMap<K,V>,(int32) -> #int32` yields three pieces.
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

        // Parse a `<...>` argument list into types. A leading `#` (stale
        // element-ownership mangles in old obligation files) is stripped
        // and ignored. False + err on any unresolvable argument.
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

        // Array canonical `T[]` (heap reference form): resolve the element
        // and wrap, mirroring the parse-path bracket loop. Fixed-size `T[N]`
        // stays unsupported (never a template argument in practice) — falls
        // through to the unresolvable error below.
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

        // Lambda-specialization clones (`…$spec$__cajeta_lambda_N`) are
        // codegen artifacts, not instantiations replay can (or need to)
        // recreate: the clone's body lands in the module whose codegen drove
        // it, so a clean module's cached .bc already carries its own clones
        // byte-frozen. (A clone a skipped module drove INTO stdlib would be
        // missing — that fails loud at link, never silently; v1 accepts it.)
        if (key.find("$spec$") != std::string::npos) return true;

        // Method form: host::name(params)<targs>. Resolving the host
        // instantiates it if missing, so a host-class obligation need not
        // precede its method obligations in the sidecar.
        std::string host = key.substr(0, sep);
        std::string rest = key.substr(sep + 2);
        auto paren = rest.find('(');
        if (paren == std::string::npos) {
            err = "malformed method obligation `" + key + "`";
            return false;
        }
        std::string methodName = trim(rest.substr(0, paren));

        // Step past the (nesting-aware) value-param list. The param COUNT
        // disambiguates same-name overloaded method templates below, and when
        // two overloads share a count the TYPES break the tie (compile-cache
        // D1 — v1 gave up here and failed the replay, which dropped a
        // newly-required instantiation and broke the link).
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

        // The methods map can reach the same template under several keys;
        // dedupe by identity. Same-name overloads (Json.parse, Tensor.add)
        // disambiguate by declared value-param count against the key's —
        // exact first, then key-count-1 (keys captured after `this`
        // insertion carry one extra leading param). Only a tie WITHIN a
        // count remains the D2 ambiguity — fail loud.
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
        // compile-cache D1 — break a same-count tie on the value-param TYPES,
        // which the key has been carrying all along. Instantiate each tied
        // candidate with the requested type arguments and compare its concrete
        // parameter types against the key's; the one that matches is the one
        // the original call site resolved to.
        //
        // Instantiating a loser is cheap and inert: it builds the specialized
        // Method but does NOT bring it alive (no register / prototype / body),
        // so nothing is emitted for it. `noteCrossModuleMethodInstantiation`
        // no-ops here too — replay runs outside codegen, so there is no current
        // codegen module to charge the obligation to.
        // `keySkip` is how many leading key params are NOT declared params —
        // 1 when the key was captured after `this` insertion, 0 otherwise.
        // `getParameterList()` may itself carry a leading `this` (the count
        // above uses `getParameters()`, the map, which does not), so both
        // sides are re-aligned here rather than assumed.
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
                // An unresolvable key param must not silently disqualify a
                // candidate — fall back to comparing the spelling.
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
        // A call site would bring the instantiation to life (register +
        // prototype + body); replay has no call site, so do it explicitly —
        // otherwise the body never emits and the skipped module's reference
        // is undefined at link.
        hostClass->ensureMethodInstantiationAlive(std::move(inst));
        return true;
    }

} // namespace cajeta
