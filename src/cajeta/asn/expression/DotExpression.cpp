//
// Created by James Klappenbach on 4/14/23.
//

#include "DotExpression.h"
#include "../../compile/CajetaModule.h"
#include "../../type/CajetaClass.h"
#include "../../error/Exception.h"
#include "Identifier.h"

#include <climits>
#include <cmath>

namespace cajeta {
    DotExpression::DotExpression(CajetaParser::ExpressionContext* ctx, antlr4::Token* token) : Expression(token) {
        // The DOT grammar allows several rhs forms (identifier, methodCall, THIS, etc.).
        // Only the identifier form is fully implemented; for other forms we capture an
        // empty name and rely on the lhs's codegen to surface the right error.
        if (ctx->identifier()) {
            identifier = ctx->identifier()->getText();
        }
    }

    void DotExpression::resolveTypes(CajetaModulePtr module) {
        // Resolve the LHS (instance expression) first, then look up our member on its
        // resolved class type and pin our own type to the member's type.
        AbstractSyntaxNode::resolveTypes(module);
        if (children.empty()) {
            return;
        }
        auto lhs = dynamic_pointer_cast<Expression>(children[0]);
        if (!lhs) {
            return;
        }
        auto klass = dynamic_pointer_cast<CajetaClass>(lhs->getResolvedType());
        if (!klass) {
            return;
        }
        auto it = klass->getProperties().find(identifier);
        if (it != klass->getProperties().end()) {
            resolvedType = it->second->getType();
        }
    }

    // `a.b` lowers to a struct GEP. lhs may be the alloca holding the struct (stack-local)
    // or a pointer loaded from the heap; both yield an address we can GEP into. The member
    // index comes from StructureProperty::getOrder() — set when the class registered its
    // properties during signature pass.
    string DotExpression::buildPath(const ExpressionPtr& expr) {
        if (auto id = dynamic_pointer_cast<IdentifierExpression>(expr)) {
            return id->getTextValue();
        }
        if (auto dot = dynamic_pointer_cast<DotExpression>(expr)) {
            const auto& children = const_cast<DotExpression*>(dot.get())->getChildren();
            if (children.empty()) return "";
            auto lhs = dynamic_pointer_cast<Expression>(children[0]);
            if (!lhs) return "";
            string lhsPath = buildPath(lhs);
            if (lhsPath.empty()) return "";
            return lhsPath + "." + dot->getIdentifier();
        }
        return "";
    }

    llvm::Value* DotExpression::generateCode(CajetaModulePtr module) {
        if (children.empty()) {
            return nullptr;
        }

        // Use-after-move check for field-access paths. Build the dotted path
        // from this DotExpression down to its named root, then ask the scope
        // whether any prefix of that path has been moved-out. This catches
        // patterns like `#person.name` followed by a read of `person.name`
        // or any path through it.
        //
        // Per MemoryModel.md § Path-based borrow tracking. Skip when the LHS
        // doesn't bottom out at a named identifier (e.g. `factory.make().foo`)
        // — those produce anonymous owners and are handled by the separate
        // anonymous-owner rule.
        {
            ExpressionPtr self = dynamic_pointer_cast<Expression>(shared_from_this());
            string path = buildPath(self);
            if (!path.empty()) {
                auto scope = module->getScopeStack().peek();
                if (scope && scope->isPathMoved(path)) {
                    throw Exception("use-after-move: path '" + path
                        + "' was transferred via `#` and cannot be read here",
                        "CAJETA_ERROR_USE_AFTER_MOVE");
                }
            }
        }

        // Static-namespace constants: Math.PI / Math.E / Integer.MAX_VALUE / ... .
        // These have no instance backing and don't survive the GEP path below, so we
        // short-circuit them here and emit IR constants directly.
        if (auto idExpr = dynamic_pointer_cast<IdentifierExpression>(children[0])) {
            auto& ctx = *module->getLlvmContext();
            const std::string& ns = idExpr->getTextValue();
            if (ns == "Math") {
                if (identifier == "PI") return llvm::ConstantFP::get(
                    llvm::Type::getDoubleTy(ctx), M_PI);
                if (identifier == "E")  return llvm::ConstantFP::get(
                    llvm::Type::getDoubleTy(ctx), M_E);
            } else if (ns == "Integer") {
                if (identifier == "MAX_VALUE") return llvm::ConstantInt::get(
                    llvm::Type::getInt32Ty(ctx), INT32_MAX, /*isSigned=*/true);
                if (identifier == "MIN_VALUE") return llvm::ConstantInt::get(
                    llvm::Type::getInt32Ty(ctx), INT32_MIN, /*isSigned=*/true);
            } else if (ns == "Long") {
                if (identifier == "MAX_VALUE") return llvm::ConstantInt::get(
                    llvm::Type::getInt64Ty(ctx), INT64_MAX, /*isSigned=*/true);
                if (identifier == "MIN_VALUE") return llvm::ConstantInt::get(
                    llvm::Type::getInt64Ty(ctx), INT64_MIN, /*isSigned=*/true);
            }
        }

        llvm::Value* base = children[0]->generateCode(module);
        if (!base) {
            return nullptr;
        }

        auto lhs = dynamic_pointer_cast<Expression>(children[0]);
        if (!lhs) {
            return nullptr;
        }
        auto klass = dynamic_pointer_cast<CajetaClass>(lhs->getResolvedType());
        if (!klass) {
            return nullptr;
        }
        auto it = klass->getProperties().find(identifier);
        if (it == klass->getProperties().end()) {
            return nullptr;
        }
        StructurePropertyPtr property = it->second;
        return module->getBuilder()->CreateStructGEP(klass->getLlvmType(), base,
            property->getOrder(), identifier);
    }

} // code
