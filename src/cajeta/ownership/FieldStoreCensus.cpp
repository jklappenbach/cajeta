#include "FieldStoreCensus.h"

#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <tuple>

#include "../method/Method.h"
#include "../type/CajetaArray.h"
#include "../type/CajetaClass.h"
#include "../type/FormalParameter.h"
#include "../type/StructureProperty.h"
#include "../asn/LocalVariableDeclaration.h"
#include "../asn/VariableDeclarator.h"
#include "../asn/expression/Expression.h"
#include "../asn/expression/BinaryOpExpression.h"
#include "../asn/expression/DotExpression.h"
#include "../asn/expression/Identifier.h"
#include "../asn/expression/LiteralExpression.h"
#include "../asn/expression/MethodCallExpression.h"
#include "../asn/expression/NewExpression.h"
#include "../error/DiagnosticEngine.h"
#include "../error/Exception.h"
#include <algorithm>

namespace cajeta::ownership {

    namespace {
        int g_override = -1;
        std::vector<FieldStoreRecord> g_records;

        struct Binding {
            std::string kind;
            std::string type;
            std::string viaFormal;
            CajetaTypePtr ptr;
        };

        bool carriesNoTitle(const CajetaTypePtr& t) {
            if (!t) return false;
            if (t->getTypeFlags() & PRIMITIVE_FLAG) return true;
            return t->isValueType();
        }

        std::string typeName(const CajetaTypePtr& t) {
            if (!t) return "?";
            return t->toCanonical();
        }

        // The innermost receiver of a dotted or indexed path, and whether any hop was taken.
        AbstractSyntaxNodePtr rootOf(const AbstractSyntaxNodePtr& e, bool& hopped) {
            AbstractSyntaxNodePtr cur = e;
            hopped = false;
            while (cur) {
                bool isDot = std::dynamic_pointer_cast<DotExpression>(cur) != nullptr;
                bool isIdx = std::dynamic_pointer_cast<ArrayIndexExpression>(cur) != nullptr;
                if (!isDot && !isIdx) break;
                auto& kids = cur->getChildren();
                if (kids.empty()) break;
                cur = kids[0];
                hopped = true;
            }
            return cur;
        }

        std::string producerKind(const AbstractSyntaxNodePtr& e) {
            if (auto ne = std::dynamic_pointer_cast<NewExpression>(e)) {
                return ne->getStackAlloc() ? "stack" : "heap";
            }
            if (std::dynamic_pointer_cast<MethodCallExpression>(e)) return "call";
            if (std::dynamic_pointer_cast<LiteralExpression>(e)) return "literal";
            if (std::dynamic_pointer_cast<BinaryOpExpression>(e)) return "expr";
            if (std::dynamic_pointer_cast<LambdaExpression>(e)) return "lambda";
            return "";
        }
    }

    bool FieldStoreCensus::enabled() {
        if (g_override >= 0) return g_override == 1;
        static const bool env = std::getenv("CAJETA_FIELD_STORE_CENSUS") != nullptr;
        return env;
    }

    void FieldStoreCensus::setEnabled(bool on) { g_override = on ? 1 : 0; }

    const std::vector<FieldStoreRecord>& FieldStoreCensus::records() { return g_records; }

    void FieldStoreCensus::clear() { g_records.clear(); }

