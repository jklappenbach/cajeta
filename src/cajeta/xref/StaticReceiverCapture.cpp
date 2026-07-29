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

        // A class's field names, including inherited (mirrors the findProp
        // recursion in DotExpression).
        void collectFields(const CajetaClassPtr& klass, std::set<std::string>& out) {
            if (!klass) return;
            for (auto& [name, prop] : klass->getProperties()) out.insert(name);
            for (auto& parent : klass->getSuperClasses()) collectFields(parent, out);
        }

        // Depth-first over every descendant node. forEachSubNode dispatches
        // virtually so it reaches payloads held in private slots (call args,
        // loop/try bodies, return expressions). VariableDeclarator is the one
        // exception — it keeps its initializer in a private field WITHOUT
        // overriding forEachSubNode, so `T x = Registry.count();` would hide the
        // static receiver. Descend into that initializer explicitly.
        void walk(const AbstractSyntaxNodePtr& node,
                  const std::function<void(const AbstractSyntaxNodePtr&)>& fn) {
            if (!node) return;
            fn(node);
            node->forEachSubNode([&](const AbstractSyntaxNodePtr& c) { walk(c, fn); });
            if (auto vd = std::dynamic_pointer_cast<VariableDeclarator>(node))
                walk(vd->getInitializer(), fn);
        }

        // The leading identifier of a static receiver, or null: children[0] of a
        // method call (`Ident.m(...)`) or dot access (`Ident.field`) when it is a
        // bare identifier.
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

                // "Value names": the identifiers a receiver could be that are
                // NOT types. Fields (incl. inherited) + parameters + EVERY local
                // declared anywhere in the body. Collected across the whole body
                // first — a local declared later still masks an earlier use, so
                // the pass never mistakes a shadowed name for a type.
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
                    // A value (local/param/field) is never a type receiver — do
                    // not record, so a shadowing name can never jump wrong.
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
