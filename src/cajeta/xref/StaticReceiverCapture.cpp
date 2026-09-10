#include "StaticReceiverCapture.h"

#include <functional>
#include <set>
#include <string>

#include "XrefIndex.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/type/CajetaType.h"
#include "cajeta/type/StructureProperty.h"
#include "cajeta/type/FormalParameter.h"
#include "cajeta/method/Method.h"
#include "cajeta/asn/Block.h"
#include "cajeta/asn/LocalVariableDeclaration.h"
#include "cajeta/asn/VariableDeclarator.h"
#include "cajeta/asn/expression/MethodCallExpression.h"
#include "cajeta/asn/expression/DotExpression.h"
#include "cajeta/asn/expression/Identifier.h"

namespace cajeta::xref {

    namespace {

        void collectFields(const CajetaClassPtr& klass, std::set<std::string>& out) {
            if (!klass) return;
            for (auto& [name, prop] : klass->getProperties()) out.insert(name);
            for (auto& parent : klass->getSuperClasses()) collectFields(parent, out);
        }

        // Depth-first over every descendant; VariableDeclarator holds its initializer
        // in a private field without overriding forEachSubNode, so descend that too.
        void walk(const AbstractSyntaxNodePtr& node,
                  const std::function<void(const AbstractSyntaxNodePtr&)>& fn) {
            if (!node) return;
            fn(node);
            node->forEachSubNode([&](const AbstractSyntaxNodePtr& c) { walk(c, fn); });
            if (auto vd = std::dynamic_pointer_cast<VariableDeclarator>(node))
                walk(vd->getInitializer(), fn);
        }

        // The leading identifier of a static receiver — `Ident.m(...)` or `Ident.field` — or null.
        std::shared_ptr<IdentifierExpression>
        receiverIdentifier(const AbstractSyntaxNodePtr& node) {
            AbstractSyntaxNodePtr recv;
            if (auto mce = std::dynamic_pointer_cast<MethodCallExpression>(node)) {
                if (!mce->getChildren().empty()) recv = mce->getChildren()[0];
            } else if (auto de = std::dynamic_pointer_cast<DotExpression>(node)) {
                if (!de->getChildren().empty()) recv = de->getChildren()[0];
            }
            return std::dynamic_pointer_cast<IdentifierExpression>(recv);
        }

    } // namespace

    void captureStaticReceivers(const CajetaModulePtr& module) {
        if (!captureEnabled() || !module) return;

        for (auto& [canon, klass] : module->getStructures()) {
            if (!klass || klass->isAnnotation()) continue;

            std::set<std::string> classFields;
            collectFields(klass, classFields);

            for (auto& method : klass->getMethodList()) {
                if (!method) continue;
                auto block = method->getBlock();
                if (!block) continue;

                // "Value names": fields, parameters and every local anywhere in the
                // body, collected first so a later local still masks an earlier use.
                std::set<std::string> values = classFields;
                for (auto& p : method->getParameterList())
                    if (p) values.insert(p->getName());
                walk(block, [&](const AbstractSyntaxNodePtr& n) {
                    if (auto lvd =
                            std::dynamic_pointer_cast<LocalVariableDeclaration>(n)) {
                        for (auto& vd : lvd->getVariableDeclarators())
                            if (vd) values.insert(vd->getIdentifier());
                    }
                });

                walk(block, [&](const AbstractSyntaxNodePtr& n) {
                    auto recv = receiverIdentifier(n);
                    if (!recv) return;
                    const std::string& name = recv->getTextValue();
                    // A value is never a type receiver, so a shadowing name cannot jump wrong.
                    if (name.empty() || values.count(name)) return;
                    std::string target =
                        CajetaType::canonicalNameScoped(name, module);
                    if (target.empty()) return;   // not a known type: no edge
                    const std::string& file = recv->getSourceFile();
                    if (file.empty()) return;      // synthesized node: no position
                    noteTypeReference(target, file, recv->getSourceLine(),
                                      recv->getSourceColumn());
                });
            }
        }
    }

}
