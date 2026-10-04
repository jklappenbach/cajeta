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
            if (std::dynamic_pointer_cast<NewExpression>(e)) return "heap";
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

    void FieldStoreCensus::run(const std::list<CajetaModulePtr>& modules) {
        std::set<std::string> seen;
        for (auto& module : modules) {
            if (!module) continue;
            for (auto& [qname, klass] : module->getStructures()) {
                if (!klass || !klass->getQName()) continue;
                auto& props = klass->getProperties();
                std::string className = klass->getQName()->toCanonical();
                className = className.substr(0, className.find('<'));
                for (auto& [mkey, method] : klass->getMethods()) {
                    if (!method || !method->getBlock()) continue;
                    std::map<std::string, Binding> scope;
                    std::map<std::string, int> ordinal;
                    for (auto& [pname, fp] : method->getParameters()) {
                        if (!fp || pname == "this") continue;
                        scope[pname] = {fp->isTransferred() ? "formal-sharp" : "formal-plain",
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
                                                (int) bin->getSourceLine(), target, op, kind, name, type};
                                            std::string key = className + " " + rec.methodName + " " + target + " " + op
                                                + " " + kind + " " + name;
                                            key += " " + std::to_string(ordinal[key]++);
                                            if (seen.insert(key).second) {
                                                std::cerr << "[field-store] " << className << "."
                                                          << rec.methodName << ":" << rec.line
                                                          << " into=" << target << " op=" << op
                                                          << " src=" << kind << " name=" << name
                                                          << " type=" << type << "\n";
                                                g_records.push_back(std::move(rec));
                                            }
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

} // namespace cajeta::ownership