    void FieldStoreCensus::walk(const std::list<CajetaModulePtr>& modules,
                                const std::function<void(const FieldStoreRecord&)>& sink) {
        std::set<std::string> seen;
        for (auto& module : modules) {
            if (!module) continue;
            std::string origin = module == CajetaModule::getStdlibModule() ? "stdlib"
                : module->isClasspathOrigin() ? "dependency" : "project";
            for (auto& [qname, klass] : module->getStructures()) {
                if (!klass || !klass->getQName()) continue;
                auto& props = klass->getProperties();
                std::string className = klass->getQName()->toCanonical();
                className = className.substr(0, className.find('<'));
                std::string file = klass->getDeclaringFile();
                if (file.empty()) file = module->getSourcePath();
                if (file.empty()) {
                    file = "<stdlib>/" + className;
                    std::replace(file.begin(), file.end(), '.', '/');
                    file += ".cajeta";
                }
                for (auto& [mkey, method] : klass->getMethods()) {
                    if (!method || !method->getBlock()) continue;
                    std::map<std::string, Binding> scope;
                    std::map<std::string, int> ordinal;
                    for (auto& [pname, fp] : method->getParameters()) {
                        if (!fp || pname == "this") continue;
                        scope[pname] = {fp->isTransferred() ? "formal-sharp" : fp->isBorrowOnly() ? "formal-borrow" : "formal-plain",
                                        carriesNoTitle(fp->getType()) ? "" : typeName(fp->getType()),
                                        pname, fp->getType()};
                    }

                    auto classify = [&](const AbstractSyntaxNodePtr& e,
                                        std::string& name, std::string& type) -> std::string {
                        if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(e)) {
                            name = id->getTextValue();
                            auto it = scope.find(name);
                            if (it != scope.end()) {
                                type = it->second.type;
                                if (type.empty()) return "";
                                return it->second.kind;
                            }
                            auto pit = props.find(name);
                            if (pit != props.end()) {
                                if (carriesNoTitle(pit->second->getType())) return "";
                                type = typeName(pit->second->getType());
                                return "field-read";
                            }
                            return "";
                        }
                        bool hopped = false;
                        auto root = rootOf(e, hopped);
                        if (!hopped) return "";
                        auto expr = std::dynamic_pointer_cast<Expression>(e);
                        auto rt = expr ? expr->getResolvedType() : nullptr;
                        if (rt && carriesNoTitle(rt)) return "";
                        type = typeName(rt);
                        auto rid = std::dynamic_pointer_cast<IdentifierExpression>(root);
                        bool viaThis = std::dynamic_pointer_cast<ThisExpression>(root) != nullptr
                            || (rid && rid->getTextValue() == "this");
                        if (viaThis) {
                            name = "this";
                            auto idx = std::dynamic_pointer_cast<ArrayIndexExpression>(e);
                            auto dot = std::dynamic_pointer_cast<DotExpression>(
                                idx ? idx->getChildren()[0] : e);
                            auto pit = dot ? props.find(dot->getIdentifier()) : props.end();
                            if (!rt && pit != props.end() && dot->getChildren()[0] == root) {
                                CajetaTypePtr ft = pit->second->getType();
                                if (idx) {
                                    auto arr = std::dynamic_pointer_cast<CajetaArray>(ft);
                                    ft = arr ? arr->getElementType() : nullptr;
                                }
                                if (ft && carriesNoTitle(ft)) return "";
                                type = typeName(ft);
                            }
                            return "field-read";
                        }
                        if (!rid) return "";
                        name = rid->getTextValue();
                        auto it = scope.find(name);
                        if (!rt && it != scope.end()) {
                            auto idx = std::dynamic_pointer_cast<ArrayIndexExpression>(e);
                            auto arr = std::dynamic_pointer_cast<CajetaArray>(it->second.ptr);
                            if (idx && arr && idx->getChildren()[0] == root) {
                                if (carriesNoTitle(arr->getElementType())) return "";
                                type = typeName(arr->getElementType());
                            }
                        }
                        if (it == scope.end()) {
                            auto pit = props.find(name);
                            if (pit == props.end()) return "";
                            auto arr = std::dynamic_pointer_cast<CajetaArray>(pit->second->getType());
                            auto idx = std::dynamic_pointer_cast<ArrayIndexExpression>(e);
                            if (!rt && idx && arr && idx->getChildren()[0] == root) {
                                if (carriesNoTitle(arr->getElementType())) return "";
                                type = typeName(arr->getElementType());
                            }
                            return "field-read";
                        }
                        if (!rt) {
                            auto dot = std::dynamic_pointer_cast<DotExpression>(e);
                            auto rk = dot ? std::dynamic_pointer_cast<CajetaClass>(
                                it->second.ptr) : nullptr;
                            if (rk && dot->getChildren()[0] == root) {
                                auto pit = rk->getProperties().find(dot->getIdentifier());
                                if (pit != rk->getProperties().end()) {
                                    if (carriesNoTitle(pit->second->getType())) return "";
                                    type = typeName(pit->second->getType());
                                }
                            }
                        }
                        const std::string& k = it->second.kind;
                        if (k.rfind("formal", 0) == 0) return "interior-of-" + k;
                        if (!it->second.viaFormal.empty()) {
                            name = it->second.viaFormal;
                            return "interior-of-formal-via-local";
                        }
                        return "interior-of-local";
                    };

                    std::function<void(const AbstractSyntaxNodePtr&)> walk =
                        [&](const AbstractSyntaxNodePtr& node) {
                            if (!node) return;
                            if (std::dynamic_pointer_cast<LambdaExpression>(node)) return;
                            if (auto lvd = std::dynamic_pointer_cast<LocalVariableDeclaration>(node)) {
                                for (auto& d : lvd->getVariableDeclarators()) {
                                    if (!d) continue;
                                    Binding b;
                                    CajetaTypePtr dt = lvd->getType();
                                    b.type = carriesNoTitle(dt) ? "" : typeName(dt);
                                    b.ptr = dt;
                                    b.kind = "local-unset";
                                    auto init = std::dynamic_pointer_cast<VariableInitializer>(
                                        d->getInitializer());
                                    if (init && !init->getChildren().empty()) {
                                        auto rhs = init->getChildren()[0];
                                        bool sharp = false;
                                        if (auto mv = std::dynamic_pointer_cast<MoveExpression>(rhs)) {
                                            sharp = true;
                                            if (!mv->getChildren().empty()) rhs = mv->getChildren()[0];
                                        }
                                        std::string pk = producerKind(rhs);
                                        std::string srcName;
                                        std::string srcType;
                                        std::string sk = pk.empty() ? classify(rhs, srcName, srcType) : "";
                                        if (!pk.empty()) {
                                            b.kind = "local-" + pk;
                                        } else if (sk.rfind("formal", 0) == 0) {
                                            b.kind = sharp ? "local-sharp-of-" + sk : "local-alias-of-" + sk;
                                            b.viaFormal = srcName;
                                        } else if (!sk.empty()) {
                                            b.kind = sharp ? "local-sharp-of-" + sk : "local-alias-of-" + sk;
                                            auto it = scope.find(srcName);
                                            if (it != scope.end()) b.viaFormal = it->second.viaFormal;
                                        } else {
                                            b.kind = sharp ? "local-sharp" : "local-other";
                                        }
                                    }
                                    scope[d->getIdentifier()] = b;
                                }
                            }
                            if (auto bin = std::dynamic_pointer_cast<BinaryOpExpression>(node)) {
                                auto& kids = bin->getChildren();
                                if (bin->getBinaryOp() == BINARY_OP_ASSIGN && kids.size() >= 2) {
                                    auto lhs = kids[0];
                                    std::string target;
                                    bool hopped = false;
                                    auto lroot = rootOf(lhs, hopped);
                                    bool isIdx = std::dynamic_pointer_cast<ArrayIndexExpression>(lhs) != nullptr;
                                    if (hopped) {
                                        int depth = 0;
                                        for (auto c = lhs; c && c != lroot; c = c->getChildren()[0]) ++depth;
                                        if (std::dynamic_pointer_cast<ThisExpression>(lroot)) {
                                            target = isIdx ? "slot" : (depth > 1 ? "nested" : "field");
                                        } else if (auto rid = std::dynamic_pointer_cast<IdentifierExpression>(lroot)) {
                                            const std::string& rn = rid->getTextValue();
                                            auto it = scope.find(rn);
                                            if (it == scope.end()) {
                                                target = props.count(rn) ? (isIdx ? "slot" : "nested")
                                                                         : (isIdx ? "slot-of-other" : "field-of-other");
                                            } else if (it->second.kind.rfind("formal", 0) == 0) {
                                                target = isIdx ? "slot-of-formal" : "field-of-formal";
                                            } else {
                                                target = isIdx ? "slot-of-local" : "field-of-local";
                                            }
                                        } else {
                                            target = isIdx ? "slot-of-other" : "field-of-other";
                                        }
                                    } else if (auto lid = std::dynamic_pointer_cast<IdentifierExpression>(lhs)) {
                                        const std::string& ln = lid->getTextValue();
                                        if (!scope.count(ln)) {
                                            auto pit = props.find(ln);
                                            if (pit != props.end()) {
                                                target = pit->second->isStatic() ? "static" : "field";
                                            }
                                        }
                                    }
                                    if (!target.empty()) {
                                        auto rhs = kids[1];
                                        std::string op = "=";
                                        if (auto mv = std::dynamic_pointer_cast<MoveExpression>(rhs)) {
                                            op = mv->isSharpStore() ? "#=" : "=#";
                                            if (!mv->getChildren().empty()) rhs = mv->getChildren()[0];
                                        }
                                        std::string name;
                                        std::string type;
                                        std::string kind = classify(rhs, name, type);
                                        if (!kind.empty()) {
                                            FieldStoreRecord rec{className, method->getName(),
                                                (int) bin->getSourceLine(), target, op, kind, name, type,
                                                file, origin, (int) bin->getSourceColumn() + 1};
                                            std::string key = className + " " + rec.methodName + " " + target + " " + op
                                                + " " + kind + " " + name;
                                            key += " " + std::to_string(ordinal[key]++);
                                            if (seen.insert(key).second) sink(rec);
                                        }
                                    }
                                }
                            }
                            node->forEachSubNode(walk);
                        };
                    walk(method->getBlock());
                }
            }
        }
    }

    void FieldStoreCensus::run(const std::list<CajetaModulePtr>& modules) {
        walk(modules, [](const FieldStoreRecord& rec) {
            std::cerr << "[field-store] " << rec.className << "." << rec.methodName << ":"
                      << rec.line << " into=" << rec.target << " op=" << rec.op
                      << " src=" << rec.source << " name=" << rec.name
                      << " type=" << rec.type << "\n";
            g_records.push_back(rec);
        });
    }

    namespace {
        bool keepsBeyondTheCall(const std::string& target) {
            return target == "field" || target == "nested" || target == "slot"
                || target == "static" || target == "field-of-formal"
                || target == "slot-of-formal";
        }

        bool startsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

        // `cajeta.lang.String` as `String`, `a.b.Node<a.b.K>` as `Node<a.b.K>`: the spelling a user writes.
        std::string shortType(const std::string& t) {
            size_t lt = t.find('<');
            size_t dot = t.rfind('.', lt == std::string::npos ? std::string::npos : lt);
            return dot == std::string::npos ? t : t.substr(dot + 1);
        }

        // The error code and message for a store that breaks spec 1.2, or an empty code.
        std::pair<std::string, std::string> violation(const FieldStoreRecord& r) {
            if (r.op != "=" || !keepsBeyondTheCall(r.target)) return {};
            if (r.type.empty() || r.type[0] == '?') return {};
            const std::string& s = r.source;
            std::string where = "the " + std::string(r.target.find("slot") != std::string::npos
                ? "slot" : r.target == "static" ? "static" : "field");
            if (s == "formal-sharp" || s == "local-alias-of-formal-sharp") {
                return {"CAJETA_ERROR_KEEP_NEEDS_SHARP_STORE",
                    "parameter `" + r.name + "` is `#`: it owns its argument and frees it when "
                    "the call returns, so `=` leaves " + where + " pointing at freed memory. "
                    "Fix: store it with `#=`, which takes the title."};
            }
            if (s == "formal-plain" || s == "local-alias-of-formal-plain") {
                return {"CAJETA_ERROR_KEEP_NEEDS_SHARP_STORE",
                    "`=` keeps parameter `" + r.name + "` as a borrow, but a caller may pass "
                    "it with `#`, and then the parameter frees it when the call returns. Fix: "
                    "store it with `#=`, which records whatever the caller passed, or spell "
                    "the parameter `^" + shortType(r.type) + "` if it is only ever borrowed."};
            }
            if (startsWith(s, "interior-of-formal") && s != "interior-of-formal-borrow") {
                return {"CAJETA_ERROR_INTERIOR_KEEP_NEEDS_BORROW_PARAM",
                    "`=` keeps a value read out of parameter `" + r.name + "`, which only lives "
                    "as long as `" + r.name + "` does. Fix: spell the parameter "
                    "`^` so callers must keep it alive, or store with `#=`."};
            }
            if (s == "local-heap" || s == "local-call" || s == "local-expr"
                    || startsWith(s, "local-sharp")) {
                return {"CAJETA_ERROR_KEEP_NEEDS_SHARP_STORE",
                    "local `" + r.name + "` may own its value, and it frees it at the end of "
                    "its scope, so `=` leaves " + where + " pointing at freed memory. Fix: "
                    "store it with `#=`, which takes the title when the local holds one."};
            }
            return {};
        }
    }

    void FieldStoreCensus::check(const std::list<CajetaModulePtr>& modules) {
        walk(modules, [](const FieldStoreRecord& rec) {
            auto [code, message] = violation(rec);
            if (code.empty()) return;
            if (enabled()) {
                std::cerr << "[field-store-violation] " << rec.file << ":" << rec.line << ": "
                          << code << " " << rec.className << "." << rec.methodName << "\n";
                return;
            }
            DiagnosticEngine* eng = DiagnosticEngine::active();
            if (eng && eng->collectsErrors()) {
                eng->report("error", code, message, rec.file, rec.line, rec.column, rec.origin);
                return;
            }
            std::cerr << "cajeta: " << rec.file << ":" << rec.line << ":" << rec.column << ": "
                      << code << ": " << message << "\n";
            throw Exception(message, code, rec.file, rec.line, rec.column);
        });
    }

} // namespace cajeta::ownership
