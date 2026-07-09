#include "ObligationReplay.h"

#include "../method/Method.h"
#include "../type/CajetaClass.h"

#include <set>
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

        // Parse a `<...>` argument list into types + owning flags (leading
        // `#` per element-ownership mangling). False + err on any
        // unresolvable argument.
        bool resolveArgList(const std::string& inner,
                            std::vector<CajetaTypePtr>& args,
                            std::vector<bool>& owning,
                            std::string& err) {
            for (auto& piece : splitTopLevel(inner)) {
                std::string arg = trim(piece);
                bool own = !arg.empty() && arg[0] == '#';
                if (own) arg = trim(arg.substr(1));
                CajetaTypePtr t = resolveCanonicalType(arg, err);
                if (!t) return false;
                args.push_back(std::move(t));
                owning.push_back(own);
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
        std::vector<bool> owning;
        if (!resolveArgList(s.substr(lt + 1, s.size() - lt - 2),
                            args, owning, err))
            return nullptr;
        CajetaClassPtr inst = klass->instantiate(std::move(args),
                                                 std::move(owning));
        if (!inst) err = "instantiation failed for `" + s + "`";
        return inst;
    }

    bool replayObligation(const std::string& rawKey, std::string& err) {
        std::string key = trim(rawKey);
        if (key.empty()) return true;

        auto sep = key.find("::");
        if (sep == std::string::npos)
            return resolveCanonicalType(key, err) != nullptr;

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

        // Step past the (nesting-aware) value-param list — ignored per D2.
        int depth = 0;
        size_t i = paren;
        for (; i < rest.size(); ++i) {
            if (rest[i] == '(') depth++;
            else if (rest[i] == ')' && --depth == 0) { ++i; break; }
        }
        std::vector<CajetaTypePtr> targs;
        if (i < rest.size() && rest[i] == '<' && rest.back() == '>') {
            std::vector<bool> ignoredOwning;
            if (!resolveArgList(rest.substr(i + 1, rest.size() - i - 2),
                                targs, ignoredOwning, err))
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
        // dedupe by identity. >1 DISTINCT same-name template = the D2
        // ambiguity (value-param signatures not compared in v1) — fail loud.
        std::set<Method*> distinct;
        MethodPtr tmpl;
        for (auto& [mapKey, m] : hostClass->getMethods()) {
            if (m && m->isMethodTemplate() && m->getName() == methodName) {
                distinct.insert(m.get());
                tmpl = m;
            }
        }
        if (distinct.empty()) {
            err = "no method template `" + methodName + "` on `" + host + "`";
            return false;
        }
        if (distinct.size() > 1) {
            err = "ambiguous method template `" + methodName + "` on `"
                + host + "` (v1 replay does not compare value-param"
                          " signatures)";
            return false;
        }
        if (!tmpl->instantiateMethodTemplate(std::move(targs))) {
            err = "method-template instantiation failed for `" + key + "`";
            return false;
        }
        return true;
    }

} // namespace cajeta
