//
// Created by James Klappenbach on 4/19/23.
//

#include <cstdlib>
#include <cstdio>
#include "MethodCallExpression.h"
#include "cajeta/ownership/TitleClassifier.h"
#include "CallExpression.h"
#include "../../error/DiagnosticEngine.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/compile/ScriptUnitSynthesis.h"
#include "cajeta/compile/SessionState.h"
#include "cajeta/xref/XrefIndex.h"
#include "cajeta/compile/Compiler.h"
#include "cajeta/type/CajetaArray.h"
#include "cajeta/type/CajetaVector.h"
#include "cajeta/type/VectorOps.h"
#include "cajeta/type/CajetaMatrix.h"
#include "cajeta/type/MatrixOps.h"
#include "cajeta/type/CajetaQuaternion.h"
#include "cajeta/type/QuaternionOps.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/type/CajetaConstantType.h"
#include "cajeta/type/CajetaTask.h"
#include "cajeta/type/CajetaView.h"
#include "cajeta/type/CajetaFunctionType.h"
#include "cajeta/method/Method.h"
#include "cajeta/asn/Statement.h"
#include "cajeta/transform/GradBackward.h"
#include "cajeta/transform/VmapBatch.h"
#include "cajeta/transform/FuseExpr.h"
#include "cajeta/compile/Optimizer.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "cajeta/synth/SourceSynthesisParse.h"
#include "cajeta/synth/SourceSynthesis.h"
#include "cajeta/compile/CajetaLlvmVisitor.h"
#include "cajeta/asn/ClassBodyDeclaration.h"
#include "cajeta/type/QualifiedName.h"
#include "cajeta/method/FactoryProviderMethod.h"
#include "cajeta/error/Diagnostics.h"
#include "cajeta/error/Exception.h"
#include "cajeta/util/MemoryManager.h"
#include "Expression.h"
#include "BinaryOpExpression.h"
#include "DotExpression.h"
#include "Identifier.h"
#include "LiteralExpression.h"
#include "cajeta/xpu/core/XpuAttributes.h"
#include "NewExpression.h"
#include "cajeta/type/Scope.h"
#include "cajeta/field/Field.h"
#include "cajeta/field/ParameterField.h"
#include "cajeta/field/BoundClosureField.h"
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Intrinsics.h>

namespace cajeta {

    // Wraps a malloc'd C string into a `cajeta.lang.String` instance (6.2.2 tagged
    // core). `freeAfterWrap` false marks a `.rodata` cstr that must not be freed;
    // returns `cstr` unchanged while the class String type is not yet loaded.
    llvm::Value* wrapCStringIntoClassString(
            CajetaModulePtr module,
            llvm::Value* cstr,
            const char* namePrefix,
            bool freeAfterWrap) {
        auto& llvmCtx = *module->getLlvmContext();
        auto* builder = module->getBuilder();
        llvm::Type* ptrTy = llvm::PointerType::get(llvmCtx, 0);
        llvm::Type* i64Ty = llvm::Type::getInt64Ty(llvmCtx);
        llvm::Type* i32Ty = llvm::Type::getInt32Ty(llvmCtx);
        llvm::Type* i8Ty = llvm::Type::getInt8Ty(llvmCtx);

        CajetaTypePtr stringTy = CajetaType::of("String");
        auto stringKlass = std::dynamic_pointer_cast<CajetaClass>(stringTy);
        if (!stringKlass || !stringKlass->getLlvmType()
                || !llvm::isa<llvm::StructType>(stringKlass->getLlvmType())) {
            return cstr;
        }
        std::string pfx = namePrefix ? namePrefix : "str";
        (void) i8Ty;
        (void) i64Ty;
        (void) i32Ty;

        llvm::Constant* vtableRef = llvm::ConstantPointerNull::get(
            llvm::cast<llvm::PointerType>(ptrTy));
        if (auto* vt = stringKlass->getVirtualTableGlobal()) {
            vtableRef = CajetaModule::ensureGlobalInModule(
                module->getLlvmModule(), vt);
        }
        llvm::FunctionType* wrapTy = llvm::FunctionType::get(
            ptrTy, {ptrTy, ptrTy, llvm::Type::getInt32Ty(llvmCtx)}, false);
        llvm::FunctionCallee wrapFn =
            module->getLlvmModule()->getOrInsertFunction(
                "__cajeta_string_wrap_cstr", wrapTy);
        return builder->CreateCall(wrapFn,
            {cstr, vtableRef,
             llvm::ConstantInt::get(llvm::Type::getInt32Ty(llvmCtx),
                 freeAfterWrap ? 1 : 0)},
            pfx + ".wrap");
    }

    // Spills an aggregate closure argument to an entry-block slot and returns its
    // address: class / interface / array params cross the call boundary as `ptr`,
    // so a value-materialized interface fat pointer would fail LLVM verify.
    static llvm::Value* spillAggregateForByPointerArg(CajetaModulePtr module,
                                                      llvm::Value* v) {
        auto* builder = module->getBuilder();
        llvm::Function* curFn = builder->GetInsertBlock()->getParent();
        llvm::IRBuilder<> entryBuilder(
            &curFn->getEntryBlock(), curFn->getEntryBlock().begin());
        llvm::Value* slot = entryBuilder.CreateAlloca(
            v->getType(), nullptr, "byptr_arg");
        builder->CreateStore(v, slot);
        return slot;
    }

    llvm::Value* emitClosureCall(CajetaModulePtr module,
                                 llvm::Value* closurePtr,
                                 const std::shared_ptr<CajetaFunctionType>& fnType,
                                 const vector<MethodCallParameter>& args,
                                 CajetaTypePtr& outResolvedType,
                                 llvm::Function* directFn) {
        auto& llvmCtx = *module->getLlvmContext();
        auto* builder = module->getBuilder();
        llvm::Type* ptrTy = llvm::PointerType::get(llvmCtx, 0);
        // L3-3 closure record layout: { ptr fn, ptr captures, ptr drop_fn }. The call
        // site reads fn + captures only; drop_fn is the runtime's at scope exit.
        llvm::StructType* closureTy = llvm::StructType::get(
            llvmCtx, {ptrTy, ptrTy, ptrTy});
        llvm::Value* callee;
        llvm::Value* captures;
        if (directFn) {
            callee = directFn;
            captures = llvm::ConstantPointerNull::get(ptrTy);
        } else {
            llvm::Value* fnSlot = builder->CreateStructGEP(
                closureTy, closurePtr, 0, "closure.fn");
            callee = builder->CreateLoad(ptrTy, fnSlot, "fn_ptr");
            llvm::Value* capSlot = builder->CreateStructGEP(
                closureTy, closurePtr, 1, "closure.captures");
            captures = builder->CreateLoad(ptrTy, capSlot, "captures_ptr");
        }

        // sret form: the caller allocates the result slot and threads it as the
        // closure's hidden arg 0; the call returns void and yields the slot pointer.
        llvm::Value* sretSlot = nullptr;
        auto retClass = dynamic_pointer_cast<CajetaClass>(fnType->getReturnType());
        if (fnType->usesSret() && retClass) {
            llvm::Function* curFn = builder->GetInsertBlock()->getParent();
            llvm::IRBuilder<> entryBuilder(
                &curFn->getEntryBlock(), curFn->getEntryBlock().begin());
            sretSlot = entryBuilder.CreateAlloca(
                retClass->getLlvmType(), nullptr, "fn_sret");
        }
        vector<llvm::Value*> callArgs;
        if (sretSlot) callArgs.push_back(sretSlot);
        callArgs.push_back(captures);
        size_t baseIdx = sretSlot ? 2 : 1;
        llvm::FunctionType* sig = fnType->getLlvmFunctionType();
        for (size_t i = 0; i < args.size(); ++i) {
            if (args[i].expression && !args[i].expression->getResolvedType()) {
                args[i].expression->resolveTypes(module);
            }
            llvm::Value* v = args[i].expression->generateCode(module);
            auto exprAst = dynamic_pointer_cast<Expression>(args[i].expression);
            v = loadIfLValue(module, v, exprAst);
            size_t sigIdx = baseIdx + i;
            if (sig && sigIdx < sig->getNumParams() && v
                    && v->getType() != sig->getParamType(sigIdx)) {
                llvm::Type* expected = sig->getParamType(sigIdx);
                if (expected->isIntegerTy() && v->getType()->isIntegerTy()) {
                    v = builder->CreateIntCast(v, expected, /*isSigned=*/true);
                } else if (expected->isFloatingPointTy()
                        && v->getType()->isFloatingPointTy()) {
                    v = builder->CreateFPCast(v, expected);
                } else if (expected->isPointerTy() && !v->getType()->isPointerTy()) {
                    v = spillAggregateForByPointerArg(module, v);
                }
            }
            callArgs.push_back(v);
        }
        llvm::CallInst* call = builder->CreateCall(sig, callee, callArgs);
        if (sretSlot && retClass) {
            call->addParamAttr(0, llvm::Attribute::get(
                llvmCtx, llvm::Attribute::StructRet, retClass->getLlvmType()));
            if (fnType->getReturnType()) outResolvedType = fnType->getReturnType();
            return sretSlot;
        }
        return call;
    }

    // Builds the node from a `methodCall` context (identifier / `this` / `super`
    // forms). An optional `<typeList>` before '(' is Form C explicit call-site type
    // args for a method-templated callee; inference is the common case.
    MethodCallExpression::MethodCallExpression(
        CajetaParser::MethodCallContext* ctx,
        antlr4::Token* token) : Expression(token) { exprKind = ExprKind::MethodCall;
        nameLine = getSourceLine();
        nameColumn = getSourceColumn();
        if (ctx->SUPER()) {
            superCtorCall = true;
            methodCallName = "super";
            if (auto* tok = ctx->SUPER()->getSymbol()) {
                nameLine = static_cast<int>(tok->getLine());
                nameColumn = tok->getCharPositionInLine();
            }
        } else if (ctx->identifier()) {
            methodCallName = ctx->identifier()->getText();
            if (auto* tok = ctx->identifier()->getStart()) {
                nameLine = static_cast<int>(tok->getLine());
                nameColumn = tok->getCharPositionInLine();
            }
        } else {
            methodCallName = "this";
        }
        if (auto* paramList = ctx->parameterList()) {
            for (auto& ctxParameterEntry : paramList->parameterEntry()) {
                MethodCallParameter entry;
                entry.expression = Expression::fromContext(
                    ctxParameterEntry->expression());
                if (ctxParameterEntry->parameterLabel()) {
                    entry.label = ctxParameterEntry->parameterLabel()->getText();
                }
                if (ctxParameterEntry->REFERENCE()) {
                    entry.callerTransferred = true;
                }
                parameters.push_back(entry);
            }
        }
        if (auto* targs = ctx->typeArguments()) {
            auto activeMod = CajetaModule::getActiveModule();
            for (auto* ta : targs->typeArgument()) {
                if (ta->integerLiteral() != nullptr) {
                    explicitMethodTypeArgs.push_back(CajetaConstantType::of(
                        CajetaConstantType::parseLiteral(ta->integerLiteral())));
                } else if (ta->typeType()) {
                    auto t = CajetaType::fromContext(ta->typeType(), activeMod);
                    if (t) explicitMethodTypeArgs.push_back(t);
                }
            }
        }
    }

    // Maps a `System.<stream>` name to its POSIX file descriptor, or -1 when the
    // name is not a recognized stream. `stderror` is accepted as an alias.
    static int systemStreamFd(const std::string& name) {
        if (name == "stdout") return 1;
        if (name == "stderr" || name == "stderror") return 2;
        if (name == "stdin") return 0;
        return -1;
    }

    // Returns the misspelled name behind a `System.<something>` receiver that is no
    // known stream, so the caller can raise a diagnostic. Empty string means the
    // receiver is not that shape and ordinary method resolution should proceed.
    static std::string detectSystemUnknownStream(const AbstractSyntaxNodePtr& receiver) {
        auto dot = dynamic_pointer_cast<DotExpression>(receiver);
        if (!dot) return "";
        const std::string& name = dot->getIdentifier();
        if (systemStreamFd(name) >= 0) return "";
        if (name == "env" || name == "property" || name == "args") return "";
        const auto& dotChildren = const_cast<DotExpression*>(dot.get())->getChildren();
        if (dotChildren.empty()) return "";
        auto sys = dynamic_pointer_cast<IdentifierExpression>(dotChildren[0]);
        if (!sys) return "";
        if (sys->getTextValue() != "System") return "";
        return name;
    }

    // Returns "env", "property" or "args" for a `System.<ns>` receiver driving the
    // environment / system-property / argv intrinsics, or empty string.
    static std::string detectSystemNamespaceReceiver(const AbstractSyntaxNodePtr& receiver) {
        auto dot = dynamic_pointer_cast<DotExpression>(receiver);
        if (!dot) return "";
        const std::string& name = dot->getIdentifier();
        if (name != "env" && name != "property" && name != "args") return "";
        const auto& dotChildren = const_cast<DotExpression*>(dot.get())->getChildren();
        if (dotChildren.empty()) return "";
        auto sys = dynamic_pointer_cast<IdentifierExpression>(dotChildren[0]);
        if (!sys) return "";
        if (sys->getTextValue() != "System") return "";
        return name;
    }

    // Returns the file descriptor behind a `System.<stream>` receiver, else -1, so
    // `System.stdout.println(...)` lowers without field/method resolution.
    static int detectSystemStreamReceiver(const AbstractSyntaxNodePtr& receiver) {
        auto dot = dynamic_pointer_cast<DotExpression>(receiver);
        if (!dot) return -1;
        int fd = systemStreamFd(dot->getIdentifier());
        if (fd < 0) return -1;
        const auto& dotChildren = const_cast<DotExpression*>(dot.get())->getChildren();
        if (dotChildren.empty()) return -1;
        auto sys = dynamic_pointer_cast<IdentifierExpression>(dotChildren[0]);
        if (!sys) return -1;
        if (sys->getTextValue() != "System") return -1;
        return fd;
    }

    // Lowers an evaluated argument to the runtime `char*` ABI, loading through
    // l-values and unwrapping a class String to its NUL-terminated data pointer.
    static llvm::Value* loadStringArg(CajetaModulePtr module, const AbstractSyntaxNodePtr& argNode) {
        auto* builder = module->getBuilder();
        auto& llvmCtx = *module->getLlvmContext();
        llvm::Value* v = argNode->generateCode(module);
        auto argExpr = std::dynamic_pointer_cast<Expression>(argNode);
        CajetaTypePtr argTy = argExpr ? argExpr->getResolvedType() : nullptr;
        if (!argTy && argExpr) {
            argExpr->resolveTypes(module);
            argTy = argExpr->getResolvedType();
        }
        if (argExpr) {
            v = loadIfLValue(module, v, argExpr);
        } else if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(v)) {
            v = builder->CreateLoad(a->getAllocatedType(), a);
        }
        if (argTy) {
            auto cls = std::dynamic_pointer_cast<CajetaClass>(argTy);
            if (cls && cls->getQName()
                    && cls->getQName()->getTypeName() == "String"
                    && cls->getQName()->getPackageName() == "cajeta.lang"
                    && cls->getLlvmType()
                    && llvm::isa<llvm::StructType>(cls->getLlvmType())) {
                llvm::Function* cstrFn =
                    module->getRuntimeFunction("__cajeta_string_cstr");
                v = builder->CreateCall(cstrFn, {v}, "strArg.cstr");
            }
        }
        return v;
    }

    // Lowers an evaluated ARRAY argument to the runtime ABI: the array's header
    // pointer, loading through l-values so a field or element argument passes the
    // header rather than its slot address.
    static llvm::Value* loadArrayArg(CajetaModulePtr module, const AbstractSyntaxNodePtr& argNode) {
        auto* builder = module->getBuilder();
        llvm::Value* v = argNode->generateCode(module);
        auto argExpr = std::dynamic_pointer_cast<Expression>(argNode);
        if (argExpr && !argExpr->getResolvedType()) {
            argExpr->resolveTypes(module);
        }
        if (argExpr) {
            v = loadIfLValue(module, v, argExpr);
        } else if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(v)) {
            v = builder->CreateLoad(a->getAllocatedType(), a);
        }
        return v;
    }

    bool MethodCallExpression::freshOwnedStringTemp(const AbstractSyntaxNodePtr& e) {
        auto isLangString = [](CajetaTypePtr t) -> bool {
            auto cls = dynamic_pointer_cast<CajetaClass>(t);
            return cls && cls->getQName()
                && cls->getQName()->getTypeName() == "String"
                && cls->getQName()->getPackageName() == "cajeta.lang";
        };
        if (auto bop = dynamic_pointer_cast<BinaryOpExpression>(e)) {
            return bop->getBinaryOp() == BINARY_OP_ADD
                && !bop->isArenaEligible()
                && isLangString(bop->getResolvedType());
        }
        if (auto amce = dynamic_pointer_cast<MethodCallExpression>(e)) {
            // NOT bindingTakesTitle(). This is the RECLAMATION question - does the
            // enclosing statement free this temporary - and its false-on-unresolved default
            // is deliberate. The two questions must not share an accessor.
            return amce->isResolvedReturnsOwnership()
                && isLangString(amce->getResolvedType());
        }
        return false;
    }

    shared_ptr<CajetaClass> MethodCallExpression::freshSharedValueTempClass(
            const AbstractSyntaxNodePtr& e) {
        auto amce = dynamic_pointer_cast<MethodCallExpression>(e);
        if (!amce) return nullptr;
        auto cls = dynamic_pointer_cast<CajetaClass>(amce->getResolvedType());
        if (cls && cls->isValueType() && cls->isSharedCapableValue()) {
            return cls;
        }
        return nullptr;
    }

    shared_ptr<CajetaClass> MethodCallExpression::droppableTempClass(
            const CajetaTypePtr& t) {
        auto cls = dynamic_pointer_cast<CajetaClass>(t);
        if (!cls) return nullptr;
        if (dynamic_pointer_cast<CajetaView>(t)) return nullptr;
        if (cls->isInterface() || cls->isValueType()) return nullptr;
        if (cls->isSharedCapableValue()) return nullptr;
        auto qn = cls->getQName();
        if (qn && qn->getTypeName() == "String"
                && qn->getPackageName() == "cajeta.lang") {
            return nullptr;
        }
        return cls;
    }

    shared_ptr<CajetaClass> MethodCallExpression::freshHeapCreatorTempClass(
            const AbstractSyntaxNodePtr& e) {
        auto ne = dynamic_pointer_cast<NewExpression>(e);
        if (!ne || ne->getStackAlloc() || ne->getSharedAlloc()) return nullptr;
        return droppableTempClass(ne->getResolvedType());
    }

    bool MethodCallExpression::freshHeapArrayLiteralArg(
            const AbstractSyntaxNodePtr& e) {
        auto lit = dynamic_pointer_cast<ArrayLiteralExpression>(e);
        return lit && !lit->isStackAlloc() && !lit->isArenaEligible();
    }

    // The inlined forward value source and the grad source of one differentiation pass.
    struct GradPieces { std::string valueExpr; std::string gradExpr; };

    // Returns the first ReturnStatement's expression under `node`, or null. That
    // expression is not in `children`, so the walk goes through forEachSubNode.
    static Expression* findReturnExpr(const AbstractSyntaxNodePtr& node) {
        if (!node) return nullptr;
        if (auto* ret = dynamic_cast<ReturnStatement*>(node.get()))
            return ret->getExpression().get();
        Expression* found = nullptr;
        node->forEachSubNode([&](const AbstractSyntaxNodePtr& c) {
            if (!found) found = findReturnExpr(c);
        });
        return found;
    }

    // Builds the call resolver over the class being compiled: maps a call name and
    // arity to a same-class static single-return helper, flagging @NoGrad as
    // stop-gradient. Anything not a unique same-class static resolves to not-found.
    static cajeta::transform::CallResolver makeCallResolver(CajetaModulePtr module) {
        CajetaClassPtr enclosing = module->getStructureStack().empty()
            ? nullptr : module->getStructureStack().back();
        return [enclosing](const std::string& recv, const std::string& name,
                           size_t arity) -> cajeta::transform::InlineTarget {
            cajeta::transform::InlineTarget t;
            CajetaClassPtr host = enclosing;
            if (!recv.empty()
                    && !(enclosing && enclosing->getQName()
                         && enclosing->getQName()->getTypeName() == recv)) {
                auto& cmap = CajetaType::getCanonicalMap();
                auto it = cmap.find(recv);
                host = (it != cmap.end())
                    ? dynamic_pointer_cast<CajetaClass>(it->second) : nullptr;
            }
            if (!host) return t;
            MethodPtr match;
            for (auto& m : host->getMethodList()) {
                if (m && m->isStatic() && m->getName() == name
                        && m->getParameterList().size() == arity) {
                    if (match) return cajeta::transform::InlineTarget{};
                    match = m;
                }
            }
            if (!match) return t;
            t.found = true;
            t.noGrad = match->findAnnotation("NoGrad") != nullptr;
            auto rt = match->getReturnType();
            std::string rc = rt ? rt->toCanonical() : std::string();
            t.returnIsTensor = rc.rfind("cajeta.math.Tensor", 0) == 0
                            || rc.rfind("Tensor<", 0) == 0;
            t.returnTy = rc;
            t.qualifiedName = host->getQName()->getTypeName() + "." + name;
            for (auto& p : match->getParameterList()) t.paramNames.push_back(p->getName());
            t.body = findReturnExpr(match->getBlock());
            return t;
        };
    }

    // Symbolically differentiates `bodyExpr` w.r.t. `pnames[argnum]`, returning the
    // inlined forward value source and the grad source (an explicit zero when the
    // body is independent of it). Throws located errors for an unsupported body.
    static GradPieces differentiateBody(
            MethodCallExpression* self, Expression* bodyExpr,
            const std::vector<std::string>& pnames,
            const std::map<std::string, bool>& paramRank,
            size_t argnum, const std::string& elem, bool selIsTensor,
            const cajeta::transform::CallResolver& resolveCall) {
        auto locErr = [&](const std::string& msg, const char* id) {
            return Exception(msg, id, "", self->getSourceLine(), self->getSourceColumn());
        };
        std::vector<cajeta::transform::AdNode> nodes;
        std::map<std::string, size_t> pidx;
        std::string err;
        if (!cajeta::transform::buildDag(bodyExpr, pnames, paramRank, resolveCall,
                                         nodes, pidx, &err)) {
            throw locErr(err, "CAJETA_ERROR_TRANSFORM_UNSUPPORTED_BODY");
        }
        if (!nodes.empty() && nodes.back().isTensor) {
            throw locErr("transform intrinsic 'Grad' differentiates a scalar-valued "
                         "function (reduce with sum(...))",
                         "CAJETA_ERROR_TRANSFORM_UNSUPPORTED_BODY");
        }
        std::string zero = selIsTensor
            ? ("Tensor.zerosLike<" + elem + ">(" + pnames[argnum] + ")") : "0.0f";
        std::string gradExpr, missing;
        auto pn = pidx.find(pnames[argnum]);
        if (pn == pidx.end()) {
            gradExpr = zero;
        } else {
            gradExpr = cajeta::transform::reverseModeGrad(nodes, pn->second, elem, &missing);
            if (!missing.empty()) {
                throw locErr("transform intrinsic 'Grad': no VJP rule for primitive '"
                             + missing + "'", "CAJETA_ERROR_TRANSFORM_NO_VJP_RULE");
            }
            if (gradExpr.empty()) gradExpr = zero;
        }
        return { nodes.empty() ? std::string() : nodes.back().valueExpr, gradExpr };
    }

    // Emits the Tier-A backward helper class for a differentiated function and
    // returns the callable closure its make() yields. `outResolvedType` receives
    // make()'s return type. Shared by the first-order and higher-order Grad paths.
    static llvm::Value* synthesizeMakeClosure(
            MethodCallExpression* self, CajetaModulePtr module,
            const std::string& className, const std::string& source,
            const char* what, CajetaTypePtr& outResolvedType);

    static llvm::Value* emitBackwardClosure(
            MethodCallExpression* self, CajetaModulePtr module,
            const std::string& classSeed,
            const std::vector<std::string>& pnames,
            const std::vector<std::string>& paramTys,
            const std::string& valueTy, const std::string& gradTy,
            const std::string& outputVal, const std::string& gradExpr,
            bool importTensor, CajetaTypePtr& outResolvedType) {
        std::string className = "__GradBwd_" + cajeta::synth::deriveSynthName(
            "grad", classSeed, {outputVal, gradExpr});
        std::string pkg = module->getQName()->getPackageName();
        std::string source = (pkg.empty() ? std::string() : ("package " + pkg + ";\n"))
            + cajeta::transform::emitBackwardSource(className, pnames, paramTys,
                                                    valueTy, gradTy, outputVal, gradExpr,
                                                    importTensor);
        if (std::getenv("CAJETA_GRAD_DUMP")) {
            fprintf(stderr, "=== GRAD BACKWARD SOURCE ===\n%s\n=== END ===\n",
                    source.c_str());
        }

        return synthesizeMakeClosure(self, module, className, source, "Grad",
                                     outResolvedType);
    }

    // The shared Tier-A synthesis seam: parses `source`, extracts its single static
    // `make()`, codegens it and emits a call, so the transform's value is the
    // closure record make() returns. `what` names the intrinsic in diagnostics.
    static llvm::Value* synthesizeMakeClosure(
            MethodCallExpression* self, CajetaModulePtr module,
            const std::string& className, const std::string& source,
            const char* what, CajetaTypePtr& outResolvedType) {
        auto locErr = [&](const std::string& msg, const char* id) {
            return Exception(msg, id, "", self->getSourceLine(), self->getSourceColumn());
        };
        const std::string intrin = std::string("transform intrinsic '") + what + "'";

        auto* compUnit = cajeta::synth::parseSynthesizedUnit(source);
        CajetaParser::ClassDeclarationContext* classDecl = nullptr;
        for (auto* td : compUnit->typeDeclaration()) {
            if (auto* cd = td->classDeclaration()) { classDecl = cd; break; }
        }
        if (!classDecl) {
            throw locErr(intrin + ": synthesized source did not parse",
                         "CAJETA_ERROR_TRANSFORM_SYNTH_FAILED");
        }

        auto wrapperQName = QualifiedName::getOrInsert(
            className, module->getQName()->getPackageName());
        auto wrapperClass = std::make_shared<CajetaClass>(
            module, wrapperQName, std::list<QualifiedNamePtr>{});

        // Restores active module, structure stack and builder insertion point on EVERY
        // exit, an exception out of the synthesized source's codegen included.
        struct SynthStateGuard {
            CajetaModulePtr module;
            CajetaModulePtr prevActive;
            std::list<CajetaClassPtr> savedStack;
            llvm::IRBuilderBase::InsertPoint savedIP;
            SynthStateGuard(CajetaModulePtr m, const CajetaClassPtr& wrapper)
                : module(m), prevActive(CajetaModule::getActiveModule()),
                  savedIP(m->getBuilder()->saveIP()) {
                CajetaModule::setActiveModule(m);
                auto& stk = m->getStructureStack();
                savedStack.swap(stk);
                stk.push_back(wrapper);
            }
            ~SynthStateGuard() {
                auto& stk = module->getStructureStack();
                stk.clear();
                stk.swap(savedStack);
                CajetaModule::setActiveModule(prevActive);
                module->getBuilder()->restoreIP(savedIP);
            }
        };

        MethodPtr makeMethod;
        llvm::Function* makeFn = nullptr;
        {
            SynthStateGuard guard(module, wrapperClass);

            CajetaLlvmVisitor visitor(module);
            auto bodyAny = visitor.visitClassBody(classDecl->classBody());
            auto classBody = std::any_cast<ClassBodyDeclarationPtr>(bodyAny);

            std::vector<MethodPtr> synthMethods;
            for (auto& decl : classBody->getDeclarations()) {
                if (auto md = std::dynamic_pointer_cast<MethodDeclaration>(decl)) {
                    if (!md->getMethod()) continue;
                    md->updateParent(wrapperClass);
                    synthMethods.push_back(md->getMethod());
                    if (md->getMethod()->getName() == "make") {
                        makeMethod = md->getMethod();
                    }
                }
            }
            if (!makeMethod) {
                throw locErr(intrin + ": synthesized source produced no make()",
                             "CAJETA_ERROR_TRANSFORM_SYNTH_FAILED");
            }
            for (auto& m : synthMethods) m->generatePrototype();
            for (auto& m : synthMethods) m->generateCode();
            makeFn = makeMethod->getLlvmFunction();
        }

        if (!makeFn) {
            throw locErr(intrin + ": synthesized source failed to emit",
                         "CAJETA_ERROR_TRANSFORM_SYNTH_FAILED");
        }

        llvm::Value* result = module->getBuilder()->CreateCall(
            makeMethod->getLlvmFunctionType(), makeFn, {});
        outResolvedType = makeMethod->getReturnType();
        return result;
    }

    // Synthesizes Grad(f)'s Tier-A backward and returns it as a callable value:
    // walks f's body into a forward DAG, reverse-composes the VJP rules, then emits
    // and calls the helper class's make(). The result type is make()'s return type.
    static llvm::Value* emitGradBackward(MethodCallExpression* self,
                                         const std::shared_ptr<LambdaExpression>& lam,
                                         const std::shared_ptr<CajetaFunctionType>& fnType,
                                         CajetaModulePtr module,
                                         CajetaTypePtr& outResolvedType) {
        auto locErr = [&](const std::string& msg, const char* id) {
            return Exception(msg, id, "", self->getSourceLine(), self->getSourceColumn());
        };

        const auto& pnames = lam->getParamNames();
        const auto& ptypes = lam->getParamTypes();
        if (pnames.empty() || pnames.size() != ptypes.size()) {
            throw locErr("transform intrinsic 'Grad' requires a lambda with one or "
                         "more explicitly-typed parameters",
                         "CAJETA_ERROR_TRANSFORM_UNSUPPORTED_BODY");
        }

        int64_t argnum = 0;
        const auto& gTypeArgs = self->getExplicitMethodTypeArgs();
        if (!gTypeArgs.empty()) {
            auto c = std::dynamic_pointer_cast<CajetaConstantType>(gTypeArgs[0]);
            if (!c) {
                throw locErr("transform intrinsic 'Grad<...>' argnum selector must be "
                             "an integer constant", "CAJETA_ERROR_TRANSFORM_UNSUPPORTED_BODY");
            }
            argnum = c->getValue();
        }
        if (argnum < 0 || argnum >= (int64_t)pnames.size()) {
            throw locErr("transform intrinsic 'Grad<" + std::to_string(argnum)
                         + ">' selects an argument out of range for the "
                         + std::to_string(pnames.size()) + "-parameter function",
                         "CAJETA_ERROR_TRANSFORM_UNSUPPORTED_BODY");
        }

        lam->setExpectedType(fnType);
        lam->resolveTypes(module);
        auto bodyExpr = std::dynamic_pointer_cast<Expression>(lam->getBody());
        if (!bodyExpr) {
            throw locErr("transform intrinsic 'Grad' v1 requires an expression-body "
                         "lambda", "CAJETA_ERROR_TRANSFORM_UNSUPPORTED_BODY");
        }

        auto tensorElem = [](const std::string& ty, std::string& elemOut) -> bool {
            bool t = ty.rfind("cajeta.math.Tensor", 0) == 0
                  || ty.rfind("Tensor<", 0) == 0;
            elemOut = ty;
            if (t) {
                auto lt = ty.find('<');
                auto gt = ty.rfind('>');
                if (lt != std::string::npos && gt != std::string::npos && gt > lt + 1)
                    elemOut = ty.substr(lt + 1, gt - lt - 1);
            }
            return t;
        };
        std::map<std::string, bool> paramRank;
        for (size_t i = 0; i < pnames.size(); ++i) {
            std::string ignore;
            paramRank[pnames[i]] = tensorElem(ptypes[i]->toCanonical(), ignore);
        }
        std::string selTy = ptypes[argnum]->toCanonical();
        std::string elem;
        bool selIsTensor = tensorElem(selTy, elem);

        auto resolveCall = makeCallResolver(module);
        GradPieces gp = differentiateBody(self, bodyExpr.get(), pnames, paramRank,
                                          (size_t)argnum, elem, selIsTensor, resolveCall);
        std::string outputVal = gp.valueExpr;
        std::string gradExpr = gp.gradExpr;

        std::string valueTy = fnType->getReturnType()
            ? fnType->getReturnType()->toCanonical() : std::string("void");
        if ((valueTy.empty() || valueTy == "void") && bodyExpr->getResolvedType()) {
            valueTy = bodyExpr->getResolvedType()->toCanonical();
        }
        if (valueTy.empty() || valueTy == "void") {
            if (auto sumCall = std::dynamic_pointer_cast<MethodCallExpression>(bodyExpr)) {
                if ((sumCall->getMethodCallName() == "sum"
                         || sumCall->getMethodCallName() == "mean")
                        && !sumCall->getExplicitMethodTypeArgs().empty()) {
                    auto r = sumCall->getExplicitMethodTypeArgs().back();
                    if (r) valueTy = r->toCanonical();
                }
            }
        }
        if (valueTy.empty() || valueTy == "void") {
            if (auto call = std::dynamic_pointer_cast<MethodCallExpression>(bodyExpr)) {
                std::string vrecv;
                if (!call->getChildren().empty()) {
                    if (auto* rid = dynamic_cast<IdentifierExpression*>(
                            call->getChildren()[0].get())) {
                        vrecv = rid->getTextValue();
                    }
                }
                auto t = resolveCall(vrecv, call->getMethodCallName(),
                                     call->getParameters().size());
                if (t.found && !t.returnTy.empty()) valueTy = t.returnTy;
            }
        }
        if ((valueTy.empty() || valueTy == "void") && !selIsTensor) {
            valueTy = selTy;
        }
        if (valueTy.empty() || valueTy == "void") {
            throw locErr("transform intrinsic 'Grad': could not determine the value "
                         "type of the differentiated function",
                         "CAJETA_ERROR_TRANSFORM_UNSUPPORTED_BODY");
        }

        std::vector<std::string> paramTys;
        paramTys.reserve(pnames.size());
        bool importTensor = false;
        for (size_t i = 0; i < pnames.size(); ++i) {
            paramTys.push_back(ptypes[i]->toCanonical());
            importTensor = importTensor || paramRank[pnames[i]];
        }
        if (outputVal.find("Tensor.") != std::string::npos
                || gradExpr.find("Tensor.") != std::string::npos) {
            importTensor = true;
        }

        return emitBackwardClosure(self, module, fnType->toCanonical(), pnames,
                                   paramTys, valueTy, selTy, outputVal, gradExpr,
                                   importTensor, outResolvedType);
    }

    // GradAll<K>(f): one backward closure returning GradResult<V, G[]> with grads
    // for the leading K parameters in argument order (bare GradAll grades every
    // one). v1 requires the K differentiated params to share one type spelling.
    static llvm::Value* emitGradAllBackward(MethodCallExpression* self,
                                            const std::shared_ptr<LambdaExpression>& lam,
                                            const std::shared_ptr<CajetaFunctionType>& fnType,
                                            CajetaModulePtr module,
                                            CajetaTypePtr& outResolvedType) {
        auto locErr = [&](const std::string& msg, const char* id) {
            return Exception(msg, id, "", self->getSourceLine(), self->getSourceColumn());
        };

        const auto& pnames = lam->getParamNames();
        const auto& ptypes = lam->getParamTypes();
        if (pnames.empty() || pnames.size() != ptypes.size()) {
            throw locErr("transform intrinsic 'GradAll' requires a lambda with one "
                         "or more explicitly-typed parameters",
                         "CAJETA_ERROR_TRANSFORM_UNSUPPORTED_BODY");
        }

        int64_t kSel = (int64_t) pnames.size();
        const auto& gTypeArgs = self->getExplicitMethodTypeArgs();
        if (!gTypeArgs.empty()) {
            auto c = std::dynamic_pointer_cast<CajetaConstantType>(gTypeArgs[0]);
            if (!c) {
                throw locErr("transform intrinsic 'GradAll<...>' arg-count selector "
                             "must be an integer constant",
                             "CAJETA_ERROR_TRANSFORM_UNSUPPORTED_BODY");
            }
            kSel = c->getValue();
        }
        if (kSel < 1 || kSel > (int64_t) pnames.size()) {
            throw locErr("transform intrinsic 'GradAll<" + std::to_string(kSel)
                         + ">' selects a leading-argument count out of range for "
                         "the " + std::to_string(pnames.size())
                         + "-parameter function (1 <= K <= arity)",
                         "CAJETA_ERROR_TRANSFORM_UNSUPPORTED_BODY");
        }

        for (int64_t i = 1; i < kSel; ++i) {
            if (ptypes[i]->toCanonical() != ptypes[0]->toCanonical()) {
                throw locErr("transform intrinsic 'GradAll<" + std::to_string(kSel)
                             + ">': the differentiated leading parameters must "
                             "share the same type (grads return as one array); got '"
                             + ptypes[0]->toCanonical() + "' and '"
                             + ptypes[i]->toCanonical() + "'",
                             "CAJETA_ERROR_TRANSFORM_UNSUPPORTED_BODY");
            }
        }

        lam->setExpectedType(fnType);
        lam->resolveTypes(module);
        auto bodyExpr = std::dynamic_pointer_cast<Expression>(lam->getBody());
        if (!bodyExpr) {
            throw locErr("transform intrinsic 'GradAll' v1 requires an "
                         "expression-body lambda",
                         "CAJETA_ERROR_TRANSFORM_UNSUPPORTED_BODY");
        }

        auto tensorElem = [](const std::string& ty, std::string& elemOut) -> bool {
            bool t = ty.rfind("cajeta.math.Tensor", 0) == 0
                  || ty.rfind("Tensor<", 0) == 0;
            elemOut = ty;
            if (t) {
                auto lt = ty.find('<');
                auto gt = ty.rfind('>');
                if (lt != std::string::npos && gt != std::string::npos && gt > lt + 1)
                    elemOut = ty.substr(lt + 1, gt - lt - 1);
            }
            return t;
        };
        std::map<std::string, bool> paramRank;
        for (size_t i = 0; i < pnames.size(); ++i) {
            std::string ignore;
            paramRank[pnames[i]] = tensorElem(ptypes[i]->toCanonical(), ignore);
        }
        std::string selTy = ptypes[0]->toCanonical();
        std::string elem;
        bool selIsTensor = tensorElem(selTy, elem);

        auto resolveCall = makeCallResolver(module);
        std::string outputVal;
        std::vector<std::string> gradExprs;
        gradExprs.reserve((size_t) kSel);
        for (int64_t k = 0; k < kSel; ++k) {
            GradPieces gp = differentiateBody(self, bodyExpr.get(), pnames,
                                              paramRank, (size_t) k, elem,
                                              selIsTensor, resolveCall);
            if (k == 0) outputVal = gp.valueExpr;
            gradExprs.push_back(gp.gradExpr);
        }

        std::string valueTy = fnType->getReturnType()
            ? fnType->getReturnType()->toCanonical() : std::string("void");
        if ((valueTy.empty() || valueTy == "void") && bodyExpr->getResolvedType()) {
            valueTy = bodyExpr->getResolvedType()->toCanonical();
        }
        if (valueTy.empty() || valueTy == "void") {
            if (auto sumCall = std::dynamic_pointer_cast<MethodCallExpression>(bodyExpr)) {
                if ((sumCall->getMethodCallName() == "sum"
                         || sumCall->getMethodCallName() == "mean")
                        && !sumCall->getExplicitMethodTypeArgs().empty()) {
                    auto r = sumCall->getExplicitMethodTypeArgs().back();
                    if (r) valueTy = r->toCanonical();
                }
            }
        }
        if (valueTy.empty() || valueTy == "void") {
            if (auto call = std::dynamic_pointer_cast<MethodCallExpression>(bodyExpr)) {
                std::string vrecv;
                if (!call->getChildren().empty()) {
                    if (auto* rid = dynamic_cast<IdentifierExpression*>(
                            call->getChildren()[0].get())) {
                        vrecv = rid->getTextValue();
                    }
                }
                auto t = resolveCall(vrecv, call->getMethodCallName(),
                                     call->getParameters().size());
                if (t.found && !t.returnTy.empty()) valueTy = t.returnTy;
            }
        }
        if ((valueTy.empty() || valueTy == "void") && !selIsTensor) {
            valueTy = selTy;
        }
        if (valueTy.empty() || valueTy == "void") {
            throw locErr("transform intrinsic 'GradAll': could not determine the "
                         "value type of the differentiated function",
                         "CAJETA_ERROR_TRANSFORM_UNSUPPORTED_BODY");
        }

        std::vector<std::string> paramTys;
        paramTys.reserve(pnames.size());
        bool importTensor = false;
        for (size_t i = 0; i < pnames.size(); ++i) {
            paramTys.push_back(ptypes[i]->toCanonical());
            importTensor = importTensor || paramRank[pnames[i]];
        }
        for (const auto& ge : gradExprs) {
            if (ge.find("Tensor.") != std::string::npos) importTensor = true;
        }
        if (outputVal.find("Tensor.") != std::string::npos) importTensor = true;

        std::string seed = outputVal;
        for (const auto& ge : gradExprs) seed += "|" + ge;
        std::string className = "__GradAllBwd_" + cajeta::synth::deriveSynthName(
            "gradall", fnType->toCanonical(), {seed, valueTy});
        std::string pkg = module->getQName()->getPackageName();
        std::string source = (pkg.empty() ? std::string() : ("package " + pkg + ";\n"))
            + cajeta::transform::emitGradAllSource(className, pnames, paramTys,
                                                   valueTy, selTy, outputVal,
                                                   gradExprs, importTensor);
        if (std::getenv("CAJETA_GRAD_DUMP")) {
            fprintf(stderr, "=== GRADALL BACKWARD SOURCE ===\n%s\n=== END ===\n",
                    source.c_str());
        }
        return synthesizeMakeClosure(self, module, className, source, "GradAll",
                                     outResolvedType);
    }

    // Synthesizes Vmap(f)'s batched form: builds f's forward DAG, requires a
    // batching rule for every primitive in it, then emits the helper whose make()
    // applies f's inlined body across the argument's leading axis.
    static llvm::Value* emitVmapBatched(MethodCallExpression* self,
                                        const std::shared_ptr<LambdaExpression>& lam,
                                        const std::shared_ptr<CajetaFunctionType>& fnType,
                                        CajetaModulePtr module,
                                        CajetaTypePtr& outResolvedType) {
        auto locErr = [&](const std::string& msg, const char* id) {
            return Exception(msg, id, "", self->getSourceLine(), self->getSourceColumn());
        };

        const auto& pnames = lam->getParamNames();
        const auto& ptypes = lam->getParamTypes();
        if (pnames.size() != 1 || ptypes.size() != 1) {
            throw locErr("transform intrinsic 'Vmap' v1 batches a single-parameter "
                         "function over one leading axis",
                         "CAJETA_ERROR_TRANSFORM_UNSUPPORTED_BODY");
        }

        lam->setExpectedType(fnType);
        lam->resolveTypes(module);
        auto bodyExpr = std::dynamic_pointer_cast<Expression>(lam->getBody());
        if (!bodyExpr) {
            throw locErr("transform intrinsic 'Vmap' v1 requires an expression-body "
                         "lambda", "CAJETA_ERROR_TRANSFORM_UNSUPPORTED_BODY");
        }

        std::map<std::string, bool> paramRank;
        paramRank[pnames[0]] = false;
        auto resolveCall = makeCallResolver(module);
        std::vector<cajeta::transform::AdNode> nodes;
        std::map<std::string, size_t> paramNodeIndex;
        std::string err;
        if (!cajeta::transform::buildDag(bodyExpr.get(), pnames, paramRank, resolveCall,
                                         nodes, paramNodeIndex, &err) || nodes.empty()) {
            throw locErr("transform intrinsic 'Vmap': no batching rule for this body — "
                         + (err.empty() ? std::string("empty body") : err),
                         "CAJETA_ERROR_TRANSFORM_NO_BATCH_RULE");
        }
        for (const auto& n : nodes) {
            if (!n.primitive.empty()
                    && !cajeta::transform::hasBatchRule(n.primitive)) {
                throw locErr("transform intrinsic 'Vmap': no batching rule for "
                             "primitive '" + n.primitive + "'",
                             "CAJETA_ERROR_TRANSFORM_NO_BATCH_RULE");
            }
        }

        std::string paramTy = ptypes[0]->toCanonical();
        std::string resultTy = fnType->getReturnType()
            ? fnType->getReturnType()->toCanonical() : std::string("void");
        if ((resultTy.empty() || resultTy == "void") && bodyExpr->getResolvedType()) {
            resultTy = bodyExpr->getResolvedType()->toCanonical();
        }
        if (resultTy.empty() || resultTy == "void") {
            throw locErr("transform intrinsic 'Vmap': could not determine the batch "
                         "element type", "CAJETA_ERROR_TRANSFORM_UNSUPPORTED_BODY");
        }

        const std::string& bodySrc = nodes.back().valueExpr;
        std::string className = "__VmapBatch_" + cajeta::synth::deriveSynthName(
            "vmap", pnames[0], {bodySrc, resultTy});
        std::string pkg = module->getQName()->getPackageName();
        std::string source = (pkg.empty() ? std::string() : ("package " + pkg + ";\n"))
            + cajeta::transform::emitBatchedSource(className, pnames[0], paramTy,
                                                   resultTy, bodySrc);
        if (std::getenv("CAJETA_VMAP_DUMP")) {
            fprintf(stderr, "=== VMAP BATCHED SOURCE ===\n%s\n=== END ===\n",
                    source.c_str());
        }
        return synthesizeMakeClosure(self, module, className, source, "Vmap",
                                     outResolvedType);
    }

    // Vmap(Grad(f)): batches the differentiated form for per-example gradients.
    // Intercepted before the argument resolves - `Grad(f)` has no function type
    // until it is itself transformed - so the per-example body IS Grad's result.
    static llvm::Value* emitVmapOfGrad(MethodCallExpression* self,
                                       const std::shared_ptr<MethodCallExpression>& innerGrad,
                                       CajetaModulePtr module,
                                       CajetaTypePtr& outResolvedType) {
        auto locErr = [&](const std::string& msg, const char* id) {
            return Exception(msg, id, "", self->getSourceLine(), self->getSourceColumn());
        };

        if (innerGrad->getParameters().size() != 1) {
            throw locErr("transform intrinsic 'Vmap(Grad(f))' requires a single-argument "
                         "Grad", "CAJETA_ERROR_TRANSFORM_UNSUPPORTED_BODY");
        }
        auto lam = std::dynamic_pointer_cast<LambdaExpression>(
            innerGrad->getParameters()[0].expression);
        if (!lam) {
            throw locErr("transform intrinsic 'Vmap(Grad(f))' requires a lambda literal",
                         "CAJETA_ERROR_TRANSFORM_NOT_SPECIALIZABLE");
        }
        lam->resolveTypes(module);
        auto fnType = std::dynamic_pointer_cast<CajetaFunctionType>(lam->getResolvedType());
        if (!fnType) {
            throw locErr("transform intrinsic 'Vmap(Grad(f))' could not resolve f's "
                         "function type", "CAJETA_ERROR_TRANSFORM_NOT_FUNCTION");
        }
        const auto& pnames = lam->getParamNames();
        const auto& ptypes = lam->getParamTypes();
        if (pnames.size() != 1 || ptypes.size() != 1) {
            throw locErr("transform intrinsic 'Vmap(Grad(f))' v1 batches a single-"
                         "parameter function", "CAJETA_ERROR_TRANSFORM_UNSUPPORTED_BODY");
        }
        auto bodyExpr = std::dynamic_pointer_cast<Expression>(lam->getBody());
        if (!bodyExpr) {
            throw locErr("transform intrinsic 'Vmap(Grad(f))' v1 requires an "
                         "expression-body lambda", "CAJETA_ERROR_TRANSFORM_UNSUPPORTED_BODY");
        }

        std::string paramTy = ptypes[0]->toCanonical();
        if (paramTy.rfind("cajeta.math.Tensor", 0) == 0 || paramTy.rfind("Tensor<", 0) == 0) {
            throw locErr("transform intrinsic 'Vmap(Grad(f))' is scalar-only in v1 "
                         "(tensor batching needs per-op rules)",
                         "CAJETA_ERROR_TRANSFORM_NO_BATCH_RULE");
        }

        std::map<std::string, bool> paramRank;
        paramRank[pnames[0]] = false;
        auto resolveCall = makeCallResolver(module);
        GradPieces gp = differentiateBody(self, bodyExpr.get(), pnames, paramRank,
                                          0, paramTy, false, resolveCall);

        std::string valueTy = bodyExpr->getResolvedType()
            ? bodyExpr->getResolvedType()->toCanonical() : paramTy;
        std::string resultTy = "GradResult<" + valueTy + "," + paramTy + ">";
        std::string bodySrc = "stack " + resultTy + "(" + gp.valueExpr + ", "
                            + gp.gradExpr + ")";

        std::string className = "__VmapBatch_" + cajeta::synth::deriveSynthName(
            "vmapgrad", pnames[0], {gp.valueExpr, gp.gradExpr});
        std::string pkg = module->getQName()->getPackageName();
        std::string source = (pkg.empty() ? std::string() : ("package " + pkg + ";\n"))
            + cajeta::transform::emitBatchedSource(className, pnames[0], paramTy,
                                                   resultTy, bodySrc, true);
        if (std::getenv("CAJETA_VMAP_DUMP")) {
            fprintf(stderr, "=== VMAP(GRAD) SOURCE ===\n%s\n=== END ===\n", source.c_str());
        }
        return synthesizeMakeClosure(self, module, className, source, "Vmap",
                                     outResolvedType);
    }

    // `Fuse(f)` over an elementwise tensor expression: lowers every DAG node to
    // element-level scalar source and synthesizes one loop allocating a single
    // result tensor. Building runs nothing; calling the returned function does.
    static llvm::Value* emitFusedExpr(MethodCallExpression* self,
                                      const std::shared_ptr<LambdaExpression>& lam,
                                      const std::shared_ptr<CajetaFunctionType>& fnType,
                                      CajetaModulePtr module,
                                      CajetaTypePtr& outResolvedType) {
        auto locErr = [&](const std::string& msg, const char* id) {
            return Exception(msg, id, "", self->getSourceLine(), self->getSourceColumn());
        };

        const auto& pnames = lam->getParamNames();
        const auto& ptypes = lam->getParamTypes();
        if (pnames.size() != 1 || ptypes.size() != 1) {
            throw locErr("transform intrinsic 'Fuse' v1 fuses a single-parameter "
                         "tensor expression",
                         "CAJETA_ERROR_TRANSFORM_UNSUPPORTED_BODY");
        }
        std::string paramTy = ptypes[0]->toCanonical();
        std::string elem;
        {
            auto lt = paramTy.find('<');
            auto gt = paramTy.rfind('>');
            bool isTensor = paramTy.rfind("cajeta.math.Tensor", 0) == 0
                         || paramTy.rfind("Tensor<", 0) == 0;
            if (!isTensor || lt == std::string::npos || gt == std::string::npos) {
                throw locErr("transform intrinsic 'Fuse' v1 requires a Tensor<E> "
                             "parameter", "CAJETA_ERROR_TRANSFORM_UNSUPPORTED_BODY");
            }
            elem = paramTy.substr(lt + 1, gt - lt - 1);
        }

        lam->setExpectedType(fnType);
        lam->resolveTypes(module);
        auto bodyExpr = std::dynamic_pointer_cast<Expression>(lam->getBody());
        if (!bodyExpr) {
            throw locErr("transform intrinsic 'Fuse' v1 requires an expression-body "
                         "lambda", "CAJETA_ERROR_TRANSFORM_UNSUPPORTED_BODY");
        }

        std::map<std::string, bool> paramRank;
        paramRank[pnames[0]] = true;
        auto resolveCall = makeCallResolver(module);
        std::vector<cajeta::transform::AdNode> nodes;
        std::map<std::string, size_t> paramNodeIndex;
        std::string err;
        if (!cajeta::transform::buildDag(bodyExpr.get(), pnames, paramRank, resolveCall,
                                         nodes, paramNodeIndex, &err) || nodes.empty()) {
            throw locErr("transform intrinsic 'Fuse': cannot fuse this body — " + err,
                         "CAJETA_ERROR_TRANSFORM_NOT_FUSIBLE");
        }
        const cajeta::transform::AdNode& root = nodes.back();
        bool reductionRoot = cajeta::transform::isReduction(root.primitive);
        size_t bodyIdx = reductionRoot ? root.operands[0] : nodes.size() - 1;

        std::string elemErr;
        std::vector<cajeta::transform::Hoist> hoists;
        std::string elemSrc = cajeta::transform::elementExpr(
            nodes, bodyIdx, "__i", &hoists, &elemErr);
        if (elemSrc.empty()) {
            throw locErr("transform intrinsic 'Fuse': cannot fuse this body — "
                         + elemErr, "CAJETA_ERROR_TRANSFORM_NOT_FUSIBLE");
        }
        if (reductionRoot && root.primitive == "std") {
            throw locErr("transform intrinsic 'Fuse': 'std' as the whole fused "
                         "expression is not staged in v1 (use it inside an "
                         "elementwise body)",
                         "CAJETA_ERROR_TRANSFORM_NOT_FUSIBLE");
        }

        std::string className = "__Fused_" + cajeta::synth::deriveSynthName(
            "fuse", pnames[0], {elemSrc, elem, root.primitive});
        std::string pkg = module->getQName()->getPackageName();
        std::string source = (pkg.empty() ? std::string() : ("package " + pkg + ";\n"))
            + (reductionRoot
                ? cajeta::transform::emitFusedReductionSource(
                      className, pnames[0], elem, root.primitive, elemSrc, hoists)
                : cajeta::transform::emitFusedSource(
                      className, pnames[0], elem, elemSrc, hoists));
        if (std::getenv("CAJETA_FUSE_DUMP")) {
            fprintf(stderr, "=== FUSED SOURCE ===\n%s\n=== END ===\n", source.c_str());
        }
        return synthesizeMakeClosure(self, module, className, source, "Fuse",
                                     outResolvedType);
    }

    // Annotation sugar: a call to a static method carrying @Grad/@Vmap/@Jit
    // desugars to the nested combinator form (last written applied first) and runs
    // the same recognizer driver, so composition and error shapes are identical.
    static llvm::Value* emitTransformAnnotatedCall(
            MethodCallExpression* self, MethodPtr m,
            const std::vector<std::string>& chain,
            CajetaModulePtr module, CajetaTypePtr& outResolvedType) {
        auto locErr = [&](const std::string& msg, const char* id) {
            return Exception(msg, id, "", self->getSourceLine(), self->getSourceColumn());
        };
        Expression* bodyRaw = findReturnExpr(m->getBlock());
        if (!m->isStatic() || !bodyRaw) {
            throw locErr("transform annotations (@Grad/@Vmap/@Jit) require a "
                         "static single-return-expression method",
                         "CAJETA_ERROR_TRANSFORM_UNSUPPORTED_BODY");
        }
        std::vector<std::string> pnames;
        std::vector<CajetaTypePtr> ptypes;
        for (auto& p : m->getParameterList()) {
            pnames.push_back(p->getName());
            ptypes.push_back(p->getType());
        }
        AbstractSyntaxNodePtr bodyAlias(AbstractSyntaxNodePtr(), bodyRaw);
        auto lam = std::make_shared<LambdaExpression>(
            nullptr, pnames, ptypes, bodyAlias);
        lam->setSourceSpan(self->getSourceLine(), self->getSourceColumn());
        lam->setExpectedType(std::make_shared<CajetaFunctionType>(
            module, ptypes, m->getReturnType()));

        ExpressionPtr cur = lam;
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            MethodCallParameter p;
            p.expression = cur;
            auto node = std::make_shared<MethodCallExpression>(
                *it, std::vector<MethodCallParameter>{p});
            node->setSourceSpan(self->getSourceLine(), self->getSourceColumn());
            cur = node;
        }
        llvm::Value* closure = cur->generateCode(module);
        auto fnT = std::dynamic_pointer_cast<CajetaFunctionType>(
            cur->getResolvedType());
        if (!closure || !fnT) {
            throw locErr("transform annotations on '" + m->getName()
                         + "' produced no callable transform",
                         "CAJETA_ERROR_TRANSFORM_SYNTH_FAILED");
        }
        llvm::Value* closurePtr = closure;
        if (!closure->getType()->isPointerTy()) {
            auto* builder = module->getBuilder();
            llvm::Function* curFn = builder->GetInsertBlock()->getParent();
            llvm::IRBuilder<> eb(&curFn->getEntryBlock(),
                                 curFn->getEntryBlock().begin());
            llvm::Value* slot = eb.CreateAlloca(closure->getType(), nullptr,
                                                "sugar.closure");
            builder->CreateStore(closure, slot);
            closurePtr = slot;
        }
        return emitClosureCall(module, closurePtr, fnT, self->getParameters(),
                               outResolvedType);
    }

    // Higher-order Grad(Grad(...(f))): differentiates symbolically `depth` times by
    // re-parsing each pass's grad source, so the result carries {f^(d-1), f^(d)}.
    // v1 restricts this to a single scalar parameter.
    static llvm::Value* emitNestedGrad(MethodCallExpression* self,
                                       CajetaModulePtr module,
                                       CajetaTypePtr& outResolvedType) {
        auto locErr = [&](const std::string& msg, const char* id) {
            return Exception(msg, id, "", self->getSourceLine(), self->getSourceColumn());
        };

        size_t depth = 1;
        auto cur = std::dynamic_pointer_cast<Expression>(self->getParameters()[0].expression);
        std::shared_ptr<LambdaExpression> baseLam;
        while (cur) {
            if (auto lam = std::dynamic_pointer_cast<LambdaExpression>(cur)) {
                baseLam = lam; break;
            }
            auto mc = std::dynamic_pointer_cast<MethodCallExpression>(cur);
            if (!mc || mc->getMethodCallName() != "Grad"
                    || mc->getParameters().size() != 1) {
                break;
            }
            ++depth;
            cur = std::dynamic_pointer_cast<Expression>(mc->getParameters()[0].expression);
        }
        if (!baseLam) {
            throw locErr("transform intrinsic 'Grad' requires a lambda literal whose "
                         "body can be differentiated",
                         "CAJETA_ERROR_TRANSFORM_NOT_SPECIALIZABLE");
        }

        const auto& pnames = baseLam->getParamNames();
        const auto& ptypes = baseLam->getParamTypes();
        if (pnames.size() != 1 || ptypes.size() != 1 || !ptypes[0]) {
            throw locErr("higher-order Grad(Grad(...)) differentiates a single scalar "
                         "parameter in v1", "CAJETA_ERROR_TRANSFORM_UNSUPPORTED_BODY");
        }
        std::string selTy = ptypes[0]->toCanonical();
        if (selTy.rfind("cajeta.math.Tensor", 0) == 0 || selTy.rfind("Tensor<", 0) == 0) {
            throw locErr("higher-order Grad(Grad(...)) is scalar-only in v1 (tensor "
                         "second-order needs broadcast-op VJP rules)",
                         "CAJETA_ERROR_TRANSFORM_UNSUPPORTED_BODY");
        }
        std::map<std::string, bool> paramRank{{pnames[0], false}};

        auto baseBody = std::dynamic_pointer_cast<Expression>(baseLam->getBody());
        if (!baseBody) {
            throw locErr("transform intrinsic 'Grad' requires an expression-body lambda",
                         "CAJETA_ERROR_TRANSFORM_UNSUPPORTED_BODY");
        }

        auto resolveCall = makeCallResolver(module);
        std::vector<std::string> deriv;
        GradPieces p = differentiateBody(self, baseBody.get(), pnames, paramRank,
                                         0, selTy, false, resolveCall);
        deriv.push_back(p.valueExpr);
        deriv.push_back(p.gradExpr);
        for (size_t k = 2; k <= depth; ++k) {
            auto* exprCtx = cajeta::synth::parseExpressionFragment(deriv.back());
            auto parsed = Expression::fromContext(exprCtx);
            if (!parsed) {
                throw locErr("higher-order Grad: could not re-parse the intermediate "
                             "gradient", "CAJETA_ERROR_TRANSFORM_SYNTH_FAILED");
            }
            GradPieces pk = differentiateBody(self, parsed.get(), pnames, paramRank,
                                              0, selTy, false, resolveCall);
            deriv.push_back(pk.gradExpr);
        }

        std::string outputVal = deriv[depth - 1];
        std::string gradExpr = deriv[depth];
        std::string classSeed = "gradgrad" + std::to_string(depth) + ":" + selTy;
        return emitBackwardClosure(self, module, classSeed, pnames, {selTy},
                                   selTy, selTy, outputVal, gradExpr,
                                   /*importTensor*/ false, outResolvedType);
    }

    CajetaClassPtr MethodCallExpression::resolveReceiverClassShallow(
            const std::shared_ptr<MethodCallExpression>& call,
            CajetaModulePtr module, bool allowSuper) {
        CajetaClassPtr recv;
        auto& kids = call->getChildren();
        if (kids.empty()) {
            if (!module->getStructureStack().empty()) {
                recv = module->getStructureStack().back();
            }
        } else {
            auto recvExpr = std::dynamic_pointer_cast<Expression>(kids.front());
            if (auto id = std::dynamic_pointer_cast<IdentifierExpression>(recvExpr)) {
                if (id->getTextValue() == "this") {
                    if (!module->getStructureStack().empty()) {
                        recv = module->getStructureStack().back();
                    }
                } else if (allowSuper && id->getTextValue() == "super") {
                    if (!module->getStructureStack().empty()) {
                        auto& supers =
                            module->getStructureStack().back()->getSuperClasses();
                        if (!supers.empty()) recv = supers.front();
                    }
                } else {
                    recv = std::dynamic_pointer_cast<CajetaClass>(
                        CajetaType::resolveNamed(
                            QualifiedName::getOrCreate(id->getTextValue()),
                            module));
                }
            }
            if (!recv && recvExpr) {
                if (!recvExpr->getResolvedType()) {
                    try { recvExpr->resolveTypes(module); } catch (...) { }
                }
                recv = std::dynamic_pointer_cast<CajetaClass>(
                    recvExpr->getResolvedType());
            }
        }
        return recv;
    }

    MethodPtr MethodCallExpression::resolveCalleeByArgTypes(CajetaModulePtr module) {
        auto self = std::dynamic_pointer_cast<MethodCallExpression>(shared_from_this());
        if (!self || !module) return nullptr;
        CajetaClassPtr recv = resolveReceiverClassShallow(self, module,
                                                          /*allowSuper=*/true);
        if (!recv) return nullptr;

        // A static generic call on an OPEN template: it holds no Method objects, so the
        // candidate walk below can never match - materialize the instantiation first.
        if (recv->isTemplate() && !explicitMethodTypeArgs.empty()
                && recv->getTypeParameters().size()
                       == explicitMethodTypeArgs.size()) {
            bool allBound = true;
            for (auto& a : explicitMethodTypeArgs) if (!a) allBound = false;
            if (allBound) {
                try {
                    if (auto inst = std::dynamic_pointer_cast<CajetaClass>(
                            recv->instantiate(explicitMethodTypeArgs)))
                        recv = inst;
                } catch (...) { /* keep the open template: no candidates */ }
            }
        }

        std::vector<MethodPtr> candidates;
        std::set<std::string> seenSignatures;
        std::set<CajetaClass*> visited;
        std::vector<CajetaClassPtr> queue{recv};
        while (!queue.empty()) {
            CajetaClassPtr k = queue.front();
            queue.erase(queue.begin());
            if (!k || !visited.insert(k.get()).second) continue;
            for (auto& mm : k->getMethodList()) {
                if (!mm || mm->getName() != methodCallName) continue;
                auto pl = mm->getParameterList();
                size_t off = (!pl.empty() && pl.front()->getName() == "this") ? 1 : 0;
                if (pl.size() - off != parameters.size()) continue;
                // Key on the parameter types, not the owner, so an override does not also
                // admit the method it overrides.
                std::string sig;
                for (size_t i = off; i < pl.size(); i++)
                    sig += (pl[i] && pl[i]->getType())
                         ? pl[i]->getType()->toCanonical() + "," : "?,";
                if (!seenSignatures.insert(sig).second) continue;
                candidates.push_back(mm);
            }
            for (auto& parent : k->getSuperClasses()) queue.push_back(parent);
        }

        if (candidates.size() == 1) return candidates.front();
        if (candidates.empty()) return nullptr;

        std::vector<std::string> argKeys;
        for (auto& p : parameters) {
            if (!p.expression) return nullptr;
            auto t = p.expression->getResolvedType();
            if (!t) return nullptr;
            argKeys.push_back(t->toCanonical());
        }

        MethodPtr match;
        for (auto& mm : candidates) {
            auto pl = mm->getParameterList();
            size_t off = (!pl.empty() && pl.front()->getName() == "this") ? 1 : 0;
            bool allMatch = true;
            for (size_t i = 0; i < argKeys.size(); i++) {
                auto ft = pl[i + off] ? pl[i + off]->getType() : nullptr;
                if (!ft || ft->toCanonical() != argKeys[i]) { allMatch = false; break; }
            }
            if (!allMatch) continue;
            if (match) return nullptr;
            match = mm;
        }
        return match;
    }

    MethodPtr MethodCallExpression::resolveArgCalleeShallow(
            const std::shared_ptr<MethodCallExpression>& call,
            CajetaModulePtr module) {
        if (!call || !module) return nullptr;
        if (auto rm = call->getResolvedMethod()) return rm;
        CajetaClassPtr recv = resolveReceiverClassShallow(call, module);
        if (!recv) return nullptr;
        MethodPtr match;
        for (auto& mm : recv->getMethodList()) {
            if (!mm || mm->getName() != call->getMethodCallName()) continue;
            auto pl = mm->getParameterList();
            size_t formalCount = pl.size();
            if (!pl.empty() && pl.front()->getName() == "this") formalCount--;
            if (formalCount != call->getParameters().size()) continue;
            if (match) return nullptr;
            match = mm;
        }
        return match;
    }

    void rejectTransferOfBorrowArgs(CajetaModulePtr module,
                                    const vector<MethodCallParameter>& args) {
        auto scope = module->getScopeStack().peek();
        if (!scope) return;
        for (auto& p : args) {
            if (!p.callerTransferred) continue;
            // Only a bare `#name` is decidable here: `#a.b` and `#f()` need path-based
            // provenance, and the check must never block valid code, so they are left be.
            auto id = std::dynamic_pointer_cast<IdentifierExpression>(
                p.expression);
            if (!id) continue;
            scope->rejectTransferOfBorrow(id->getTextValue());
        }
    }

    CajetaTypePtr MethodCallExpression::rebindMethodTypeArgs(
            const CajetaTypePtr& declared, const MethodPtr& callee) {
        if (!declared || !callee) return declared;
        if (explicitMethodTypeArgs.empty()) return declared;
        const auto& tps = callee->getMethodTypeParameters();
        if (tps.size() != explicitMethodTypeArgs.size()) return declared;

        if (declared->getQName()) {
            const std::string& n = declared->getQName()->getTypeName();
            for (size_t i = 0; i < tps.size(); i++)
                if (n == tps[i].name && explicitMethodTypeArgs[i])
                    return explicitMethodTypeArgs[i];
        }

        if (auto rk = std::dynamic_pointer_cast<CajetaClass>(declared)) {
            if (rk->isTemplate()
                    && rk->getTypeParameters().size() == tps.size()) {
                for (auto& a : explicitMethodTypeArgs) if (!a) return declared;
                try {
                    if (auto inst = rk->instantiate(explicitMethodTypeArgs))
                        return inst;
                } catch (...) { /* fall through: keep the declared type */ }
            }
        }
        return declared;
    }

    // Is `name` a bare @Kernel of the class being generated? The receiver of a
    // launch must be a bare kernel name in the launching class (XPU-N02), and
    // `k.manifest()` follows the same rule.
    static MethodPtr findKernelByBareName(const CajetaModulePtr& module,
                                          const std::string& name) {
        CajetaClassPtr klass;
        if (!module->getStructureStack().empty())
            klass = module->getStructureStack().back();
        if (!klass && module->getCurrentMethod())
            klass = module->getCurrentMethod()->getParent();
        if (!klass) return nullptr;
        for (auto& [k, m] : klass->getMethods())
            if (m && m->getName() == name && cajeta::xpu::isKernel(*m)) return m;
        return nullptr;
    }

    void MethodCallExpression::resolveTypes(CajetaModulePtr module) {
        // LINT ONLY. In a build this must walk children and nothing else: resolving a
        // call's ARGUMENTS during the type pass pins them before template substitution
        // runs, which breaks codegen. The build records its edges from generateCode.
        if (!module || !module->isResolutionOnly()) {
            AbstractSyntaxNode::resolveTypes(module);
            return;
        }

        xref::CallSiteScope xrefSite(getSourceFile(), nameLine, nameColumn);

        for (auto& child : children) {
            if (!child) continue;
            try { child->resolveTypes(module); } catch (...) { }
        }

        if (methodCallName == "manifest" && parameters.empty()
                && children.size() == 1) {
            if (auto recvId = std::dynamic_pointer_cast<IdentifierExpression>(children[0])) {
                if (findKernelByBareName(module, recvId->getTextValue())) {
                    try {
                        resolvedType = CajetaType::of("KernelManifest", "cajeta.xpu");
                    } catch (...) { }
                    return;
                }
            }
        }

        if (methodCallName == "stream" && parameters.empty()
                && !children.empty()) {
            auto recvExpr = std::dynamic_pointer_cast<Expression>(children.front());
            if (recvExpr) {
                if (auto arrayType = std::dynamic_pointer_cast<CajetaArray>(
                        recvExpr->getResolvedType())) {
                    try {
                        auto streamKlass = std::dynamic_pointer_cast<CajetaClass>(
                            CajetaType::of("ArrayStream", "cajeta.lang.stream"));
                        if (streamKlass && streamKlass->isTemplate()) {
                            auto instantiated = std::dynamic_pointer_cast<CajetaClass>(
                                streamKlass->instantiate(
                                    {arrayType->getElementType()}));
                            if (instantiated) {
                                MethodPtr ctor;
                                for (auto& [_, mm] : instantiated->getMethods()) {
                                    if (!mm || !mm->isConstructor()) continue;
                                    auto pl = mm->getParameterList();
                                    size_t off = (!pl.empty()
                                        && pl.front()->getName() == "this") ? 1 : 0;
                                    if (pl.size() - off != 2) continue;
                                    if (ctor) { ctor = nullptr; break; }
                                    ctor = mm;
                                }
                                if (ctor) CajetaClass::noteResolvedCallXref(
                                    ctor, /*isConstructor=*/true, module);
                                resolvedType = instantiated;
                                return;
                            }
                        }
                    } catch (...) { /* unresolved intrinsic: no edge */ }
                }
            }
        }

        // The CALLEE resolves BEFORE the arguments: a lambda argument's parameters take
        // their types from the enclosing call's formal, so the callee must be known.
        MethodPtr callee;
        try {
            callee = resolveArgCalleeShallow(
                std::dynamic_pointer_cast<MethodCallExpression>(shared_from_this()),
                module);
            if (!callee) callee = resolveCalleeByArgTypes(module);
        } catch (...) { callee = nullptr; }

        if (callee) {
            auto pl = callee->getParameterList();
            size_t off = (!pl.empty() && pl.front()->getName() == "this") ? 1 : 0;
            for (size_t i = 0; i < parameters.size() && i + off < pl.size(); i++) {
                auto lam = std::dynamic_pointer_cast<LambdaExpression>(
                    parameters[i].expression);
                if (lam && pl[i + off] && pl[i + off]->getType())
                    lam->setExpectedType(pl[i + off]->getType());
            }
            if (!resolvedType && callee->getReturnType())
                resolvedType = rebindMethodTypeArgs(callee->getReturnType(), callee);
        }

        for (auto& p : parameters) {
            if (!p.expression) continue;
            try { p.expression->resolveTypes(module); } catch (...) { }
        }

        if (!xref::captureEnabled()) return;

        if (!callee) {
            try { callee = resolveCalleeByArgTypes(module); } catch (...) { return; }
            if (callee && !resolvedType && callee->getReturnType())
                resolvedType = rebindMethodTypeArgs(callee->getReturnType(), callee);
        }
        if (!callee) return;

        CajetaClass::noteResolvedCallXref(callee, /*isConstructor=*/false, module);
    }

    // Emits this call. Runs the intrinsic interceptors first (kernel, reflection,
    // System / File / net / process / Math / SIMD families), then ordinary receiver
    // resolution, argument lowering, the ownership transfer word, and dispatch.
    llvm::Value* MethodCallExpression::generateCode(CajetaModulePtr module) {
        codegenRan = true;
        // Opens this call site for its codegen: CajetaClass::resolveMethod attributes
        // what it resolves to the innermost open site. The file comes from THIS NODE -
        // a stdlib body generated under a user module must not blame the user's file.
        xref::CallSiteScope xrefSite(getSourceFile(), nameLine, nameColumn);

        auto* builder = module->getBuilder();
        llvm::LLVMContext& llvmCtx = *module->getLlvmContext();
        std::vector<llvm::Value*> argTitleFlags(parameters.size(), nullptr);
        std::vector<ownership::ArgTitle> argTitles(parameters.size());
        flaggedTitleValue = nullptr;

        // Rejects `#borrow` at an argument before any IR is emitted; sited here because
        // the title-flag loop below sits past several specialized early-return paths.
        rejectTransferOfBorrowArgs(module, parameters);

        // ----- xpu-tile-manifest 12.1: `k.manifest()` on a bare @Kernel name -----
        if (methodCallName == "manifest" && parameters.empty()
                && children.size() == 1 && explicitMethodTypeArgs.empty()) {
            if (auto recvId = std::dynamic_pointer_cast<IdentifierExpression>(children[0])) {
                const std::string kernelName = recvId->getTextValue();
                if (findKernelByBareName(module, kernelName)) {
                    auto kmClass = std::dynamic_pointer_cast<CajetaClass>(
                        CajetaType::of("KernelManifest", "cajeta.xpu"));
                    if (!kmClass) {
                        throw Exception(
                            "cajeta.xpu.KernelManifest is not available; cannot "
                            "lower '" + kernelName + ".manifest()'",
                            "XPU-N02");
                    }
                    TextLiteralExpression nameLit("\"" + kernelName + "\"",
                                                  LITERAL_TYPE_STRING);
                    llvm::Value* nameStr = nameLit.generateCode(module);
                    std::vector<ParameterEntry> entries;
                    entries.push_back(
                        ParameterEntry(CajetaType::of("String"), "", nameStr));
                    std::string ofName = "of";
                    llvm::Value* record = kmClass->invokeMethod(
                        ofName, entries, /*isConstructor=*/false,
                        /*thisInstance=*/nullptr, module);
                    resolvedType = kmClass;
                    return record;
                }
            }
        }

        // ----- tryAs<T>() intrinsic (reified capture -> Optional<T>) -----
        if (methodCallName == "tryAs"
                && explicitMethodTypeArgs.size() == 1
                && explicitMethodTypeArgs[0]
                && parameters.empty()
                && !children.empty()) {
            auto recvExpr = std::dynamic_pointer_cast<Expression>(children[0]);
            if (recvExpr) {
                if (!recvExpr->getResolvedType()) recvExpr->resolveTypes(module);
                llvm::Value* raw = children[0]->generateCode(module);
                llvm::Value* objPtr = loadIfLValue(module, raw, recvExpr);
                CajetaTypePtr targetType = explicitMethodTypeArgs[0];
                auto optTmpl = std::dynamic_pointer_cast<CajetaClass>(
                    CajetaType::of("Optional", "cajeta.lang"));
                if (objPtr && objPtr->getType()->isPointerTy()
                        && optTmpl && optTmpl->isTemplate()) {
                    auto optInst = std::dynamic_pointer_cast<CajetaClass>(
                        optTmpl->instantiate({targetType}));
                    if (optInst) {
                        llvm::PointerType* ptrTy =
                            llvm::PointerType::get(llvmCtx, 0);
                        llvm::Value* nullPtr =
                            llvm::ConstantPointerNull::get(ptrTy);
                        llvm::Value* matchBit = llvm::ConstantInt::getFalse(
                            llvm::Type::getInt1Ty(llvmCtx));
                        if (llvm::Function* fn = module->getRuntimeFunction(
                                "__cajeta_instanceof_named")) {
                            llvm::Value* namePtr = builder->CreateGlobalString(
                                targetType->toCanonical(), "tryas.target");
                            llvm::Value* r = builder->CreateCall(
                                fn, {objPtr, namePtr}, "tryas.match");
                            matchBit = builder->CreateICmpNE(
                                r, llvm::ConstantInt::get(r->getType(), 0));
                        }
                        llvm::Value* value = builder->CreateSelect(
                            matchBit, objPtr, nullPtr, "tryas.val");
                        CajetaTypePtr boolTy = CajetaType::of("boolean");
                        std::vector<ParameterEntry> entries;
                        entries.push_back(ParameterEntry(boolTy, "", matchBit));
                        entries.push_back(ParameterEntry(targetType, "", value));
                        llvm::Value* opt =
                            optInst->heapConstruct(module, entries);
                        resolvedType = optInst;
                        return opt;
                    }
                }
            }
        }

        // ----- transform-intrinsics U1: Grad/Jit/Vmap/Pmap combinator recognition -----
        if (children.empty() && parameters.size() == 1
                && (methodCallName == "Grad" || methodCallName == "GradAll"
                    || methodCallName == "Jit"
                    || methodCallName == "Vmap" || methodCallName == "Pmap"
                    || methodCallName == "Fuse")) {
            if (methodCallName == "Grad") {
                if (auto innerGrad = std::dynamic_pointer_cast<MethodCallExpression>(
                        parameters[0].expression)) {
                    if (innerGrad->getMethodCallName() == "Grad") {
                        CajetaTypePtr gradType;
                        llvm::Value* g = emitNestedGrad(this, module, gradType);
                        resolvedType = gradType;
                        return g;
                    }
                    if (innerGrad->getMethodCallName() == "Fuse"
                            && innerGrad->getParameters().size() == 1) {
                        auto lam = std::dynamic_pointer_cast<LambdaExpression>(
                            innerGrad->getParameters()[0].expression);
                        if (!lam) {
                            throw Exception(
                                "transform intrinsic 'Grad(Fuse(f))' requires a "
                                "lambda literal whose body can be differentiated",
                                "CAJETA_ERROR_TRANSFORM_NOT_SPECIALIZABLE",
                                "", getSourceLine(), getSourceColumn());
                        }
                        lam->resolveTypes(module);
                        auto fnType = std::dynamic_pointer_cast<CajetaFunctionType>(
                            lam->getResolvedType());
                        if (!fnType) {
                            throw Exception(
                                "transform intrinsic 'Grad(Fuse(f))' could not "
                                "resolve f's function type",
                                "CAJETA_ERROR_TRANSFORM_NOT_FUNCTION",
                                "", getSourceLine(), getSourceColumn());
                        }
                        CajetaTypePtr gradType;
                        llvm::Value* g = emitGradBackward(this, lam, fnType,
                                                          module, gradType);
                        resolvedType = gradType;
                        return g;
                    }
                }
            }
            if (methodCallName == "Jit") {
                if (auto innerT = std::dynamic_pointer_cast<MethodCallExpression>(
                        parameters[0].expression)) {
                    const std::string& n = innerT->getMethodCallName();
                    if (n == "Vmap" || n == "Grad" || n == "GradAll") {
                        std::set<const llvm::Function*> before;
                        for (auto& fn : module->getLlvmModule()->functions())
                            before.insert(&fn);
                        llvm::Value* inner = innerT->generateCode(module);
                        for (auto& fn : module->getLlvmModule()->functions()) {
                            if (fn.isDeclaration() || before.count(&fn)) continue;
                            llvm::StringRef n = fn.getName();
                            if (!n.contains("__GradBwd_")
                                    && !n.contains("__GradAllBwd_")
                                    && !n.contains("__VmapBatch_")
                                    && !n.contains("__Fused_")
                                    && !n.contains("__cajeta_lambda_"))
                                continue;
                            cajeta::fuseFunction(fn, nullptr);
                            fn.setName("__JitFused_" + fn.getName().str());
                        }
                        resolvedType = innerT->getResolvedType();
                        return inner;
                    }
                }
            }
            if (methodCallName == "Vmap") {
                if (auto innerGrad = std::dynamic_pointer_cast<MethodCallExpression>(
                        parameters[0].expression)) {
                    if (innerGrad->getMethodCallName() == "Grad") {
                        CajetaTypePtr vgType;
                        llvm::Value* v = emitVmapOfGrad(this, innerGrad, module, vgType);
                        resolvedType = vgType;
                        return v;
                    }
                }
            }
            if (auto argExpr = dynamic_pointer_cast<Expression>(parameters[0].expression)) {
                if (!argExpr->getResolvedType()) argExpr->resolveTypes(module);
                auto fnType = dynamic_pointer_cast<CajetaFunctionType>(
                    argExpr->getResolvedType());
                if (!fnType) {
                    throw Exception(
                        "transform intrinsic '" + methodCallName
                        + "' requires a function argument",
                        "CAJETA_ERROR_TRANSFORM_NOT_FUNCTION",
                        "", getSourceLine(), getSourceColumn());
                }
                if (methodCallName == "Grad") {
                    auto lam = std::dynamic_pointer_cast<LambdaExpression>(
                        parameters[0].expression);
                    if (!lam) {
                        throw Exception(
                            "transform intrinsic 'Grad' requires a lambda literal "
                            "whose body can be differentiated",
                            "CAJETA_ERROR_TRANSFORM_NOT_SPECIALIZABLE",
                            "", getSourceLine(), getSourceColumn());
                    }
                    CajetaTypePtr gradType;
                    llvm::Value* g = emitGradBackward(this, lam, fnType, module, gradType);
                    resolvedType = gradType;
                    return g;
                }
                if (methodCallName == "GradAll") {
                    auto lam = std::dynamic_pointer_cast<LambdaExpression>(
                        parameters[0].expression);
                    if (!lam) {
                        throw Exception(
                            "transform intrinsic 'GradAll' requires a lambda literal "
                            "whose body can be differentiated",
                            "CAJETA_ERROR_TRANSFORM_NOT_SPECIALIZABLE",
                            "", getSourceLine(), getSourceColumn());
                    }
                    CajetaTypePtr gradType;
                    llvm::Value* g = emitGradAllBackward(this, lam, fnType, module,
                                                         gradType);
                    resolvedType = gradType;
                    return g;
                }
                if (methodCallName == "Fuse") {
                    auto lam = std::dynamic_pointer_cast<LambdaExpression>(
                        parameters[0].expression);
                    if (!lam) {
                        throw Exception(
                            "transform intrinsic 'Fuse' requires a lambda literal "
                            "whose body can be fused",
                            "CAJETA_ERROR_TRANSFORM_NOT_SPECIALIZABLE",
                            "", getSourceLine(), getSourceColumn());
                    }
                    CajetaTypePtr fuseType;
                    llvm::Value* v = emitFusedExpr(this, lam, fnType, module, fuseType);
                    resolvedType = fuseType;
                    return v;
                }
                if (methodCallName == "Vmap") {
                    auto lam = std::dynamic_pointer_cast<LambdaExpression>(
                        parameters[0].expression);
                    if (!lam) {
                        throw Exception(
                            "transform intrinsic 'Vmap' requires a lambda literal "
                            "whose body can be batched",
                            "CAJETA_ERROR_TRANSFORM_NOT_SPECIALIZABLE",
                            "", getSourceLine(), getSourceColumn());
                    }
                    CajetaTypePtr vmapType;
                    llvm::Value* v = emitVmapBatched(this, lam, fnType, module, vmapType);
                    resolvedType = vmapType;
                    return v;
                }
                llvm::Value* raw = argExpr->generateCode(module);
                llvm::Value* fnVal = loadIfLValue(module, raw, argExpr);
                llvm::Constant* record = nullptr;
                llvm::Function* target =
                    CajetaClass::extractClosureTarget(fnVal, &record);
                if (!target) {
                    throw Exception(
                        "transform intrinsic '" + methodCallName
                        + "' requires a specializable target — a statically-known "
                          "function whose body is visible at the call site; the "
                          "argument here is resolved only at runtime",
                        "CAJETA_ERROR_TRANSFORM_NOT_SPECIALIZABLE",
                        "", getSourceLine(), getSourceColumn());
                }
                if (methodCallName == "Pmap") {
                    throw Exception(
                        "transform intrinsic 'Pmap' is not yet implemented "
                        "(deferred in v1; use Vmap/Jit for now)",
                        "CAJETA_ERROR_TRANSFORM_PMAP_UNIMPLEMENTED",
                        "", getSourceLine(), getSourceColumn());
                }
                if (methodCallName == "Jit" && record) {
                    if (auto* gvRec = llvm::dyn_cast<llvm::GlobalVariable>(record)) {
                        if (auto* init = llvm::dyn_cast<llvm::ConstantStruct>(
                                gvRec->getInitializer())) {
                            llvm::ValueToValueMapTy vm;
                            llvm::Function* clone = llvm::CloneFunction(target, vm);
                            clone->setName("__JitFused_" + target->getName().str());
                            cajeta::fuseFunction(*clone, nullptr);
                            std::vector<llvm::Constant*> elems;
                            for (unsigned i = 0; i < init->getType()->getNumElements(); ++i)
                                elems.push_back(i == 0
                                    ? static_cast<llvm::Constant*>(clone)
                                    : init->getAggregateElement(i));
                            auto* fusedRec = new llvm::GlobalVariable(
                                *module->getLlvmModule(), init->getType(),
                                /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
                                llvm::ConstantStruct::get(init->getType(), elems),
                                gvRec->getName() + ".jit");
                            resolvedType = fnType;
                            return fusedRec;
                        }
                    }
                }
                resolvedType = fnType;
                return fnVal;
            }
        }

        // ----- REFL-12: bounded reflection lowering -----
        // Both append the bound as a synthesized `Shape.class` arg; heapInstance KEEPS
        // the `<Shape>` token (it binds Optional<T>), subtypes DROPS it (wildcard return).
        if (!boundedReflInjected
                && explicitMethodTypeArgs.size() == 1
                && explicitMethodTypeArgs[0]
                && !children.empty()) {
            bool isBoundedHeapInstance =
                methodCallName == "heapInstance" && parameters.size() == 1;
            bool isBoundedSubtypes =
                methodCallName == "subtypes" && parameters.empty();
            bool isMethodAnnotatedToken =
                methodCallName == "classesWithMethodAnnotated"
                && parameters.empty();
            bool isAnnotatedToken =
                (methodCallName == "classesAnnotated" && parameters.empty())
                || isMethodAnnotatedToken;
            if (isBoundedHeapInstance || isBoundedSubtypes || isAnnotatedToken) {
                if (auto recvId = std::dynamic_pointer_cast<IdentifierExpression>(
                        children[0])) {
                    auto rt = CajetaType::of(recvId->getTextValue());
                    if (rt && rt->toCanonical() == "cajeta.reflect.Class") {
                        std::string tok =
                            explicitMethodTypeArgs[0]->toCanonical();
                        if (isAnnotatedToken) {
                            auto ac = std::dynamic_pointer_cast<CajetaClass>(
                                explicitMethodTypeArgs[0]);
                            if (ac && ac->isAnnotation() && ac->getQName())
                                tok = "code." + ac->getQName()->getTypeName();
                        }
                        MethodCallParameter arg;
                        if (isAnnotatedToken) {
                            arg.expression =
                                std::make_shared<TextLiteralExpression>(
                                    "\"" + tok + "\"", LITERAL_TYPE_STRING);
                        } else {
                            arg.expression =
                                std::make_shared<ClassLiteralExpression>(
                                    tok, nullptr);
                        }
                        parameters.push_back(arg);
                        boundedReflInjected = true;
                        auto& keep = CajetaModule::reflectionKeep();
                        using RS = CajetaModule::ReflSite;
                        if (isAnnotatedToken) {
                            auto p = tok.rfind('.');
                            keep.sites.push_back({isMethodAnnotatedToken
                                    ? RS::MethodAnnotated : RS::Annotated,
                                p == std::string::npos ? tok : tok.substr(p + 1)});
                            explicitMethodTypeArgs.clear();
                        } else {
                            keep.sites.push_back({RS::BoundClosure, tok});
                            if (isBoundedSubtypes) explicitMethodTypeArgs.clear();
                        }
                    }
                }
            }
        }

        if (!children.empty()) {
            static const std::set<std::string> kClassReflEntry = {
                "forName", "allClasses", "classesInPackage",
                "classesAnnotated", "classesWithMethodAnnotated",
                "subtypes", "heapInstance"};
            bool isClassEntry = false, isGetType = false;
            if (kClassReflEntry.count(methodCallName)) {
                if (auto recvId = std::dynamic_pointer_cast<IdentifierExpression>(
                        children[0])) {
                    auto rt = CajetaType::of(recvId->getTextValue());
                    isClassEntry =
                        rt && rt->toCanonical() == "cajeta.reflect.Class";
                }
            } else if (methodCallName == "getType") {
                if (auto recvExpr =
                        std::dynamic_pointer_cast<Expression>(children[0])) {
                    if (!recvExpr->getResolvedType()) {
                        recvExpr->resolveTypes(module);
                    }
                    auto rt = recvExpr->getResolvedType();
                    isGetType = rt &&
                        rt->toCanonical() == "cajeta.reflect.TemplateArgument";
                }
            }
            bool callerInReflect = false;
            if (auto cm = module->getCurrentMethod()) {
                if (auto owner = cm->getParent()) {
                    callerInReflect =
                        owner->toCanonical().rfind("cajeta.reflect.", 0) == 0;
                }
            }
            if ((isClassEntry || isGetType) && !callerInReflect) {
                auto& keep = CajetaModule::reflectionKeep();
                auto stringLiteralArg = [&](int i) -> std::string {
                    if (i >= (int) parameters.size()) return "";
                    auto lit = std::dynamic_pointer_cast<TextLiteralExpression>(
                        parameters[i].expression);
                    if (!lit ||
                            lit->getLiteralType() != LITERAL_TYPE_STRING) {
                        return "";
                    }
                    std::string v = lit->getRawValue();
                    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
                        return v.substr(1, v.size() - 2);
                    }
                    return "";
                };
                auto shortName = [](const std::string& canon) {
                    auto p = canon.rfind('.');
                    return p == std::string::npos ? canon : canon.substr(p + 1);
                };
                using RS = CajetaModule::ReflSite;
                using M = CajetaModule;
                if (isGetType) {
                    M::noteForceAll("TemplateArgument.getType() — resolves any "
                        "template-argument type by name; bound it with a concrete "
                        "Class<T> instead");
                } else if (methodCallName == "forName") {
                    std::string s = stringLiteralArg(0);
                    if (!s.empty()) keep.sites.push_back({RS::ForNameLiteral, s});
                    else M::noteForceAll("forName(<non-literal>) — pass a string "
                        "literal, or use Class.subtypes<Base>() for a bounded set");
                } else if (methodCallName == "classesInPackage") {
                    std::string s = stringLiteralArg(0);
                    if (!s.empty()) keep.sites.push_back({RS::PackageLiteral, s});
                    else M::noteForceAll("classesInPackage(<non-literal>) — pass a "
                        "package-name string literal");
                } else if (methodCallName == "classesAnnotated") {
                    std::string s = stringLiteralArg(0);
                    if (!s.empty())
                        keep.sites.push_back({RS::Annotated, shortName(s)});
                    else M::noteForceAll("classesAnnotated(<non-literal>) — pass an "
                        "annotation token classesAnnotated<@A>() or a name literal");
                } else if (methodCallName == "classesWithMethodAnnotated") {
                    std::string s = stringLiteralArg(0);
                    if (!s.empty())
                        keep.sites.push_back({RS::MethodAnnotated, shortName(s)});
                    else M::noteForceAll("classesWithMethodAnnotated(<non-literal>)"
                        " — pass an annotation token"
                        " classesWithMethodAnnotated<@A>() or a name literal");
                } else if (methodCallName == "heapInstance"
                        || methodCallName == "subtypes") {
                    if (!boundedReflInjected)
                        M::noteForceAll(methodCallName + "(...) without a <T> bound "
                            "— add a type bound, e.g. subtypes<Base>()");
                } else {
                    M::noteForceAll(methodCallName + "() — enumerates the whole "
                        "registry; use Class.subtypes<Base>() for a bounded set");
                }
            }
        }

        // ----- Buffer<T>.elementBytes() intrinsic -----
        // Cajeta has no source-level sizeof: folds to T's DataLayout byte size.
        if (methodCallName == "elementBytes") {
            if (auto cm = module->getCurrentMethod()) {
                auto parent = cm->getParent();
                if (parent && parent->isInstantiation()
                        && parent->toCanonical().rfind(
                               "cajeta.xpu.KernelBuffer", 0) == 0
                        && !parent->getTypeArguments().empty()) {
                    auto elemT = parent->getTypeArguments()[0];
                    llvm::Type* lt = elemT ? elemT->getLlvmType() : nullptr;
                    uint64_t sz = 0;
                    if (lt) {
                        sz = module->getLlvmModule()->getDataLayout()
                                 .getTypeAllocSize(lt);
                    }
                    if (auto u64 = CajetaType::of("uint64")) resolvedType = u64;
                    return llvm::ConstantInt::get(
                        llvm::Type::getInt64Ty(llvmCtx), sz);
                }
            }
        }

        // ----- REFL-11: constant-fold statically-known reflection -----
        // Only a `final`-class-typed identifier receiver folds: its dynamic type then
        // provably equals its static type, so the baked-in layout is what runtime reads.
        if (!children.empty()) {
            auto innerCall =
                std::dynamic_pointer_cast<MethodCallExpression>(children[0]);
            bool isClassOf = false;
            if (innerCall && innerCall->getMethodCallName() == "of"
                    && innerCall->getParameters().size() == 1
                    && !innerCall->getChildren().empty()) {
                if (auto recvId = std::dynamic_pointer_cast<IdentifierExpression>(
                        innerCall->getChildren()[0])) {
                    auto rt = CajetaType::of(recvId->getTextValue());
                    isClassOf =
                        rt && rt->toCanonical() == "cajeta.reflect.Class";
                }
            }
            if (isClassOf) {
                {
                    ExpressionPtr ofArg = innerCall->getParameters()[0].expression;
                    auto ofIdent =
                        std::dynamic_pointer_cast<IdentifierExpression>(ofArg);
                    if (ofIdent) {
                        if (!ofArg->getResolvedType()) ofArg->resolveTypes(module);
                        auto K = std::dynamic_pointer_cast<CajetaClass>(
                            ofArg->getResolvedType());
                        if (K && K->getModifiers().count(FINAL) > 0) {
                            if (parameters.empty()) {
                                auto* i32Ty = llvm::Type::getInt32Ty(llvmCtx);
                                if (methodCallName == "getFieldCount") {
                                    resolvedType = CajetaType::of("int32");
                                    return llvm::ConstantInt::get(i32Ty,
                                        (uint64_t)(uint32_t)
                                            K->getPropertyList().size());
                                }
                                if (methodCallName == "getMethodCount") {
                                    resolvedType = CajetaType::of("int32");
                                    return llvm::ConstantInt::get(i32Ty,
                                        (uint64_t)(uint32_t)
                                            K->getMethodList().size());
                                }
                                if (methodCallName == "getParentCount") {
                                    resolvedType = CajetaType::of("int32");
                                    return llvm::ConstantInt::get(i32Ty,
                                        (uint64_t)(uint32_t)
                                            K->getSuperClasses().size());
                                }
                                if (methodCallName == "getModifierFlags") {
                                    int32_t bits = 0;
                                    for (auto m : K->getModifiers())
                                        bits |= (int32_t) m;
                                    resolvedType = CajetaType::of("int32");
                                    return llvm::ConstantInt::get(i32Ty,
                                        (uint64_t)(uint32_t) bits);
                                }
                                if (methodCallName == "getInstanceSize") {
                                    uint64_t sz = 0;
                                    if (auto lt = K->getLlvmType())
                                        sz = module->getLlvmModule()
                                                 ->getDataLayout()
                                                 .getTypeAllocSize(lt);
                                    resolvedType = CajetaType::of("int64");
                                    return llvm::ConstantInt::get(
                                        llvm::Type::getInt64Ty(llvmCtx), sz);
                                }
                            }
                            const char* wantFieldType = nullptr;
                            if (methodCallName == "getInt32")
                                wantFieldType = "int32";
                            else if (methodCallName == "getInt64")
                                wantFieldType = "int64";
                            else if (methodCallName == "getFloat32")
                                wantFieldType = "float32";
                            else if (methodCallName == "getFloat64")
                                wantFieldType = "float64";
                            if (wantFieldType && parameters.size() == 2) {
                                auto objIdent =
                                    std::dynamic_pointer_cast<IdentifierExpression>(
                                        parameters[0].expression);
                                auto idxLit =
                                    std::dynamic_pointer_cast<
                                        IntegerLiteralExpression>(
                                            parameters[1].expression);
                                bool sameRecv = objIdent
                                    && objIdent->getTextValue()
                                           == ofIdent->getTextValue();
                                int64_t litIdx = -1;
                                if (idxLit && idxLit->getIntegerLiteralType()
                                        == INTEGER_LITERAL_TYPE_DECIMAL) {
                                    bool ok = true;
                                    std::string digits;
                                    for (char ch : idxLit->getRawValue()) {
                                        if (ch == '_') continue;
                                        if (ch < '0' || ch > '9') { ok = false; break; }
                                        digits.push_back(ch);
                                    }
                                    if (ok && !digits.empty()) {
                                        try { litIdx = std::stoll(digits); }
                                        catch (...) { litIdx = -1; }
                                    }
                                }
                                auto& props = K->getPropertyList();
                                if (sameRecv && litIdx >= 0
                                        && (size_t) litIdx < props.size()) {
                                    auto it = props.begin();
                                    std::advance(it, (size_t) litIdx);
                                    StructurePropertyPtr p = *it;
                                    bool sealed =
                                        K->getModifiers().count(REFLECT_SEALED) > 0;
                                    bool priv =
                                        p->getModifiers().count(PRIVATE) > 0;
                                    bool typeMatch = p->getType()
                                        && p->getType()->toCanonical()
                                               == wantFieldType;
                                    int llvmIdx = K->getFieldLlvmIndex(p);
                                    auto* instStruct =
                                        llvm::dyn_cast_or_null<llvm::StructType>(
                                            K->getLlvmType());
                                    if (!(sealed && priv) && typeMatch
                                            && !p->isStatic() && instStruct
                                            && llvmIdx >= 0
                                            && (unsigned) llvmIdx
                                                   < instStruct->getNumElements()) {
                                        const llvm::StructLayout* layout =
                                            module->getLlvmModule()
                                                ->getDataLayout()
                                                .getStructLayout(instStruct);
                                        uint64_t off = layout->getElementOffset(
                                            (unsigned) llvmIdx);
                                        llvm::Value* objSlot =
                                            parameters[0].expression
                                                ->generateCode(module);
                                        llvm::Value* objVal = objSlot
                                            ? builder->CreateLoad(
                                                  llvm::PointerType::get(
                                                      llvmCtx, 0),
                                                  objSlot, "refl.fold.obj")
                                            : nullptr;
                                        if (objVal) {
                                            llvm::Value* fieldPtr =
                                                builder->CreateGEP(
                                                    llvm::Type::getInt8Ty(llvmCtx),
                                                    objVal,
                                                    llvm::ConstantInt::get(
                                                        llvm::Type::getInt64Ty(
                                                            llvmCtx),
                                                        off),
                                                    "refl.fold.fieldptr");
                                            resolvedType = p->getType();
                                            return builder->CreateLoad(
                                                p->getType()->getLlvmType(),
                                                fieldPtr, "refl.fold.load");
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        if ((methodCallName == "sync" || methodCallName == "waitHost" ||
             methodCallName == "free") && !children.empty()) {
            if (auto recvId =
                    std::dynamic_pointer_cast<IdentifierExpression>(children[0])) {
                if (auto sc = module->getScopeStack().peek()) {
                    std::string canonical;
                    const std::string& recv = recvId->getTextValue();
                    if (sc->containsField(recv)) {
                        if (auto f = sc->getField(recv)) {
                            if (f->getType()) canonical = f->getType()->toCanonical();
                        }
                    }
                    bool isStream = canonical == "cajeta.xpu.KernelStream";
                    bool isEvent  = canonical == "cajeta.xpu.Event";
                    bool isBuffer =
                        canonical.rfind("cajeta.xpu.KernelBuffer", 0) == 0;
                    if ((isStream && methodCallName == "sync") ||
                        (isEvent && methodCallName == "waitHost")) {
                        sc->releaseLaunchBorrows();
                    } else if (isBuffer && methodCallName == "free" &&
                               sc->isLaunchBorrowed(recv)) {
                        throw Exception(
                            "buffer '" + recv + "' is freed while a launch still "
                            "references it — sync the stream (Stream.sync()) "
                            "before freeing", "XPU-K02");
                    }
                }
            }
        }

        if (superCtorCall) {
            if (module->getStructureStack().empty()) {
                throw Exception("`super(...)` used outside of a class ctor",
                    "CAJETA_ERROR_SUPER_OUTSIDE_CLASS");
            }
            auto here = std::dynamic_pointer_cast<CajetaClass>(
                module->getStructureStack().back());
            if (!here || here->getSuperClasses().empty()) {
                throw Exception("`super(...)` used in a class with no parent",
                    "CAJETA_ERROR_SUPER_NO_PARENT");
            }
            CajetaClassPtr parentCls = here->getSuperClasses().front();
            std::vector<ParameterEntry> entries;
            for (auto& p : parameters) {
                if (p.expression && !p.expression->getResolvedType()) {
                    p.expression->resolveTypes(module);
                }
                llvm::Value* v = p.expression ? p.expression->generateCode(module) : nullptr;
                CajetaTypePtr t = p.expression ? p.expression->getResolvedType() : nullptr;
                if (v && p.expression) {
                    auto exprAst = std::dynamic_pointer_cast<Expression>(p.expression);
                    v = loadIfLValue(module, v, exprAst);
                }
                entries.emplace_back(t, p.label, v);
            }
            auto scope = module->getScopeStack().peek();
            FieldPtr thisField = scope ? scope->getField("this") : nullptr;
            if (!thisField) {
                throw Exception("`super(...)` used in a static context with no `this`",
                    "CAJETA_ERROR_SUPER_IN_STATIC");
            }
            llvm::Value* thisValue = builder->CreateLoad(
                thisField->getOrCreateAllocation()->getAllocatedType(),
                thisField->getOrCreateAllocation());
            uint64_t off = here->getSubObjectByteOffset(parentCls.get());
            if (off != 0) {
                llvm::Type* i8Ty = llvm::Type::getInt8Ty(
                    *module->getLlvmContext());
                thisValue = builder->CreateInBoundsGEP(i8Ty, thisValue,
                    llvm::ConstantInt::get(
                        llvm::Type::getInt64Ty(*module->getLlvmContext()),
                        off),
                    "super_ctor_subobj");
            }
            std::string ctorName = parentCls->getTemplateOrigin()
                ? parentCls->getTemplateOrigin()->getQName()->getTypeName()
                : parentCls->getQName()->getTypeName();
            return parentCls->invokeMethod(ctorName, entries,
                /*isConstructor=*/true, thisValue, /*callerModule=*/module,
                /*forceDirectCall=*/true);
        }

        // ----- Indirect call through a function-typed local -----
        // The local's slot holds a `ptr` to the closure record { ptr fn, ptr captures }.
        if (children.empty() && !module->getScopeStack().isEmpty()) {
            auto scope = module->getScopeStack().peek();
            FieldPtr field = scope ? scope->getField(methodCallName) : nullptr;
            if (field) {
                if (auto bound = dynamic_pointer_cast<BoundClosureField>(field)) {
                    auto fnType = dynamic_pointer_cast<CajetaFunctionType>(
                        bound->getType());
                    if (fnType) {
                        return emitClosureCall(module, /*closurePtr=*/nullptr,
                            fnType, parameters, resolvedType,
                            bound->getBoundFunction());
                    }
                }
                auto fnType = dynamic_pointer_cast<CajetaFunctionType>(field->getType());
                if (fnType) {
                    llvm::Type* ptrTy = llvm::PointerType::get(llvmCtx, 0);
                    llvm::AllocaInst* slot = field->getOrCreateAllocation();
                    llvm::Value* closurePtr = builder->CreateLoad(
                        ptrTy, slot, "closure_ptr");
                    return emitClosureCall(module, closurePtr, fnType,
                                           parameters, resolvedType);
                }
            }
        }

        // ----- Struct view construction: `MyStruct(byte[] bytes)` -----
        if (children.empty() && parameters.size() == 1) {
            auto structType = dynamic_pointer_cast<CajetaView>(
                CajetaType::of(methodCallName));
            if (structType) {
                if (structType->getHasElementArrayField()
                        && parameters[0].callerTransferred) {
                    throw Exception(
                        "owning form `" + methodCallName + "(#bytes)` is not "
                        "supported for views with element-array fields (v1.1): "
                        "the offset table lives in the frame arena and cannot "
                        "escape the constructing frame. Use the borrow form `"
                        + methodCallName + "(bytes)`.",
                        "CAJETA_ERROR_VIEW_ELEMENT_ARRAY_OWNING");
                }
                llvm::Value* bytesPtr = parameters[0].expression->generateCode(module);
                if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(bytesPtr)) {
                    bytesPtr = builder->CreateLoad(a->getAllocatedType(), a);
                }
                if (!bytesPtr) return nullptr;

                llvm::Type* i64Ty = llvm::Type::getInt64Ty(llvmCtx);
                llvm::Type* i8Ty = llvm::Type::getInt8Ty(llvmCtx);

                uint64_t structBytes = structType->getMinimumSize();
                uint64_t elemBytes = 1;
                if (auto argExpr = dynamic_pointer_cast<Expression>(parameters[0].expression)) {
                    if (!argExpr->getResolvedType()) argExpr->resolveTypes(module);
                    if (auto arrType = dynamic_pointer_cast<CajetaArray>(argExpr->getResolvedType())) {
                        if (auto elemTy = arrType->getElementLlvmType(&llvmCtx)) {
                            elemBytes = module->getLlvmModule()->getDataLayout().getTypeAllocSize(elemTy);
                            if (elemBytes == 0) elemBytes = 1;
                        }
                    }
                }

                llvm::Value* count = builder->CreateLoad(i64Ty, bytesPtr, "view_buf_count");
                llvm::Value* haveBytes = builder->CreateMul(count,
                    llvm::ConstantInt::get(i64Ty, elemBytes), "view_buf_bytes");
                llvm::Value* ok = builder->CreateICmpUGE(haveBytes,
                    llvm::ConstantInt::get(i64Ty, structBytes), "view_size_ok");

                llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
                llvm::BasicBlock* failBB = llvm::BasicBlock::Create(
                    llvmCtx, "view_size_fail", parentFn);
                llvm::BasicBlock* okBB = llvm::BasicBlock::Create(
                    llvmCtx, "view_size_ok", parentFn);
                builder->CreateCondBr(ok, okBB, failBB);

                builder->SetInsertPoint(failBB);
                if (llvm::Function* throwFn = module->getRuntimeFunction("__cajeta_throw")) {
                    uint64_t tag = (uint64_t) 0xCA1E7A00 | (structBytes & 0xFF);
                    llvm::PointerType* ptrTy = llvm::PointerType::get(llvmCtx, 0);
                    llvm::Value* tagPtr = builder->CreateIntToPtr(
                        llvm::ConstantInt::get(i64Ty, tag), ptrTy);
                    builder->CreateCall(throwFn, {tagPtr});
                }
                builder->CreateUnreachable();

                builder->SetInsertPoint(okBB);
                llvm::Value* dataPtr = builder->CreateInBoundsGEP(
                    i8Ty, bytesPtr,
                    llvm::ConstantInt::get(i64Ty, 8), "view_data_ptr");
                llvm::Value* viewValue = dataPtr;

                // Length-prefix validation sweep: walk the view's properties in declaration
                // order, advancing a running offset (var-size field: read prefix, verify
                // offset+4+prefix <= bufferBytes; post-var fixed field: advance its static size).
                int varSizeCount = structType->getVariableSizeFieldCount();
                if (varSizeCount > 0) {
                    uint64_t fixedPrefixSize = structType->getFixedSize();
                    llvm::Value* offset = llvm::ConstantInt::get(
                        i64Ty, fixedPrefixSize);
                    const llvm::DataLayout& dl = module->getLlvmModule()->getDataLayout();
                    llvm::Type* i32Ty = llvm::Type::getInt32Ty(llvmCtx);
                    int diagIdx = 0;
                    llvm::Value* totalVarElems = llvm::ConstantInt::get(i64Ty, 0);

                    auto emitCheck = [&](llvm::Value* okCond, const char* nm) {
                        llvm::BasicBlock* fBB = llvm::BasicBlock::Create(
                            llvmCtx, std::string(nm) + "_fail", parentFn);
                        llvm::BasicBlock* oBB = llvm::BasicBlock::Create(
                            llvmCtx, std::string(nm) + "_ok", parentFn);
                        builder->CreateCondBr(okCond, oBB, fBB);
                        builder->SetInsertPoint(fBB);
                        if (llvm::Function* throwFn =
                                module->getRuntimeFunction("__cajeta_throw")) {
                            uint64_t tag = (uint64_t) 0xCA1E7A00
                                | (uint64_t)(diagIdx & 0xFF);
                            llvm::PointerType* ptrTy =
                                llvm::PointerType::get(llvmCtx, 0);
                            llvm::Value* tagPtr = builder->CreateIntToPtr(
                                llvm::ConstantInt::get(i64Ty, tag), ptrTy);
                            builder->CreateCall(throwFn, {tagPtr});
                        }
                        builder->CreateUnreachable();
                        builder->SetInsertPoint(oBB);
                        diagIdx++;
                    };

                    auto emitReadPrefix = [&](llvm::Value* off,
                                              const char* nm,
                                              ViewEndianness pe) -> llvm::Value* {
                        llvm::Value* after4 = builder->CreateAdd(
                            off, llvm::ConstantInt::get(i64Ty, 4));
                        emitCheck(builder->CreateICmpULE(after4, haveBytes), nm);
                        llvm::Value* pPtr = builder->CreateInBoundsGEP(
                            i8Ty, dataPtr, off);
                        llvm::Value* p32 = CajetaView::emitSwapIfNeeded(
                            module, pe, builder->CreateLoad(i32Ty, pPtr));
                        llvm::Value* p64 = builder->CreateIntCast(
                            p32, i64Ty, /*isSigned=*/true);
                        emitCheck(builder->CreateICmpSGE(p64,
                            llvm::ConstantInt::get(i64Ty, 0)), nm);
                        return p64;
                    };
                    ViewEndianness outerE = structType->getEndianness();

                    auto emitScalarVarAdvance =
                        [&](const StructurePropertyPtr& p,
                            llvm::Value* off,
                            ViewEndianness pe) -> llvm::Value* {
                        llvm::Value* len = emitReadPrefix(off, "vlen", pe);
                        uint64_t elemBytes = 1;
                        if (p) {
                            if (auto arrType = dynamic_pointer_cast<CajetaArray>(
                                    p->getType())) {
                                if (auto et = arrType->getElementLlvmType(&llvmCtx)) {
                                    elemBytes = dl.getTypeAllocSize(et);
                                    if (elemBytes == 0) elemBytes = 1;
                                }
                            }
                        }
                        llvm::Value* dataBytes = (elemBytes == 1) ? len
                            : builder->CreateMul(len,
                                llvm::ConstantInt::get(i64Ty, elemBytes));
                        llvm::Value* after = builder->CreateAdd(
                            builder->CreateAdd(off,
                                llvm::ConstantInt::get(i64Ty, 4)),
                            dataBytes, "vlen_after_field");
                        emitCheck(builder->CreateICmpULE(after, haveBytes),
                            "vlen");
                        return after;
                    };

                    auto emitViewElementAdvance =
                        [&](const CajetaViewPtr& elemView,
                            llvm::Value* off) -> llvm::Value* {
                        ViewEndianness elemE = elemView->getEndianness();
                        for (auto& ep : elemView->getPropertyList()) {
                            if (CajetaView::isVariableSize(ep)) {
                                off = emitScalarVarAdvance(ep, off, elemE);
                            } else {
                                uint64_t sz = dl.getTypeAllocSize(
                                    ep->getType()->getLlvmType());
                                off = builder->CreateAdd(off,
                                    llvm::ConstantInt::get(i64Ty, sz),
                                    "elem_fixed_after");
                                emitCheck(builder->CreateICmpULE(off,
                                    haveBytes), "elem_fixed");
                            }
                        }
                        return off;
                    };

                    auto emitElementArrayAdvance =
                        [&](const StructurePropertyPtr& p,
                            llvm::Value* off) -> llvm::Value* {
                        llvm::Value* count = emitReadPrefix(off, "ecount", outerE);
                        if (CajetaView::elementArrayHasVarSizeElements(p)) {
                            totalVarElems = builder->CreateAdd(
                                totalVarElems, count, "vea_totelems");
                        }
                        llvm::Value* start = builder->CreateAdd(
                            off, llvm::ConstantInt::get(i64Ty, 4));
                        CajetaViewPtr elemView;
                        if (auto arrType = dynamic_pointer_cast<CajetaArray>(
                                p->getType())) {
                            elemView = dynamic_pointer_cast<CajetaView>(
                                arrType->getElementType());
                        }
                        llvm::BasicBlock* preBB = builder->GetInsertBlock();
                        llvm::BasicBlock* hdrBB = llvm::BasicBlock::Create(
                            llvmCtx, "earr_hdr", parentFn);
                        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(
                            llvmCtx, "earr_body", parentFn);
                        llvm::BasicBlock* exitBB = llvm::BasicBlock::Create(
                            llvmCtx, "earr_exit", parentFn);
                        builder->CreateBr(hdrBB);
                        builder->SetInsertPoint(hdrBB);
                        llvm::PHINode* kPhi = builder->CreatePHI(i64Ty, 2, "earr_k");
                        llvm::PHINode* offPhi = builder->CreatePHI(i64Ty, 2, "earr_off");
                        kPhi->addIncoming(llvm::ConstantInt::get(i64Ty, 0), preBB);
                        offPhi->addIncoming(start, preBB);
                        builder->CreateCondBr(
                            builder->CreateICmpSLT(kPhi, count), bodyBB, exitBB);
                        builder->SetInsertPoint(bodyBB);
                        llvm::Value* offAfter = elemView
                            ? emitViewElementAdvance(elemView, offPhi)
                            : emitScalarVarAdvance(nullptr, offPhi, outerE);
                        llvm::Value* kNext = builder->CreateAdd(
                            kPhi, llvm::ConstantInt::get(i64Ty, 1));
                        llvm::BasicBlock* bodyEndBB = builder->GetInsertBlock();
                        builder->CreateBr(hdrBB);
                        kPhi->addIncoming(kNext, bodyEndBB);
                        offPhi->addIncoming(offAfter, bodyEndBB);
                        builder->SetInsertPoint(exitBB);
                        return offPhi;
                    };

                    bool sawVar = false;
                    for (auto& p : structType->getPropertyList()) {
                        bool isVar = CajetaView::isVariableSize(p);
                        if (!sawVar && !isVar) {
                            continue;
                        }
                        if (isVar) {
                            sawVar = true;
                            offset = CajetaView::isElementArray(p)
                                ? emitElementArrayAdvance(p, offset)
                                : emitScalarVarAdvance(p, offset, outerE);
                        } else {
                            uint64_t sz = dl.getTypeAllocSize(
                                p->getType()->getLlvmType());
                            offset = builder->CreateAdd(offset,
                                llvm::ConstantInt::get(i64Ty, sz),
                                "vlen_after_postvar_fixed");
                            emitCheck(builder->CreateICmpULE(offset,
                                haveBytes), "vlen_postvar");
                        }
                    }

                    // ---- view v1.1: offset table + descriptor (pass 2) ----
                    if (structType->getHasElementArrayField()) {
                        llvm::Function* arenaAlloc =
                            module->getRuntimeFunction("__cajeta_arena_alloc");
                        if (arenaAlloc) {
                            int fixedSlots = structType->tableFixedSlotCount();
                            llvm::Value* slotCount = builder->CreateAdd(
                                llvm::ConstantInt::get(i64Ty, fixedSlots),
                                totalVarElems, "vea_slots");
                            llvm::Value* table = builder->CreateCall(
                                arenaAlloc,
                                {builder->CreateMul(slotCount,
                                    llvm::ConstantInt::get(i64Ty, 8))});
                            auto tableSlotPtr = [&](llvm::Value* idx) {
                                return builder->CreateInBoundsGEP(
                                    i64Ty, table, idx, "vea_slot");
                            };
                            llvm::Value* fillOff = llvm::ConstantInt::get(
                                i64Ty, fixedPrefixSize);
                            llvm::Value* regionCursor = llvm::ConstantInt::get(
                                i64Ty, fixedSlots);
                            bool fSawVar = false;
                            for (auto& p : structType->getPropertyList()) {
                                bool isVar = CajetaView::isVariableSize(p);
                                if (!fSawVar && !isVar) continue;
                                if (isVar) fSawVar = true;
                                int slot = structType->tableSlotOf(p);
                                builder->CreateStore(fillOff,
                                    tableSlotPtr(llvm::ConstantInt::get(
                                        i64Ty, slot)));
                                if (!isVar) {
                                    uint64_t sz = dl.getTypeAllocSize(
                                        p->getType()->getLlvmType());
                                    fillOff = builder->CreateAdd(fillOff,
                                        llvm::ConstantInt::get(i64Ty, sz));
                                    continue;
                                }
                                if (!CajetaView::isElementArray(p)) {
                                    fillOff = CajetaView::emitAccessAdvance(
                                        module, p, dataPtr, fillOff, outerE);
                                    continue;
                                }
                                llvm::Value* cPtr = builder->CreateInBoundsGEP(
                                    i8Ty, dataPtr, fillOff, "vea_fill_cptr");
                                llvm::Value* cnt = builder->CreateIntCast(
                                    CajetaView::emitSwapIfNeeded(module, outerE,
                                        builder->CreateLoad(
                                            llvm::Type::getInt32Ty(llvmCtx),
                                            cPtr)),
                                    i64Ty, /*isSigned=*/true);
                                llvm::Value* elemsStart = builder->CreateAdd(
                                    fillOff, llvm::ConstantInt::get(i64Ty, 4));
                                auto arrT = dynamic_pointer_cast<CajetaArray>(
                                    p->getType());
                                auto elemView = dynamic_pointer_cast<CajetaView>(
                                    arrT->getElementType());
                                if (!CajetaView::elementArrayHasVarSizeElements(p)) {
                                    fillOff = builder->CreateAdd(elemsStart,
                                        builder->CreateMul(cnt,
                                            llvm::ConstantInt::get(i64Ty,
                                                elemView->getFixedSize())));
                                    continue;
                                }
                                builder->CreateStore(regionCursor,
                                    tableSlotPtr(llvm::ConstantInt::get(
                                        i64Ty, slot + 1)));
                                llvm::BasicBlock* preBB = builder->GetInsertBlock();
                                llvm::BasicBlock* hdrBB = llvm::BasicBlock::Create(
                                    llvmCtx, "vea_fill_hdr", parentFn);
                                llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(
                                    llvmCtx, "vea_fill_body", parentFn);
                                llvm::BasicBlock* exitBB = llvm::BasicBlock::Create(
                                    llvmCtx, "vea_fill_exit", parentFn);
                                builder->CreateBr(hdrBB);
                                builder->SetInsertPoint(hdrBB);
                                llvm::PHINode* kPhi = builder->CreatePHI(
                                    i64Ty, 2, "vea_fill_k");
                                llvm::PHINode* oPhi = builder->CreatePHI(
                                    i64Ty, 2, "vea_fill_off");
                                kPhi->addIncoming(
                                    llvm::ConstantInt::get(i64Ty, 0), preBB);
                                oPhi->addIncoming(elemsStart, preBB);
                                builder->CreateCondBr(
                                    builder->CreateICmpSLT(kPhi, cnt),
                                    bodyBB, exitBB);
                                builder->SetInsertPoint(bodyBB);
                                builder->CreateStore(oPhi, tableSlotPtr(
                                    builder->CreateAdd(regionCursor, kPhi)));
                                llvm::Value* oNext;
                                if (elemView) {
                                    oNext = CajetaView::emitElementAdvance(
                                        module, elemView, dataPtr, oPhi);
                                } else {
                                    llvm::Value* sPtr = builder->CreateInBoundsGEP(
                                        i8Ty, dataPtr, oPhi, "vea_fill_sptr");
                                    llvm::Value* sLen = builder->CreateIntCast(
                                        CajetaView::emitSwapIfNeeded(module,
                                            outerE,
                                            builder->CreateLoad(
                                                llvm::Type::getInt32Ty(llvmCtx),
                                                sPtr)),
                                        i64Ty, /*isSigned=*/true);
                                    oNext = builder->CreateAdd(
                                        builder->CreateAdd(oPhi,
                                            llvm::ConstantInt::get(i64Ty, 4)),
                                        sLen);
                                }
                                llvm::Value* kNext = builder->CreateAdd(kPhi,
                                    llvm::ConstantInt::get(i64Ty, 1));
                                llvm::BasicBlock* bodyEndBB =
                                    builder->GetInsertBlock();
                                builder->CreateBr(hdrBB);
                                kPhi->addIncoming(kNext, bodyEndBB);
                                oPhi->addIncoming(oNext, bodyEndBB);
                                builder->SetInsertPoint(exitBB);
                                fillOff = oPhi;
                                regionCursor = builder->CreateAdd(
                                    regionCursor, cnt, "vea_region_next");
                            }
                            llvm::Value* desc = builder->CreateCall(arenaAlloc,
                                {llvm::ConstantInt::get(i64Ty, 16)});
                            builder->CreateStore(dataPtr, desc);
                            llvm::Value* tSlot = builder->CreateInBoundsGEP(
                                i8Ty, desc,
                                llvm::ConstantInt::get(i64Ty, 8), "vea_desc_t");
                            builder->CreateStore(table, tSlot);
                            viewValue = desc;
                        }
                    }
                }

                if (parameters[0].callerTransferred) {
                    if (auto idExpr = std::dynamic_pointer_cast<IdentifierExpression>(
                            parameters[0].expression)) {
                        if (auto scope = module->getScopeStack().peek()) {
                            FieldPtr field = scope->getField(idExpr->getTextValue());
                            if (field) {
                                if (field->getDropEntry()) {
                                    ownership::deactivateLocalEntry(module, field);
                                }
                            }
                        }
                    }
                }

                return viewValue;
            }
        }

        // ----- Bare class-construction syntax rejected (UnifiedClasses.md P1b) -----
        if (children.empty()) {
            auto resolvedType = CajetaType::of(methodCallName);
            auto classType = std::dynamic_pointer_cast<CajetaClass>(resolvedType);
            auto viewType  = std::dynamic_pointer_cast<CajetaView>(resolvedType);
            if (classType && !classType->isInterface() && !viewType) {
                char buf[640];
                snprintf(buf, sizeof(buf),
                    "class '%s' cannot be constructed via bare `%s(...)` "
                    "syntax in the unified-class model. Use `heap %s(...)` "
                    "for heap allocation or `stack %s(...)` for stack "
                    "allocation (`stack` lands fully in Phase 2). The "
                    "`new` keyword continues to work during the deprecation "
                    "cycle.",
                    methodCallName.c_str(), methodCallName.c_str(),
                    methodCallName.c_str(), methodCallName.c_str());
                throw Exception(buf, "CAJETA_ERROR_BARE_CLASS_CONSTRUCTION");
            }
        }

        // ----- System.<stream>.<method>(...) intrinsic -----
        if (!children.empty()) {
            // ----- System.env / System.property / System.args -----
            {
                std::string sysNs = detectSystemNamespaceReceiver(children[0]);
                if (!sysNs.empty()) {
                    llvm::Type* i8Ty  = llvm::Type::getInt8Ty(llvmCtx);
                    llvm::Type* i32Ty = llvm::Type::getInt32Ty(llvmCtx);
                    llvm::Type* i64Ty = llvm::Type::getInt64Ty(llvmCtx);
                    llvm::Type* ptrTy = llvm::PointerType::get(llvmCtx, 0);

                    if (sysNs == "args" && methodCallName == "count"
                        && parameters.empty()) {
                        llvm::Function* countFn =
                            module->getRuntimeFunction("__cajeta_args_count");
                        if (!countFn) return nullptr;
                        setResolvedType(CajetaType::of("int64"));
                        return builder->CreateCall(countFn, {}, "system.args.count");
                    }
                    if (sysNs == "args" && methodCallName == "set") {
                        throw Exception(
                            "`System.args` is read-only — it reports how the "
                            "process was invoked. Use `System.property.set` for "
                            "a value you want later code to observe.",
                            "CAJETA_ERROR_ARGS_READ_ONLY");
                    }

                    if (methodCallName == "get" && parameters.size() == 1) {
                        std::string fnName = "__cajeta_" + sysNs + "_get";
                        llvm::Function* getFn = module->getRuntimeFunction(fnName);
                        if (!getFn) return nullptr;
                        llvm::Value* nameArg;
                        if (sysNs == "args") {
                            auto idxAst = std::dynamic_pointer_cast<Expression>(
                                parameters[0].expression);
                            nameArg = parameters[0].expression->generateCode(module);
                            nameArg = loadIfLValue(module, nameArg, idxAst);
                            if (nameArg && nameArg->getType()->isIntegerTy()
                                && nameArg->getType() != i64Ty) {
                                nameArg = builder->CreateSExtOrTrunc(nameArg, i64Ty,
                                                                     "system.args.idx");
                            }
                        } else {
                            nameArg = loadStringArg(module, parameters[0].expression);
                        }
                        llvm::Value* cstrResult = builder->CreateCall(getFn, {nameArg},
                            std::string("system.") + sysNs + ".get.cstr");

                        auto stringType = CajetaType::of("String");
                        auto stringClass = dynamic_pointer_cast<CajetaClass>(stringType);
                        if (!stringClass || !stringClass->getLlvmType()) {
                            return cstrResult;
                        }

                        llvm::Function* curFn = builder->GetInsertBlock()->getParent();
                        llvm::BasicBlock* nullBB = llvm::BasicBlock::Create(
                            llvmCtx, "system." + sysNs + ".null", curFn);
                        llvm::BasicBlock* wrapBB = llvm::BasicBlock::Create(
                            llvmCtx, "system." + sysNs + ".wrap", curFn);
                        llvm::BasicBlock* joinBB = llvm::BasicBlock::Create(
                            llvmCtx, "system." + sysNs + ".join", curFn);

                        llvm::Value* isNull = builder->CreateICmpEQ(cstrResult,
                            llvm::ConstantPointerNull::get(
                                llvm::cast<llvm::PointerType>(ptrTy)));
                        builder->CreateCondBr(isNull, nullBB, wrapBB);

                        builder->SetInsertPoint(nullBB);
                        llvm::Value* nullStr = llvm::ConstantPointerNull::get(
                            llvm::cast<llvm::PointerType>(ptrTy));
                        builder->CreateBr(joinBB);

                        builder->SetInsertPoint(wrapBB);
                        llvm::Value* sPtr = wrapCStringIntoClassString(
                            module, cstrResult,
                            (std::string("system.") + sysNs).c_str(),
                            /*freeAfterWrap=*/false);
                        llvm::BasicBlock* wrapEndBB = builder->GetInsertBlock();
                        builder->CreateBr(joinBB);

                        builder->SetInsertPoint(joinBB);
                        llvm::PHINode* phi = builder->CreatePHI(ptrTy, 2,
                            "system." + sysNs + ".get");
                        phi->addIncoming(nullStr, nullBB);
                        phi->addIncoming(sPtr, wrapEndBB);

                        setResolvedType(CajetaType::of("String"));
                        return phi;
                    }
                    if (methodCallName == "set" && parameters.size() == 2) {
                        std::string fnName = "__cajeta_" + sysNs + "_set";
                        llvm::Function* setFn = module->getRuntimeFunction(fnName);
                        if (!setFn) return nullptr;
                        llvm::Value* nameArg  = loadStringArg(module, parameters[0].expression);
                        llvm::Value* valueArg = loadStringArg(module, parameters[1].expression);
                        return builder->CreateCall(setFn, {nameArg, valueArg});
                    }
                }
            }

            std::string sysMisspelling = detectSystemUnknownStream(children[0]);
            if (!sysMisspelling.empty()) {
                std::string hint;
                if (module->getFlags().diagHints) {
                    std::vector<std::string> candidates = {
                        "stdout", "stderr", "stdin"
                    };
                    auto suggestions = cajeta::pickSimilar(sysMisspelling, candidates,
                        /*maxDistance=*/3);
                    hint = cajeta::formatDidYouMean(suggestions);
                }
                throw Exception(
                    "no `System." + sysMisspelling
                    + "` — cajeta exposes only `System.stdout`, "
                    "`System.stderr`, and `System.stdin` as I/O streams."
                    + hint,
                    "CAJETA_ERROR_UNKNOWN_SYSTEM_STREAM");
            }
            int streamFd = detectSystemStreamReceiver(children[0]);
            if (streamFd >= 0) {
                llvm::Type* i32Ty = llvm::Type::getInt32Ty(llvmCtx);
                llvm::Type* i64Ty = llvm::Type::getInt64Ty(llvmCtx);
                llvm::Type* ptrTy = llvm::PointerType::get(llvmCtx, 0);

                llvm::Value* streamArg = llvm::ConstantInt::get(i32Ty, streamFd);

                if ((methodCallName == "print" || methodCallName == "println")
                        && parameters.size() >= 2) {
                    const char* runtimeName = methodCallName == "println"
                        ? "__cajeta_logln" : "__cajeta_log";
                    llvm::Function* logFn = module->getRuntimeFunction(runtimeName);
                    if (logFn) {
                        llvm::Value* fmt = loadStringArg(module, parameters[0].expression);
                        size_t argCount = parameters.size() - 1;

                        llvm::Value* argv = builder->CreateAlloca(
                            ptrTy,
                            llvm::ConstantInt::get(i64Ty, argCount),
                            "printtmpl.argv");

                        for (size_t ai = 0; ai < argCount; ++ai) {
                            llvm::Value* raw = loadStringArg(module,
                                parameters[ai + 1].expression);
                            llvm::Type* rawTy = raw->getType();
                            llvm::Value* cstr = nullptr;
                            if (rawTy->isPointerTy()) {
                                cstr = raw;
                            } else if (rawTy->isIntegerTy(1)) {
                                llvm::Value* widened = builder->CreateZExt(raw, i32Ty);
                                llvm::Function* fn = module->getRuntimeFunction("__cajeta_bool_to_str");
                                cstr = builder->CreateCall(fn, {widened},
                                    "printtmpl.bool");
                            } else if (rawTy->isIntegerTy()) {
                                llvm::Value* widened = builder->CreateIntCast(raw, i64Ty, /*isSigned=*/true);
                                llvm::Function* fn = module->getRuntimeFunction("__cajeta_i64_to_str");
                                cstr = builder->CreateCall(fn, {widened},
                                    "printtmpl.i64");
                            } else if (rawTy->isFloatingPointTy()) {
                                llvm::Type* f64Ty = llvm::Type::getDoubleTy(llvmCtx);
                                if (rawTy != f64Ty) {
                                    raw = builder->CreateFPCast(raw, f64Ty);
                                }
                                llvm::Function* fn = module->getRuntimeFunction("__cajeta_f64_to_str");
                                cstr = builder->CreateCall(fn, {raw},
                                    "printtmpl.f64");
                            } else {
                                cstr = llvm::ConstantPointerNull::get(
                                    llvm::cast<llvm::PointerType>(ptrTy));
                            }
                            llvm::Value* slot = builder->CreateGEP(
                                ptrTy, argv,
                                llvm::ConstantInt::get(i64Ty, ai),
                                std::string("printtmpl.slot.") + std::to_string(ai));
                            builder->CreateStore(cstr, slot);
                        }

                        llvm::Value* argcArg = llvm::ConstantInt::get(
                            i64Ty, (uint64_t) argCount);
                        return builder->CreateCall(logFn,
                            {streamArg, fmt, argcArg, argv});
                    }
                }

                if ((methodCallName == "print" || methodCallName == "println")
                        && parameters.size() == 1) {
                    llvm::Value* arg = loadStringArg(module, parameters[0].expression);
                    llvm::Type* argTy = arg->getType();
                    const std::string base = methodCallName == "println"
                        ? "__cajeta_println" : "__cajeta_print";
                    llvm::Function* fn = nullptr;
                    if (argTy->isPointerTy()) {
                        fn = module->getRuntimeFunction(base);
                    } else if (argTy->isIntegerTy(1)) {
                        arg = builder->CreateZExt(arg, i32Ty);
                        fn = module->getRuntimeFunction(base + "_bool");
                    } else if (argTy->isIntegerTy()) {
                        arg = builder->CreateIntCast(arg, i64Ty, /*isSigned=*/true);
                        fn = module->getRuntimeFunction(base + "_i64");
                    } else if (argTy->isFloatingPointTy()) {
                        llvm::Type* f64Ty = llvm::Type::getDoubleTy(llvmCtx);
                        if (argTy != f64Ty) {
                            arg = builder->CreateFPCast(arg, f64Ty);
                        }
                        fn = module->getRuntimeFunction(base + "_f64");
                    }
                    if (fn) {
                        llvm::Value* printResult =
                            builder->CreateCall(fn, {streamArg, arg});
                        if (freshOwnedStringTemp(parameters[0].expression)) {
                            if (auto* cstrCall =
                                    llvm::dyn_cast<llvm::CallInst>(arg)) {
                                llvm::Function* callee =
                                    cstrCall->getCalledFunction();
                                if (callee && callee->getName()
                                        == "__cajeta_string_cstr") {
                                    if (llvm::Function* sdrop =
                                            module->getRuntimeFunction(
                                                "__cajeta_string_drop")) {
                                        builder->CreateCall(sdrop,
                                            {cstrCall->getArgOperand(0)});
                                    }
                                }
                            }
                        }
                        return printResult;
                    }
                }
                if (methodCallName == "printf" && parameters.size() >= 2) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_log");
                    if (!fn) return nullptr;
                    llvm::Value* fmt = loadStringArg(module, parameters[0].expression);
                    auto argsExpr = dynamic_pointer_cast<Expression>(parameters[1].expression);
                    llvm::Value* argsHdr = parameters[1].expression->generateCode(module);
                    if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(argsHdr)) {
                        argsHdr = builder->CreateLoad(a->getAllocatedType(), a);
                    }
                    CajetaArrayPtr arrType;
                    if (argsExpr) {
                        if (!argsExpr->getResolvedType()) argsExpr->resolveTypes(module);
                        arrType = dynamic_pointer_cast<CajetaArray>(argsExpr->getResolvedType());
                    }
                    if (!arrType) {
                        std::string actualType = "?";
                        if (argsExpr && argsExpr->getResolvedType()) {
                            actualType = argsExpr->getResolvedType()->toCanonical();
                        }
                        std::string msg =
                            "System." + std::string(streamFd == 2 ? "stderr" : "stdout")
                            + ".printf expects (String fmt, String[] args); got second "
                            "argument of type `" + actualType
                            + "`. Build a String[] of the substitutions, or use "
                            "concatenation: `System.stdout.println(\"x=\" + value);`.";
                        throw Exception(msg, "CAJETA_ERROR_PRINTF_BAD_ARGS");
                    }
                    llvm::Type* hdrTy = arrType->getLlvmType();
                    llvm::Value* sizePtr = builder->CreateStructGEP(hdrTy, argsHdr,
                        CajetaArray::SIZE_FIELD_INDEX);
                    llvm::Value* count = builder->CreateLoad(i64Ty, sizePtr);
                    // Mask the shared-state sign bit (slice-spec 3.3).
                    count = builder->CreateAnd(count,
                        llvm::ConstantInt::get(i64Ty, 0x7FFFFFFFFFFFFFFFULL));
                    llvm::Value* dataPtr = builder->CreateStructGEP(hdrTy, argsHdr,
                        CajetaArray::DATA_FIELD_INDEX);

                    // A `String[]` slot is sizeof(class String) wide, but writers store an 8-byte
                    // pointer into its first word. GEP through the array's struct shape, not by
                    // pointer stride, so reads land on the same slot boundaries the writes used.
                    CajetaTypePtr elemTy = arrType->getElementType();
                    auto elemClass = std::dynamic_pointer_cast<CajetaClass>(elemTy);
                    bool elemsAreClassString = elemClass
                        && elemClass->getQName()
                        && elemClass->getQName()->getTypeName() == "String"
                        && elemClass->getQName()->getPackageName() == "cajeta.lang";
                    if (elemsAreClassString) {
                        llvm::Type* ptrTy = llvm::PointerType::get(llvmCtx, 0);
                        llvm::Type* i8Ty = llvm::Type::getInt8Ty(llvmCtx);
                        llvm::Type* stringStructTy = elemClass->getLlvmType();
                        if (!stringStructTy
                                || !llvm::isa<llvm::StructType>(stringStructTy)) {
                            return builder->CreateCall(fn, {streamArg, fmt, count, dataPtr});
                        }
                        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
                        llvm::Value* cstrArr = builder->CreateAlloca(
                            ptrTy, count, "printf.cstrs");
                        llvm::BasicBlock* condBB = llvm::BasicBlock::Create(
                            llvmCtx, "printf.loop.cond", parentFn);
                        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(
                            llvmCtx, "printf.loop.body", parentFn);
                        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(
                            llvmCtx, "printf.loop.done", parentFn);
                        llvm::AllocaInst* iSlot = builder->CreateAlloca(
                            i64Ty, nullptr, "printf.i");
                        builder->CreateStore(
                            llvm::ConstantInt::get(i64Ty, 0), iSlot);
                        builder->CreateBr(condBB);
                        builder->SetInsertPoint(condBB);
                        llvm::Value* i = builder->CreateLoad(i64Ty, iSlot, "printf.i.cur");
                        llvm::Value* cmp = builder->CreateICmpULT(i, count, "printf.i.lt");
                        builder->CreateCondBr(cmp, bodyBB, doneBB);
                        builder->SetInsertPoint(bodyBB);
                        llvm::Value* strPtrSlot = builder->CreateGEP(
                            hdrTy, argsHdr,
                            { llvm::ConstantInt::get(i64Ty, 0),
                              llvm::ConstantInt::get(
                                  llvm::Type::getInt32Ty(llvmCtx),
                                  CajetaArray::DATA_FIELD_INDEX),
                              i },
                            "printf.strSlot");
                        llvm::Value* strPtr = builder->CreateLoad(
                            ptrTy, strPtrSlot, "printf.strPtr");
                        llvm::Value* isNull = builder->CreateICmpEQ(strPtr,
                            llvm::ConstantPointerNull::get(
                                llvm::cast<llvm::PointerType>(ptrTy)),
                            "printf.isNull");
                        llvm::BasicBlock* extractBB = llvm::BasicBlock::Create(
                            llvmCtx, "printf.extract", parentFn);
                        llvm::BasicBlock* storeNullBB = llvm::BasicBlock::Create(
                            llvmCtx, "printf.storeNull", parentFn);
                        llvm::BasicBlock* afterBB = llvm::BasicBlock::Create(
                            llvmCtx, "printf.after", parentFn);
                        builder->CreateCondBr(isNull, storeNullBB, extractBB);
                        builder->SetInsertPoint(extractBB);
                        llvm::FunctionType* cstrTy = llvm::FunctionType::get(
                            ptrTy, {ptrTy}, false);
                        llvm::FunctionCallee cstrFn =
                            module->getLlvmModule()->getOrInsertFunction(
                                "__cajeta_string_cstr", cstrTy);
                        llvm::Value* cstr = builder->CreateCall(
                            cstrFn, {strPtr}, "printf.cstr");
                        builder->CreateBr(afterBB);
                        builder->SetInsertPoint(storeNullBB);
                        builder->CreateBr(afterBB);
                        builder->SetInsertPoint(afterBB);
                        llvm::PHINode* finalCstr = builder->CreatePHI(ptrTy, 2, "printf.cstr.final");
                        finalCstr->addIncoming(cstr, extractBB);
                        finalCstr->addIncoming(
                            llvm::ConstantPointerNull::get(
                                llvm::cast<llvm::PointerType>(ptrTy)),
                            storeNullBB);
                        llvm::Value* destSlot = builder->CreateInBoundsGEP(
                            ptrTy, cstrArr, i, "printf.destSlot");
                        builder->CreateStore(finalCstr, destSlot);
                        llvm::Value* iNext = builder->CreateAdd(i,
                            llvm::ConstantInt::get(i64Ty, 1), "printf.i.next");
                        builder->CreateStore(iNext, iSlot);
                        builder->CreateBr(condBB);
                        builder->SetInsertPoint(doneBB);
                        return builder->CreateCall(fn, {streamArg, fmt, count, cstrArr});
                    }
                    return builder->CreateCall(fn, {streamArg, fmt, count, dataPtr});
                }
            }
        }

        // ----- File.<static>(...) intrinsic (cajeta.io.file Phase A) -----
        if (!children.empty()) {
            auto fileId = dynamic_pointer_cast<IdentifierExpression>(children[0]);
            if (fileId && fileId->getTextValue() == "File") {
                auto& cmap = CajetaType::getCanonicalMap();
                auto it = cmap.find("File");
                if (it == cmap.end()) {
                    it = cmap.find("cajeta.io.file.File");
                }
                CajetaClassPtr fileClass;
                if (it != cmap.end()) {
                    fileClass = std::dynamic_pointer_cast<CajetaClass>(it->second);
                }
                bool isOurFile = fileClass && fileClass->getQName()
                    && fileClass->getQName()->toCanonical()
                        == "cajeta.io.file.File";
                if (isOurFile) {
                    llvm::Type* ptrTy = llvm::PointerType::get(llvmCtx, 0);
                    llvm::Type* i32Ty = llvm::Type::getInt32Ty(llvmCtx);
                    llvm::Type* i64Ty = llvm::Type::getInt64Ty(llvmCtx);
                    llvm::Type* i8Ty  = llvm::Type::getInt8Ty(llvmCtx);

                    auto loadPathArg = [&](size_t idx) -> llvm::Value* {
                        return loadStringArg(module, parameters[idx].expression);
                    };

                    auto loadArrayDataPtr = [&](size_t idx) -> llvm::Value* {
                        llvm::Value* arr = loadArrayArg(
                            module, parameters[idx].expression);
                        return builder->CreateInBoundsGEP(i8Ty, arr,
                            llvm::ConstantInt::get(i64Ty, 8),
                            "file.data");
                    };

                    if (methodCallName == "readAllBytes" && parameters.size() == 1) {
                        llvm::Function* fn = module->getRuntimeFunction(
                            "__cajeta_file_read_all");
                        if (fn) {
                            llvm::Value* path = loadPathArg(0);
                            llvm::Value* arr = builder->CreateCall(fn, {path});
                            auto& cmap2 = CajetaType::getCanonicalMap();
                            auto it2 = cmap2.find("int8[]");
                            if (it2 != cmap2.end()) {
                                resolvedType = it2->second;
                            }
                            return arr;
                        }
                    }
                    if (methodCallName == "writeAllBytes" && parameters.size() == 3) {
                        llvm::Function* fn = module->getRuntimeFunction(
                            "__cajeta_file_write_all");
                        if (fn) {
                            llvm::Value* path = loadPathArg(0);
                            llvm::Value* data = loadArrayDataPtr(1);
                            llvm::Value* len  = parameters[2].expression->generateCode(module);
                            if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(len)) {
                                len = builder->CreateLoad(a->getAllocatedType(), a);
                            }
                            if (len && len->getType() != i64Ty
                                    && len->getType()->isIntegerTy()) {
                                len = builder->CreateIntCast(len, i64Ty, true);
                            }
                            return builder->CreateCall(fn, {path, data, len});
                        }
                    }
                    if (methodCallName == "openRead" && parameters.size() == 1) {
                        llvm::Function* openFn = module->getRuntimeFunction(
                            "__cajeta_file_open");
                        if (openFn) {
                            llvm::Value* path = loadPathArg(0);
                            llvm::Value* fd = builder->CreateCall(openFn,
                                {path, llvm::ConstantInt::get(i32Ty, 0)},
                                "file.fd");
                            CajetaClassPtr readerCls;
                            auto& cmap3 = CajetaType::getCanonicalMap();
                            auto rit = cmap3.find("FileReader");
                            if (rit == cmap3.end()) {
                                rit = cmap3.find("cajeta.io.file.FileReader");
                            }
                            if (rit != cmap3.end()) {
                                readerCls = std::dynamic_pointer_cast<CajetaClass>(rit->second);
                            }
                            if (readerCls && readerCls->getLlvmType()
                                    && llvm::isa<llvm::StructType>(readerCls->getLlvmType())) {
                                auto* structTy = llvm::cast<llvm::StructType>(
                                    readerCls->getLlvmType());
                                const llvm::DataLayout& dl =
                                    module->getLlvmModule()->getDataLayout();
                                llvm::Constant* size = llvm::ConstantInt::get(
                                    i64Ty, dl.getTypeAllocSize(structTy));
                                llvm::Value* inst = MemoryManager::createMallocInstruction(
                                    module, size, builder->GetInsertBlock());
                                builder->CreateMemSet(inst,
                                    llvm::ConstantInt::get(i8Ty, 0),
                                    size, llvm::MaybeAlign(8));
                                llvm::Constant* vtableRef =
                                    llvm::ConstantPointerNull::get(
                                        llvm::cast<llvm::PointerType>(ptrTy));
                                if (auto* vt = readerCls->getVirtualTableGlobal()) {
                                    vtableRef = CajetaModule::ensureGlobalInModule(
                                        module->getLlvmModule(), vt);
                                }
                                builder->CreateStore(vtableRef,
                                    builder->CreateStructGEP(structTy, inst, 0,
                                        "reader.vtable_slot"));
                                builder->CreateStore(fd,
                                    builder->CreateStructGEP(structTy, inst, 1,
                                        "reader.fd_slot"));
                                builder->CreateStore(
                                    llvm::ConstantInt::get(i64Ty, 0),
                                    builder->CreateStructGEP(structTy, inst, 2,
                                        "reader.pos_slot"));
                                resolvedReturnsOwnership = true;
                                resolvedType = readerCls;
                                return inst;
                            }
                        }
                    }
                    if ((methodCallName == "open" && parameters.size() == 2)
                            || (methodCallName == "openExclusive"
                                && parameters.size() == 1)) {
                        llvm::Function* openFn = module->getRuntimeFunction(
                            "__cajeta_file_open");
                        if (openFn) {
                            llvm::Value* path = loadPathArg(0);
                            llvm::Value* mode;
                            if (methodCallName == "openExclusive") {
                                mode = llvm::ConstantInt::get(i32Ty, 4);
                            } else {
                                mode = parameters[1].expression->generateCode(module);
                                if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(mode)) {
                                    mode = builder->CreateLoad(a->getAllocatedType(), a);
                                }
                                if (mode && mode->getType() != i32Ty
                                        && mode->getType()->isIntegerTy()) {
                                    mode = builder->CreateIntCast(mode, i32Ty, true);
                                }
                            }
                            llvm::Value* fd = builder->CreateCall(openFn,
                                {path, mode}, "file.fd");
                            if (fileClass && fileClass->getLlvmType()
                                    && llvm::isa<llvm::StructType>(fileClass->getLlvmType())) {
                                auto* structTy = llvm::cast<llvm::StructType>(
                                    fileClass->getLlvmType());
                                const llvm::DataLayout& dl =
                                    module->getLlvmModule()->getDataLayout();
                                llvm::Constant* size = llvm::ConstantInt::get(
                                    i64Ty, dl.getTypeAllocSize(structTy));
                                llvm::Value* inst = MemoryManager::createMallocInstruction(
                                    module, size, builder->GetInsertBlock());
                                builder->CreateMemSet(inst,
                                    llvm::ConstantInt::get(i8Ty, 0),
                                    size, llvm::MaybeAlign(8));
                                llvm::Constant* vtableRef =
                                    llvm::ConstantPointerNull::get(
                                        llvm::cast<llvm::PointerType>(ptrTy));
                                if (auto* vt = fileClass->getVirtualTableGlobal()) {
                                    vtableRef = CajetaModule::ensureGlobalInModule(
                                        module->getLlvmModule(), vt);
                                }
                                builder->CreateStore(vtableRef,
                                    builder->CreateStructGEP(structTy, inst, 0,
                                        "file.vtable_slot"));
                                builder->CreateStore(fd,
                                    builder->CreateStructGEP(structTy, inst, 1,
                                        "file.fd_slot"));
                                builder->CreateStore(
                                    llvm::ConstantInt::get(i64Ty, 0),
                                    builder->CreateStructGEP(structTy, inst, 2,
                                        "file.pos_slot"));
                                resolvedReturnsOwnership = true;
                                resolvedType = fileClass;
                                return inst;
                            }
                        }
                    }
                    if (methodCallName == "openWrite" && parameters.size() == 2) {
                        llvm::Function* openFn = module->getRuntimeFunction(
                            "__cajeta_file_open");
                        if (openFn) {
                            llvm::Value* path = loadPathArg(0);
                            llvm::Value* mode = parameters[1].expression->generateCode(module);
                            if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(mode)) {
                                mode = builder->CreateLoad(a->getAllocatedType(), a);
                            }
                            if (mode && mode->getType() != i32Ty
                                    && mode->getType()->isIntegerTy()) {
                                mode = builder->CreateIntCast(mode, i32Ty, true);
                            }
                            llvm::Value* fd = builder->CreateCall(openFn,
                                {path, mode}, "file.fd");
                            CajetaClassPtr writerCls;
                            auto& cmap4 = CajetaType::getCanonicalMap();
                            auto wit = cmap4.find("FileWriter");
                            if (wit == cmap4.end()) {
                                wit = cmap4.find("cajeta.io.file.FileWriter");
                            }
                            if (wit != cmap4.end()) {
                                writerCls = std::dynamic_pointer_cast<CajetaClass>(wit->second);
                            }
                            if (writerCls && writerCls->getLlvmType()
                                    && llvm::isa<llvm::StructType>(writerCls->getLlvmType())) {
                                auto* structTy = llvm::cast<llvm::StructType>(
                                    writerCls->getLlvmType());
                                const llvm::DataLayout& dl =
                                    module->getLlvmModule()->getDataLayout();
                                llvm::Constant* size = llvm::ConstantInt::get(
                                    i64Ty, dl.getTypeAllocSize(structTy));
                                llvm::Value* inst = MemoryManager::createMallocInstruction(
                                    module, size, builder->GetInsertBlock());
                                builder->CreateMemSet(inst,
                                    llvm::ConstantInt::get(i8Ty, 0),
                                    size, llvm::MaybeAlign(8));
                                llvm::Constant* vtableRef =
                                    llvm::ConstantPointerNull::get(
                                        llvm::cast<llvm::PointerType>(ptrTy));
                                if (auto* vt = writerCls->getVirtualTableGlobal()) {
                                    vtableRef = CajetaModule::ensureGlobalInModule(
                                        module->getLlvmModule(), vt);
                                }
                                builder->CreateStore(vtableRef,
                                    builder->CreateStructGEP(structTy, inst, 0,
                                        "writer.vtable_slot"));
                                builder->CreateStore(fd,
                                    builder->CreateStructGEP(structTy, inst, 1,
                                        "writer.fd_slot"));
                                builder->CreateStore(
                                    llvm::ConstantInt::get(i64Ty, 0),
                                    builder->CreateStructGEP(structTy, inst, 2,
                                        "writer.pos_slot"));
                                resolvedReturnsOwnership = true;
                                resolvedType = writerCls;
                                return inst;
                            }
                        }
                    }
                }
            }
        }

        // ----- TcpStream.connect / TcpListener.bind static intrinsic (NET-1.3/1.4) -----
        // SocketAddress layout: { vtable@0, ip@1 (IpAddress*), port@2 (i32) }.
        // IpAddress layout: { vtable@0, family@1 (i32 ordinal), octets@2 (int8[]*) }.
        if (!children.empty()) {
            auto netId = dynamic_pointer_cast<IdentifierExpression>(children[0]);
            std::string netName = netId ? netId->getTextValue() : "";
            bool wantStream   = (netName == "TcpStream");
            bool wantListener = (netName == "TcpListener");
            // ----- UdpSocket / socket-option intrinsic (b2) -----
            bool wantUdp      = (netName == "UdpSocket");
            if (wantStream || wantListener || wantUdp) {
                auto& cmapN = CajetaType::getCanonicalMap();
                std::string canonKey = wantStream ? "cajeta.io.net.TcpStream"
                                     : wantListener ? "cajeta.io.net.TcpListener"
                                                    : "cajeta.io.net.UdpSocket";
                auto itN = cmapN.find(netName);
                if (itN == cmapN.end()) itN = cmapN.find(canonKey);
                CajetaClassPtr netCls;
                if (itN != cmapN.end()) {
                    netCls = std::dynamic_pointer_cast<CajetaClass>(itN->second);
                }
                bool isOurNet = netCls && netCls->getQName()
                    && netCls->getQName()->toCanonical() == canonKey;
                bool isConnect = wantStream && methodCallName == "connect"
                                 && parameters.size() == 1;
                bool isConnectAsync = wantStream && methodCallName == "connectAsyncNative"
                                 && parameters.size() == 1;
                bool isBind    = wantListener && methodCallName == "bind"
                                 && parameters.size() == 1;
                bool isBindBacklog = wantListener
                                 && methodCallName == "bindWithBacklog"
                                 && parameters.size() == 2;
                if (isBindBacklog) isBind = true;
                bool isUdpBind = wantUdp && methodCallName == "bind"
                                 && parameters.size() == 1;
                if (isOurNet && (isConnect || isConnectAsync || isBind || isUdpBind)) {
                    llvm::Type* ptrTy = llvm::PointerType::get(llvmCtx, 0);
                    llvm::Type* i32Ty = llvm::Type::getInt32Ty(llvmCtx);
                    llvm::Type* i64Ty = llvm::Type::getInt64Ty(llvmCtx);
                    llvm::Type* i8Ty  = llvm::Type::getInt8Ty(llvmCtx);

                    llvm::Function* sockFn = module->getRuntimeFunction("__cajeta_net_socket");
                    llvm::Function* packFn = module->getRuntimeFunction("__cajeta_net_sockaddr_pack");
                    llvm::Function* connFn = module->getRuntimeFunction("__cajeta_net_connect");
                    llvm::Function* bindFn = module->getRuntimeFunction("__cajeta_net_bind");
                    llvm::Function* listenFn = module->getRuntimeFunction("__cajeta_net_listen");
                    llvm::Function* reuseFn = module->getRuntimeFunction("__cajeta_net_set_reuseaddr");
                    llvm::Function* closeFn = module->getRuntimeFunction("__cajeta_net_close");
                    llvm::Function* throwFn = module->getRuntimeFunction("__cajeta_throw");
                    llvm::Function* setNbFn   = module->getRuntimeFunction("__cajeta_net_set_nonblocking");
                    llvm::Function* inProgFn  = module->getRuntimeFunction("__cajeta_net_is_in_progress");
                    llvm::Function* awaitWrFn = module->getRuntimeFunction("__cajeta_net_await_writable");
                    llvm::Function* connResFn = module->getRuntimeFunction("__cajeta_net_connect_result");
                    llvm::Function* lastErrFn = module->getRuntimeFunction("__cajeta_net_last_error");

                    CajetaClassPtr saCls;
                    CajetaClassPtr ipCls;
                    {
                        auto sIt = cmapN.find("cajeta.io.net.SocketAddress");
                        if (sIt == cmapN.end()) sIt = cmapN.find("SocketAddress");
                        if (sIt != cmapN.end())
                            saCls = std::dynamic_pointer_cast<CajetaClass>(sIt->second);
                        auto iIt = cmapN.find("cajeta.io.net.IpAddress");
                        if (iIt == cmapN.end()) iIt = cmapN.find("IpAddress");
                        if (iIt != cmapN.end())
                            ipCls = std::dynamic_pointer_cast<CajetaClass>(iIt->second);
                    }

                    if (sockFn && packFn
                            && (isConnect ? (connFn != nullptr)
                                : isConnectAsync ? (connFn && setNbFn && inProgFn
                                                    && awaitWrFn && connResFn)
                                : isUdpBind ? (bindFn != nullptr)
                                            : (bindFn && listenFn))
                            && saCls && ipCls
                            && llvm::isa<llvm::StructType>(saCls->getLlvmType())
                            && llvm::isa<llvm::StructType>(ipCls->getLlvmType())) {
                        auto* saTy = llvm::cast<llvm::StructType>(saCls->getLlvmType());
                        auto* ipTy = llvm::cast<llvm::StructType>(ipCls->getLlvmType());

                        llvm::Value* sa = parameters[0].expression->generateCode(module);
                        if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(sa)) {
                            sa = builder->CreateLoad(a->getAllocatedType(), a);
                        }
                        llvm::Value* ipSlot = builder->CreateStructGEP(saTy, sa, 1, "na.ip_slot");
                        llvm::Value* ip = builder->CreateLoad(ptrTy, ipSlot, "na.ip");
                        llvm::Value* portSlot = builder->CreateStructGEP(saTy, sa, 2, "na.port_slot");
                        llvm::Value* port = builder->CreateLoad(i32Ty, portSlot, "na.port");
                        llvm::Value* famSlot = builder->CreateStructGEP(ipTy, ip, 1, "na.fam_slot");
                        llvm::Value* family = builder->CreateLoad(i32Ty, famSlot, "na.family");
                        llvm::Value* octSlot = builder->CreateStructGEP(ipTy, ip, 2, "na.oct_slot");
                        llvm::Value* octArr = builder->CreateLoad(ptrTy, octSlot, "na.octets_arr");
                        llvm::Value* octData = builder->CreateInBoundsGEP(
                            i8Ty, octArr, llvm::ConstantInt::get(i64Ty, 8), "na.octets");

                        llvm::Value* scratch = builder->CreateAlloca(
                            llvm::ArrayType::get(i8Ty, 128), nullptr, "na.scratch");
                        llvm::Value* addrlen = builder->CreateCall(packFn,
                            {family, octData, port, scratch,
                             llvm::ConstantInt::get(i32Ty, 128)}, "na.addrlen");

                        llvm::Value* isV4 = builder->CreateICmpEQ(family,
                            llvm::ConstantInt::get(i32Ty, 0), "na.isV4");
                        llvm::Value* nativeFamily = builder->CreateSelect(isV4,
                            llvm::ConstantInt::get(i32Ty, 2),
                            llvm::ConstantInt::get(i32Ty, 23), "na.nativeFamily");
                        int32_t sockType = isUdpBind ? 2 : 1;
                        llvm::Value* fd = builder->CreateCall(sockFn,
                            {nativeFamily, llvm::ConstantInt::get(i32Ty, sockType),
                             llvm::ConstantInt::get(i32Ty, 0)}, "na.fd");

                        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
                        auto throwIf = [&](llvm::Value* cond, uint64_t tag,
                                           llvm::Value* fdToClose) {
                            llvm::BasicBlock* failBB = llvm::BasicBlock::Create(
                                llvmCtx, "na.fail", parentFn);
                            llvm::BasicBlock* okBB = llvm::BasicBlock::Create(
                                llvmCtx, "na.ok", parentFn);
                            builder->CreateCondBr(cond, failBB, okBB);
                            builder->SetInsertPoint(failBB);
                            if (fdToClose && closeFn) {
                                builder->CreateCall(closeFn, {fdToClose});
                            }
                            if (throwFn) {
                                llvm::Value* tagPtr = builder->CreateIntToPtr(
                                    llvm::ConstantInt::get(i64Ty, tag), ptrTy);
                                builder->CreateCall(throwFn, {tagPtr});
                            }
                            builder->CreateUnreachable();
                            builder->SetInsertPoint(okBB);
                        };

                        throwIf(builder->CreateICmpSLE(addrlen,
                            llvm::ConstantInt::get(i32Ty, 0)), 0x100, nullptr);

                        throwIf(builder->CreateICmpSLT(fd,
                            llvm::ConstantInt::get(i32Ty, 0)), 0x101, nullptr);

                        if (isConnect) {
                            llvm::Value* rc = builder->CreateCall(connFn,
                                {fd, scratch, addrlen}, "na.connect_rc");
                            throwIf(builder->CreateICmpNE(rc,
                                llvm::ConstantInt::get(i32Ty, 0)), 0x102, fd);
                        } else if (isConnectAsync) {
                            // ----- non-blocking connect (NET-3.3 connectAsync) -----
                            // is_in_progress() reads the errno connect just set, so nothing may syscall
                            // between the two calls - only the icmp / branch below.
                            builder->CreateCall(setNbFn,
                                {fd, llvm::ConstantInt::get(i32Ty, 1)});
                            llvm::Value* rc = builder->CreateCall(connFn,
                                {fd, scratch, addrlen}, "na.aconnect_rc");
                            llvm::Value* immediate = builder->CreateICmpEQ(rc,
                                llvm::ConstantInt::get(i32Ty, 0), "na.aconnect_now");
                            llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(
                                llvmCtx, "na.aconnect_done", parentFn);
                            llvm::BasicBlock* pendingBB = llvm::BasicBlock::Create(
                                llvmCtx, "na.aconnect_pending", parentFn);
                            builder->CreateCondBr(immediate, doneBB, pendingBB);

                            builder->SetInsertPoint(pendingBB);
                            llvm::Value* inProg = builder->CreateCall(inProgFn,
                                {}, "na.aconnect_inprog");
                            llvm::Value* isInProg = builder->CreateICmpNE(inProg,
                                llvm::ConstantInt::get(i32Ty, 0), "na.aconnect_isinprog");
                            llvm::BasicBlock* awaitBB = llvm::BasicBlock::Create(
                                llvmCtx, "na.aconnect_await", parentFn);
                            llvm::BasicBlock* hardFailBB = llvm::BasicBlock::Create(
                                llvmCtx, "na.aconnect_hardfail", parentFn);
                            builder->CreateCondBr(isInProg, awaitBB, hardFailBB);

                            builder->SetInsertPoint(hardFailBB);
                            llvm::Value* hardErr = nullptr;
                            if (lastErrFn) {
                                hardErr = builder->CreateCall(lastErrFn, {},
                                    "na.aconnect_lasterr");
                            }
                            if (closeFn) builder->CreateCall(closeFn, {fd});
                            if (throwFn) {
                                llvm::Value* tagVal;
                                if (hardErr) {
                                    llvm::Value* e64 = builder->CreateSExt(
                                        hardErr, i64Ty);
                                    tagVal = builder->CreateAdd(
                                        llvm::ConstantInt::get(i64Ty, 0x200), e64);
                                } else {
                                    tagVal = llvm::ConstantInt::get(i64Ty,
                                        0x200 + 99);
                                }
                                llvm::Value* tagPtr = builder->CreateIntToPtr(
                                    tagVal, ptrTy);
                                builder->CreateCall(throwFn, {tagPtr});
                            }
                            builder->CreateUnreachable();

                            builder->SetInsertPoint(awaitBB);
                            builder->CreateCall(awaitWrFn, {fd});
                            llvm::Value* soerr = builder->CreateCall(connResFn,
                                {fd}, "na.aconnect_soerr");
                            llvm::Value* soOk = builder->CreateICmpEQ(soerr,
                                llvm::ConstantInt::get(i32Ty, 0), "na.aconnect_sook");
                            llvm::BasicBlock* soFailBB = llvm::BasicBlock::Create(
                                llvmCtx, "na.aconnect_sofail", parentFn);
                            builder->CreateCondBr(soOk, doneBB, soFailBB);

                            builder->SetInsertPoint(soFailBB);
                            if (closeFn) builder->CreateCall(closeFn, {fd});
                            if (throwFn) {
                                llvm::Value* so64 = builder->CreateSExt(
                                    soerr, i64Ty);
                                llvm::Value* tagVal = builder->CreateAdd(
                                    llvm::ConstantInt::get(i64Ty, 0x200), so64);
                                llvm::Value* tagPtr = builder->CreateIntToPtr(
                                    tagVal, ptrTy);
                                builder->CreateCall(throwFn, {tagPtr});
                            }
                            builder->CreateUnreachable();

                            builder->SetInsertPoint(doneBB);
                        } else if (isUdpBind) {
                            // ----- UdpSocket.bind (b2): bind only, no listen -----
                            if (reuseFn) {
                                builder->CreateCall(reuseFn,
                                    {fd, llvm::ConstantInt::get(i32Ty, 1)});
                            }
                            llvm::Value* brc = builder->CreateCall(bindFn,
                                {fd, scratch, addrlen}, "na.udp_bind_rc");
                            throwIf(builder->CreateICmpNE(brc,
                                llvm::ConstantInt::get(i32Ty, 0)), 0x105, fd);
                        } else {
                            if (reuseFn) {
                                builder->CreateCall(reuseFn,
                                    {fd, llvm::ConstantInt::get(i32Ty, 1)});
                            }
                            llvm::Value* brc = builder->CreateCall(bindFn,
                                {fd, scratch, addrlen}, "na.bind_rc");
                            throwIf(builder->CreateICmpNE(brc,
                                llvm::ConstantInt::get(i32Ty, 0)), 0x103, fd);
                            llvm::Value* backlog =
                                llvm::ConstantInt::get(i32Ty, 128);
                            if (isBindBacklog) {
                                llvm::Value* b = loadIfLValue(module,
                                    parameters[1].expression->generateCode(module),
                                    dynamic_pointer_cast<Expression>(
                                        parameters[1].expression));
                                if (b && b->getType()->isIntegerTy()) {
                                    backlog = builder->CreateIntCast(
                                        b, i32Ty, /*isSigned=*/true);
                                }
                            }
                            llvm::Value* lrc = builder->CreateCall(listenFn,
                                {fd, backlog}, "na.listen_rc");
                            throwIf(builder->CreateICmpNE(lrc,
                                llvm::ConstantInt::get(i32Ty, 0)), 0x104, fd);
                        }

                        if (netCls->getLlvmType()
                                && llvm::isa<llvm::StructType>(netCls->getLlvmType())) {
                            auto* outTy = llvm::cast<llvm::StructType>(netCls->getLlvmType());
                            const llvm::DataLayout& dl =
                                module->getLlvmModule()->getDataLayout();
                            llvm::Constant* size = llvm::ConstantInt::get(
                                i64Ty, dl.getTypeAllocSize(outTy));
                            llvm::Value* inst = MemoryManager::createMallocInstruction(
                                module, size, builder->GetInsertBlock());
                            builder->CreateMemSet(inst,
                                llvm::ConstantInt::get(i8Ty, 0),
                                size, llvm::MaybeAlign(8));
                            llvm::Constant* vtableRef =
                                llvm::ConstantPointerNull::get(
                                    llvm::cast<llvm::PointerType>(ptrTy));
                            if (auto* vt = netCls->getVirtualTableGlobal()) {
                                vtableRef = CajetaModule::ensureGlobalInModule(
                                    module->getLlvmModule(), vt);
                            }
                            builder->CreateStore(vtableRef,
                                builder->CreateStructGEP(outTy, inst, 0, "na.vtable_slot"));
                            builder->CreateStore(fd,
                                builder->CreateStructGEP(outTy, inst, 1, "na.fd_slot"));
                            resolvedType = netCls;
                            return inst;
                        }
                    }
                }
            }
        }

        // ----- Math.<fn>(...) intrinsic -----
        if (!children.empty()) {
            auto mathId = dynamic_pointer_cast<IdentifierExpression>(children[0]);
            if (mathId && mathId->getTextValue() == "Math") {
                llvm::Type* i64Ty = llvm::Type::getInt64Ty(llvmCtx);
                llvm::Type* f64Ty = llvm::Type::getDoubleTy(llvmCtx);
                auto loadArg = [&](size_t i) -> llvm::Value* {
                    auto& p = parameters[i].expression;
                    llvm::Value* v = p->generateCode(module);
                    auto ast = dynamic_pointer_cast<Expression>(p);
                    if (ast && !ast->getResolvedType()) {
                        ast->resolveTypes(module);
                    }
                    return loadIfLValue(module, v, ast);
                };
                auto toF64 = [&](llvm::Value* v) {
                    if (v->getType()->isIntegerTy()) return builder->CreateSIToFP(v, f64Ty);
                    if (v->getType() != f64Ty) return builder->CreateFPCast(v, f64Ty);
                    return v;
                };
                auto toI64 = [&](llvm::Value* v) {
                    if (v->getType()->isIntegerTy()) {
                        return v->getType() == i64Ty
                            ? v : builder->CreateIntCast(v, i64Ty, /*isSigned=*/true);
                    }
                    return v;
                };
                llvm::Module* lm = module->getLlvmModule();
                if (methodCallName == "abs" && parameters.size() == 1) {
                    llvm::Value* x = loadArg(0);
                    if (x->getType()->isFloatingPointTy()) {
                        llvm::Function* fn = llvm::Intrinsic::getOrInsertDeclaration(
                            lm, llvm::Intrinsic::fabs, {x->getType()});
                        return builder->CreateCall(fn, {x});
                    }
                    x = toI64(x);
                    llvm::Function* fn = llvm::Intrinsic::getOrInsertDeclaration(
                        lm, llvm::Intrinsic::abs, {i64Ty});
                    return builder->CreateCall(fn, {x, llvm::ConstantInt::getFalse(llvmCtx)});
                }
                if ((methodCallName == "max" || methodCallName == "min")
                        && parameters.size() == 2) {
                    llvm::Value* a = loadArg(0);
                    llvm::Value* b = loadArg(1);
                    bool isFp = a->getType()->isFloatingPointTy()
                             || b->getType()->isFloatingPointTy();
                    if (isFp) {
                        a = toF64(a);
                        b = toF64(b);
                        llvm::Intrinsic::ID id = methodCallName == "max"
                            ? llvm::Intrinsic::maxnum : llvm::Intrinsic::minnum;
                        llvm::Function* fn = llvm::Intrinsic::getOrInsertDeclaration(lm, id, {f64Ty});
                        return builder->CreateCall(fn, {a, b});
                    }
                    a = toI64(a);
                    b = toI64(b);
                    llvm::Intrinsic::ID id = methodCallName == "max"
                        ? llvm::Intrinsic::smax : llvm::Intrinsic::smin;
                    llvm::Function* fn = llvm::Intrinsic::getOrInsertDeclaration(lm, id, {i64Ty});
                    return builder->CreateCall(fn, {a, b});
                }
                if (methodCallName == "sqrt" && parameters.size() == 1) {
                    llvm::Value* x = toF64(loadArg(0));
                    llvm::Function* fn = llvm::Intrinsic::getOrInsertDeclaration(
                        lm, llvm::Intrinsic::sqrt, {f64Ty});
                    return builder->CreateCall(fn, {x});
                }
                if (methodCallName == "pow" && parameters.size() == 2) {
                    llvm::Value* x = toF64(loadArg(0));
                    llvm::Value* y = toF64(loadArg(1));
                    llvm::Function* fn = llvm::Intrinsic::getOrInsertDeclaration(
                        lm, llvm::Intrinsic::pow, {f64Ty});
                    return builder->CreateCall(fn, {x, y});
                }
                if (methodCallName == "atan2" && parameters.size() == 2) {
                    llvm::Value* y = toF64(loadArg(0));
                    llvm::Value* x = toF64(loadArg(1));
                    llvm::Function* fn = llvm::Intrinsic::getOrInsertDeclaration(
                        lm, llvm::Intrinsic::atan2, {f64Ty});
                    return builder->CreateCall(fn, {y, x});
                }
                if (methodCallName == "floor" && parameters.size() == 1) {
                    llvm::Value* x = toF64(loadArg(0));
                    llvm::Function* fn = llvm::Intrinsic::getOrInsertDeclaration(
                        lm, llvm::Intrinsic::floor, {f64Ty});
                    return builder->CreateCall(fn, {x});
                }
                if (methodCallName == "ceil" && parameters.size() == 1) {
                    llvm::Value* x = toF64(loadArg(0));
                    llvm::Function* fn = llvm::Intrinsic::getOrInsertDeclaration(
                        lm, llvm::Intrinsic::ceil, {f64Ty});
                    return builder->CreateCall(fn, {x});
                }
                if (methodCallName == "round" && parameters.size() == 1) {
                    // Java's Math.round(double) is (long)floor(x + 0.5), ties toward +inf, NOT
                    // llvm.round, which ties away from zero: they disagree on negative .5 values
                    // (round(-2.5) is -2 in Java, -3 through llvm.round).
                    llvm::Value* x = toF64(loadArg(0));
                    llvm::Value* half = llvm::ConstantFP::get(f64Ty, 0.5);
                    llvm::Value* shifted = builder->CreateFAdd(x, half);
                    llvm::Function* fn = llvm::Intrinsic::getOrInsertDeclaration(
                        lm, llvm::Intrinsic::floor, {f64Ty});
                    llvm::Value* rounded = builder->CreateCall(fn, {shifted});
                    return builder->CreateFPToSI(rounded, i64Ty);
                }
                struct UnaryFn { const char* name; llvm::Intrinsic::ID id; };
                static const UnaryFn unaryFns[] = {
                    {"sin",   llvm::Intrinsic::sin},
                    {"cos",   llvm::Intrinsic::cos},
                    {"asin",  llvm::Intrinsic::asin},
                    {"acos",  llvm::Intrinsic::acos},
                    {"atan",  llvm::Intrinsic::atan},
                    {"log",   llvm::Intrinsic::log},
                    {"log10", llvm::Intrinsic::log10},
                    {"exp",   llvm::Intrinsic::exp},
                    {"exp2",  llvm::Intrinsic::exp2},
                };
                for (const auto& u : unaryFns) {
                    if (methodCallName == u.name && parameters.size() == 1) {
                        llvm::Value* x = toF64(loadArg(0));
                        llvm::Function* fn = llvm::Intrinsic::getOrInsertDeclaration(lm, u.id, {f64Ty});
                        return builder->CreateCall(fn, {x});
                    }
                }
                if (methodCallName == "tan" && parameters.size() == 1) {
                    llvm::Value* x = toF64(loadArg(0));
                    llvm::Function* sinFn = llvm::Intrinsic::getOrInsertDeclaration(
                        lm, llvm::Intrinsic::sin, {f64Ty});
                    llvm::Function* cosFn = llvm::Intrinsic::getOrInsertDeclaration(
                        lm, llvm::Intrinsic::cos, {f64Ty});
                    return builder->CreateFDiv(
                        builder->CreateCall(sinFn, {x}),
                        builder->CreateCall(cosFn, {x}));
                }
            }
        }

        // ----- Integer/Long/Double/Boolean/String static-namespace intrinsics -----
        if (!children.empty()) {
            auto idExpr = dynamic_pointer_cast<IdentifierExpression>(children[0]);
            if (idExpr) {
                const std::string& ns = idExpr->getTextValue();
                llvm::Type* i64Ty = llvm::Type::getInt64Ty(llvmCtx);
                llvm::Type* i32Ty = llvm::Type::getInt32Ty(llvmCtx);
                llvm::Type* f64Ty = llvm::Type::getDoubleTy(llvmCtx);
                auto loadStr = [&](size_t i) {
                    return loadStringArg(module, parameters[i].expression);
                };
                auto loadValue = [&](size_t i) {
                    auto& p = parameters[i].expression;
                    llvm::Value* v = p->generateCode(module);
                    auto ast = dynamic_pointer_cast<Expression>(p);
                    if (ast && !ast->getResolvedType()) {
                        ast->resolveTypes(module);
                    }
                    return loadIfLValue(module, v, ast);
                };
                if (ns == "Cajeta" && methodCallName == "dropCount" && parameters.empty()) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_drop_count_get");
                    return builder->CreateCall(fn, {});
                }
                if (ns == "Cajeta" && methodCallName == "dropCountReset" && parameters.empty()) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_drop_count_reset");
                    return builder->CreateCall(fn, {});
                }
                if (ns == "Cajeta" && methodCallName == "dropChainHeadAllocLine" && parameters.empty()) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_drop_chain_head_alloc_line");
                    return builder->CreateCall(fn, {});
                }
                if (ns == "Cajeta" && methodCallName == "dropChainHeadAllocFile" && parameters.empty()) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_drop_chain_head_alloc_file");
                    return builder->CreateCall(fn, {});
                }
                if (ns == "Cajeta" && methodCallName == "dumpDropChain" && parameters.empty()) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_dump_drop_chain");
                    return builder->CreateCall(fn, {});
                }
                if (ns == "Cajeta" && methodCallName == "runAtExit" && parameters.empty()) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_run_atexit_handlers");
                    return builder->CreateCall(fn, {});
                }
                if (ns == "Cajeta" && methodCallName == "lockNew" && parameters.empty()) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_lock_new");
                    return builder->CreateCall(fn, {});
                }
                if (ns == "Cajeta" && methodCallName == "lockAcquire" && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_lock_acquire");
                    llvm::Value* h = loadValue(0);
                    return builder->CreateCall(fn, {h});
                }
                if (ns == "Cajeta" && methodCallName == "lockRelease" && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_lock_release");
                    llvm::Value* h = loadValue(0);
                    return builder->CreateCall(fn, {h});
                }
                if (ns == "Cajeta" && methodCallName == "lockTryAcquire" && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_lock_try_acquire");
                    llvm::Value* h = loadValue(0);
                    return builder->CreateCall(fn, {h});
                }
                if (ns == "Cajeta" && methodCallName == "lockDestroy" && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_lock_destroy");
                    llvm::Value* h = loadValue(0);
                    return builder->CreateCall(fn, {h});
                }
                if (ns == "Cajeta" && methodCallName == "fiberLocalPush" && parameters.size() == 2) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_fiber_local_push");
                    return builder->CreateCall(fn, {loadValue(0), loadValue(1)});
                }
                if (ns == "Cajeta" && methodCallName == "fiberLocalPop" && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_fiber_local_pop");
                    return builder->CreateCall(fn, {loadValue(0)});
                }
                if (ns == "Cajeta" && methodCallName == "fiberLocalGet" && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_fiber_local_get");
                    return builder->CreateCall(fn, {loadValue(0)});
                }
                if (ns == "Cajeta" && methodCallName == "fiberLocalIsBound" && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_fiber_local_is_bound");
                    return builder->CreateCall(fn, {loadValue(0)});
                }
                if (ns == "Cajeta" && methodCallName == "fiberContextCapture" && parameters.empty()) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_fiber_context_capture");
                    return builder->CreateCall(fn, {});
                }
                if (ns == "Cajeta" && methodCallName == "fiberContextInstall" && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_fiber_context_install");
                    return builder->CreateCall(fn, {loadValue(0)});
                }
                if (ns == "Cajeta" && methodCallName == "fiberContextFree" && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_fiber_context_free");
                    return builder->CreateCall(fn, {loadValue(0)});
                }
                // Low-level memory / bit intrinsics: an array's data lives at header+8 (the
                // count word precedes it, the same ABI the @Native bridge uses).
                if (ns == "Cajeta" && methodCallName == "loadU64" && parameters.size() == 2) {
                    auto* i8Ty = builder->getInt8Ty();
                    auto* i64Ty = builder->getInt64Ty();
                    llvm::Value* hdr = loadValue(0);
                    llvm::Value* off = loadValue(1);
                    llvm::Value* data = builder->CreateGEP(
                        i8Ty, hdr, builder->getInt64(8), "buf_data");
                    llvm::Value* eltPtr = builder->CreateGEP(
                        i8Ty, data, off, "buf_word_ptr");
                    llvm::LoadInst* ld = builder->CreateLoad(i64Ty, eltPtr, "buf_word");
                    ld->setAlignment(llvm::Align(1));
                    return ld;
                }
                if (ns == "Cajeta" && methodCallName == "storeU64" && parameters.size() == 3) {
                    auto* i8Ty = builder->getInt8Ty();
                    llvm::Value* hdr = loadValue(0);
                    llvm::Value* off = loadValue(1);
                    llvm::Value* val = loadValue(2);
                    llvm::Value* data = builder->CreateGEP(
                        i8Ty, hdr, builder->getInt64(8), "buf_data");
                    llvm::Value* eltPtr = builder->CreateGEP(
                        i8Ty, data, off, "buf_word_ptr");
                    llvm::StoreInst* st = builder->CreateStore(val, eltPtr);
                    st->setAlignment(llvm::Align(1));
                    return st;
                }
                if (ns == "Cajeta" && methodCallName == "hashBytes"
                        && (parameters.size() == 2 || parameters.size() == 3)) {
                    auto* i8Ty = builder->getInt8Ty();
                    llvm::Value* hdr = loadValue(0);
                    const bool hasOff = parameters.size() == 3;
                    llvm::Value* len = loadValue(hasOff ? 2 : 1);
                    llvm::Value* data = builder->CreateGEP(
                        i8Ty, hdr, builder->getInt64(8), "hash_data");
                    if (hasOff) {
                        llvm::Value* off = loadValue(1);
                        data = builder->CreateGEP(i8Ty, data, off, "hash_win");
                    }
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_hash_bytes");
                    resolvedType = CajetaType::of("int64");
                    return builder->CreateCall(fn, {data, len});
                }
                if (ns == "Cajeta" && methodCallName == "allocBytes" && parameters.size() == 1) {
                    auto* i64Ty = builder->getInt64Ty();
                    llvm::Value* count = loadValue(0);
                    if (count->getType() != i64Ty) {
                        count = builder->CreateIntCast(count, i64Ty, /*isSigned=*/true);
                    }
                    CajetaTypePtr i8 = CajetaType::of("int8");
                    auto arrTy = std::make_shared<CajetaArray>(module, i8);
                    module->getStructures()[arrTy->toCanonical()] =
                        std::static_pointer_cast<CajetaClass>(arrTy);
                    const llvm::DataLayout& dl = module->getLlvmModule()->getDataLayout();
                    llvm::Value* headerSize = builder->getInt64(
                        dl.getTypeAllocSize(arrTy->getLlvmType()));
                    llvm::Value* elemSize = builder->getInt64(
                        dl.getTypeAllocSize(arrTy->getElementLlvmType(&llvmCtx)));
                    llvm::Function* allocFn =
                        module->getRuntimeFunction("__cajeta_new_array_header_uninit");
                    resolvedType = arrTy;
                    return builder->CreateCall(allocFn, {headerSize, elemSize, count});
                }
                if (ns == "Cajeta" && methodCallName == "stringSlice" && parameters.size() == 3) {
                    auto* i32Ty = builder->getInt32Ty();
                    llvm::Value* s = loadValue(0);
                    llvm::Value* b = loadValue(1);
                    if (b->getType() != i32Ty) b = builder->CreateIntCast(b, i32Ty, true);
                    llvm::Value* l = loadValue(2);
                    if (l->getType() != i32Ty) l = builder->CreateIntCast(l, i32Ty, true);
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_string_slice");
                    resolvedType = CajetaType::of("String", "cajeta.lang");
                    return builder->CreateCall(fn, {s, b, l});
                }
                if (ns == "Cajeta" && methodCallName == "boundsFail" && parameters.size() == 2) {
                    llvm::Value* i = loadValue(0);
                    if (i->getType() != i64Ty) i = builder->CreateIntCast(i, i64Ty, true);
                    llvm::Value* n = loadValue(1);
                    if (n->getType() != i64Ty) n = builder->CreateIntCast(n, i64Ty, true);
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_array_bounds_fail");
                    resolvedType = CajetaType::of("void");
                    return fn ? builder->CreateCall(fn, {i, n}) : nullptr;
                }
                if (ns == "Cajeta" && methodCallName == "stringSliceBorrow" && parameters.size() == 3) {
                    auto* i32TyB = builder->getInt32Ty();
                    llvm::Value* s = loadValue(0);
                    llvm::Value* b = loadValue(1);
                    if (b->getType() != i32TyB) b = builder->CreateIntCast(b, i32TyB, true);
                    llvm::Value* l = loadValue(2);
                    if (l->getType() != i32TyB) l = builder->CreateIntCast(l, i32TyB, true);
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_string_slice_borrow");
                    resolvedType = CajetaType::of("String", "cajeta.lang");
                    return builder->CreateCall(fn, {s, b, l});
                }
                // ----- Utf8 tagged-form natives (slice-spec 8) -----
                // Utf8 args pass by ADDRESS: a value-type lvalue already IS the storage
                // address, while `this` is an alloca HOLDING the pointer (load once).
                auto utf8Addr = [&](size_t i) -> llvm::Value* {
                    auto& p = parameters[i].expression;
                    llvm::Value* v = p->generateCode(module);
                    if (dynamic_pointer_cast<ThisExpression>(p)) {
                        v = builder->CreateLoad(
                            llvm::PointerType::get(llvmCtx, 0), v);
                    }
                    return v;
                };
                if (ns == "Cajeta" && methodCallName == "utf8OfString" && parameters.size() == 2) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_utf8_of_string");
                    resolvedType = CajetaType::of("void");
                    return builder->CreateCall(fn, {utf8Addr(0), loadValue(1)});
                }
                if (ns == "Cajeta" && methodCallName == "utf8ByteAt" && parameters.size() == 2) {
                    llvm::Value* idx = loadValue(1);
                    if (idx->getType() != i32Ty) idx = builder->CreateIntCast(idx, i32Ty, true);
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_utf8_byte_at");
                    resolvedType = CajetaType::of("int8");
                    return builder->CreateCall(fn, {utf8Addr(0), idx});
                }
                if (ns == "Cajeta" && methodCallName == "utf8Equals" && parameters.size() == 2) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_utf8_equals");
                    resolvedType = CajetaType::of("boolean");
                    llvm::Value* r = builder->CreateCall(fn, {utf8Addr(0), utf8Addr(1)});
                    return builder->CreateICmpNE(r, llvm::ConstantInt::get(i32Ty, 0));
                }
                if (ns == "Cajeta" && methodCallName == "utf8EqualsString" && parameters.size() == 2) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_utf8_equals_string");
                    resolvedType = CajetaType::of("boolean");
                    llvm::Value* r = builder->CreateCall(fn, {utf8Addr(0), loadValue(1)});
                    return builder->CreateICmpNE(r, llvm::ConstantInt::get(i32Ty, 0));
                }
                if (ns == "Cajeta" && methodCallName == "utf8Hash" && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_utf8_hash");
                    resolvedType = CajetaType::of("int64");
                    return builder->CreateCall(fn, {utf8Addr(0)});
                }
                if (ns == "Cajeta" && methodCallName == "utf8Retain" && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_utf8_retain");
                    resolvedType = CajetaType::of("void");
                    return builder->CreateCall(fn, {utf8Addr(0)});
                }
                if (ns == "Cajeta" && methodCallName == "utf8Release" && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_utf8_release");
                    resolvedType = CajetaType::of("void");
                    return builder->CreateCall(fn, {utf8Addr(0)});
                }
                if (ns == "Cajeta" && methodCallName == "sharedPopulation" && parameters.empty()) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_shared_population");
                    resolvedType = CajetaType::of("int64");
                    return builder->CreateCall(fn, {});
                }
                if (ns == "Cajeta" && methodCallName == "owned"
                        && parameters.size() == 1) {
                    resolvedType = CajetaType::of("boolean");
                    auto ownedId = dynamic_pointer_cast<IdentifierExpression>(
                        parameters[0].expression);
                    auto ownedCm = module->getCurrentMethod();
                    int ownedPos = -1;
                    if (ownedId && ownedCm) {
                        int seen = -1;
                        for (auto& fp : ownedCm->getParameterList()) {
                            if (!fp || fp->getName() == "this") continue;
                            ++seen;
                            if (fp->getName() == ownedId->getTextValue()) {
                                ownedPos = seen;
                                break;
                            }
                        }
                    }
                    if (ownedPos < 0 || ownedPos >= 64) {
                        throw Exception(
                            "Cajeta.owned(v): `v` must be a formal parameter "
                            "of the enclosing method — owned() answers \"did "
                            "THIS call surrender v's title\", and only "
                            "formals carry a transfer-word bit. For locals, "
                            "track ownership with `#=` / slot bits instead.",
                            "CAJETA_ERROR_OWNED_NON_FORMAL");
                    }
                    if (auto ownedScope = module->getScopeStack().peek()) {
                        if (FieldPtr ownedF = ownedScope->getField(
                                ownedId->getTextValue())) {
                            ownedF->setOwnershipAudited(true);
                        }
                    }
                    llvm::Value* ownedW = ownedCm->getTransferWordArg();
                    if (!ownedW) {
                        return (llvm::Value*) builder->getInt1(false);
                    }
                    llvm::Value* ownedBit = builder->CreateAnd(
                        builder->CreateLShr(ownedW,
                            builder->getInt64((uint64_t) ownedPos)),
                        builder->getInt64(1), "owned_bit");
                    return builder->CreateICmpNE(ownedBit,
                        builder->getInt64(0), "owned_flag");
                }
                if (ns == "Cajeta" && methodCallName == "moveMask") {
                    throw Exception(
                        "Cajeta.moveMask() is retired. Bookkeeping stores "
                        "spell `#=` (the slot/member bit records the "
                        "caller's transfer); code that genuinely BRANCHES "
                        "on ownership reads `Cajeta.owned(formal)`.",
                        "CAJETA_ERROR_MOVEMASK_RETIRED");
                }
                if (ns == "Cajeta" && methodCallName == "liveCount" && parameters.empty()) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_live_set_population");
                    resolvedType = CajetaType::of("int64");
                    return builder->CreateCall(fn, {});
                }
                if (ns == "Cajeta" && methodCallName == "allocatedBytes" && parameters.empty()) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_total_allocated_bytes");
                    resolvedType = CajetaType::of("int64");
                    return builder->CreateCall(fn, {});
                }
                if (ns == "Cajeta" && methodCallName == "arenaInUse" && parameters.empty()) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_arena_bytes");
                    resolvedType = CajetaType::of("int64");
                    return builder->CreateCall(fn, {});
                }
                if (ns == "Cajeta" && methodCallName == "dropValue" && parameters.size() == 1) {
                    {
                        ExpressionPtr dvArg = dynamic_pointer_cast<Expression>(
                            parameters[0].expression);
                        bool dvSharp = parameters[0].callerTransferred;
                        if (isMoveKind(dvArg)) {
                            dvArg = moveInner(dvArg);
                            dvSharp = true;
                        }
                        auto dvAix = dvSharp
                            ? dynamic_pointer_cast<ArrayIndexExpression>(dvArg)
                            : nullptr;
                        if (dvAix && !dvAix->getChildren().empty()) {
                            if (!dvAix->getResolvedType()) dvAix->resolveTypes(module);
                            auto dvRecv = dynamic_pointer_cast<Expression>(
                                dvAix->getChildren()[0]);
                            bool dvSimple = dvRecv
                                && (dynamic_pointer_cast<IdentifierExpression>(dvRecv)
                                    || dynamic_pointer_cast<DotExpression>(dvRecv));
                            bool dvArrElem = dvSimple
                                && CajetaClass::arrayElementCarriesArraySlotBits(
                                       dvAix->getResolvedType());
                            if (dvSimple && (CajetaClass::arrayElementCarriesSlotBits(
                                    dvAix->getResolvedType()) || dvArrElem)) {
                                llvm::Type* dvI64 =
                                    llvm::Type::getInt64Ty(*module->getLlvmContext());
                                llvm::Value* dvSlot = dvAix->generateCode(module);
                                llvm::Value* dvRv = dvRecv->generateCode(module);
                                llvm::Value* dvHdr = loadIfLValue(module, dvRv, dvRecv);
                                auto dvArr = dynamic_pointer_cast<CajetaArray>(
                                    dvRecv->getResolvedType());
                                const llvm::DataLayout& dvDl =
                                    module->getLlvmModule()->getDataLayout();
                                uint64_t dvHs = 8, dvEs = 8;
                                if (dvArr) {
                                    dvHs = dvDl.getTypeAllocSize(dvArr->getLlvmType());
                                    dvEs = dvArr->elementStrideBytes(
                                        dvDl, module->getLlvmContext());
                                }
                                llvm::Value* dvIdx = builder->CreateSDiv(
                                    builder->CreateSub(
                                        builder->CreateSub(
                                            builder->CreatePtrToInt(dvSlot, dvI64),
                                            builder->CreatePtrToInt(dvHdr, dvI64)),
                                        llvm::ConstantInt::get(dvI64, dvHs)),
                                    llvm::ConstantInt::get(dvI64, dvEs));
                                if (dvArrElem) {
                                    if (llvm::Function* dvFn = module->getRuntimeFunction(
                                            "__cajeta_tail_arrelem_drop_one")) {
                                        builder->CreateCall(dvFn, {dvHdr,
                                            llvm::ConstantInt::get(dvI64, dvHs),
                                            llvm::ConstantInt::get(dvI64, dvEs), dvIdx,
                                            llvm::ConstantInt::get(dvI64,
                                                CajetaClass::arrayElementInnerDropKind(
                                                    dvAix->getResolvedType()))});
                                        return nullptr;
                                    }
                                } else if (llvm::Function* dvFn = module->getRuntimeFunction(
                                        "__cajeta_tail_elem_drop_one")) {
                                    builder->CreateCall(dvFn, {dvHdr,
                                        llvm::ConstantInt::get(dvI64, dvHs),
                                        llvm::ConstantInt::get(dvI64, dvEs), dvIdx});
                                    return nullptr;
                                }
                            }
                        }
                    }
                    llvm::Value* v = loadValue(0);
                    auto argAst = dynamic_pointer_cast<Expression>(parameters[0].expression);
                    CajetaTypePtr at = argAst ? argAst->getResolvedType() : nullptr;
                    if (at) {
                        if (auto arr = std::dynamic_pointer_cast<CajetaArray>(at)) {
                            if (!arr->isInlineArray()) {
                                if (llvm::Function* fn = module->getRuntimeFunction(
                                        "__cajeta_free_array")) {
                                    builder->CreateCall(fn, {v});
                                }
                            }
                        } else if (auto klass = std::dynamic_pointer_cast<CajetaClass>(at)) {
                            bool isStr = klass->getQName()
                                && klass->getQName()->getTypeName() == "String"
                                && klass->getQName()->getPackageName() == "cajeta.lang";
                            if (isStr) {
                                if (llvm::Function* fn = module->getRuntimeFunction(
                                        "__cajeta_string_drop")) {
                                    builder->CreateCall(fn, {v});
                                }
                            } else if (!std::dynamic_pointer_cast<CajetaView>(at)
                                    && !klass->isInterface()
                                    && klass->hasVtablePointerAtSlotZero()) {
                                klass->patchVirtualTableDropFn();
                                if (llvm::Function* fn = module->getRuntimeFunction(
                                        "__cajeta_class_virtual_drop")) {
                                    builder->CreateCall(fn, {v});
                                }
                            }
                        }
                    }
                    if (auto dvId = dynamic_pointer_cast<IdentifierExpression>(
                            parameters[0].expression)) {
                        if (auto dvScope = module->getScopeStack().peek()) {
                            if (FieldPtr dvField = dvScope->getField(
                                    dvId->getTextValue())) {
                                if (dvField->getDropEntry()) {
                                    ownership::deactivateLocalEntry(module, dvField);
                                }
                            }
                        }
                    }
                    return nullptr;
                }
                if (ns == "Cajeta" && methodCallName == "flagged"
                        && parameters.size() == 2) {
                    llvm::Value* v = loadValue(0);
                    llvm::Value* ownedV = loadValue(1);
                    if (ownedV && !ownedV->getType()->isIntegerTy(64)) {
                        ownedV = builder->CreateZExt(ownedV, i64Ty, "flag_i64");
                    }
                    flaggedTitleValue = ownedV;
                    if (parameters[0].callerTransferred) {
                        if (auto idExpr = std::dynamic_pointer_cast<IdentifierExpression>(
                                parameters[0].expression)) {
                            if (auto scope = module->getScopeStack().peek()) {
                                if (FieldPtr fld = scope->getField(idExpr->getTextValue())) {
                                    if (fld->getDropEntry()) {
                                        ownership::deactivateLocalEntry(module, fld);
                                    }
                                }
                            }
                        }
                    }
                    auto argAst = dynamic_pointer_cast<Expression>(
                        parameters[0].expression);
                    if (argAst && argAst->getResolvedType()) {
                        resolvedType = argAst->getResolvedType();
                    }
                    return v;
                }
                if (ns == "Cajeta" && methodCallName == "f32ToBits" && parameters.size() == 1) {
                    llvm::Value* x = loadValue(0);
                    x = builder->CreateFPCast(x, builder->getFloatTy(),
                                              "f32_bits.fit");
                    llvm::Value* b = builder->CreateBitCast(x, i32Ty, "f32_bits");
                    resolvedType = CajetaType::of("int32");
                    return b;
                }
                if (ns == "Cajeta" && methodCallName == "bitsToF32" && parameters.size() == 1) {
                    auto* f32Ty = llvm::Type::getFloatTy(llvmCtx);
                    llvm::Value* b = loadValue(0);
                    // Coerce to the DECLARED width first. The parameter is int32 but the VALUE need
                    // not be (`h << 16` promotes to int64), and bitcast requires equal bit widths,
                    // so bitcasting the operand as handed builds malformed IR.
                    b = builder->CreateIntCast(b, i32Ty, /*isSigned=*/true,
                                               "bits_f32.fit");
                    llvm::Value* x = builder->CreateBitCast(b, f32Ty, "bits_f32");
                    resolvedType = CajetaType::of("float32");
                    return x;
                }
                if (ns == "Cajeta" && methodCallName == "f64ToBits" && parameters.size() == 1) {
                    llvm::Value* x = loadValue(0);
                    x = builder->CreateFPCast(x, builder->getDoubleTy(),
                                              "f64_bits.fit");
                    llvm::Value* b = builder->CreateBitCast(x, i64Ty, "f64_bits");
                    resolvedType = CajetaType::of("int64");
                    return b;
                }
                if (ns == "Cajeta" && methodCallName == "bitsToF64" && parameters.size() == 1) {
                    llvm::Value* b = loadValue(0);
                    b = builder->CreateIntCast(b, i64Ty, /*isSigned=*/true,
                                               "bits_f64.fit");
                    llvm::Value* x = builder->CreateBitCast(b, f64Ty, "bits_f64");
                    resolvedType = CajetaType::of("float64");
                    return x;
                }
                if (ns == "Cajeta" && methodCallName == "ctz64" && parameters.size() == 1) {
                    auto* lmod = module->getLlvmModule();
                    auto* i64Ty = builder->getInt64Ty();
                    llvm::Value* x = loadValue(0);
                    llvm::Function* cttz = llvm::Intrinsic::getOrInsertDeclaration(
                        lmod, llvm::Intrinsic::cttz, {i64Ty});
                    llvm::Value* r = builder->CreateCall(
                        cttz, {x, builder->getFalse()}, "ctz");
                    return builder->CreateTrunc(r, builder->getInt32Ty(), "ctz32");
                }
                if (ns == "Cajeta" && methodCallName == "vload16" && parameters.size() == 2) {
                    auto* i8Ty = builder->getInt8Ty();
                    auto* v16  = llvm::FixedVectorType::get(i8Ty, 16);
                    llvm::Value* hdr = loadValue(0);
                    llvm::Value* off = loadValue(1);
                    llvm::Value* data = builder->CreateGEP(
                        i8Ty, hdr, builder->getInt64(8), "buf_data");
                    llvm::Value* ptr = builder->CreateGEP(
                        i8Ty, data, off, "buf_blk_ptr");
                    llvm::LoadInst* ld = builder->CreateLoad(v16, ptr, "vload16");
                    ld->setAlignment(llvm::Align(1));
                    return ld;
                }
                // --- Wide SIMD primitives (Vector<int64,8> = 512-bit / AVX-512) ---
                if (ns == "Cajeta" && methodCallName == "vload8i64" && parameters.size() == 2) {
                    auto* i8Ty = builder->getInt8Ty();
                    auto* v8 = llvm::FixedVectorType::get(builder->getInt64Ty(), 8);
                    llvm::Value* hdr = loadValue(0);
                    llvm::Value* off = loadValue(1);
                    llvm::Value* data = builder->CreateGEP(
                        i8Ty, hdr, builder->getInt64(8), "buf_data");
                    llvm::Value* ptr = builder->CreateGEP(i8Ty, data, off, "v8_blk_ptr");
                    llvm::LoadInst* ld = builder->CreateLoad(v8, ptr, "vload8i64");
                    ld->setAlignment(llvm::Align(1));
                    resolvedType = CajetaVector::validateAndCreate(
                        module, CajetaType::of("int64"), 8);
                    return ld;
                }
                if (ns == "Cajeta" && methodCallName == "vstore8i64" && parameters.size() == 3) {
                    auto* i8Ty = builder->getInt8Ty();
                    llvm::Value* vec = loadValue(0);
                    llvm::Value* hdr = loadValue(1);
                    llvm::Value* off = loadValue(2);
                    llvm::Value* data = builder->CreateGEP(
                        i8Ty, hdr, builder->getInt64(8), "buf_data");
                    llvm::Value* ptr = builder->CreateGEP(i8Ty, data, off, "v8_st_ptr");
                    llvm::StoreInst* st = builder->CreateStore(vec, ptr);
                    st->setAlignment(llvm::Align(1));
                    return st;
                }
                // --- float64 SIMD (Vector<float64,8> = 512-bit / AVX-512) ---
                // Unlike vload8i64 (int8[] buffer, BYTE offset) these take a float64[] and an
                // ELEMENT index: data starts 8 bytes in and the GEP is double-stride.
                if (ns == "Cajeta" && methodCallName == "vload8f64" && parameters.size() == 2) {
                    auto* i8Ty = builder->getInt8Ty();
                    auto* dblTy = builder->getDoubleTy();
                    auto* v8 = llvm::FixedVectorType::get(dblTy, 8);
                    llvm::Value* hdr = loadValue(0);
                    llvm::Value* idx = loadValue(1);
                    idx = builder->CreateIntCast(idx, builder->getInt64Ty(), /*isSigned=*/true);
                    llvm::Value* data = builder->CreateGEP(
                        i8Ty, hdr, builder->getInt64(8), "f64_data");
                    llvm::Value* ptr = builder->CreateGEP(dblTy, data, idx, "v8f64_ptr");
                    llvm::LoadInst* ld = builder->CreateLoad(v8, ptr, "vload8f64");
                    ld->setAlignment(llvm::Align(1));
                    resolvedType = CajetaVector::validateAndCreate(
                        module, CajetaType::of("float64"), 8);
                    return ld;
                }
                if (ns == "Cajeta" && methodCallName == "vstore8f64" && parameters.size() == 3) {
                    auto* i8Ty = builder->getInt8Ty();
                    auto* dblTy = builder->getDoubleTy();
                    llvm::Value* vec = loadValue(0);
                    llvm::Value* hdr = loadValue(1);
                    llvm::Value* idx = loadValue(2);
                    idx = builder->CreateIntCast(idx, builder->getInt64Ty(), /*isSigned=*/true);
                    llvm::Value* data = builder->CreateGEP(
                        i8Ty, hdr, builder->getInt64(8), "f64_data");
                    llvm::Value* ptr = builder->CreateGEP(dblTy, data, idx, "v8f64_st_ptr");
                    llvm::StoreInst* st = builder->CreateStore(vec, ptr);
                    st->setAlignment(llvm::Align(1));
                    return st;
                }
                if (ns == "Cajeta" && methodCallName == "vsum8f64" && parameters.size() == 1) {
                    llvm::Value* vec = loadValue(0);
                    llvm::Value* acc0 = llvm::ConstantFP::get(builder->getDoubleTy(), 0.0);
                    llvm::Value* red = builder->CreateFAddReduce(acc0, vec);
                    if (auto* inst = llvm::dyn_cast<llvm::Instruction>(red)) {
                        llvm::FastMathFlags fmf;
                        fmf.setFast();
                        inst->setFastMathFlags(fmf);
                    }
                    resolvedType = CajetaType::of("float64");
                    return red;
                }
                if (ns == "Cajeta" && methodCallName == "vswapPairs" && parameters.size() == 1) {
                    llvm::Value* vec = loadValue(0);
                    int maskArr[8] = {1, 0, 3, 2, 5, 4, 7, 6};
                    llvm::SmallVector<int, 8> mask(maskArr, maskArr + 8);
                    llvm::Value* sw = builder->CreateShuffleVector(vec, vec, mask, "vswapPairs");
                    resolvedType = CajetaVector::validateAndCreate(
                        module, CajetaType::of("int64"), 8);
                    return sw;
                }
                if (ns == "Cajeta" && methodCallName == "vlane" && parameters.size() == 2) {
                    llvm::Value* vec = loadValue(0);
                    llvm::Value* idx = loadValue(1);
                    resolvedType = CajetaType::of("int64");
                    return builder->CreateExtractElement(vec, idx, "vlane");
                }
                if (ns == "Cajeta" && methodCallName == "popcount64" && parameters.size() == 1) {
                    auto* lmod = module->getLlvmModule();
                    auto* i64Ty = builder->getInt64Ty();
                    llvm::Value* x = loadValue(0);
                    llvm::Function* ctpop = llvm::Intrinsic::getOrInsertDeclaration(
                        lmod, llvm::Intrinsic::ctpop, {i64Ty});
                    llvm::Value* r = builder->CreateCall(ctpop, {x}, "popcnt");
                    return builder->CreateTrunc(r, builder->getInt32Ty(), "popcnt32");
                }
                if (ns == "Cajeta" && methodCallName == "condvarNew" && parameters.empty()) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_condvar_new");
                    return builder->CreateCall(fn, {});
                }
                if (ns == "Cajeta" && methodCallName == "condvarWait" && parameters.size() == 2) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_condvar_wait");
                    llvm::Value* cv = loadValue(0);
                    llvm::Value* lock = loadValue(1);
                    return builder->CreateCall(fn, {cv, lock});
                }
                if (ns == "Cajeta" && methodCallName == "condvarNotifyAll" && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_condvar_notify_all");
                    llvm::Value* cv = loadValue(0);
                    return builder->CreateCall(fn, {cv});
                }
                if (ns == "Cajeta" && methodCallName == "condvarDestroy" && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_condvar_destroy");
                    llvm::Value* cv = loadValue(0);
                    return builder->CreateCall(fn, {cv});
                }
                if (ns == "Cajeta" && methodCallName == "rwlockNew" && parameters.empty()) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_rwlock_new");
                    return builder->CreateCall(fn, {});
                }
                if (ns == "Cajeta" && methodCallName == "rwlockRdlock" && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_rwlock_rdlock");
                    return builder->CreateCall(fn, {loadValue(0)});
                }
                if (ns == "Cajeta" && methodCallName == "rwlockWrlock" && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_rwlock_wrlock");
                    return builder->CreateCall(fn, {loadValue(0)});
                }
                if (ns == "Cajeta" && methodCallName == "rwlockRdunlock" && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_rwlock_rdunlock");
                    return builder->CreateCall(fn, {loadValue(0)});
                }
                if (ns == "Cajeta" && methodCallName == "rwlockWrunlock" && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_rwlock_wrunlock");
                    return builder->CreateCall(fn, {loadValue(0)});
                }
                if (ns == "Cajeta" && methodCallName == "rwlockDestroy" && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_rwlock_destroy");
                    return builder->CreateCall(fn, {loadValue(0)});
                }
                if (ns == "Cajeta" && methodCallName == "atomicI32New" && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_atomic_i32_new");
                    llvm::Value* v = loadValue(0);
                    if (v->getType() != i32Ty) v = builder->CreateIntCast(v, i32Ty, /*isSigned=*/true);
                    return builder->CreateCall(fn, {v});
                }
                if (ns == "Cajeta" && methodCallName == "atomicI32Destroy" && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_atomic_i32_destroy");
                    return builder->CreateCall(fn, {loadValue(0)});
                }
                if (ns == "Cajeta" && methodCallName == "atomicI32Load" && parameters.size() == 1) {
                    auto* load = builder->CreateLoad(i32Ty, loadValue(0), "atomic.i32.load");
                    load->setAtomic(llvm::AtomicOrdering::SequentiallyConsistent);
                    load->setAlignment(llvm::Align(4));
                    return load;
                }
                if (ns == "Cajeta" && methodCallName == "atomicI32Store" && parameters.size() == 2) {
                    llvm::Value* v = loadValue(1);
                    if (v->getType() != i32Ty) v = builder->CreateIntCast(v, i32Ty, /*isSigned=*/true);
                    auto* store = builder->CreateStore(v, loadValue(0));
                    store->setAtomic(llvm::AtomicOrdering::SequentiallyConsistent);
                    store->setAlignment(llvm::Align(4));
                    return store;
                }
                if (ns == "Cajeta" && methodCallName == "atomicI32FetchAdd" && parameters.size() == 2) {
                    llvm::Value* v = loadValue(1);
                    if (v->getType() != i32Ty) v = builder->CreateIntCast(v, i32Ty, /*isSigned=*/true);
                    return builder->CreateAtomicRMW(
                        llvm::AtomicRMWInst::Add, loadValue(0), v,
                        llvm::MaybeAlign(4),
                        llvm::AtomicOrdering::SequentiallyConsistent);
                }
                if (ns == "Cajeta" && methodCallName == "atomicI32CompareAndSet"
                        && parameters.size() == 3) {
                    llvm::Value* expected = loadValue(1);
                    llvm::Value* desired = loadValue(2);
                    if (expected->getType() != i32Ty) expected = builder->CreateIntCast(expected, i32Ty, true);
                    if (desired->getType() != i32Ty) desired = builder->CreateIntCast(desired, i32Ty, true);
                    auto* cmpxchg = builder->CreateAtomicCmpXchg(
                        loadValue(0), expected, desired,
                        llvm::MaybeAlign(4),
                        llvm::AtomicOrdering::SequentiallyConsistent,
                        llvm::AtomicOrdering::SequentiallyConsistent);
                    return builder->CreateExtractValue(cmpxchg, 1, "atomic.cas.ok");
                }
                if (ns == "Cajeta" && methodCallName == "atomicI64New" && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_atomic_i64_new");
                    llvm::Value* v = loadValue(0);
                    if (v->getType() != i64Ty) v = builder->CreateIntCast(v, i64Ty, /*isSigned=*/true);
                    return builder->CreateCall(fn, {v});
                }
                if (ns == "Cajeta" && methodCallName == "atomicI64Destroy" && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_atomic_i64_destroy");
                    return builder->CreateCall(fn, {loadValue(0)});
                }
                if (ns == "Cajeta" && methodCallName == "atomicI64Load" && parameters.size() == 1) {
                    auto* load = builder->CreateLoad(i64Ty, loadValue(0), "atomic.i64.load");
                    load->setAtomic(llvm::AtomicOrdering::SequentiallyConsistent);
                    load->setAlignment(llvm::Align(8));
                    return load;
                }
                if (ns == "Cajeta" && methodCallName == "atomicI64Store" && parameters.size() == 2) {
                    llvm::Value* v = loadValue(1);
                    if (v->getType() != i64Ty) v = builder->CreateIntCast(v, i64Ty, /*isSigned=*/true);
                    auto* store = builder->CreateStore(v, loadValue(0));
                    store->setAtomic(llvm::AtomicOrdering::SequentiallyConsistent);
                    store->setAlignment(llvm::Align(8));
                    return store;
                }
                if (ns == "Cajeta" && methodCallName == "atomicI64FetchAdd" && parameters.size() == 2) {
                    llvm::Value* v = loadValue(1);
                    if (v->getType() != i64Ty) v = builder->CreateIntCast(v, i64Ty, /*isSigned=*/true);
                    return builder->CreateAtomicRMW(
                        llvm::AtomicRMWInst::Add, loadValue(0), v,
                        llvm::MaybeAlign(8),
                        llvm::AtomicOrdering::SequentiallyConsistent);
                }
                if (ns == "Cajeta" && methodCallName == "atomicI64CompareAndSet"
                        && parameters.size() == 3) {
                    llvm::Value* expected = loadValue(1);
                    llvm::Value* desired = loadValue(2);
                    if (expected->getType() != i64Ty) expected = builder->CreateIntCast(expected, i64Ty, true);
                    if (desired->getType() != i64Ty) desired = builder->CreateIntCast(desired, i64Ty, true);
                    auto* cmpxchg = builder->CreateAtomicCmpXchg(
                        loadValue(0), expected, desired,
                        llvm::MaybeAlign(8),
                        llvm::AtomicOrdering::SequentiallyConsistent,
                        llvm::AtomicOrdering::SequentiallyConsistent);
                    return builder->CreateExtractValue(cmpxchg, 1, "atomic.cas.ok");
                }
                auto atomicLoadOrd = [&](llvm::Type* ty, unsigned alignBytes,
                                          llvm::AtomicOrdering ord,
                                          const char* nameHint) -> llvm::Value* {
                    auto* load = builder->CreateLoad(ty, loadValue(0), nameHint);
                    load->setAtomic(ord);
                    load->setAlignment(llvm::Align(alignBytes));
                    return load;
                };
                auto atomicStoreOrd = [&](llvm::Type* ty, unsigned alignBytes,
                                           llvm::AtomicOrdering ord) -> llvm::Value* {
                    llvm::Value* v = loadValue(1);
                    if (v->getType() != ty) v = builder->CreateIntCast(v, ty, /*isSigned=*/true);
                    auto* store = builder->CreateStore(v, loadValue(0));
                    store->setAtomic(ord);
                    store->setAlignment(llvm::Align(alignBytes));
                    return store;
                };
                auto atomicFetchAddOrd = [&](llvm::Type* ty, unsigned alignBytes,
                                              llvm::AtomicOrdering ord) -> llvm::Value* {
                    llvm::Value* v = loadValue(1);
                    if (v->getType() != ty) v = builder->CreateIntCast(v, ty, /*isSigned=*/true);
                    return builder->CreateAtomicRMW(
                        llvm::AtomicRMWInst::Add, loadValue(0), v,
                        llvm::MaybeAlign(alignBytes), ord);
                };
                auto atomicCasOrd = [&](llvm::Type* ty, unsigned alignBytes,
                                         llvm::AtomicOrdering succ,
                                         llvm::AtomicOrdering fail) -> llvm::Value* {
                    llvm::Value* expected = loadValue(1);
                    llvm::Value* desired = loadValue(2);
                    if (expected->getType() != ty) expected = builder->CreateIntCast(expected, ty, /*isSigned=*/true);
                    if (desired->getType() != ty) desired = builder->CreateIntCast(desired, ty, /*isSigned=*/true);
                    auto* cmpxchg = builder->CreateAtomicCmpXchg(
                        loadValue(0), expected, desired,
                        llvm::MaybeAlign(alignBytes), succ, fail);
                    return builder->CreateExtractValue(cmpxchg, 1, "atomic.cas.ok");
                };
                if (ns == "Cajeta" && methodCallName == "atomicI32LoadRelaxed" && parameters.size() == 1) {
                    return atomicLoadOrd(i32Ty, 4, llvm::AtomicOrdering::Monotonic, "atomic.i32.load.rlx");
                }
                if (ns == "Cajeta" && methodCallName == "atomicI32LoadAcquire" && parameters.size() == 1) {
                    return atomicLoadOrd(i32Ty, 4, llvm::AtomicOrdering::Acquire, "atomic.i32.load.acq");
                }
                if (ns == "Cajeta" && methodCallName == "atomicI32StoreRelaxed" && parameters.size() == 2) {
                    return atomicStoreOrd(i32Ty, 4, llvm::AtomicOrdering::Monotonic);
                }
                if (ns == "Cajeta" && methodCallName == "atomicI32StoreRelease" && parameters.size() == 2) {
                    return atomicStoreOrd(i32Ty, 4, llvm::AtomicOrdering::Release);
                }
                if (ns == "Cajeta" && methodCallName == "atomicI32FetchAddRelaxed" && parameters.size() == 2) {
                    return atomicFetchAddOrd(i32Ty, 4, llvm::AtomicOrdering::Monotonic);
                }
                if (ns == "Cajeta" && methodCallName == "atomicI32CompareAndSetAcquire" && parameters.size() == 3) {
                    return atomicCasOrd(i32Ty, 4,
                        llvm::AtomicOrdering::Acquire,
                        llvm::AtomicOrdering::Acquire);
                }
                if (ns == "Cajeta" && methodCallName == "atomicI64LoadRelaxed" && parameters.size() == 1) {
                    return atomicLoadOrd(i64Ty, 8, llvm::AtomicOrdering::Monotonic, "atomic.i64.load.rlx");
                }
                if (ns == "Cajeta" && methodCallName == "atomicI64LoadAcquire" && parameters.size() == 1) {
                    return atomicLoadOrd(i64Ty, 8, llvm::AtomicOrdering::Acquire, "atomic.i64.load.acq");
                }
                if (ns == "Cajeta" && methodCallName == "atomicI64StoreRelaxed" && parameters.size() == 2) {
                    return atomicStoreOrd(i64Ty, 8, llvm::AtomicOrdering::Monotonic);
                }
                if (ns == "Cajeta" && methodCallName == "atomicI64StoreRelease" && parameters.size() == 2) {
                    return atomicStoreOrd(i64Ty, 8, llvm::AtomicOrdering::Release);
                }
                if (ns == "Cajeta" && methodCallName == "atomicI64FetchAddRelaxed" && parameters.size() == 2) {
                    return atomicFetchAddOrd(i64Ty, 8, llvm::AtomicOrdering::Monotonic);
                }
                if (ns == "Cajeta" && methodCallName == "atomicI64CompareAndSetAcquire" && parameters.size() == 3) {
                    return atomicCasOrd(i64Ty, 8,
                        llvm::AtomicOrdering::Acquire,
                        llvm::AtomicOrdering::Acquire);
                }
                if (ns == "Cajeta" && methodCallName == "taskWaitTimeout"
                        && parameters.size() == 2) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_task_wait_timeout");
                    llvm::Value* doneAddr = loadValue(0);
                    llvm::Value* deadline = loadValue(1);
                    if (deadline->getType() != i64Ty) {
                        deadline = builder->CreateIntCast(deadline, i64Ty, /*isSigned=*/true);
                    }
                    return builder->CreateCall(fn, {doneAddr, deadline});
                }
                if (ns == "Cajeta" && methodCallName == "currentTimeNanos"
                        && parameters.empty()) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_currentTimeNanos");
                    return builder->CreateCall(fn, {});
                }
                if (ns == "Cajeta" && methodCallName == "ioWait"
                        && parameters.size() == 2) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_io_wait");
                    llvm::Value* fdv = loadValue(0);
                    llvm::Value* evv = loadValue(1);
                    if (fdv->getType() != i32Ty) fdv = builder->CreateIntCast(fdv, i32Ty, true);
                    if (evv->getType() != i32Ty) evv = builder->CreateIntCast(evv, i32Ty, true);
                    return builder->CreateCall(fn, {fdv, evv});
                }
                if (ns == "Cajeta" && methodCallName == "eventfdCreate"
                        && parameters.empty()) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_eventfd_create");
                    return builder->CreateCall(fn, {});
                }
                if (ns == "Cajeta" && methodCallName == "eventfdSignal"
                        && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_eventfd_signal");
                    llvm::Value* fdv = loadValue(0);
                    if (fdv->getType() != i32Ty) fdv = builder->CreateIntCast(fdv, i32Ty, true);
                    return builder->CreateCall(fn, {fdv});
                }
                if (ns == "Cajeta" && methodCallName == "eventfdConsume"
                        && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_eventfd_consume");
                    llvm::Value* fdv = loadValue(0);
                    if (fdv->getType() != i32Ty) fdv = builder->CreateIntCast(fdv, i32Ty, true);
                    return builder->CreateCall(fn, {fdv});
                }
                if (ns == "Cajeta" && methodCallName == "fdClose"
                        && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_fd_close");
                    llvm::Value* fdv = loadValue(0);
                    if (fdv->getType() != i32Ty) fdv = builder->CreateIntCast(fdv, i32Ty, true);
                    return builder->CreateCall(fn, {fdv});
                }
                if (ns == "Cajeta" && methodCallName == "taskDonePointer"
                        && parameters.size() == 1) {
                    auto argExpr = dynamic_pointer_cast<Expression>(parameters[0].expression);
                    if (argExpr && !argExpr->getResolvedType()) {
                        argExpr->resolveTypes(module);
                    }
                    auto argType = argExpr ? argExpr->getResolvedType() : nullptr;
                    auto task = dynamic_pointer_cast<CajetaTask>(argType);
                    if (!task) {
                        throw Exception(
                            "Cajeta.taskDonePointer requires a Task<T> argument",
                            "CAJETA_ERROR_TYPE_MISMATCH");
                    }
                    llvm::Value* taskPtr = loadValue(0);
                    return builder->CreateStructGEP(
                        task->getLlvmType(), taskPtr,
                        CajetaTask::DONE_FIELD_INDEX, "task_done_ptr");
                }
                if (ns == "Cajeta" && methodCallName == "taskCancel"
                        && parameters.size() == 1) {
                    auto argExpr = dynamic_pointer_cast<Expression>(parameters[0].expression);
                    if (argExpr && !argExpr->getResolvedType()) {
                        argExpr->resolveTypes(module);
                    }
                    auto argType = argExpr ? argExpr->getResolvedType() : nullptr;
                    auto task = dynamic_pointer_cast<CajetaTask>(argType);
                    if (!task) {
                        throw Exception(
                            "Cajeta.taskCancel requires a Task<T> argument",
                            "CAJETA_ERROR_TYPE_MISMATCH");
                    }
                    llvm::Value* taskPtr = loadValue(0);
                    llvm::Type* ptrTy2 = llvm::PointerType::get(llvmCtx, 0);
                    llvm::Value* fiberSlot = builder->CreateStructGEP(
                        task->getLlvmType(), taskPtr,
                        CajetaTask::FIBER_FIELD_INDEX, "task_fiber_slot");
                    llvm::Value* fiberPtr = builder->CreateLoad(
                        ptrTy2, fiberSlot, "task_fiber");
                    llvm::Function* cancelFn = module->getRuntimeFunction(
                        "__cajeta_fiber_cancel");
                    llvm::Value* sentinel = builder->CreateIntToPtr(
                        llvm::ConstantInt::get(
                            llvm::Type::getInt64Ty(llvmCtx), 1), ptrTy2);
                    return builder->CreateCall(cancelFn, {fiberPtr, sentinel});
                }
                if (ns == "Cajeta" && methodCallName == "fiberSleepNanos"
                        && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_fiber_sleep_nanos");
                    llvm::Value* nanos = loadValue(0);
                    if (nanos->getType() != i64Ty) {
                        nanos = builder->CreateIntCast(nanos, i64Ty, /*isSigned=*/true);
                    }
                    return builder->CreateCall(fn, {nanos});
                }
                if (ns == "System" && methodCallName == "exit" && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_exit");
                    llvm::Value* code = loadValue(0);
                    if (code->getType()->isIntegerTy() && code->getType() != i32Ty) {
                        code = builder->CreateIntCast(code, i32Ty, /*isSigned=*/true);
                    }
                    return builder->CreateCall(fn, {code});
                }
                if (ns == "System" && methodCallName == "currentTimeMillis"
                        && parameters.empty()) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_currentTimeMillis");
                    return builder->CreateCall(fn, {});
                }
                if (ns == "Math" && methodCallName == "random" && parameters.empty()) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_random");
                    return builder->CreateCall(fn, {});
                }
                if (ns == "Integer" && methodCallName == "parseInt" && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_parse_i64");
                    llvm::Value* call = builder->CreateCall(fn, {loadStr(0)});
                    return builder->CreateIntCast(call, i32Ty, /*isSigned=*/true);
                }
                if (ns == "Long" && methodCallName == "parseLong" && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_parse_i64");
                    return builder->CreateCall(fn, {loadStr(0)});
                }
                if (ns == "Double" && methodCallName == "parseDouble" && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_parse_f64");
                    return builder->CreateCall(fn, {loadStr(0)});
                }
                if ((ns == "Integer" || ns == "Long") && parameters.size() == 1
                        && (methodCallName == "bitCount"
                         || methodCallName == "numberOfLeadingZeros"
                         || methodCallName == "numberOfTrailingZeros"
                         || methodCallName == "reverse")) {
                    bool isLong = (ns == "Long");
                    llvm::Type* opTy = isLong ? i64Ty : i32Ty;
                    llvm::Value* x = loadValue(0);
                    if (x->getType() != opTy && x->getType()->isIntegerTy()) {
                        x = builder->CreateIntCast(x, opTy, /*isSigned=*/true);
                    }
                    llvm::Module* lm = module->getLlvmModule();
                    llvm::Intrinsic::ID id;
                    bool needsZeroFlag = false;
                    if (methodCallName == "bitCount") {
                        id = llvm::Intrinsic::ctpop;
                    } else if (methodCallName == "numberOfLeadingZeros") {
                        id = llvm::Intrinsic::ctlz;
                        needsZeroFlag = true;
                    } else if (methodCallName == "numberOfTrailingZeros") {
                        id = llvm::Intrinsic::cttz;
                        needsZeroFlag = true;
                    } else {
                        id = llvm::Intrinsic::bitreverse;
                    }
                    llvm::Function* fn = llvm::Intrinsic::getOrInsertDeclaration(lm, id, {opTy});
                    llvm::Value* call;
                    if (needsZeroFlag) {
                        call = builder->CreateCall(fn,
                            {x, llvm::ConstantInt::getFalse(llvmCtx)});
                    } else {
                        call = builder->CreateCall(fn, {x});
                    }
                    if (methodCallName != "reverse") {
                        if (call->getType() != i32Ty) {
                            call = builder->CreateIntCast(call, i32Ty, /*isSigned=*/true);
                        }
                    }
                    return call;
                }
                if (ns == "Boolean" && methodCallName == "parseBoolean" && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_parse_bool");
                    llvm::Value* call = builder->CreateCall(fn, {loadStr(0)});
                    return builder->CreateICmpNE(call,
                        llvm::ConstantInt::get(call->getType(), 0));
                }
                if ((ns == "Integer" || ns == "Long" || ns == "Double" || ns == "Boolean")
                        && methodCallName == "toString" && parameters.size() == 1) {
                    llvm::Value* v = loadValue(0);
                    llvm::Type* t = v->getType();
                    resolvedType = CajetaType::of("String");
                    if (ns == "Boolean" || t->isIntegerTy(1)) {
                        if (t->isIntegerTy(1)) v = builder->CreateZExt(v, i32Ty);
                        else if (t != i32Ty && t->isIntegerTy()) v = builder->CreateIntCast(v, i32Ty, true);
                        llvm::Function* fn = module->getRuntimeFunction("__cajeta_bool_to_str");
                        llvm::Value* cstr = builder->CreateCall(fn, {v});
                        return wrapCStringIntoClassString(module, cstr,
                            "boolStr", /*freeAfterWrap=*/false);
                    }
                    if (t->isFloatingPointTy() || ns == "Double") {
                        if (t != f64Ty) v = t->isIntegerTy()
                            ? builder->CreateSIToFP(v, f64Ty)
                            : builder->CreateFPCast(v, f64Ty);
                        llvm::Function* fn = module->getRuntimeFunction("__cajeta_f64_to_str");
                        llvm::Value* cstr = builder->CreateCall(fn, {v});
                        return wrapCStringIntoClassString(module, cstr, "f64Str");
                    }
                    if (t->isIntegerTy() && t != i64Ty) v = builder->CreateIntCast(v, i64Ty, true);
                    llvm::Function* fn = module->getRuntimeFunction("__cajeta_i64_to_str");
                    llvm::Value* cstr = builder->CreateCall(fn, {v});
                    return wrapCStringIntoClassString(module, cstr, "i64Str");
                }
                if (ns == "String" && methodCallName == "valueOf" && parameters.size() == 1) {
                    llvm::Value* v = loadValue(0);
                    llvm::Type* t = v->getType();
                    resolvedType = CajetaType::of("String");
                    if (t->isPointerTy()) return v;
                    if (t->isIntegerTy(1)) {
                        v = builder->CreateZExt(v, i32Ty);
                        llvm::Function* fn = module->getRuntimeFunction("__cajeta_bool_to_str");
                        llvm::Value* cstr = builder->CreateCall(fn, {v});
                        return wrapCStringIntoClassString(module, cstr,
                            "valueOfBool", /*freeAfterWrap=*/false);
                    }
                    auto argTy = parameters[0].expression
                        ? std::dynamic_pointer_cast<Expression>(parameters[0].expression)
                          : nullptr;
                    auto argResolved = argTy ? argTy->getResolvedType() : nullptr;
                    bool isCharArg = argResolved && argResolved->getQName()
                        && argResolved->getQName()->getTypeName() == "char";
                    if (isCharArg && t->isIntegerTy() && t != llvm::Type::getInt8Ty(llvmCtx)) {
                        v = builder->CreateIntCast(v,
                            llvm::Type::getInt8Ty(llvmCtx), /*isSigned=*/true);
                        llvm::Function* fn = module->getRuntimeFunction("__cajeta_str_fromChar");
                        llvm::Value* cstr = builder->CreateCall(fn, {v});
                        return wrapCStringIntoClassString(module, cstr, "valueOfChar");
                    }
                    if (t->isIntegerTy(8)) {
                        llvm::Function* fn = module->getRuntimeFunction("__cajeta_str_fromChar");
                        llvm::Value* cstr = builder->CreateCall(fn, {v});
                        return wrapCStringIntoClassString(module, cstr, "valueOfChar");
                    }
                    if (t->isIntegerTy()) {
                        if (t != i64Ty) v = builder->CreateIntCast(v, i64Ty, true);
                        llvm::Function* fn = module->getRuntimeFunction("__cajeta_i64_to_str");
                        llvm::Value* cstr = builder->CreateCall(fn, {v});
                        return wrapCStringIntoClassString(module, cstr, "valueOfInt");
                    }
                    if (t->isFloatingPointTy()) {
                        if (t != f64Ty) v = builder->CreateFPCast(v, f64Ty);
                        llvm::Function* fn = module->getRuntimeFunction("__cajeta_f64_to_str");
                        llvm::Value* cstr = builder->CreateCall(fn, {v});
                        return wrapCStringIntoClassString(module, cstr, "valueOfF64");
                    }
                }
            }
        }

        if (methodCallName == "count" && parameters.empty()
                && !children.empty()) {
            if (auto dotChild = dynamic_pointer_cast<DotExpression>(children[0])) {
                if (dotChild->resolveViewElementArrayProperty(module)) {
                    dotChild->setElementArrayPrefixMode(true);
                    llvm::Value* prefixPtr = dotChild->generateCode(module);
                    if (prefixPtr) {
                        llvm::Type* i64Ty2 =
                            llvm::Type::getInt64Ty(*module->getLlvmContext());
                        llvm::Type* i32Ty2 =
                            llvm::Type::getInt32Ty(*module->getLlvmContext());
                        resolvedType = CajetaType::of("int64");
                        ViewEndianness cntE = dotChild->getEarrViewType()
                            ? dotChild->getEarrViewType()->getEndianness()
                            : ViewEndianness::Host;
                        return builder->CreateIntCast(
                            CajetaView::emitSwapIfNeeded(module, cntE,
                                builder->CreateLoad(i32Ty2, prefixPtr,
                                    "earr_count32")),
                            i64Ty2, /*isSigned=*/true, "earr_count");
                    }
                    return nullptr;
                }
            }
        }

        llvm::Value* receiver = nullptr;
        CajetaTypePtr receiverType;
        // The flagged case must read the return-flag TLS HERE, immediately after the
        // receiver's own call, before any later call clobbers it.
        bool recvTempStatic = false;
        llvm::Value* recvTempFlag = nullptr;
        shared_ptr<CajetaClass> recvTempClass;
        if (!children.empty()) {
            receiver = children[0]->generateCode(module);
            auto exprChild = dynamic_pointer_cast<Expression>(children[0]);
            if (exprChild) {
                if (!exprChild->getResolvedType()) {
                    exprChild->resolveTypes(module);
                }
                receiverType = exprChild->getResolvedType();
            }
            if ((recvTempClass = freshHeapCreatorTempClass(children[0]))) {
                recvTempStatic = true;
            } else if (auto rmce = dynamic_pointer_cast<MethodCallExpression>(
                    children[0])) {
                if ((recvTempClass = droppableTempClass(rmce->getResolvedType()))) {
                    ownership::TitleShape rsh = ownership::classify(rmce, module);
                    ownership::TitleVerdict rv = ownership::policy(
                        rsh, ownership::ConsumerRole::ArgPlain);
                    llvm::Value* rf = ownership::verdictFlagAfterCodegen(rsh, rv, module);
                    if (!rf) {
                        recvTempClass = nullptr;
                    } else if (auto* k = llvm::dyn_cast<llvm::ConstantInt>(rf)) {
                        if (k->isZero()) recvTempClass = nullptr; else recvTempStatic = true;
                    } else {
                        recvTempFlag = rf;
                    }
                }
            }
            if (methodCallName == "getClass" && parameters.empty()
                    && receiverType) {
                auto recvCls = dynamic_pointer_cast<CajetaClass>(receiverType);
                if (recvCls && !recvCls->isInterface()
                        && recvCls->getMethods().find("getClass")
                                == recvCls->getMethods().end()) {
                    auto& cmap = CajetaType::getCanonicalMap();
                    auto wcIt = cmap.find("cajeta.reflect.Class<?>");
                    if (wcIt != cmap.end() && wcIt->second) {
                        llvm::Type* ptrTy = llvm::PointerType::get(llvmCtx, 0);
                        llvm::Value* objPtr =
                            loadIfLValue(module, receiver, exprChild);
                        llvm::FunctionType* ft =
                            llvm::FunctionType::get(ptrTy, {ptrTy}, false);
                        llvm::FunctionCallee fn =
                            module->getLlvmModule()->getOrInsertFunction(
                                "__cajeta_object_get_class", ft);
                        llvm::Value* co = builder->CreateCall(fn, {objPtr},
                            "refl.getClass");
                        resolvedType = wcIt->second;
                        return co;
                    }
                }
            }
            if (auto vecT = dynamic_pointer_cast<CajetaVector>(receiverType)) {
                llvm::Value* self = loadIfLValue(module, receiver, exprChild);
                bool isFloat = vecT->getElementType()->getLlvmType()
                                   ->isFloatingPointTy();
                bool isMask = vecT->getElementType()->getLlvmType()
                                  ->isIntegerTy(1);
                if (isMask) {
                    if (methodCallName == "all" || methodCallName == "any") {
                        if (!parameters.empty()) {
                            throw Exception(
                                "Vector mask " + methodCallName
                                + " takes no arguments",
                                "CAJETA_ERROR_VECTOR_METHOD");
                        }
                        resolvedType = CajetaType::of("boolean");
                        return methodCallName == "all"
                            ? builder->CreateAndReduce(self)
                            : builder->CreateOrReduce(self);
                    }
                    if (methodCallName == "select") {
                        if (parameters.size() != 2) {
                            throw Exception(
                                "Vector mask select expects 2 arguments "
                                "(whenTrue, whenFalse)",
                                "CAJETA_ERROR_VECTOR_METHOD");
                        }
                        llvm::Value* a = loadIfLValue(module,
                            parameters[0].expression->generateCode(module),
                            parameters[0].expression);
                        llvm::Value* b = loadIfLValue(module,
                            parameters[1].expression->generateCode(module),
                            parameters[1].expression);
                        if (!parameters[0].expression->getResolvedType())
                            parameters[0].expression->resolveTypes(module);
                        resolvedType = parameters[0].expression->getResolvedType();
                        return builder->CreateSelect(self, a, b, "vec.select");
                    }
                    throw Exception(
                        "Vector mask has no method '" + methodCallName + "'",
                        "CAJETA_ERROR_VECTOR_METHOD");
                }
                if (methodCallName == "dotAccum") {
                    if (isFloat) {
                        throw Exception(
                            "Vector.dotAccum is integer-only; use dot for float",
                            "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    if (parameters.size() != 2) {
                        throw Exception(
                            "Vector.dotAccum expects (other, acc)",
                            "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    auto* wvt = llvm::cast<llvm::FixedVectorType>(
                        self->getType());
                    if (wvt->getElementType()->getIntegerBitWidth() != 8
                            || (wvt->getNumElements() % 4) != 0) {
                        throw Exception(
                            "Vector.dotAccum needs an 8-bit vector whose lane "
                            "count is a multiple of 4 (four lanes reduce to "
                            "one int32 lane)",
                            "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    llvm::Value* other = loadIfLValue(module,
                        parameters[0].expression->generateCode(module),
                        parameters[0].expression);
                    llvm::Value* accv = loadIfLValue(module,
                        parameters[1].expression->generateCode(module),
                        parameters[1].expression);
                    auto* ovt = llvm::dyn_cast<llvm::FixedVectorType>(
                        other->getType());
                    if (ovt == nullptr
                            || ovt->getElementType()->getIntegerBitWidth() != 8
                            || ovt->getNumElements() != wvt->getNumElements()) {
                        throw Exception(
                            "Vector.dotAccum's operands must have the same "
                            "8-bit lane count",
                            "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    auto* avt = llvm::dyn_cast<llvm::FixedVectorType>(
                        accv->getType());
                    if (avt == nullptr
                            || avt->getElementType()->getIntegerBitWidth() != 32
                            || avt->getNumElements() * 4
                               != wvt->getNumElements()) {
                        throw Exception(
                            "Vector.dotAccum's accumulator must be "
                            "Vector<int32,N> where the operands are 4N lanes "
                            "of int8",
                            "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    const char* fsd = std::getenv(
                        "CAJETA_SIMD_SCALAR_FALLBACK");
                    bool forceScalarDot = fsd && fsd[0] && fsd[0] != '0';
                    bool wUnsigned = (vecT->getElementType()->getTypeFlags()
                                      & SIGNED_FLAG) == 0;
                    resolvedType = CajetaVector::validateAndCreate(
                        module, CajetaType::of("int32"),
                        avt->getNumElements());
                    vecops::DotAccumTargets dtgt;
                    dtgt.vnni        = module->targetHasIntDotAccum();
                    dtgt.avx2        = module->targetHasAvx2();
                    dtgt.armDotProd  = module->targetHasArmDotProd();
                    dtgt.armI8mm     = module->targetHasArmI8mm();
                    dtgt.forceScalar = forceScalarDot;
                    return vecops::dotAccum(*builder,
                        builder->GetInsertBlock()->getModule(), self, other,
                        accv, wUnsigned, dtgt);
                }
                if (methodCallName == "dot") {
                    if (isFloat) {
                        if (parameters.size() != 1) {
                            throw Exception("Vector.dot expects 1 argument",
                                            "CAJETA_ERROR_VECTOR_METHOD");
                        }
                        llvm::Value* other = loadIfLValue(module,
                            parameters[0].expression->generateCode(module),
                            parameters[0].expression);
                        resolvedType = vecT->getElementType();
                        return vecops::dot(*builder, self, other, true);
                    }
                    auto* ivt = llvm::cast<llvm::FixedVectorType>(self->getType());
                    if (ivt->getNumElements() != 4 ||
                        ivt->getElementType()->getIntegerBitWidth() != 8) {
                        throw Exception(
                            "integer Vector.dot currently supports 4-lane 8-bit "
                            "vectors (DP4a): Vector<int8,4> / Vector<uint8,4>",
                            "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    if (parameters.size() != 1 && parameters.size() != 2) {
                        throw Exception(
                            "Vector.dot expects (other) or (other, acc)",
                            "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    llvm::Value* other = loadIfLValue(module,
                        parameters[0].expression->generateCode(module),
                        parameters[0].expression);
                    llvm::Type* i32 =
                        llvm::Type::getInt32Ty(builder->getContext());
                    llvm::Value* acc;
                    if (parameters.size() == 2) {
                        llvm::Value* a2 = loadIfLValue(module,
                            parameters[1].expression->generateCode(module),
                            parameters[1].expression);
                        acc = builder->CreateIntCast(a2, i32, /*isSigned=*/true,
                                                     "dot.acc");
                    } else {
                        acc = llvm::ConstantInt::get(i32, 0);
                    }
                    bool sgn = (vecT->getElementType()->getTypeFlags()
                                & SIGNED_FLAG) != 0;
                    resolvedType = CajetaType::of("int32");
                    return vecops::idotWiden(*builder, self, other, acc, sgn,
                                             sgn);
                }
                if (methodCallName == "eqMask") {
                    if (parameters.size() != 1) {
                        throw Exception("Vector.eqMask expects 1 argument",
                                        "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    llvm::Value* needle = loadIfLValue(module,
                        parameters[0].expression->generateCode(module),
                        parameters[0].expression);
                    llvm::Value* nEl = vecops::coerceScalar(*builder, needle,
                        vecT->getElementType()->getLlvmType());
                    llvm::Value* bits = vecops::eqMask(*builder, self, nEl);
                    resolvedType = CajetaType::of("int32");
                    return builder->CreateZExt(bits,
                        llvm::Type::getInt32Ty(builder->getContext()),
                        "eqmask.i32");
                }
                // --- cajeta-llama Unit 17: the quantized-unpack toolkit ---

                if (methodCallName == "tableLookup") {
                    if (parameters.size() != 1) {
                        throw Exception("Vector.tableLookup expects (indices)",
                                        "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    auto* svt =
                        llvm::cast<llvm::FixedVectorType>(self->getType());
                    if (svt->getNumElements() != 16 ||
                            svt->getElementType()->getIntegerBitWidth() != 8) {
                        throw Exception(
                            "Vector.tableLookup requires a 16-lane 8-bit "
                            "receiver (the table): Vector<int8,16> / "
                            "Vector<uint8,16>", "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    llvm::Value* idxV = loadIfLValue(module,
                        parameters[0].expression->generateCode(module),
                        parameters[0].expression);
                    if (idxV->getType() != self->getType()) {
                        throw Exception(
                            "Vector.tableLookup indices must match the "
                            "table's Vector shape",
                            "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    const char* fs = std::getenv("CAJETA_SIMD_SCALAR_FALLBACK");
                    bool forceScalar = fs && fs[0] && fs[0] != '0';
                    llvm::Module* tlm =
                        builder->GetInsertBlock()->getParent()->getParent();
                    resolvedType = std::static_pointer_cast<CajetaType>(vecT);
                    return vecops::tableLookup(*builder, tlm, self, idxV,
                                               forceScalar);
                }
                if (methodCallName == "widenLo" || methodCallName == "widenHi") {
                    if (!parameters.empty()) {
                        throw Exception("Vector.widenLo/widenHi take no "
                                        "arguments", "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    auto elemT = vecT->getElementType();
                    if (isFloat) {
                        throw Exception("Vector.widen* requires an integer "
                                        "element type", "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    unsigned w = elemT->getLlvmType()->getIntegerBitWidth();
                    unsigned n = vecT->getLanes();
                    if (w >= 64 || n < 2) {
                        throw Exception("Vector.widen*: element width must be "
                                        "< 64 and lane count >= 2",
                                        "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    bool sgn = (elemT->getTypeFlags() & SIGNED_FLAG) != 0;
                    std::string next = (sgn ? "int" : "uint")
                        + std::to_string(w * 2);
                    resolvedType = CajetaVector::getOrCreate(module,
                        CajetaType::of(next), n / 2);
                    return vecops::widenHalf(*builder, self,
                        methodCallName == "widenLo", sgn);
                }
                // No instruction: LLVM integer types carry no signedness, so this only changes
                // what the lowerings decide from the element type - dotAccum's VNNI tier keys
                // off it, and without this the fastest tier is unreachable and silently so.
                if (methodCallName == "asUnsigned"
                        || methodCallName == "asSigned") {
                    if (!parameters.empty()) {
                        throw Exception(
                            "Vector.asUnsigned/asSigned take no arguments",
                            "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    if (isFloat) {
                        throw Exception(
                            "Vector.asUnsigned/asSigned require an integer "
                            "element type", "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    unsigned w = vecT->getElementType()
                        ->getLlvmType()->getIntegerBitWidth();
                    bool want = methodCallName == "asUnsigned";
                    std::string next = (want ? "uint" : "int")
                        + std::to_string(w);
                    resolvedType = CajetaVector::getOrCreate(module,
                        CajetaType::of(next), vecT->getLanes());
                    return self;
                }
                if (methodCallName == "dotSum") {
                    if (parameters.size() != 2) {
                        throw Exception("Vector.dotSum expects (other, acc)",
                                        "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    auto elemT = vecT->getElementType();
                    if (isFloat
                        || elemT->getLlvmType()->getIntegerBitWidth() != 8
                        || (vecT->getLanes() % 4) != 0) {
                        throw Exception(
                            "Vector.dotSum needs an 8-bit vector whose lane "
                            "count is a multiple of 4",
                            "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    llvm::Value* other = loadIfLValue(module,
                        parameters[0].expression->generateCode(module),
                        parameters[0].expression);
                    llvm::Value* acc = loadIfLValue(module,
                        parameters[1].expression->generateCode(module),
                        parameters[1].expression);
                    bool sgn = (elemT->getTypeFlags() & SIGNED_FLAG) != 0;
                    unsigned lanes = vecT->getLanes();
                    auto* wideTy = llvm::FixedVectorType::get(
                        llvm::Type::getInt32Ty(*module->getLlvmContext()),
                        lanes);
                    llvm::Value* ww = sgn
                        ? builder->CreateSExt(self, wideTy, "dotsum.w")
                        : builder->CreateZExt(self, wideTy, "dotsum.w");
                    llvm::Value* aw = builder->CreateSExt(other, wideTy,
                                                          "dotsum.a");
                    llvm::Value* prod = builder->CreateMul(ww, aw,
                                                           "dotsum.p");
                    llvm::Value* sum = builder->CreateAddReduce(prod);
                    resolvedType = CajetaType::of("int32");
                    return builder->CreateAdd(sum, acc, "dotsum");
                }
                if (methodCallName == "lut4") {
                    if (parameters.size() != 1) {
                        throw Exception("Vector.lut4 expects (table)",
                                        "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    auto elemT = vecT->getElementType();
                    if (isFloat
                        || elemT->getLlvmType()->getIntegerBitWidth() != 8) {
                        throw Exception("Vector.lut4 needs an 8-bit index "
                                        "vector", "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    llvm::Value* table = loadIfLValue(module,
                        parameters[0].expression->generateCode(module),
                        parameters[0].expression);
                    auto* tvt = llvm::dyn_cast<llvm::FixedVectorType>(
                        table->getType());
                    if (tvt == nullptr
                        || !tvt->getElementType()->isIntegerTy(8)
                        || tvt->getNumElements() != 16) {
                        throw Exception("Vector.lut4's table must be a 16-lane "
                                        "8-bit vector",
                                        "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    resolvedType = CajetaVector::getOrCreate(module,
                        vecT->getElementType(), vecT->getLanes());
                    return vecops::lut4Portable(*builder, self, table);
                }
                if (methodCallName == "asWords"
                        || methodCallName == "asBytes") {
                    if (!parameters.empty()) {
                        throw Exception(
                            "Vector.asWords/asBytes take no arguments",
                            "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    if (isFloat) {
                        throw Exception(
                            "Vector.asWords/asBytes require an integer "
                            "element type", "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    unsigned w = vecT->getElementType()
                        ->getLlvmType()->getIntegerBitWidth();
                    unsigned n = vecT->getLanes();
                    if (methodCallName == "asWords") {
                        if (w != 8 || (n % 4) != 0) {
                            throw Exception(
                                "Vector.asWords needs an 8-bit vector whose "
                                "lane count is a multiple of 4",
                                "CAJETA_ERROR_VECTOR_METHOD");
                        }
                        resolvedType = CajetaVector::getOrCreate(module,
                            CajetaType::of("int32"), n / 4);
                        return builder->CreateBitCast(self,
                            llvm::FixedVectorType::get(
                                llvm::Type::getInt32Ty(
                                    *module->getLlvmContext()), n / 4),
                            "as.words");
                    }
                    if (w != 32) {
                        throw Exception("Vector.asBytes needs a 32-bit "
                                        "vector", "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    resolvedType = CajetaVector::getOrCreate(module,
                        CajetaType::of("int8"), n * 4);
                    return builder->CreateBitCast(self,
                        llvm::FixedVectorType::get(
                            llvm::Type::getInt8Ty(*module->getLlvmContext()),
                            n * 4), "as.bytes");
                }
                if (methodCallName == "narrow") {
                    if (parameters.size() != 1) {
                        throw Exception("Vector.narrow expects (other) — the "
                                        "high half's source",
                                        "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    auto elemT = vecT->getElementType();
                    if (isFloat) {
                        throw Exception("Vector.narrow requires an integer "
                                        "element type", "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    unsigned w = elemT->getLlvmType()->getIntegerBitWidth();
                    if (w <= 8) {
                        throw Exception("Vector.narrow: element width must be "
                                        "> 8", "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    llvm::Value* other = loadIfLValue(module,
                        parameters[0].expression->generateCode(module),
                        parameters[0].expression);
                    if (other->getType() != self->getType()) {
                        throw Exception("Vector.narrow: other must have the "
                                        "receiver's Vector type",
                                        "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    bool sgn = (elemT->getTypeFlags() & SIGNED_FLAG) != 0;
                    std::string prev = (sgn ? "int" : "uint")
                        + std::to_string(w / 2);
                    resolvedType = CajetaVector::getOrCreate(module,
                        CajetaType::of(prev), vecT->getLanes() * 2);
                    return vecops::narrowPair(*builder, self, other);
                }
                if (methodCallName == "toF32") {
                    if (!parameters.empty()) {
                        throw Exception("Vector.toF32 takes no arguments",
                                        "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    auto* svt =
                        llvm::cast<llvm::FixedVectorType>(self->getType());
                    if (isFloat) {
                        unsigned w = svt->getElementType()
                            ->getPrimitiveSizeInBits();
                        if (w >= 32) {
                            throw Exception("Vector.toF32 needs an integer or "
                                            "narrower-float element type "
                                            "(float16 / bfloat16)",
                                            "CAJETA_ERROR_VECTOR_METHOD");
                        }
                        resolvedType = CajetaVector::getOrCreate(module,
                            CajetaType::of("float32"), vecT->getLanes());
                        return vecops::convertFpLanes(*builder, self,
                            llvm::Type::getFloatTy(builder->getContext()));
                    }
                    bool sgn = (vecT->getElementType()->getTypeFlags()
                                & SIGNED_FLAG) != 0;
                    resolvedType = CajetaVector::getOrCreate(module,
                        CajetaType::of("float32"), vecT->getLanes());
                    return vecops::convertToF32(*builder, self, sgn);
                }
                if (methodCallName == "toF16") {
                    if (!parameters.empty() || !isFloat) {
                        throw Exception("Vector.toF16 takes no arguments and "
                                        "a float-element receiver",
                                        "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    resolvedType = CajetaVector::getOrCreate(module,
                        CajetaType::of("float16"), vecT->getLanes());
                    return vecops::convertFpLanes(*builder, self,
                        llvm::Type::getHalfTy(builder->getContext()));
                }
                if (methodCallName == "toI32") {
                    if (!parameters.empty() || !isFloat) {
                        throw Exception("Vector.toI32 takes no arguments and "
                                        "a float-element receiver",
                                        "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    resolvedType = CajetaVector::getOrCreate(module,
                        CajetaType::of("int32"), vecT->getLanes());
                    return vecops::convertToI32(*builder, self);
                }
                if (methodCallName == "bitcastF32") {
                    auto* svt =
                        llvm::cast<llvm::FixedVectorType>(self->getType());
                    if (!parameters.empty() || isFloat ||
                            svt->getElementType()->getIntegerBitWidth() != 32) {
                        throw Exception("Vector.bitcastF32 takes no arguments "
                                        "and a 32-bit-integer-element receiver",
                                        "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    resolvedType = CajetaVector::getOrCreate(module,
                        CajetaType::of("float32"), vecT->getLanes());
                    return vecops::bitcastLanes(*builder, self,
                        llvm::Type::getFloatTy(builder->getContext()));
                }
                if (methodCallName == "bitcastI32") {
                    if (!parameters.empty() || !isFloat) {
                        throw Exception("Vector.bitcastI32 takes no arguments "
                                        "and a float32-element receiver",
                                        "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    resolvedType = CajetaVector::getOrCreate(module,
                        CajetaType::of("int32"), vecT->getLanes());
                    return vecops::bitcastLanes(*builder, self,
                        llvm::Type::getInt32Ty(builder->getContext()));
                }
                if (methodCallName == "compressStore") {
                    if (parameters.size() != 2) {
                        throw Exception(
                            "Vector.compressStore expects (dst, mask): dst an "
                            "array element l-value (e.g. out[w]), mask an N-lane "
                            "comparison mask", "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    llvm::Value* ptr =
                        parameters[0].expression->generateCode(module);
                    if (!ptr || !ptr->getType()->isPointerTy()) {
                        throw Exception(
                            "Vector.compressStore destination must be an array "
                            "element l-value (e.g. out[w])",
                            "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    llvm::Value* mask = loadIfLValue(module,
                        parameters[1].expression->generateCode(module),
                        parameters[1].expression);
                    auto* mvt = llvm::dyn_cast<llvm::FixedVectorType>(
                        mask->getType());
                    auto* dvt =
                        llvm::cast<llvm::FixedVectorType>(self->getType());
                    if (!mvt || !mvt->getElementType()->isIntegerTy(1) ||
                            mvt->getNumElements() != dvt->getNumElements()) {
                        throw Exception(
                            "Vector.compressStore mask must be an N-lane "
                            "comparison mask matching this vector's lane count",
                            "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    vecops::compressStore(*builder, ptr, self, mask);
                    resolvedType = CajetaType::of("int32");
                    return vecops::maskPopcount(*builder, mask);
                }
                if (methodCallName == "length") {
                    if (!isFloat) {
                        throw Exception(
                            "Vector.length requires a floating-point element type",
                            "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    resolvedType = vecT->getElementType();
                    return vecops::length(*builder, self);
                }
                if (methodCallName == "normalize") {
                    if (!isFloat) {
                        throw Exception(
                            "Vector.normalize requires a floating-point element type",
                            "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    resolvedType = vecT;
                    return vecops::normalize(*builder, self);
                }
                llvm::Type* elemLlvm = vecT->getElementType()->getLlvmType();
                bool isSigned =
                    (vecT->getElementType()->getTypeFlags() & SIGNED_FLAG) != 0;
                if (methodCallName == "min" || methodCallName == "max") {
                    if (!isFloat) {
                        throw Exception(
                            "Vector." + methodCallName + " requires a "
                            "floating-point element type (integer min/max is a "
                            "follow-on)", "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    if (parameters.size() != 1) {
                        throw Exception(
                            "Vector." + methodCallName + " expects 1 argument",
                            "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    llvm::Value* other = loadIfLValue(module,
                        parameters[0].expression->generateCode(module),
                        parameters[0].expression);
                    resolvedType = vecT;
                    return methodCallName == "min"
                        ? vecops::vmin(*builder, self, other, isFloat, isSigned)
                        : vecops::vmax(*builder, self, other, isFloat, isSigned);
                }
                if (methodCallName == "clamp") {
                    if (!isFloat) {
                        throw Exception(
                            "Vector.clamp requires a floating-point element type "
                            "(integer clamp is a follow-on)",
                            "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    if (parameters.size() != 2) {
                        throw Exception(
                            "Vector.clamp expects 2 arguments (lo, hi)",
                            "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    llvm::Value* lo = vecops::coerceScalar(*builder,
                        loadIfLValue(module,
                            parameters[0].expression->generateCode(module),
                            parameters[0].expression), elemLlvm);
                    llvm::Value* hi = vecops::coerceScalar(*builder,
                        loadIfLValue(module,
                            parameters[1].expression->generateCode(module),
                            parameters[1].expression), elemLlvm);
                    resolvedType = vecT;
                    return vecops::clamp(*builder, self, lo, hi, isFloat, isSigned);
                }
                if (methodCallName == "lerp") {
                    if (!isFloat) {
                        throw Exception(
                            "Vector.lerp requires a floating-point element type",
                            "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    if (parameters.size() != 2) {
                        throw Exception(
                            "Vector.lerp expects 2 arguments (other, t)",
                            "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    llvm::Value* other = loadIfLValue(module,
                        parameters[0].expression->generateCode(module),
                        parameters[0].expression);
                    llvm::Value* t = vecops::coerceScalar(*builder,
                        loadIfLValue(module,
                            parameters[1].expression->generateCode(module),
                            parameters[1].expression), elemLlvm);
                    resolvedType = vecT;
                    return vecops::lerp(*builder, self, other, t);
                }
                if (methodCallName == "cross" || methodCallName == "reflect"
                        || methodCallName == "refract"
                        || methodCallName == "distance") {
                    if (!isFloat) {
                        throw Exception(
                            "Vector." + methodCallName + " requires a "
                            "floating-point element type",
                            "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    if (methodCallName == "cross" && vecT->getLanes() != 3) {
                        throw Exception(
                            "Vector.cross requires 3-component vectors (got "
                            + vecT->toCanonical() + ")",
                            "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    size_t want = methodCallName == "refract" ? 2 : 1;
                    if (parameters.size() != want) {
                        throw Exception(
                            "Vector." + methodCallName + " expects "
                            + std::to_string(want) + " argument(s)",
                            "CAJETA_ERROR_VECTOR_METHOD");
                    }
                    llvm::Value* other = loadIfLValue(module,
                        parameters[0].expression->generateCode(module),
                        parameters[0].expression);
                    if (methodCallName == "cross") {
                        resolvedType = vecT;
                        return vecops::cross(*builder, self, other);
                    }
                    if (methodCallName == "reflect") {
                        resolvedType = vecT;
                        return vecops::reflect(*builder, self, other);
                    }
                    if (methodCallName == "distance") {
                        resolvedType = vecT->getElementType();
                        return vecops::distance(*builder, self, other);
                    }
                    llvm::Value* eta = vecops::coerceScalar(*builder,
                        loadIfLValue(module,
                            parameters[1].expression->generateCode(module),
                            parameters[1].expression), elemLlvm);
                    resolvedType = vecT;
                    return vecops::refract(*builder, self, other, eta);
                }
                throw Exception(
                    "Vector has no method '" + methodCallName + "'",
                    "CAJETA_ERROR_VECTOR_METHOD");
            }
            if (auto matT = dynamic_pointer_cast<CajetaMatrix>(receiverType)) {
                llvm::Value* self = loadIfLValue(module, receiver, exprChild);
                bool isFloat = matT->getElementType()->getLlvmType()
                                   ->isFloatingPointTy();
                unsigned R = matT->getRows(), C = matT->getCols();
                if (matT->getElementType()->getLlvmType()->isIntegerTy(1)) {
                    if (methodCallName == "all" || methodCallName == "any") {
                        if (!parameters.empty()) {
                            throw Exception(
                                "Matrix mask " + methodCallName
                                + " takes no arguments",
                                "CAJETA_ERROR_MATRIX_METHOD");
                        }
                        resolvedType = CajetaType::of("boolean");
                        return methodCallName == "all"
                            ? builder->CreateAndReduce(self)
                            : builder->CreateOrReduce(self);
                    }
                    if (methodCallName == "select") {
                        if (parameters.size() != 2) {
                            throw Exception(
                                "Matrix mask select expects 2 arguments",
                                "CAJETA_ERROR_MATRIX_METHOD");
                        }
                        llvm::Value* a = loadIfLValue(module,
                            parameters[0].expression->generateCode(module),
                            parameters[0].expression);
                        llvm::Value* b = loadIfLValue(module,
                            parameters[1].expression->generateCode(module),
                            parameters[1].expression);
                        if (!parameters[0].expression->getResolvedType())
                            parameters[0].expression->resolveTypes(module);
                        resolvedType = parameters[0].expression->getResolvedType();
                        return builder->CreateSelect(self, a, b, "mat.select");
                    }
                    throw Exception(
                        "Matrix mask has no method '" + methodCallName + "'",
                        "CAJETA_ERROR_MATRIX_METHOD");
                }
                if (methodCallName == "transpose") {
                    resolvedType = CajetaMatrix::getOrCreate(
                        module, matT->getElementType(), C, R);
                    return matops::transpose(*builder, self, R, C);
                }
                if (methodCallName == "identity") {
                    if (R != C) {
                        throw Exception(
                            "Matrix.identity requires a square matrix (got "
                            + matT->toCanonical() + ")",
                            "CAJETA_ERROR_MATRIX_METHOD");
                    }
                    resolvedType = matT;
                    return matops::identity(
                        *builder, matT->getElementType()->getLlvmType(), R);
                }
                if (methodCallName == "row" || methodCallName == "col") {
                    if (parameters.size() != 1) {
                        throw Exception(
                            "Matrix." + methodCallName + " expects 1 argument",
                            "CAJETA_ERROR_MATRIX_METHOD");
                    }
                    llvm::Value* idx = loadIfLValue(module,
                        parameters[0].expression->generateCode(module),
                        parameters[0].expression);
                    llvm::Type* i32Ty =
                        llvm::Type::getInt32Ty(*module->getLlvmContext());
                    if (idx->getType() != i32Ty)
                        idx = builder->CreateIntCast(idx, i32Ty, false,
                                                     "mat.meth.idx");
                    if (methodCallName == "row") {
                        resolvedType = CajetaVector::getOrCreate(
                            module, matT->getElementType(), C);
                        return matops::row(*builder, self, R, C, idx);
                    }
                    resolvedType = CajetaVector::getOrCreate(
                        module, matT->getElementType(), R);
                    return matops::col(*builder, self, R, C, idx);
                }
                if (methodCallName == "hadamard") {
                    if (parameters.size() != 1) {
                        throw Exception("Matrix.hadamard expects 1 argument",
                                        "CAJETA_ERROR_MATRIX_METHOD");
                    }
                    auto other = parameters[0].expression;
                    llvm::Value* b = loadIfLValue(module,
                        other->generateCode(module), other);
                    resolvedType = matT;
                    return matops::hadamard(*builder, self, b, isFloat);
                }
                if (methodCallName == "determinant"
                        || methodCallName == "inverse") {
                    if (R != C || R < 2 || R > 4) {
                        throw Exception(
                            "Matrix." + methodCallName + " requires a square "
                            "2x2, 3x3, or 4x4 matrix (got " + matT->toCanonical()
                            + ")", "CAJETA_ERROR_MATRIX_METHOD");
                    }
                    if (!isFloat) {
                        throw Exception(
                            "Matrix." + methodCallName + " requires a "
                            "floating-point element type",
                            "CAJETA_ERROR_MATRIX_METHOD");
                    }
                    if (!parameters.empty()) {
                        throw Exception(
                            "Matrix." + methodCallName + " takes no arguments",
                            "CAJETA_ERROR_MATRIX_METHOD");
                    }
                    if (methodCallName == "determinant") {
                        resolvedType = matT->getElementType();
                        return matops::determinant(*builder, self, R);
                    }
                    resolvedType = matT;
                    return matops::inverse(*builder, self, R);
                }
                throw Exception(
                    "Matrix has no method '" + methodCallName + "'",
                    "CAJETA_ERROR_MATRIX_METHOD");
            }
            if (auto qT = dynamic_pointer_cast<CajetaQuaternion>(receiverType)) {
                llvm::Value* self = loadIfLValue(module, receiver, exprChild);
                if (methodCallName == "normalize") {
                    resolvedType = qT;
                    return vecops::normalize(*builder, self);
                }
                if (methodCallName == "conjugate") {
                    resolvedType = qT;
                    return quatops::conjugate(*builder, self);
                }
                if (methodCallName == "length") {
                    resolvedType = qT->getElementType();
                    return vecops::length(*builder, self);
                }
                if (methodCallName == "dot") {
                    if (parameters.size() != 1) {
                        throw Exception("Quaternion.dot expects 1 argument",
                                        "CAJETA_ERROR_QUATERNION_METHOD");
                    }
                    llvm::Value* other = loadIfLValue(module,
                        parameters[0].expression->generateCode(module),
                        parameters[0].expression);
                    resolvedType = qT->getElementType();
                    return vecops::dot(*builder, self, other, /*isFloat=*/true);
                }
                if (methodCallName == "nlerp") {
                    if (parameters.size() != 2) {
                        throw Exception(
                            "Quaternion.nlerp expects 2 arguments (other, t)",
                            "CAJETA_ERROR_QUATERNION_METHOD");
                    }
                    llvm::Value* other = loadIfLValue(module,
                        parameters[0].expression->generateCode(module),
                        parameters[0].expression);
                    llvm::Value* t = vecops::coerceScalar(*builder,
                        loadIfLValue(module,
                            parameters[1].expression->generateCode(module),
                            parameters[1].expression),
                        qT->getElementType()->getLlvmType());
                    resolvedType = qT;
                    return quatops::nlerp(*builder, self, other, t);
                }
                if (methodCallName == "slerp") {
                    if (parameters.size() != 2) {
                        throw Exception(
                            "Quaternion.slerp expects 2 arguments (other, t)",
                            "CAJETA_ERROR_QUATERNION_METHOD");
                    }
                    llvm::Value* other = loadIfLValue(module,
                        parameters[0].expression->generateCode(module),
                        parameters[0].expression);
                    llvm::Value* t = vecops::coerceScalar(*builder,
                        loadIfLValue(module,
                            parameters[1].expression->generateCode(module),
                            parameters[1].expression),
                        qT->getElementType()->getLlvmType());
                    quatops::TrigEmitter trig =
                        [&](const std::string& nm,
                            llvm::ArrayRef<llvm::Value*> as) -> llvm::Value* {
                            return builder->CreateUnaryIntrinsic(
                                nm == "sin" ? llvm::Intrinsic::sin
                                            : llvm::Intrinsic::acos, as[0]);
                        };
                    resolvedType = qT;
                    return quatops::slerp(*builder, self, other, t, trig);
                }
                throw Exception(
                    "Quaternion has no method '" + methodCallName + "'",
                    "CAJETA_ERROR_QUATERNION_METHOD");
            }
            if (receiver) {
                if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(receiver)) {
                    // Three alloca shapes reach here: a ptr / primitive slot loads through to the
                    // instance pointer, while an sret slot and a @ValueType slot hold the aggregate
                    // itself - their address IS the receiver, so loading would pass it by value.
                    auto recvCls = dynamic_pointer_cast<CajetaClass>(receiverType);
                    bool valueTypeReceiver = recvCls && recvCls->isValueType()
                        && !a->getAllocatedType()->isPointerTy();
                    bool sretStructSlot = a->getAllocatedType()->isStructTy();
                    if (!valueTypeReceiver && !sretStructSlot) {
                        receiver = builder->CreateLoad(a->getAllocatedType(), a);
                    }
                } else if (dynamic_pointer_cast<ArrayIndexExpression>(exprChild)) {
                    // Class-ref array elements store an 8-byte `ptr`; interface elements store a
                    // 24-byte fat-pointer body INLINE, so the element GEP already is that body.
                    auto elemRc = dynamic_pointer_cast<CajetaClass>(receiverType);
                    bool elemIsInterface = elemRc && elemRc->isInterface();
                    bool elemIsValueType = elemRc && elemRc->isValueType();
                    if (!elemIsInterface && !elemIsValueType) {
                        receiver = builder->CreateLoad(
                            llvm::PointerType::get(*module->getLlvmContext(), 0), receiver);
                    }
                } else if ((dynamic_pointer_cast<DotExpression>(exprChild)
                            || dynamic_pointer_cast<IdentifierExpression>(exprChild))
                        && (llvm::isa<llvm::GetElementPtrInst>(receiver)
                            || llvm::isa<llvm::GlobalVariable>(receiver))
                        && receiverType
                        && dynamic_pointer_cast<CajetaClass>(receiverType)
                        && !dynamic_pointer_cast<CajetaView>(receiverType)) {
                    // A class-ref or array field's slot stores a `ptr` to the referent, so load
                    // through it; view, interface and value-type fields stay inline, so their slot
                    // address already IS the language-level value.
                    auto rc = dynamic_pointer_cast<CajetaClass>(receiverType);
                    if (!rc->isInterface() && !rc->isValueType()) {
                        receiver = builder->CreateLoad(
                            llvm::PointerType::get(*module->getLlvmContext(), 0),
                            receiver);
                    }
                } else if (!receiver->getType()->isPointerTy()
                        && (receiver->getType()->isStructTy()
                            || receiver->getType()->isVectorTy()
                            || receiver->getType()->isArrayTy())) {
                    auto recvVc = dynamic_pointer_cast<CajetaClass>(receiverType);
                    if (recvVc && recvVc->isValueType()) {
                        llvm::Value* spill =
                            builder->CreateAlloca(receiver->getType(),
                                nullptr, "fresh.value.recv");
                        builder->CreateStore(receiver, spill);
                        receiver = spill;
                    }
                }
            }
            if (!receiver && !receiverType) {
                if (auto idExpr = dynamic_pointer_cast<IdentifierExpression>(exprChild)) {
                    auto scoped = CajetaType::ofScoped(
                        idExpr->getTextValue(), module);
                    if (scoped) {
                        if (scoped->getTypeFlags() & ENUM_FLAG) {
                            receiverType = scoped;
                        }
                        if (auto cls = dynamic_pointer_cast<CajetaClass>(scoped)) {
                            if (cls->isTemplate()) {
                                std::vector<CajetaTypePtr> wildArgs;
                                for (size_t i = 0;
                                        i < cls->getTypeParameters().size(); ++i) {
                                    wildArgs.push_back(
                                        CajetaType::wildcardSentinel());
                                }
                                auto inst = cls->instantiate(wildArgs);
                                receiverType = inst ? std::static_pointer_cast<
                                    CajetaType>(inst) : scoped;
                            } else {
                                receiverType = scoped;
                            }
                        }
                    }
                }
            }
        }

        // ----- cajeta.math.DType.codeOf<T>() intrinsic -----
        // Folds T's reified dtype to a constant i32: code = (kind << 16) | (bits << 4)
        // | variant, kind BOOL=0 INT=1 UINT=2 FLOAT=3 (variant splits same-width floats).
        if (receiverType && methodCallName == "codeOf"
                && receiverType->toCanonical() == "cajeta.math.DType"
                && explicitMethodTypeArgs.size() == 1
                && explicitMethodTypeArgs[0]) {
            CajetaTypePtr dtypeT = explicitMethodTypeArgs[0];
            CajetaTypeFlags f = dtypeT->getTypeFlags();
            uint64_t typeId = (uint64_t) (f & TYPE_ID_MASK);
            int32_t kind = 0;
            int32_t bits = 0;
            int32_t variant = 0;
            if (typeId == (uint64_t) BOOLEAN_ID) {
                kind = 0;
                bits = 8;
            } else {
                if (f & BIT_4_FLAG) bits = 4;
                else if (f & BIT_6_FLAG) bits = 6;
                else if (f & BIT_8_FLAG) bits = 8;
                else if (f & BIT_16_FLAG) bits = 16;
                else if (f & BIT_32_FLAG) bits = 32;
                else if (f & BIT_64_FLAG) bits = 64;
                else if (f & BIT_128_FLAG) bits = 128;
                if (f & FLOAT_FLAG) {
                    kind = 3;
                    if (typeId == (uint64_t) BFLOAT16_ID) variant = 1;
                    else if (typeId == (uint64_t) FLOAT8E4M3_ID) variant = 2;
                    else if (typeId == (uint64_t) FLOAT8E5M2_ID) variant = 3;
                    else if (typeId == (uint64_t) FLOAT8E4M3FNUZ_ID) variant = 4;
                    else if (typeId == (uint64_t) FLOAT8E5M2FNUZ_ID) variant = 5;
                    else if (typeId == (uint64_t) FLOAT6E2M3_ID) variant = 6;
                    else if (typeId == (uint64_t) FLOAT6E3M2_ID) variant = 7;
                    else if (typeId == (uint64_t) FLOAT4E2M1_ID) variant = 8;
                } else if (f & INT_FLAG) {
                    kind = (f & SIGNED_FLAG) ? 1 : 2;
                }
            }
            int32_t code = (kind << 16) | (bits << 4) | variant;
            resolvedType = CajetaType::of("int32");
            return llvm::ConstantInt::get(
                llvm::Type::getInt32Ty(llvmCtx), (uint64_t) (uint32_t) code);
        }

        if (receiver && receiverType) {
            auto recvClass = dynamic_pointer_cast<CajetaClass>(receiverType);
            bool sameNamedMethod = false;
            if (recvClass) {
                std::function<bool(const CajetaClassPtr&)> hasMethod =
                    [&](const CajetaClassPtr& cls) -> bool {
                        for (auto& mEntry : cls->getMethods()) {
                            if (mEntry.second
                                    && mEntry.second->getName() == methodCallName
                                    && !mEntry.second->isConstructor()) {
                                return true;
                            }
                        }
                        for (auto& parent : cls->getSuperClasses()) {
                            if (hasMethod(parent)) return true;
                        }
                        return false;
                    };
                sameNamedMethod = hasMethod(recvClass);
            }
            if (recvClass && !sameNamedMethod) {
                StructurePropertyPtr fnField;
                CajetaClassPtr fieldOwner;
                std::function<bool(const CajetaClassPtr&)> findFnField =
                    [&](const CajetaClassPtr& cls) -> bool {
                        auto& props = cls->getProperties();
                        auto it = props.find(methodCallName);
                        if (it != props.end()
                                && dynamic_pointer_cast<CajetaFunctionType>(
                                    it->second->getType())) {
                            fnField = it->second;
                            fieldOwner = cls;
                            return true;
                        }
                        for (auto& parent : cls->getSuperClasses()) {
                            if (findFnField(parent)) return true;
                        }
                        return false;
                    };
                if (findFnField(recvClass)) {
                    auto fnType = dynamic_pointer_cast<CajetaFunctionType>(
                        fnField->getType());
                    llvm::Type* ptrTy = llvm::PointerType::get(llvmCtx, 0);
                    llvm::StructType* closureTy = llvm::StructType::get(
                        llvmCtx, {ptrTy, ptrTy, ptrTy});
                    unsigned fieldIdx =
                        (unsigned) fieldOwner->getFieldLlvmIndex(fnField);
                    llvm::Value* slot = builder->CreateStructGEP(
                        recvClass->getLlvmType(), receiver, fieldIdx,
                        methodCallName + "_slot");
                    llvm::Value* closurePtr = builder->CreateLoad(
                        ptrTy, slot, "closure_ptr");
                    llvm::Value* fnSlot = builder->CreateStructGEP(
                        closureTy, closurePtr, 0, "closure.fn");
                    llvm::Value* callee = builder->CreateLoad(
                        ptrTy, fnSlot, "fn_ptr");
                    llvm::Value* capSlot = builder->CreateStructGEP(
                        closureTy, closurePtr, 1, "closure.captures");
                    llvm::Value* captures = builder->CreateLoad(
                        ptrTy, capSlot, "captures_ptr");
                    llvm::Value* sretSlot = nullptr;
                    auto retClass = dynamic_pointer_cast<CajetaClass>(fnType->getReturnType());
                    if (fnType->usesSret() && retClass) {
                        llvm::Function* curFn = builder->GetInsertBlock()->getParent();
                        llvm::IRBuilder<> entryBuilder(
                            &curFn->getEntryBlock(),
                            curFn->getEntryBlock().begin());
                        sretSlot = entryBuilder.CreateAlloca(
                            retClass->getLlvmType(), nullptr, "fn_sret");
                    }
                    vector<llvm::Value*> args;
                    if (sretSlot) args.push_back(sretSlot);
                    args.push_back(captures);
                    size_t baseIdx = sretSlot ? 2 : 1;
                    llvm::FunctionType* sig = fnType->getLlvmFunctionType();
                    for (size_t i = 0; i < parameters.size(); ++i) {
                        llvm::Value* v = parameters[i].expression->generateCode(module);
                        if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(v)) {
                            v = builder->CreateLoad(a->getAllocatedType(), a);
                        }
                        size_t sigIdx = baseIdx + i;
                        if (sig && sigIdx < sig->getNumParams() && v
                                && v->getType() != sig->getParamType(sigIdx)) {
                            llvm::Type* expected = sig->getParamType(sigIdx);
                            if (expected->isIntegerTy() && v->getType()->isIntegerTy()) {
                                v = builder->CreateIntCast(v, expected, /*isSigned=*/true);
                            } else if (expected->isFloatingPointTy()
                                    && v->getType()->isFloatingPointTy()) {
                                v = builder->CreateFPCast(v, expected);
                            } else if (expected->isPointerTy()
                                    && !v->getType()->isPointerTy()) {
                                v = spillAggregateForByPointerArg(module, v);
                            }
                        }
                        args.push_back(v);
                    }
                    if (fnType->getReturnType()) {
                        resolvedType = fnType->getReturnType();
                    }
                    llvm::CallInst* call = builder->CreateCall(sig, callee, args);
                    if (sretSlot && retClass) {
                        call->addParamAttr(0, llvm::Attribute::get(
                            llvmCtx, llvm::Attribute::StructRet,
                            retClass->getLlvmType()));
                        return sretSlot;
                    }
                    return call;
                }
            }
        }

        if (receiver && receiverType
                && methodCallName == "hash"
                && parameters.empty()
                && (receiverType->getTypeFlags() & PRIMITIVE_FLAG)
                && !dynamic_pointer_cast<CajetaClass>(receiverType)) {
            auto& llvmCtx = *module->getLlvmContext();
            llvm::Type* i64Ty = llvm::Type::getInt64Ty(llvmCtx);
            llvm::Type* i32Ty = llvm::Type::getInt32Ty(llvmCtx);
            llvm::Type* i8Ty  = llvm::Type::getInt8Ty(llvmCtx);

            const char* symbol = nullptr;
            llvm::Type* argTy = nullptr;
            llvm::Value* arg = receiver;

            switch (receiverType->getTypeFlags() & TYPE_ID_MASK) {
                case BOOLEAN_ID:
                    symbol = "__cajeta_hash_boolean";
                    arg = builder->CreateZExt(receiver, i8Ty);
                    argTy = i8Ty;
                    break;
                case INT8_ID:
                    symbol = "__cajeta_hash_int32";
                    arg = builder->CreateSExt(receiver, i32Ty);
                    argTy = i32Ty;
                    break;
                case UINT8_ID:
                    symbol = "__cajeta_hash_int32";
                    arg = builder->CreateZExt(receiver, i32Ty);
                    argTy = i32Ty;
                    break;
                case INT16_ID:
                    symbol = "__cajeta_hash_int32";
                    arg = builder->CreateSExt(receiver, i32Ty);
                    argTy = i32Ty;
                    break;
                case UINT16_ID:
                    symbol = "__cajeta_hash_int32";
                    arg = builder->CreateZExt(receiver, i32Ty);
                    argTy = i32Ty;
                    break;
                case INT32_ID:
                case UINT32_ID:
                    symbol = "__cajeta_hash_int32";
                    argTy = i32Ty;
                    break;
                case INT64_ID:
                case UINT64_ID:
                    symbol = "__cajeta_hash_int64";
                    argTy = i64Ty;
                    break;
                case FLOAT32_ID:
                    symbol = "__cajeta_hash_float32";
                    argTy = llvm::Type::getFloatTy(llvmCtx);
                    break;
                case FLOAT64_ID:
                    symbol = "__cajeta_hash_float64";
                    argTy = llvm::Type::getDoubleTy(llvmCtx);
                    break;
                default:
                    break;
            }
            if (symbol) {
                llvm::FunctionType* fnTy = llvm::FunctionType::get(
                    i64Ty, { argTy }, false);
                llvm::Module* lmod = module->getLlvmModule();
                llvm::Function* fn = lmod->getFunction(symbol);
                if (!fn) {
                    fn = llvm::Function::Create(
                        fnTy, llvm::Function::ExternalLinkage,
                        symbol, lmod);
                }
                return builder->CreateCall(fn, { arg });
            }
        }

        bool receiverIsString = false;
        if (receiver && receiver->getType()->isPointerTy()
                && !dynamic_pointer_cast<CajetaClass>(receiverType)) {
            auto childExpr = children.empty() ? nullptr
                : dynamic_pointer_cast<Expression>(children[0]);
            bool childIsArr = childExpr
                && dynamic_pointer_cast<CajetaArray>(childExpr->getResolvedType());
            if (!childIsArr) receiverIsString = true;
        }
        if (receiverIsString) {
            auto& llvmCtx = *module->getLlvmContext();
            llvm::Type* i64Ty = llvm::Type::getInt64Ty(llvmCtx);
            auto callBool = [&](llvm::Function* fn, llvm::ArrayRef<llvm::Value*> args) -> llvm::Value* {
                llvm::Value* call = builder->CreateCall(fn, args);
                return builder->CreateICmpNE(call,
                    llvm::ConstantInt::get(call->getType(), 0));
            };
            auto load1 = [&]() -> llvm::Value* {
                return loadStringArg(module, parameters[0].expression);
            };
            auto loadIdx = [&](size_t i) -> llvm::Value* {
                llvm::Value* v = parameters[i].expression->generateCode(module);
                if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(v)) {
                    v = builder->CreateLoad(a->getAllocatedType(), a);
                }
                if (v->getType()->isIntegerTy() && v->getType() != i64Ty) {
                    v = builder->CreateIntCast(v, i64Ty, /*isSigned=*/true);
                }
                return v;
            };
            if (methodCallName == "size") {
                llvm::Function* fn = module->getRuntimeFunction("__cajeta_str_len");
                if (fn) return builder->CreateCall(fn, {receiver});
            }
            if (methodCallName == "isEmpty" && parameters.empty()) {
                llvm::Function* fn = module->getRuntimeFunction("__cajeta_str_isEmpty");
                if (fn) return callBool(fn, {receiver});
            }
            if (methodCallName == "equals" && parameters.size() == 1) {
                llvm::Function* fn = module->getRuntimeFunction("__cajeta_str_equals");
                if (fn) return callBool(fn, {receiver, load1()});
            }
            if (methodCallName == "charAt" && parameters.size() == 1) {
                llvm::Function* fn = module->getRuntimeFunction("__cajeta_str_charAt");
                if (fn) return builder->CreateCall(fn, {receiver, loadIdx(0)});
            }
            if (methodCallName == "indexOf" && parameters.size() == 1) {
                llvm::Function* fn = module->getRuntimeFunction("__cajeta_str_indexOf");
                if (fn) return builder->CreateCall(fn, {receiver, load1()});
            }
            if (methodCallName == "startsWith" && parameters.size() == 1) {
                llvm::Function* fn = module->getRuntimeFunction("__cajeta_str_startsWith");
                if (fn) return callBool(fn, {receiver, load1()});
            }
            if (methodCallName == "endsWith" && parameters.size() == 1) {
                llvm::Function* fn = module->getRuntimeFunction("__cajeta_str_endsWith");
                if (fn) return callBool(fn, {receiver, load1()});
            }
            if (methodCallName == "contains" && parameters.size() == 1) {
                llvm::Function* fn = module->getRuntimeFunction("__cajeta_str_contains");
                if (fn) return callBool(fn, {receiver, load1()});
            }
            if (methodCallName == "substring" && parameters.size() == 2) {
                llvm::Function* fn = module->getRuntimeFunction("__cajeta_str_substring");
                if (fn) return builder->CreateCall(fn, {receiver, loadIdx(0), loadIdx(1)});
            }
            if (methodCallName == "toUpperCase" && parameters.empty()) {
                llvm::Function* fn = module->getRuntimeFunction("__cajeta_str_toUpperCase");
                if (fn) return builder->CreateCall(fn, {receiver});
            }
            if (methodCallName == "toLowerCase" && parameters.empty()) {
                llvm::Function* fn = module->getRuntimeFunction("__cajeta_str_toLowerCase");
                if (fn) return builder->CreateCall(fn, {receiver});
            }
            if (methodCallName == "trim" && parameters.empty()) {
                llvm::Function* fn = module->getRuntimeFunction("__cajeta_str_trim");
                if (fn) return builder->CreateCall(fn, {receiver});
            }
            if (methodCallName == "replace" && parameters.size() == 2) {
                llvm::Function* fn = module->getRuntimeFunction("__cajeta_str_replace");
                if (fn) return builder->CreateCall(fn,
                    {receiver, load1(),
                     loadStringArg(module, parameters[1].expression)});
            }
        }

        if (receiver && methodCallName == "count"
                && dynamic_pointer_cast<CajetaArray>(receiverType)) {
            auto arrayType = dynamic_pointer_cast<CajetaArray>(receiverType);
            llvm::Value* sizePtr = builder->CreateStructGEP(
                arrayType->getLlvmType(), receiver, CajetaArray::SIZE_FIELD_INDEX);
            llvm::Type* i64Ty = llvm::Type::getInt64Ty(*module->getLlvmContext());
            llvm::Value* rawCount = builder->CreateLoad(i64Ty, sizePtr);
            // Mask the shared-state sign bit (slice-spec 3.3).
            return builder->CreateAnd(rawCount,
                llvm::ConstantInt::get(i64Ty, 0x7FFFFFFFFFFFFFFFULL));
        }

        if (receiver && methodCallName == "stream" && parameters.empty()) {
            if (auto arrayType = dynamic_pointer_cast<CajetaArray>(receiverType)) {
                auto elemType = arrayType->getElementType();
                CajetaTypePtr streamTemplate =
                    CajetaType::of("ArrayStream", "cajeta.lang.stream");
                auto streamKlass = dynamic_pointer_cast<CajetaClass>(streamTemplate);
                if (streamKlass && streamKlass->isTemplate()) {
                    auto instantiated = dynamic_pointer_cast<CajetaClass>(
                        streamKlass->instantiate({elemType}));
                    if (instantiated) {
                        auto& llvmCtx = *module->getLlvmContext();
                        llvm::Type* structTy = instantiated->getLlvmType();
                        const llvm::DataLayout& dl =
                            module->getLlvmModule()->getDataLayout();
                        llvm::Constant* allocSize = llvm::ConstantInt::get(
                            llvm::Type::getInt64Ty(llvmCtx),
                            dl.getTypeAllocSize(structTy));
                        llvm::Value* instance = MemoryManager::createMallocInstruction(
                            module, allocSize, builder->GetInsertBlock());
                        builder->CreateMemSet(instance,
                            llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvmCtx), 0),
                            allocSize, llvm::MaybeAlign(8));
                        if (auto* vt = instantiated->getVirtualTableGlobal()) {
                            llvm::Constant* vtableRef = CajetaModule::ensureGlobalInModule(
                                module->getLlvmModule(), vt);
                            llvm::Value* slot = builder->CreateStructGEP(
                                structTy, instance, /*idx=*/0, "vtable_slot");
                            builder->CreateStore(vtableRef, slot);
                        }
                        llvm::Value* sizePtr = builder->CreateStructGEP(
                            arrayType->getLlvmType(), receiver,
                            CajetaArray::SIZE_FIELD_INDEX);
                        llvm::Value* sizeI64 = builder->CreateLoad(
                            llvm::Type::getInt64Ty(llvmCtx), sizePtr);
                        // Mask the shared-state sign bit (slice-spec 3.3).
                        sizeI64 = builder->CreateAnd(sizeI64,
                            llvm::ConstantInt::get(
                                llvm::Type::getInt64Ty(llvmCtx),
                                0x7FFFFFFFFFFFFFFFULL));
                        llvm::Value* sizeI32 = builder->CreateIntCast(
                            sizeI64, llvm::Type::getInt32Ty(llvmCtx), true);
                        vector<ParameterEntry> entries;
                        entries.push_back(ParameterEntry(arrayType, "data", receiver));
                        entries.push_back(ParameterEntry(
                            CajetaType::of("int32"), "limit", sizeI32));
                        string ctorName = "ArrayStream";
                        instantiated->invokeMethod(ctorName, entries,
                            /*isConstructor=*/true, instance, module);
                        resolvedType = instantiated;
                        return instance;
                    }
                }
            }
        }

        if (receiver && (methodCallName == "vload" || methodCallName == "vstore")) {
            if (auto arrayType = dynamic_pointer_cast<CajetaArray>(receiverType)) {
                auto* i8Ty = builder->getInt8Ty();
                CajetaTypePtr elemCT = arrayType->getElementType();
                llvm::Type* elemTy = elemCT->getLlvmType();
                const llvm::DataLayout& dl =
                    module->getLlvmModule()->getDataLayout();
                llvm::Align elemAlign = dl.getABITypeAlign(elemTy);
                auto evalArg = [&](size_t i) -> llvm::Value* {
                    auto& p = parameters[i].expression;
                    llvm::Value* v = p->generateCode(module);
                    auto ast = dynamic_pointer_cast<Expression>(p);
                    if (ast && !ast->getResolvedType()) ast->resolveTypes(module);
                    return loadIfLValue(module, v, ast);
                };

                if (methodCallName == "vload" && parameters.size() == 1
                        && explicitMethodTypeArgs.size() == 1) {
                    auto cN = dynamic_pointer_cast<CajetaConstantType>(
                        explicitMethodTypeArgs[0]);
                    if (cN) {
                        unsigned lanes = (unsigned) cN->getValue();
                        llvm::Value* idx = builder->CreateIntCast(
                            evalArg(0), builder->getInt64Ty(), /*isSigned=*/true);
                        llvm::Value* data = builder->CreateGEP(
                            i8Ty, receiver, builder->getInt64(8), "varr_data");
                        llvm::Value* ptr = builder->CreateGEP(
                            elemTy, data, idx, "vload_ptr");
                        auto* vTy = llvm::FixedVectorType::get(elemTy, lanes);
                        llvm::LoadInst* ld = builder->CreateLoad(vTy, ptr, "vload");
                        ld->setAlignment(elemAlign);
                        resolvedType = CajetaVector::validateAndCreate(
                            module, elemCT, lanes);
                        return ld;
                    }
                }

                if (methodCallName == "vstore" && parameters.size() == 2) {
                    llvm::Value* idx = builder->CreateIntCast(
                        evalArg(0), builder->getInt64Ty(), /*isSigned=*/true);
                    llvm::Value* vec = evalArg(1);
                    if (!vec->getType()->isVectorTy()) {
                        throw Exception(
                            "'vstore' requires a Vector value; got a non-vector "
                            "argument", "CAJETA_ERROR_VSTORE_NOT_VECTOR");
                    }
                    llvm::Value* data = builder->CreateGEP(
                        i8Ty, receiver, builder->getInt64(8), "varr_data");
                    llvm::Value* ptr = builder->CreateGEP(
                        elemTy, data, idx, "vstore_ptr");
                    llvm::StoreInst* st = builder->CreateStore(vec, ptr);
                    st->setAlignment(elemAlign);
                    return st;
                }
            }
        }

        if (receiver && methodCallName == "with") {
            auto recClass = dynamic_pointer_cast<CajetaClass>(receiverType);
            bool userDefinedWith = false;
            if (recClass && recClass->isRecordType()) {
                for (auto& m : recClass->getMethodList()) {
                    if (m && m->getName() == "with") { userDefinedWith = true; break; }
                }
            }
            llvm::Type* recBodyTy = (recClass && recClass->isRecordType())
                ? recClass->getLlvmType() : nullptr;
            if (recClass && recClass->isRecordType() && !userDefinedWith
                    && recBodyTy && recBodyTy->isStructTy()) {
                const llvm::DataLayout& dl =
                    module->getLlvmModule()->getDataLayout();
                llvm::Align align(dl.getABITypeAlign(recBodyTy));
                llvm::Value* sizeV = llvm::ConstantInt::get(
                    llvm::Type::getInt64Ty(*module->getLlvmContext()),
                    dl.getTypeAllocSize(recBodyTy));
                llvm::Value* src = receiver;
                if (src->getType() == recBodyTy) {
                    llvm::Value* tmp = builder->CreateAlloca(recBodyTy);
                    builder->CreateStore(src, tmp);
                    src = tmp;
                }
                llvm::Value* copy = builder->CreateAlloca(
                    recBodyTy, nullptr, "with.copy");
                builder->CreateMemCpy(copy, align, src, align, sizeV);
                for (auto& p : parameters) {
                    string fieldName = p.label;
                    if (!fieldName.empty() && fieldName.back() == ':') {
                        fieldName.pop_back();
                    }
                    if (fieldName.empty()) {
                        throw Exception(
                            "with(...) on record '"
                                + recClass->getQName()->toCanonical()
                                + "' requires labeled arguments (field: value)",
                            "CAJETA_ERROR_RECORD_WITH");
                    }
                    StructurePropertyPtr prop;
                    std::function<bool(const CajetaClassPtr&)> findP =
                        [&](const CajetaClassPtr& cls) -> bool {
                            if (!cls) return false;
                            auto pit = cls->getProperties().find(fieldName);
                            if (pit != cls->getProperties().end()) {
                                prop = pit->second;
                                return true;
                            }
                            for (auto& sup : cls->getSuperClasses()) {
                                if (findP(sup)) return true;
                            }
                            return false;
                        };
                    findP(recClass);
                    if (!prop || prop->isStatic()) {
                        throw Exception(
                            "record '" + recClass->getQName()->toCanonical()
                                + "' has no field '" + fieldName
                                + "' (with(...) labels must name declared "
                                  "instance fields)",
                            "CAJETA_ERROR_UNKNOWN_FIELD");
                    }
                    unsigned fieldIdx =
                        (unsigned) recClass->getFieldLlvmIndex(prop);
                    if (p.expression
                            && !p.expression->getResolvedType()) {
                        p.expression->resolveTypes(module);
                    }
                    llvm::Value* value = p.expression->generateCode(module);
                    value = loadIfLValue(module, value, p.expression);
                    auto propClass =
                        dynamic_pointer_cast<CajetaClass>(prop->getType());
                    llvm::Type* propLlvm = prop->getType()
                        ? prop->getType()->getLlvmType() : nullptr;
                    if (propClass && propClass->isValueType()
                            && !dynamic_pointer_cast<CajetaView>(prop->getType())
                            && propLlvm && propLlvm->isStructTy()
                            && value && value->getType()->isPointerTy()) {
                        value = builder->CreateLoad(propLlvm, value);
                    }
                    if (propLlvm && value && value->getType() != propLlvm) {
                        llvm::Type* srcTy = value->getType();
                        if (propLlvm->isIntegerTy() && srcTy->isIntegerTy()) {
                            value = builder->CreateIntCast(value, propLlvm, true);
                        } else if (propLlvm->isFloatingPointTy()
                                && srcTy->isFloatingPointTy()) {
                            value = builder->CreateFPCast(value, propLlvm);
                        } else if (propLlvm->isFloatingPointTy()
                                && srcTy->isIntegerTy()) {
                            value = builder->CreateSIToFP(value, propLlvm);
                        } else if (propLlvm->isIntegerTy()
                                && srcTy->isFloatingPointTy()) {
                            value = builder->CreateFPToSI(value, propLlvm);
                        }
                    }
                    llvm::Value* slot = builder->CreateStructGEP(
                        recBodyTy, copy, fieldIdx, "with." + fieldName);
                    builder->CreateStore(value, slot);
                }
                resolvedType = recClass;
                return copy;
            }
        }

        CajetaClassPtr targetClass;
        if (auto klass = dynamic_pointer_cast<CajetaClass>(receiverType)) {
            targetClass = klass;
        }
        bool enumReceiver = false;
        if (!targetClass && receiverType
                && (receiverType->getTypeFlags() & ENUM_FLAG)
                && receiverType->getQName()) {
            enumReceiver = true;
            auto& cmap = CajetaType::getCanonicalMap();
            auto it = cmap.find(receiverType->getQName()->toCanonical() + "$enum");
            if (it == cmap.end()) {
                it = cmap.find(receiverType->getQName()->getTypeName() + "$enum");
            }
            if (it != cmap.end()) {
                targetClass = dynamic_pointer_cast<CajetaClass>(it->second);
            }
        }
        if (!targetClass && !children.empty()) {
            if (!receiver && !receiverType) {
                if (auto idExpr = dynamic_pointer_cast<IdentifierExpression>(
                        children[0])) {
                    throw locatedException(
                        getSourceLine(), getSourceColumn() + 1,
                        "unknown type '" + idExpr->getTextValue()
                            + "' (no class, and no local of that name, is in scope)",
                        "CAJETA_ERROR_UNRESOLVED_TYPE");
                }
            }
            throw locatedException(
                getSourceLine(), getSourceColumn() + 1,
                "no member '" + methodCallName + "' on '"
                    + (receiverType ? receiverType->toCanonical()
                                    : std::string("<unresolved receiver>"))
                    + "'",
                "CAJETA_ERROR_MEMBER_NOT_FOUND");
        }
        if (!targetClass) {
            if (module->getStructureStack().empty()) {
                return nullptr;
            }
            targetClass = module->getStructureStack().back();

            if (SessionState* sess = module->getSessionState()) {
                bool hereAlready = false;
                for (auto& mm : targetClass->getMethodList()) {
                    if (mm && mm->getName() == methodCallName) {
                        hereAlready = true;
                        break;
                    }
                }
                if (!hereAlready) {
                    const auto& units = sess->getUnitClasses();
                    for (auto it = units.rbegin(); it != units.rend(); ++it) {
                        if (*it == targetClass->getQName()->toCanonical())
                            continue;
                        auto prior = dynamic_pointer_cast<CajetaClass>(
                            CajetaType::find(*it));
                        if (!prior) continue;
                        bool declares = false;
                        for (auto& mm : prior->getMethodList()) {
                            if (mm && mm->getName() == methodCallName) {
                                declares = true;
                                break;
                            }
                        }
                        if (declares) {
                            targetClass = prior;
                            break;
                        }
                    }
                }
            }
        }

        if (!superCtorCall) {
            MethodPtr sugarMatch;
            bool sugarAmbiguous = false;
            for (auto& mm : targetClass->getMethodList()) {
                if (mm && mm->isStatic() && mm->getName() == methodCallName
                        && mm->getParameterList().size() == parameters.size()) {
                    if (sugarMatch) { sugarAmbiguous = true; break; }
                    sugarMatch = mm;
                }
            }
            if (sugarMatch && !sugarAmbiguous) {
                std::vector<std::string> chain;
                for (auto& a : sugarMatch->getAnnotationInstances()) {
                    if (!a || !a->getName()) continue;
                    const std::string& an = a->getName()->getTypeName();
                    if (an == "Grad" || an == "Vmap" || an == "Jit"
                            || an == "Fuse")
                        chain.push_back(an);
                }
                if (!chain.empty()) {
                    CajetaTypePtr sugarType;
                    llvm::Value* v = emitTransformAnnotatedCall(
                        this, sugarMatch, chain, module, sugarType);
                    resolvedType = sugarType;
                    return v;
                }
            }
        }

        if ((methodCallName == "vload" || methodCallName == "vstore")
                && targetClass->getQName()
                && targetClass->getQName()->toCanonical().rfind(
                       "cajeta.xpu.KernelBuffer", 0) == 0) {
            throw Exception(
                "'" + methodCallName + "' is a kernel-only operation on a "
                "KernelBuffer and can only be called inside an @Kernel body",
                "CAJETA_ERROR_KERNEL_ONLY_OP");
        }

        // Only consider a scope-resident `this` when the enclosing method is
        // non-static: scopes carry stale `this` entries from earlier-generated methods,
        // and adopting one yields a load that fails JIT verify (does not dominate).
        llvm::Value* thisValue = receiver;
        if (enumReceiver && thisValue && thisValue->getType()->isPointerTy()
                && receiverType && receiverType->getLlvmType()) {
            thisValue = builder->CreateLoad(receiverType->getLlvmType(), thisValue,
                                            "enum.this");
        }
        if (!thisValue) {
            MethodPtr enclosing = module->getCurrentMethod();
            bool inStatic = enclosing
                && enclosing->getModifiers().find(STATIC) != enclosing->getModifiers().end();
            if (!inStatic) {
                FieldPtr thisField = module->getScopeStack().peek()->getField("this");
                if (thisField) {
                    llvm::AllocaInst* thisAlloca = thisField->getOrCreateAllocation();
                    if (thisAlloca) {
                        thisValue = builder->CreateLoad(thisAlloca->getAllocatedType(), thisAlloca);
                    }
                }
            }
        }

        bool anyLambda = false;
        for (auto& param : parameters) {
            if (auto lam = std::dynamic_pointer_cast<LambdaExpression>(
                    param.expression)) {
                if (!lam->getResolvedType()
                        || lam->getParamTypes().size() < lam->getParamNames().size()) {
                    anyLambda = true;
                    break;
                }
            }
        }
        if (anyLambda && targetClass) {
            MethodPtr candidate;
            int matches = 0;
            std::function<void(CajetaClassPtr)> findCandidate =
                [&](CajetaClassPtr cls) {
                    if (!cls) return;
                    for (auto& mEntry : cls->getMethods()) {
                        auto& m = mEntry.second;
                        if (m->getName() != methodCallName) continue;
                        bool isMethodTpl = m->isMethodTemplate();
                        if (isMethodTpl && !explicitMethodTypeArgs.empty()
                                && explicitMethodTypeArgs.size()
                                    == m->getMethodTypeParameters().size()) {
                            auto pList = m->getParameterList();
                            bool hasThisTpl = !pList.empty()
                                && pList.front()->getName() == "this";
                            int declaredTpl = (int) pList.size()
                                - (hasThisTpl ? 1 : 0);
                            if (declaredTpl != (int) parameters.size()) {
                                continue;
                            }
                            candidate = m;
                            ++matches;
                            continue;
                        }
                        if (isMethodTpl) continue;
                        if (m->isMethodTemplateInstantiation()) continue;
                        if (!explicitMethodTypeArgs.empty()) continue;
                        bool isStaticM = m->getModifiers().find(STATIC)
                            != m->getModifiers().end();
                        int declared = (int) m->getParameterList().size()
                            - (isStaticM ? 0 : 1);
                        if (declared != (int) parameters.size()) continue;
                        {
                            auto pList = m->getParameterList();
                            std::size_t base = isStaticM ? 0 : 1;
                            bool shapeOk = true;
                            for (std::size_t ai = 0; ai < parameters.size()
                                    && base + ai < pList.size(); ++ai) {
                                if (!std::dynamic_pointer_cast<LambdaExpression>(
                                        parameters[ai].expression)) {
                                    continue;
                                }
                                auto ft = pList[base + ai]
                                    ? pList[base + ai]->getType() : nullptr;
                                if (!std::dynamic_pointer_cast<CajetaFunctionType>(ft)) {
                                    shapeOk = false;
                                    break;
                                }
                            }
                            if (!shapeOk) continue;
                        }
                        candidate = m;
                        ++matches;
                    }
                    if (matches == 0) {
                        for (auto& sup : cls->getSuperClasses()) {
                            findCandidate(sup);
                            if (matches > 0) break;
                        }
                    }
                };
            findCandidate(targetClass);
            if (candidate && candidate->isMethodTemplate()
                    && !explicitMethodTypeArgs.empty()
                    && explicitMethodTypeArgs.size()
                        == candidate->getMethodTypeParameters().size()) {
                try {
                    candidate = candidate->instantiateMethodTemplate(
                        explicitMethodTypeArgs);
                } catch (const cajeta::ReuseHazardAbort&) {
                    throw;
                } catch (...) {
                    candidate = nullptr;
                }
            } else if (candidate
                    && (candidate->isMethodTemplate()
                        || candidate->isMethodTemplateInstantiation())) {
                candidate = nullptr;
            }
            if (candidate && matches == 1) {
                auto paramList = candidate->getParameterList();
                bool isStaticM = candidate->getModifiers().find(STATIC)
                    != candidate->getModifiers().end();
                bool hasThis = !paramList.empty()
                    && paramList.front()->getName() == "this";
                int paramOffset = (isStaticM || !hasThis) ? 0 : 1;
                size_t i = 0;
                for (auto& p : paramList) {
                    if ((int) i < paramOffset) { ++i; continue; }
                    size_t argIdx = i - paramOffset;
                    if (argIdx >= parameters.size()) break;
                    if (auto lambda = std::dynamic_pointer_cast<LambdaExpression>(
                            parameters[argIdx].expression)) {
                        bool needsInference = !lambda->getResolvedType()
                            || lambda->getParamTypes().size()
                                < lambda->getParamNames().size();
                        if (needsInference && p->getType()) {
                            lambda->setExpectedType(p->getType());
                        }
                    }
                    ++i;
                }
            }
        }

        vector<ParameterEntry> entries;
        size_t argIndex = (size_t) -1;
        for (auto& param : parameters) {
            ++argIndex;
            if (!param.expression->getResolvedType()) {
                param.expression->resolveTypes(module);
            }
            rejectStaleGenerationUse(module, param.expression,
                "argument " + std::to_string(argIndex + 1) + " of `"
                    + methodCallName + "`");
            llvm::Value* value = param.expression->generateCode(module);
            if (!value) {
                throw Exception(
                    "argument " + std::to_string(argIndex + 1) + " to `"
                    + methodCallName + "` did not lower to a value — it "
                    "names an unknown or non-addressable property ("
                    + module->getSourcePath() + ":"
                    + std::to_string(getSourceLine()) + ")",
                    "CAJETA_ERROR_ARG_INVALID");
            }
            // Read the title flag now - the next argument's call would clobber it.
            if (argIndex < argTitles.size()) {
                argTitles[argIndex] = ownership::classifyArgument(
                    param.expression, param.callerTransferred, module, "a `#` argument");
                CajetaTypePtr argTy = param.expression->getResolvedType();
                bool wordCarrier = param.callerTransferred
                    || droppableTempClass(argTy) != nullptr
                    || dynamic_pointer_cast<CajetaArray>(argTy) != nullptr
                    || dynamic_pointer_cast<CajetaFunctionType>(argTy) != nullptr;
                if (wordCarrier && argTitles[argIndex].flag
                        && argIndex < argTitleFlags.size()) {
                    argTitleFlags[argIndex] = argTitles[argIndex].flag;
                }
            }
            if (auto* a = llvm::dyn_cast<llvm::AllocaInst>(value)) {
                value = builder->CreateLoad(a->getAllocatedType(), a);
            } else if (llvm::isa_and_nonnull<llvm::GlobalVariable>(value)
                    && dynamic_pointer_cast<IdentifierExpression>(param.expression)) {
                auto* g = llvm::cast<llvm::GlobalVariable>(value);
                value = builder->CreateLoad(g->getValueType(), g);
            }
            if (dynamic_pointer_cast<DotExpression>(param.expression)
                    || dynamic_pointer_cast<ArrayIndexExpression>(param.expression)) {
                value = loadIfLValue(module, value, param.expression);
            }
            CajetaTypePtr et = param.expression->getResolvedType();
            if (!et) et = CajetaType::of(value);
            if (!children.empty()) {
                auto outerRecvId = dynamic_pointer_cast<IdentifierExpression>(children[0]);
                auto argMce = dynamic_pointer_cast<MethodCallExpression>(param.expression);
                if (outerRecvId && argMce
                        && argMce->getPreProjectionReturnType()) {
                    auto& argChildren = argMce->getChildren();
                    if (!argChildren.empty()) {
                        auto argRecvId = dynamic_pointer_cast<IdentifierExpression>(argChildren[0]);
                        if (argRecvId
                                && argRecvId->getTextValue()
                                    == outerRecvId->getTextValue()) {
                            et = argMce->getPreProjectionReturnType();
                        }
                    }
                }
            }
            entries.push_back(ParameterEntry(et, param.label, value));
        }

        if (targetClass && targetClass->getQName()) {
            CajetaModule::FactoryDescriptorPtr fdesc;
            for (auto& f : CajetaModule::getFactoryClasses()) {
                if (f && f->klass && f->klass->getQName()
                        && f->klass->getQName()->toCanonical()
                            == targetClass->getQName()->toCanonical()) {
                    fdesc = f;
                    break;
                }
            }
            if (fdesc) {
                for (auto& prov : fdesc->providers) {
                    if (!prov.hasAssisted || !prov.method) continue;
                    if (prov.method->getName() != methodCallName) continue;
                    int assistedCount = 0;
                    for (auto& fp : prov.params) if (!fp.injected) assistedCount++;
                    if ((int) entries.size() != assistedCount) continue;
                    std::vector<ParameterEntry> spliced;
                    size_t userIdx = 0;
                    for (auto& fp : prov.params) {
                        if (fp.injected) {
                            llvm::Value* dep = FactoryProviderMethod::emitInjectArg(
                                builder, module, fp);
                            spliced.push_back(ParameterEntry(
                                fp.param->getType(), "", dep));
                        } else if (userIdx < entries.size()) {
                            spliced.push_back(entries[userIdx++]);
                        }
                    }
                    entries = std::move(spliced);
                    break;
                }
            }
        }

        MethodPtr varargsTarget;
        for (auto& mEntry : targetClass->getMethods()) {
            auto& m = mEntry.second;
            if (m->isVarargs() && m->getName() == methodCallName) {
                varargsTarget = m;
                break;
            }
        }
        if (varargsTarget) {
            auto paramList = varargsTarget->getParameterList();
            bool isStatic = varargsTarget->getModifiers().find(STATIC)
                != varargsTarget->getModifiers().end();
            int totalParams = (int) paramList.size();
            int fixedArgs = totalParams - 1 - (isStatic ? 0 : 1);
            if (fixedArgs < 0) fixedArgs = 0;
            if ((int) entries.size() >= fixedArgs) {
                auto varParam = paramList.empty() ? nullptr : paramList.back();
                auto arrType = varParam
                    ? dynamic_pointer_cast<CajetaArray>(varParam->getType())
                    : nullptr;
                if (arrType) {
                    auto& llvmCtx = *module->getLlvmContext();
                    llvm::Type* i64Ty = llvm::Type::getInt64Ty(llvmCtx);
                    llvm::Type* i32Ty = llvm::Type::getInt32Ty(llvmCtx);
                    const llvm::DataLayout& dl =
                        module->getLlvmModule()->getDataLayout();
                    llvm::Type* headerTy = arrType->getLlvmType();
                    llvm::Type* elemTy = arrType->getElementLlvmType(&llvmCtx);
                    int trailing = (int) entries.size() - fixedArgs;
                    if (auto allocFn = module->getRuntimeFunction(
                            "__cajeta_new_array_header")) {
                        llvm::Value* hdrPtr = builder->CreateCall(allocFn, {
                            llvm::ConstantInt::get(i64Ty, dl.getTypeAllocSize(headerTy)),
                            llvm::ConstantInt::get(i64Ty, dl.getTypeAllocSize(elemTy)),
                            llvm::ConstantInt::get(i64Ty, trailing),
                        });
                        for (int i = 0; i < trailing; ++i) {
                            llvm::Value* v = entries[fixedArgs + i].value;
                            if (v && v->getType() != elemTy && elemTy->isIntegerTy()
                                    && v->getType()->isIntegerTy()) {
                                v = builder->CreateIntCast(v, elemTy, /*isSigned=*/true);
                            }
                            vector<llvm::Value*> gepIndices = {
                                llvm::ConstantInt::get(i64Ty, 0),
                                llvm::ConstantInt::get(i32Ty, CajetaArray::DATA_FIELD_INDEX),
                                llvm::ConstantInt::get(i64Ty, i),
                            };
                            llvm::Value* slot = builder->CreateGEP(headerTy, hdrPtr, gepIndices);
                            builder->CreateStore(v, slot);
                        }
                        entries.erase(entries.begin() + fixedArgs, entries.end());
                        entries.push_back(ParameterEntry(arrType, "", hdrPtr));
                    }
                }
            }
        }

        if (!targetClass->getMethods().empty()) {
            for (auto& mEntry : targetClass->getMethods()) {
                auto& m = mEntry.second;
                if (m->getName() != methodCallName) continue;
                if (m->isVarargs()) continue;
                bool isStatic = m->getModifiers().find(STATIC)
                    != m->getModifiers().end();
                auto pl = m->getParameterList();
                int thisShift = isStatic ? 0 : 1;
                int userParams = (int) pl.size() - thisShift;
                if ((int) entries.size() >= userParams) continue;
                int required = 0;
                for (int i = thisShift; i < (int) pl.size(); ++i) {
                    if (pl[i]->getDefaultValue()) break;
                    required++;
                }
                if ((int) entries.size() < required) continue;
                for (int i = thisShift + (int) entries.size();
                     i < (int) pl.size(); ++i) {
                    auto defExpr = pl[i]->getDefaultValue();
                    if (!defExpr) break;
                    if (!defExpr->getResolvedType()) {
                        defExpr->resolveTypes(module);
                    }
                    llvm::Value* dv = defExpr->generateCode(module);
                    if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(dv)) {
                        dv = builder->CreateLoad(a->getAllocatedType(), a);
                    }
                    CajetaTypePtr dt = defExpr->getResolvedType();
                    if (!dt) dt = CajetaType::of(dv);
                    entries.push_back(ParameterEntry(dt, "", dv));
                }
                break;
            }
        }

        bool floatingParamsLint = true;
        for (auto& p : entries) if (p.label.empty()) { floatingParamsLint = false; break; }
        vector<ParameterEntry> entriesCopy = entries;
        MethodPtr targetMethod = targetClass->resolveMethod(
            methodCallName, entriesCopy, /*isConstructor=*/false,
            floatingParamsLint, explicitMethodTypeArgs, module);
        if (targetMethod) {
            auto& throwsList = targetMethod->getThrowsList();
            if (!throwsList.empty()) {
                auto currentMethod = module->getCurrentMethod();
                if (currentMethod
                        && !currentMethod->isLintSuppressed("uncaught-throws")) {
                    auto& currentThrows = currentMethod->getThrowsList();
                    auto isCaughtBy = [&](const QualifiedNamePtr& thrownQ,
                                          const CajetaTypePtr& catchType) -> bool {
                        if (!thrownQ || !catchType) return false;
                        const string& catchCanonical = catchType->toCanonical();
                        if (thrownQ->toCanonical() == catchCanonical
                                || thrownQ->getTypeName() == catchCanonical) {
                            return true;
                        }
                        auto thrownType = CajetaType::of(thrownQ);
                        if (!thrownType) {
                            thrownType = CajetaType::of(thrownQ->getTypeName(), "");
                        }
                        auto thrownClass = dynamic_pointer_cast<CajetaClass>(thrownType);
                        if (!thrownClass) return false;
                        std::function<bool(const CajetaClassPtr&)> walk =
                            [&](const CajetaClassPtr& cls) -> bool {
                                if (cls->toCanonical() == catchCanonical) return true;
                                for (auto& parent : cls->getSuperClasses()) {
                                    if (walk(parent)) return true;
                                }
                                return false;
                            };
                        return walk(thrownClass);
                    };
                    auto& tryCatchStack = module->getTryCatchStack();
                    for (auto& thrownType : throwsList) {
                        bool declared = false;
                        for (auto& declaredType : currentThrows) {
                            if (thrownType->toCanonical() == declaredType->toCanonical()) {
                                declared = true;
                                break;
                            }
                        }
                        if (declared) continue;
                        bool covered = false;
                        for (auto& frame : tryCatchStack) {
                            for (auto& catchType : frame) {
                                if (isCaughtBy(thrownType, catchType)) {
                                    covered = true;
                                    break;
                                }
                            }
                            if (covered) break;
                        }
                        if (!covered) {
                            std::ostringstream w;
                            w << "warning: [uncaught-throws] call to "
                                << methodCallName
                                << " can throw " << thrownType->toCanonical()
                                << " but enclosing " << currentMethod->getName()
                                << " neither catches nor declares it\n";
                            logLine("warn", w.str());
                        }
                    }
                }
            }

            auto currentMethod = module->getCurrentMethod();
            bool enclosingIsWildcardProxy = currentMethod
                && currentMethod->getParent()
                && currentMethod->getParent()->isWildcardInstantiation();
            bool enclosingIsMethodTemplate = currentMethod
                && (currentMethod->isMethodTemplate()
                    || currentMethod->isMethodTemplateInstantiation());
            if (currentMethod && module->hasLoopContext()
                    && !enclosingIsWildcardProxy
                    && !enclosingIsMethodTemplate) {
                bool receiverIsWildcard = targetClass
                    && targetClass->isWildcardInstantiation();
                bool isElementProducing =
                    methodCallName == "next" || methodCallName == "get";
                if (receiverIsWildcard && isElementProducing
                        && !currentMethod->isLintSuppressed(
                            "wildcard-materialize-in-loop")) {
                    std::ostringstream w;
                    w << "warning: [wildcard-materialize-in-loop] "
                        << "call to '" << methodCallName
                        << "' on wildcard-typed receiver "
                        << targetClass->toCanonical()
                        << " inside a loop in "
                        << currentMethod->getName()
                        << " — dispatch goes through the template-relative "
                        << "vtable hash on every iteration; downcast to the "
                        << "concrete type at the loop boundary, or suppress "
                        << "with @SuppressLint(\"wildcard-materialize-in-loop\").\n";
                    logLine("warn", w.str());
                }
                if (targetMethod
                        && !currentMethod->isLintSuppressed(
                            "wildcard-crosses-hot-boundary")) {
                    auto retClass = dynamic_pointer_cast<CajetaClass>(
                        targetMethod->getReturnType());
                    if (retClass && retClass->isWildcardInstantiation()) {
                        std::ostringstream w;
                        w << "warning: [wildcard-crosses-hot-boundary] "
                            << "call to '" << methodCallName
                            << "' inside a loop in "
                            << currentMethod->getName()
                            << " returns wildcard-typed "
                            << retClass->toCanonical()
                            << " — the receive site can't be specialized "
                            << "at the call boundary; restructure to "
                            << "concrete or suppress with "
                            << "@SuppressLint(\"wildcard-crosses-hot-boundary\").\n";
                        logLine("warn", w.str());
                    }
                }
            }
        }

        std::shared_ptr<SuperExpression> superLhs;
        if (!children.empty()) {
            superLhs = std::dynamic_pointer_cast<SuperExpression>(children[0]);
        }
        bool isSuperCall = (superLhs != nullptr);

        // `super<C>.method()` on a method INHERITED from A: `this` was adjusted to C's
        // sub-object, but in a diamond A's canonical position is unreachable through
        // C's standalone type, so shift by canonical - via-bracketed when they differ.
        if (isSuperCall && superLhs && !superLhs->getChosenAncestorName().empty()
                && thisValue && !module->getStructureStack().empty()) {
            auto bracketed = std::dynamic_pointer_cast<CajetaClass>(
                superLhs->getResolvedType());
            auto enclosing = std::dynamic_pointer_cast<CajetaClass>(
                module->getStructureStack().back());
            if (bracketed && enclosing && targetClass) {
                bool callFloating = true;
                for (auto& e : entries) {
                    if (e.label.empty()) { callFloating = false; break; }
                }
                MethodPtr resolved = targetClass->resolveMethod(
                    methodCallName, entries,
                    /*isConstructor=*/false, callFloating);
                CajetaClassPtr declaringClass;
                if (resolved) {
                    declaringClass = resolved->getParent();
                }
                if (declaringClass && declaringClass.get() != bracketed.get()) {
                    uint64_t canonical = enclosing->getSubObjectByteOffset(
                        declaringClass.get());
                    uint64_t viaBrkt = enclosing->getSubObjectByteOffset(
                            bracketed.get())
                        + bracketed->getSubObjectByteOffset(
                            declaringClass.get());
                    if (canonical != viaBrkt) {
                        auto& ctx = *module->getLlvmContext();
                        llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
                        llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
                        int64_t delta =
                            (int64_t) canonical - (int64_t) viaBrkt;
                        thisValue = builder->CreateInBoundsGEP(i8Ty,
                            thisValue,
                            llvm::ConstantInt::get(i64Ty, delta),
                            "diamond_super_canonical");
                    }
                }
            }
        }

        // ----- Path instance-method stat intrinsics (Phase C) -----
        if (thisValue && targetClass && targetClass->getQName()
                && targetClass->getQName()->toCanonical() == "cajeta.io.file.Path") {
            auto* pathStructTy = llvm::cast<llvm::StructType>(
                targetClass->getLlvmType());
            llvm::Type* ptrTy = llvm::PointerType::get(llvmCtx, 0);
            llvm::Type* i32Ty = llvm::Type::getInt32Ty(llvmCtx);
            llvm::Type* i64Ty = llvm::Type::getInt64Ty(llvmCtx);
            llvm::Type* i8Ty  = llvm::Type::getInt8Ty(llvmCtx);
            // Path layout: { vtable@0, bytes@1 }, where bytes holds the CajetaArray
            // header: its first i64 is the length and the data starts at offset 8.
            auto loadBytesAndLen = [&]() -> std::pair<llvm::Value*, llvm::Value*> {
                llvm::Value* bytesSlot = builder->CreateStructGEP(
                    pathStructTy, thisValue, 1, "path.bytes_slot");
                llvm::Value* arrPtr = builder->CreateLoad(
                    ptrTy, bytesSlot, "path.arr");
                llvm::Value* len = builder->CreateLoad(
                    i64Ty, arrPtr, "path.len");
                llvm::Value* data = builder->CreateInBoundsGEP(
                    i8Ty, arrPtr,
                    llvm::ConstantInt::get(i64Ty, 8),
                    "path.data");
                return {data, len};
            };

            const char* statSymbol = nullptr;
            if (methodCallName == "exists" && parameters.empty()) {
                statSymbol = "__cajeta_path_exists";
            } else if (methodCallName == "isFile" && parameters.empty()) {
                statSymbol = "__cajeta_path_is_file";
            } else if (methodCallName == "isDir" && parameters.empty()) {
                statSymbol = "__cajeta_path_is_dir";
            } else if (methodCallName == "isSymlink" && parameters.empty()) {
                statSymbol = "__cajeta_path_is_symlink";
            }
            if (statSymbol) {
                llvm::Function* fn = module->getRuntimeFunction(statSymbol);
                if (fn) {
                    auto bd = loadBytesAndLen();
                    llvm::Value* result = builder->CreateCall(fn,
                        {bd.first, bd.second}, "path.stat");
                    llvm::Value* asI1 = builder->CreateICmpNE(result,
                        llvm::ConstantInt::get(i32Ty, 0),
                        "path.stat.bool");
                    resolvedType = CajetaType::of("boolean");
                    return asI1;
                }
            }

            if (methodCallName == "mkdirs" && parameters.empty()) {
                llvm::Function* fn = module->getRuntimeFunction(
                    "__cajeta_path_mkdirs");
                if (fn) {
                    auto bd = loadBytesAndLen();
                    builder->CreateCall(fn, {bd.first, bd.second});
                    resolvedType = targetClass;
                    return thisValue;
                }
            }
            if (methodCallName == "delete" && parameters.empty()) {
                llvm::Function* fn = module->getRuntimeFunction(
                    "__cajeta_path_delete");
                if (fn) {
                    auto bd = loadBytesAndLen();
                    builder->CreateCall(fn, {bd.first, bd.second});
                    resolvedType = CajetaType::of("void");
                    return nullptr;
                }
            }

            if (methodCallName == "setExecutable" && parameters.empty()) {
                llvm::Function* fn = module->getRuntimeFunction(
                    "__cajeta_path_set_executable");
                if (fn) {
                    auto bd = loadBytesAndLen();
                    llvm::Value* result = builder->CreateCall(fn,
                        {bd.first, bd.second}, "path.setx");
                    llvm::Value* ok = builder->CreateICmpEQ(result,
                        llvm::ConstantInt::get(i32Ty, 0), "path.setx.ok");
                    resolvedType = CajetaType::of("boolean");
                    return ok;
                }
            }

            if (methodCallName == "symlinkTo" && parameters.size() == 1) {
                llvm::Function* fn = module->getRuntimeFunction(
                    "__cajeta_path_symlink");
                if (fn) {
                    llvm::Value* targetPtr = loadIfLValue(module,
                        parameters[0].expression->generateCode(module),
                        parameters[0].expression);
                    llvm::Value* tBytesSlot = builder->CreateStructGEP(
                        pathStructTy, targetPtr, 1, "tgt.bytes_slot");
                    llvm::Value* tArrPtr = builder->CreateLoad(
                        ptrTy, tBytesSlot, "tgt.arr");
                    llvm::Value* tLen = builder->CreateLoad(
                        i64Ty, tArrPtr, "tgt.len");
                    llvm::Value* tData = builder->CreateInBoundsGEP(
                        i8Ty, tArrPtr,
                        llvm::ConstantInt::get(i64Ty, 8), "tgt.data");
                    auto link = loadBytesAndLen();
                    llvm::Value* result = builder->CreateCall(fn,
                        {tData, tLen, link.first, link.second},
                        "path.symlink");
                    llvm::Value* ok = builder->CreateICmpEQ(result,
                        llvm::ConstantInt::get(i32Ty, 0), "path.symlink.ok");
                    resolvedType = CajetaType::of("boolean");
                    return ok;
                }
            }

            if (methodCallName == "canonical" && parameters.empty()) {
                llvm::Function* fn = module->getRuntimeFunction(
                    "__cajeta_path_canonical");
                if (fn) {
                    auto bd = loadBytesAndLen();
                    llvm::Value* canonBytes = builder->CreateCall(fn,
                        {bd.first, bd.second}, "path.canon_arr");
                    const llvm::DataLayout& dl =
                        module->getLlvmModule()->getDataLayout();
                    llvm::Constant* size = llvm::ConstantInt::get(
                        i64Ty, dl.getTypeAllocSize(pathStructTy));
                    llvm::Value* inst = MemoryManager::createMallocInstruction(
                        module, size, builder->GetInsertBlock());
                    builder->CreateMemSet(inst,
                        llvm::ConstantInt::get(i8Ty, 0),
                        size, llvm::MaybeAlign(8));
                    llvm::Constant* vtableRef =
                        llvm::ConstantPointerNull::get(
                            llvm::cast<llvm::PointerType>(ptrTy));
                    if (auto* vt = targetClass->getVirtualTableGlobal()) {
                        vtableRef = CajetaModule::ensureGlobalInModule(
                            module->getLlvmModule(), vt);
                    }
                    builder->CreateStore(vtableRef,
                        builder->CreateStructGEP(pathStructTy, inst, 0,
                            "path.canon.vtable_slot"));
                    builder->CreateStore(canonBytes,
                        builder->CreateStructGEP(pathStructTy, inst, 1,
                            "path.canon.bytes_slot"));
                    resolvedType = targetClass;
                    return inst;
                }
            }

            if (methodCallName == "listJoined" && parameters.empty()) {
                llvm::Function* fn = module->getRuntimeFunction(
                    "__cajeta_path_list");
                if (fn) {
                    auto bd = loadBytesAndLen();
                    llvm::Value* arr = builder->CreateCall(fn,
                        {bd.first, bd.second}, "path.list_arr");
                    auto arrTy = make_shared<CajetaArray>(
                        module, CajetaType::of("int8"));
                    module->getStructures()[arrTy->toCanonical()] =
                        static_pointer_cast<CajetaClass>(arrTy);
                    resolvedType = arrTy;
                    return arr;
                }
            }
        }

        // ----- FileReader / FileWriter / File instance-method intrinsic -----
        if (thisValue && targetClass && targetClass->getQName()) {
            const std::string canonical = targetClass->getQName()->toCanonical();
            bool isReader = canonical == "cajeta.io.file.FileReader";
            bool isWriter = canonical == "cajeta.io.file.FileWriter";
            bool isFile   = canonical == "cajeta.io.file.File";
            if (isReader || isWriter || isFile) {
                auto* structTy =
                    llvm::cast<llvm::StructType>(targetClass->getLlvmType());
                llvm::Type* ptrTy = llvm::PointerType::get(llvmCtx, 0);
                llvm::Type* i32Ty = llvm::Type::getInt32Ty(llvmCtx);
                llvm::Type* i64Ty = llvm::Type::getInt64Ty(llvmCtx);
                llvm::Type* i8Ty  = llvm::Type::getInt8Ty(llvmCtx);
                // Field offsets: vtable(0), fd(1), pos(2), pinned by FileReader.cajeta /
                // FileWriter.cajeta field order.
                auto loadFd = [&]() -> llvm::Value* {
                    llvm::Value* fdSlot = builder->CreateStructGEP(
                        structTy, thisValue, 1, "fr.fd_slot");
                    return builder->CreateLoad(i32Ty, fdSlot, "fr.fd");
                };

                if (isReader && methodCallName == "read"
                        && parameters.size() == 2) {
                    llvm::Function* fn = module->getRuntimeFunction(
                        "__cajeta_file_read");
                    if (fn) {
                        llvm::Value* fd = loadFd();
                        llvm::Value* arr = loadArrayArg(
                            module, parameters[0].expression);
                        llvm::Value* dataPtr = builder->CreateInBoundsGEP(
                            i8Ty, arr,
                            llvm::ConstantInt::get(i64Ty, 8),
                            "fr.buf");
                        llvm::Value* maxV = parameters[1].expression->generateCode(module);
                        if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(maxV)) {
                            maxV = builder->CreateLoad(a->getAllocatedType(), a);
                        }
                        if (maxV && maxV->getType() != i64Ty
                                && maxV->getType()->isIntegerTy()) {
                            maxV = builder->CreateIntCast(maxV, i64Ty, true);
                        }
                        llvm::Value* nRead = builder->CreateCall(fn,
                            {fd, dataPtr, maxV}, "fr.n");
                        llvm::Value* posSlot = builder->CreateStructGEP(
                            structTy, thisValue, 2, "fr.pos_slot");
                        llvm::Value* curPos = builder->CreateLoad(
                            i64Ty, posSlot, "fr.pos_cur");
                        llvm::Value* newPos = builder->CreateAdd(
                            curPos, nRead, "fr.pos_new");
                        builder->CreateStore(newPos, posSlot);
                        llvm::Value* nRead32 = builder->CreateIntCast(
                            nRead, i32Ty, /*isSigned=*/true);
                        resolvedType = CajetaType::of("int32");
                        return nRead32;
                    }
                }
                if (isReader && methodCallName == "readString"
                        && parameters.size() == 1) {
                    llvm::Function* readFn = module->getRuntimeFunction(
                        "__cajeta_file_read");
                    if (readFn) {
                        llvm::Value* fd = loadFd();
                        llvm::Value* maxV = parameters[0].expression->generateCode(module);
                        if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(maxV)) {
                            maxV = builder->CreateLoad(a->getAllocatedType(), a);
                        }
                        if (maxV && maxV->getType() != i64Ty
                                && maxV->getType()->isIntegerTy()) {
                            maxV = builder->CreateIntCast(maxV, i64Ty, true);
                        }
                        llvm::Value* maxI64 = maxV;
                        llvm::Function* allocFn = module->getRuntimeFunction(
                            "__cajeta_new_array_header");
                        llvm::Value* arr = builder->CreateCall(allocFn,
                            { llvm::ConstantInt::get(i64Ty, 8),
                              llvm::ConstantInt::get(i64Ty, 1),
                              maxI64 },
                            "fr.str.arr");
                        llvm::Value* dataPtr = builder->CreateInBoundsGEP(
                            i8Ty, arr,
                            llvm::ConstantInt::get(i64Ty, 8),
                            "fr.str.data");
                        llvm::Value* nRead = builder->CreateCall(readFn,
                            {fd, dataPtr, maxV}, "fr.str.n");
                        llvm::Value* nReadI64 = builder->CreateIntCast(
                            nRead, i64Ty, true);
                        builder->CreateStore(nReadI64, arr);
                        llvm::Value* posSlot = builder->CreateStructGEP(
                            structTy, thisValue, 2, "fr.str.pos_slot");
                        llvm::Value* curPos = builder->CreateLoad(
                            i64Ty, posSlot, "fr.str.pos_cur");
                        llvm::Value* newPos = builder->CreateAdd(
                            curPos, nReadI64, "fr.str.pos_new");
                        builder->CreateStore(newPos, posSlot);
                        CajetaTypePtr stringTy = CajetaType::of("String");
                        auto stringKlass = std::dynamic_pointer_cast<CajetaClass>(stringTy);
                        if (stringKlass && stringKlass->getLlvmType()
                                && llvm::isa<llvm::StructType>(stringKlass->getLlvmType())) {
                            auto* sStructTy = llvm::cast<llvm::StructType>(
                                stringKlass->getLlvmType());
                            const llvm::DataLayout& dl =
                                module->getLlvmModule()->getDataLayout();
                            llvm::Constant* sSize = llvm::ConstantInt::get(
                                i64Ty, dl.getTypeAllocSize(sStructTy));
                            llvm::Value* sInst = MemoryManager::createMallocInstruction(
                                module, sSize, builder->GetInsertBlock());
                            builder->CreateMemSet(sInst,
                                llvm::ConstantInt::get(i8Ty, 0),
                                sSize, llvm::MaybeAlign(8));
                            llvm::Constant* vtableRef =
                                llvm::ConstantPointerNull::get(
                                    llvm::cast<llvm::PointerType>(ptrTy));
                            if (auto* vt = stringKlass->getVirtualTableGlobal()) {
                                vtableRef = CajetaModule::ensureGlobalInModule(
                                    module->getLlvmModule(), vt);
                            }
                            builder->CreateStore(vtableRef,
                                builder->CreateStructGEP(sStructTy, sInst, 0,
                                    "fr.str.s_vtable"));
                            llvm::Value* lenI32 = builder->CreateIntCast(
                                nRead, i32Ty, true);
                            llvm::FunctionType* adoptTy = llvm::FunctionType::get(
                                llvm::Type::getVoidTy(llvmCtx),
                                {ptrTy, ptrTy, i32Ty}, false);
                            llvm::FunctionCallee adoptFn =
                                module->getLlvmModule()->getOrInsertFunction(
                                    "__cajeta_string_adopt", adoptTy);
                            builder->CreateCall(adoptFn, {sInst, arr, lenI32});
                            resolvedType = stringKlass;
                            return sInst;
                        }
                        return arr;
                    }
                }
                if (isReader && methodCallName == "position"
                        && parameters.empty()) {
                    llvm::Value* posSlot = builder->CreateStructGEP(
                        structTy, thisValue, 2, "fr.pos_slot");
                    llvm::Value* pos = builder->CreateLoad(
                        i64Ty, posSlot, "fr.pos");
                    resolvedType = CajetaType::of("int64");
                    return pos;
                }
                if ((isReader || isWriter || isFile) && methodCallName == "close"
                        && parameters.empty()) {
                    if (isWriter) {
                        if (llvm::Function* flushFn = module->getRuntimeFunction(
                                "__cajeta_file_flush")) {
                            llvm::Value* fd = loadFd();
                            builder->CreateCall(flushFn, {fd});
                        }
                    }
                    llvm::Function* fn = module->getRuntimeFunction(
                        "__cajeta_file_close");
                    if (fn) {
                        llvm::Value* fd = loadFd();
                        builder->CreateCall(fn, {fd});
                        llvm::Value* fdSlot = builder->CreateStructGEP(
                            structTy, thisValue, 1, "fr.fd_slot");
                        builder->CreateStore(
                            llvm::ConstantInt::get(i32Ty, -1), fdSlot);
                        resolvedType = CajetaType::of("void");
                        return nullptr;
                    }
                }
                if (isWriter && methodCallName == "write"
                        && parameters.size() == 2) {
                    llvm::Function* fn = module->getRuntimeFunction(
                        "__cajeta_file_write");
                    if (fn) {
                        llvm::Value* fd = loadFd();
                        llvm::Value* arr = loadArrayArg(
                            module, parameters[0].expression);
                        llvm::Value* dataPtr = builder->CreateInBoundsGEP(
                            i8Ty, arr,
                            llvm::ConstantInt::get(i64Ty, 8),
                            "fw.data");
                        llvm::Value* lenV = parameters[1].expression->generateCode(module);
                        if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(lenV)) {
                            lenV = builder->CreateLoad(a->getAllocatedType(), a);
                        }
                        if (lenV && lenV->getType() != i64Ty
                                && lenV->getType()->isIntegerTy()) {
                            lenV = builder->CreateIntCast(lenV, i64Ty, true);
                        }
                        builder->CreateCall(fn, {fd, dataPtr, lenV});
                        llvm::Value* posSlot = builder->CreateStructGEP(
                            structTy, thisValue, 2, "fw.pos_slot");
                        llvm::Value* curPos = builder->CreateLoad(
                            i64Ty, posSlot, "fw.pos_cur");
                        llvm::Value* newPos = builder->CreateAdd(
                            curPos, lenV, "fw.pos_new");
                        builder->CreateStore(newPos, posSlot);
                        resolvedType = CajetaType::of("void");
                        return nullptr;
                    }
                }
                if (isWriter && methodCallName == "writeString"
                        && parameters.size() == 1) {
                    llvm::Value* fd = loadFd();
                    llvm::Value* sPtr = parameters[0].expression->generateCode(module);
                    if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(sPtr)) {
                        sPtr = builder->CreateLoad(a->getAllocatedType(), a);
                    }
                    llvm::FunctionType* fwsTy = llvm::FunctionType::get(
                        i32Ty, {i32Ty, ptrTy}, false);
                    llvm::FunctionCallee fwsFn =
                        module->getLlvmModule()->getOrInsertFunction(
                            "__cajeta_file_write_string", fwsTy);
                    llvm::Value* len = builder->CreateCall(
                        fwsFn, {fd, sPtr}, "fw.str.len");
                    llvm::Value* posSlot = builder->CreateStructGEP(
                        structTy, thisValue, 2, "fw.str.pos_slot");
                    llvm::Value* curPos = builder->CreateLoad(
                        i64Ty, posSlot, "fw.str.pos_cur");
                    llvm::Value* len64 = builder->CreateIntCast(
                        len, i64Ty, true);
                    llvm::Value* newPos = builder->CreateAdd(
                        curPos, len64, "fw.str.pos_new");
                    builder->CreateStore(newPos, posSlot);
                    resolvedType = CajetaType::of("void");
                    return nullptr;
                }
                if (isWriter && methodCallName == "flush"
                        && parameters.empty()) {
                    llvm::Function* fn = module->getRuntimeFunction(
                        "__cajeta_file_flush");
                    if (fn) {
                        llvm::Value* fd = loadFd();
                        builder->CreateCall(fn, {fd});
                        resolvedType = CajetaType::of("void");
                        return nullptr;
                    }
                }

                // ----- File random-access instance methods (Phase E) -----
                if (isFile) {
                    if ((methodCallName == "read" || methodCallName == "write")
                            && parameters.size() == 3) {
                        const char* rtSym = methodCallName == "read"
                            ? "__cajeta_file_read"
                            : "__cajeta_file_write";
                        llvm::Function* fn = module->getRuntimeFunction(rtSym);
                        if (fn) {
                            llvm::Value* fd = loadFd();
                            llvm::Value* arr = loadArrayArg(
                                module, parameters[0].expression);
                            llvm::Value* offV = parameters[1].expression->generateCode(module);
                            if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(offV)) {
                                offV = builder->CreateLoad(a->getAllocatedType(), a);
                            }
                            if (offV && offV->getType() != i64Ty
                                    && offV->getType()->isIntegerTy()) {
                                offV = builder->CreateIntCast(offV, i64Ty, true);
                            }
                            llvm::Value* lenV = parameters[2].expression->generateCode(module);
                            if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(lenV)) {
                                lenV = builder->CreateLoad(a->getAllocatedType(), a);
                            }
                            if (lenV && lenV->getType() != i64Ty
                                    && lenV->getType()->isIntegerTy()) {
                                lenV = builder->CreateIntCast(lenV, i64Ty, true);
                            }
                            llvm::Value* dataStart = builder->CreateInBoundsGEP(
                                i8Ty, arr,
                                llvm::ConstantInt::get(i64Ty, 8),
                                "file.data_start");
                            llvm::Value* dataPtr = builder->CreateInBoundsGEP(
                                i8Ty, dataStart, offV, "file.data_off");
                            llvm::Value* result = builder->CreateCall(fn,
                                {fd, dataPtr, lenV}, "file.rw");
                            llvm::Value* delta = methodCallName == "read"
                                ? result : lenV;
                            llvm::Value* posSlot = builder->CreateStructGEP(
                                structTy, thisValue, 2, "file.pos_slot");
                            llvm::Value* curPos = builder->CreateLoad(
                                i64Ty, posSlot, "file.pos_cur");
                            llvm::Value* newPos = builder->CreateAdd(
                                curPos, delta, "file.pos_new");
                            builder->CreateStore(newPos, posSlot);
                            resolvedType = CajetaType::of("int64");
                            return result;
                        }
                    }
                    if (methodCallName == "position" && parameters.empty()) {
                        llvm::Value* posSlot = builder->CreateStructGEP(
                            structTy, thisValue, 2, "file.pos_slot");
                        resolvedType = CajetaType::of("int64");
                        return builder->CreateLoad(i64Ty, posSlot, "file.pos");
                    }
                    if (methodCallName == "seek" && parameters.size() == 1) {
                        llvm::Function* fn = module->getRuntimeFunction(
                            "__cajeta_file_seek");
                        if (fn) {
                            llvm::Value* fd = loadFd();
                            llvm::Value* absV = parameters[0].expression->generateCode(module);
                            if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(absV)) {
                                absV = builder->CreateLoad(a->getAllocatedType(), a);
                            }
                            if (absV && absV->getType() != i64Ty
                                    && absV->getType()->isIntegerTy()) {
                                absV = builder->CreateIntCast(absV, i64Ty, true);
                            }
                            llvm::Value* newPos = builder->CreateCall(fn,
                                {fd, absV, llvm::ConstantInt::get(i32Ty, 0)},
                                "file.seek");
                            llvm::Value* posSlot = builder->CreateStructGEP(
                                structTy, thisValue, 2, "file.pos_slot");
                            builder->CreateStore(newPos, posSlot);
                            resolvedType = CajetaType::of("void");
                            return nullptr;
                        }
                    }
                    if (methodCallName == "seekFromEnd" && parameters.size() == 1) {
                        llvm::Function* fn = module->getRuntimeFunction(
                            "__cajeta_file_seek");
                        if (fn) {
                            llvm::Value* fd = loadFd();
                            llvm::Value* offV = parameters[0].expression->generateCode(module);
                            if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(offV)) {
                                offV = builder->CreateLoad(a->getAllocatedType(), a);
                            }
                            if (offV && offV->getType() != i64Ty
                                    && offV->getType()->isIntegerTy()) {
                                offV = builder->CreateIntCast(offV, i64Ty, true);
                            }
                            llvm::Value* newPos = builder->CreateCall(fn,
                                {fd, offV, llvm::ConstantInt::get(i32Ty, 2)},
                                "file.seek_end");
                            llvm::Value* posSlot = builder->CreateStructGEP(
                                structTy, thisValue, 2, "file.pos_slot");
                            builder->CreateStore(newPos, posSlot);
                            resolvedType = CajetaType::of("void");
                            return nullptr;
                        }
                    }
                    if (methodCallName == "size" && parameters.empty()) {
                        llvm::Function* fn = module->getRuntimeFunction(
                            "__cajeta_file_size_of");
                        if (fn) {
                            llvm::Value* fd = loadFd();
                            resolvedType = CajetaType::of("int64");
                            return builder->CreateCall(fn, {fd}, "file.size");
                        }
                    }
                    if (methodCallName == "truncate" && parameters.size() == 1) {
                        llvm::Function* fn = module->getRuntimeFunction(
                            "__cajeta_file_truncate");
                        if (fn) {
                            llvm::Value* fd = loadFd();
                            llvm::Value* szV = parameters[0].expression->generateCode(module);
                            if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(szV)) {
                                szV = builder->CreateLoad(a->getAllocatedType(), a);
                            }
                            if (szV && szV->getType() != i64Ty
                                    && szV->getType()->isIntegerTy()) {
                                szV = builder->CreateIntCast(szV, i64Ty, true);
                            }
                            builder->CreateCall(fn, {fd, szV});
                            resolvedType = CajetaType::of("void");
                            return nullptr;
                        }
                    }
                    if (methodCallName == "sync" && parameters.empty()) {
                        llvm::Function* fn = module->getRuntimeFunction(
                            "__cajeta_file_sync");
                        if (fn) {
                            llvm::Value* fd = loadFd();
                            builder->CreateCall(fn, {fd});
                            resolvedType = CajetaType::of("void");
                            return nullptr;
                        }
                    }
                    if (methodCallName == "flush" && parameters.empty()) {
                        llvm::Function* fn = module->getRuntimeFunction(
                            "__cajeta_file_flush");
                        if (fn) {
                            llvm::Value* fd = loadFd();
                            builder->CreateCall(fn, {fd});
                            resolvedType = CajetaType::of("void");
                            return nullptr;
                        }
                    }
                    if (methodCallName == "lock" && parameters.empty()) {
                        llvm::Function* fn = module->getRuntimeFunction(
                            "__cajeta_file_lock");
                        if (fn) {
                            llvm::Value* fd = loadFd();
                            builder->CreateCall(fn, {fd});
                            resolvedType = CajetaType::of("void");
                            return nullptr;
                        }
                    }
                    if (methodCallName == "tryLock" && parameters.empty()) {
                        llvm::Function* fn = module->getRuntimeFunction(
                            "__cajeta_file_try_lock");
                        if (fn) {
                            llvm::Value* fd = loadFd();
                            llvm::Value* result = builder->CreateCall(fn,
                                {fd}, "file.trylock");
                            llvm::Value* asI1 = builder->CreateICmpNE(result,
                                llvm::ConstantInt::get(i32Ty, 0),
                                "file.trylock.bool");
                            resolvedType = CajetaType::of("boolean");
                            return asI1;
                        }
                    }
                    if (methodCallName == "unlock" && parameters.empty()) {
                        llvm::Function* fn = module->getRuntimeFunction(
                            "__cajeta_file_unlock");
                        if (fn) {
                            llvm::Value* fd = loadFd();
                            builder->CreateCall(fn, {fd});
                            resolvedType = CajetaType::of("void");
                            return nullptr;
                        }
                    }
                }
            }
        }

        // ----- cajeta.process.Command.run() intrinsic -----
        // The C bridge marshals and BUILDS the ProcessResult. Command field ABI:
        // argv@1, cwd@2, env@3, stdinData@4, stdinLen@5, stdioFlags@6, timeoutMs@7.
        if (thisValue && targetClass && targetClass->getQName()) {
            const std::string procCanonical = targetClass->getQName()->toCanonical();
            if (procCanonical == "cajeta.process.Command"
                    && methodCallName == "run" && parameters.empty()) {
                llvm::Function* runFn =
                    module->getRuntimeFunction("__cajeta_proc_run");
                auto* cmdStructTy =
                    llvm::dyn_cast<llvm::StructType>(targetClass->getLlvmType());
                auto& cmap = CajetaType::getCanonicalMap();
                CajetaClassPtr strClass;
                {
                    auto it = cmap.find("cajeta.lang.String");
                    if (it == cmap.end()) it = cmap.find("String");
                    if (it != cmap.end())
                        strClass = std::dynamic_pointer_cast<CajetaClass>(it->second);
                }
                CajetaClassPtr prClass;
                {
                    auto it = cmap.find("cajeta.process.ProcessResult");
                    if (it == cmap.end()) it = cmap.find("ProcessResult");
                    if (it != cmap.end())
                        prClass = std::dynamic_pointer_cast<CajetaClass>(it->second);
                }
                auto* strStructTy = strClass
                    ? llvm::dyn_cast_or_null<llvm::StructType>(strClass->getLlvmType())
                    : nullptr;
                auto* prStructTy = prClass
                    ? llvm::dyn_cast_or_null<llvm::StructType>(prClass->getLlvmType())
                    : nullptr;
                if (runFn && cmdStructTy && strStructTy && prStructTy) {
                    llvm::Type* ptrTy = llvm::PointerType::get(llvmCtx, 0);
                    llvm::Type* i32Ty = llvm::Type::getInt32Ty(llvmCtx);
                    llvm::Type* i64Ty = llvm::Type::getInt64Ty(llvmCtx);
                    auto i64c = [&](uint64_t v) {
                        return llvm::ConstantInt::get(i64Ty, v);
                    };
                    auto loadPtrField = [&](unsigned idx, const char* n) {
                        llvm::Value* slot = builder->CreateStructGEP(
                            cmdStructTy, thisValue, idx, n);
                        return builder->CreateLoad(ptrTy, slot, n);
                    };
                    llvm::Value* argvArr = loadPtrField(1, "cmd.argv");
                    llvm::Value* cwdStr  = loadPtrField(2, "cmd.cwd");
                    llvm::Value* envArr  = loadPtrField(3, "cmd.env");
                    llvm::Value* stdinArr = loadPtrField(4, "cmd.stdin");
                    llvm::Value* stdinLen = builder->CreateLoad(i32Ty,
                        builder->CreateStructGEP(cmdStructTy, thisValue, 5,
                            "cmd.stdinLen.slot"), "cmd.stdinLen");
                    llvm::Value* flags = builder->CreateLoad(i32Ty,
                        builder->CreateStructGEP(cmdStructTy, thisValue, 6,
                            "cmd.flags.slot"), "cmd.flags");
                    llvm::Value* timeout = builder->CreateLoad(i64Ty,
                        builder->CreateStructGEP(cmdStructTy, thisValue, 7,
                            "cmd.timeout.slot"), "cmd.timeout");

                    const llvm::DataLayout& dl =
                        module->getLlvmModule()->getDataLayout();
                    const llvm::StructLayout* sSl = dl.getStructLayout(strStructTy);
                    const llvm::StructLayout* pSl = dl.getStructLayout(prStructTy);
                    // String layout: 0 vtable, 1 bytes, 2 byteLength.
                    llvm::Value* strSize    = i64c(dl.getTypeAllocSize(strStructTy));
                    llvm::Value* strOffBytes = i64c(sSl->getElementOffset(1));
                    llvm::Value* strOffLen   = i64c(sSl->getElementOffset(2));
                    // ProcessResult layout: 0 vtable, then the 8 ABI fields.
                    llvm::Value* resSize = i64c(dl.getTypeAllocSize(prStructTy));
                    llvm::Value* offLaunched = i64c(pSl->getElementOffset(1));
                    llvm::Value* offExited   = i64c(pSl->getElementOffset(2));
                    llvm::Value* offExitCode = i64c(pSl->getElementOffset(3));
                    llvm::Value* offSignaled = i64c(pSl->getElementOffset(4));
                    llvm::Value* offSignal   = i64c(pSl->getElementOffset(5));
                    llvm::Value* offTimedOut = i64c(pSl->getElementOffset(6));
                    llvm::Value* offStdout   = i64c(pSl->getElementOffset(7));
                    llvm::Value* offStderr   = i64c(pSl->getElementOffset(8));

                    llvm::Constant* prVtable =
                        llvm::ConstantPointerNull::get(
                            llvm::cast<llvm::PointerType>(ptrTy));
                    if (auto* vt = prClass->getVirtualTableGlobal()) {
                        prVtable = CajetaModule::ensureGlobalInModule(
                            module->getLlvmModule(), vt);
                    }

                    llvm::Value* result = builder->CreateCall(runFn, {
                        argvArr, cwdStr, envArr, stdinArr, stdinLen, flags,
                        timeout, strSize, strOffBytes, strOffLen,
                        prVtable, resSize, offLaunched, offExited, offExitCode,
                        offSignaled, offSignal, offTimedOut, offStdout, offStderr
                    }, "cmd.run");
                    resolvedType = prClass;
                    return result;
                }
            }
        }

        // ----- cajeta.process streaming: Command.spawn() + Process.* -----
        if (thisValue && targetClass && targetClass->getQName()) {
            const std::string pc = targetClass->getQName()->toCanonical();
            const bool isCmd2  = pc == "cajeta.process.Command";
            const bool isProc2 = pc == "cajeta.process.Process";
            const bool wantSpawn = isCmd2 && methodCallName == "start"
                                   && parameters.empty();
            const bool wantWaitFor = isProc2 && methodCallName == "waitFor"
                                     && parameters.empty();
            const bool wantWaitMs = isProc2 && methodCallName == "waitMillis"
                                    && parameters.size() == 1;
            const bool wantKill = isProc2 && methodCallName == "kill"
                                  && parameters.empty();
            const bool wantPid = isProc2 && methodCallName == "pid"
                                 && parameters.empty();
            const bool wantClose = isProc2 && methodCallName == "close"
                                   && parameters.empty();
            if (wantSpawn || wantWaitFor || wantWaitMs || wantKill || wantPid
                    || wantClose) {
                llvm::Type* ptrTy = llvm::PointerType::get(llvmCtx, 0);
                llvm::Type* i32Ty = llvm::Type::getInt32Ty(llvmCtx);
                llvm::Type* i64Ty = llvm::Type::getInt64Ty(llvmCtx);
                auto i64c = [&](uint64_t v) {
                    return llvm::ConstantInt::get(i64Ty, v);
                };
                auto& cmap = CajetaType::getCanonicalMap();
                auto findClass = [&](const char* canon,
                                     const char* shortName) -> CajetaClassPtr {
                    auto it = cmap.find(canon);
                    if (it == cmap.end()) it = cmap.find(shortName);
                    return it != cmap.end()
                        ? std::dynamic_pointer_cast<CajetaClass>(it->second)
                        : nullptr;
                };
                CajetaClassPtr strClass = findClass("cajeta.lang.String", "String");
                CajetaClassPtr prClass =
                    findClass("cajeta.process.ProcessResult", "ProcessResult");
                CajetaClassPtr procClass =
                    findClass("cajeta.process.Process", "Process");
                auto* strStructTy = strClass
                    ? llvm::dyn_cast_or_null<llvm::StructType>(strClass->getLlvmType())
                    : nullptr;
                auto* prStructTy = prClass
                    ? llvm::dyn_cast_or_null<llvm::StructType>(prClass->getLlvmType())
                    : nullptr;
                auto* procStructTy = procClass
                    ? llvm::dyn_cast_or_null<llvm::StructType>(procClass->getLlvmType())
                    : nullptr;
                auto* recvStructTy =
                    llvm::dyn_cast<llvm::StructType>(targetClass->getLlvmType());
                const llvm::DataLayout& dl =
                    module->getLlvmModule()->getDataLayout();

                auto prLayoutArgs = [&](std::vector<llvm::Value*>& args) {
                    const llvm::StructLayout* pSl = dl.getStructLayout(prStructTy);
                    llvm::Constant* prVtable = llvm::ConstantPointerNull::get(
                        llvm::cast<llvm::PointerType>(ptrTy));
                    if (auto* vt = prClass->getVirtualTableGlobal())
                        prVtable = CajetaModule::ensureGlobalInModule(
                            module->getLlvmModule(), vt);
                    args.push_back(prVtable);
                    args.push_back(i64c(dl.getTypeAllocSize(prStructTy)));
                    for (unsigned i = 1; i <= 8; i++)
                        args.push_back(i64c(pSl->getElementOffset(i)));
                };

                if (wantSpawn && recvStructTy && strStructTy && procStructTy) {
                    llvm::Function* fn =
                        module->getRuntimeFunction("__cajeta_proc_spawn");
                    if (fn) {
                        auto loadPtr = [&](unsigned idx, const char* n) {
                            return builder->CreateLoad(ptrTy,
                                builder->CreateStructGEP(recvStructTy, thisValue,
                                    idx, n), n);
                        };
                        llvm::Value* argvArr = loadPtr(1, "cmd.argv");
                        llvm::Value* cwdStr  = loadPtr(2, "cmd.cwd");
                        llvm::Value* envArr  = loadPtr(3, "cmd.env");
                        llvm::Value* flags = builder->CreateLoad(i32Ty,
                            builder->CreateStructGEP(recvStructTy, thisValue, 6,
                                "cmd.flags.slot"), "cmd.flags");
                        const llvm::StructLayout* sSl =
                            dl.getStructLayout(strStructTy);
                        const llvm::StructLayout* pcSl =
                            dl.getStructLayout(procStructTy);
                        llvm::Constant* procVtable =
                            llvm::ConstantPointerNull::get(
                                llvm::cast<llvm::PointerType>(ptrTy));
                        if (auto* vt = procClass->getVirtualTableGlobal())
                            procVtable = CajetaModule::ensureGlobalInModule(
                                module->getLlvmModule(), vt);
                        llvm::Value* result = builder->CreateCall(fn, {
                            argvArr, cwdStr, envArr, flags,
                            i64c(dl.getTypeAllocSize(strStructTy)),
                            i64c(sSl->getElementOffset(1)),
                            i64c(sSl->getElementOffset(2)),
                            procVtable, i64c(dl.getTypeAllocSize(procStructTy)),
                            i64c(pcSl->getElementOffset(1)),
                            i64c(pcSl->getElementOffset(2)),
                            i64c(pcSl->getElementOffset(3)),
                            i64c(pcSl->getElementOffset(4))
                        }, "cmd.spawn");
                        resolvedType = procClass;
                        return result;
                    }
                }

                if (isProc2 && recvStructTy) {
                    auto loadHandle = [&]() {
                        return builder->CreateLoad(i64Ty,
                            builder->CreateStructGEP(recvStructTy, thisValue, 1,
                                "proc.handle.slot"), "proc.handle");
                    };
                    if ((wantWaitFor || wantWaitMs) && prStructTy) {
                        llvm::Function* fn =
                            module->getRuntimeFunction("__cajeta_proc_wait");
                        if (fn) {
                            llvm::Value* handle = loadHandle();
                            llvm::Value* ms = i64c((uint64_t) -1);
                            if (wantWaitMs) {
                                ms = parameters[0].expression->generateCode(module);
                                if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(ms))
                                    ms = builder->CreateLoad(a->getAllocatedType(), a);
                                if (ms && ms->getType() != i64Ty
                                        && ms->getType()->isIntegerTy())
                                    ms = builder->CreateIntCast(ms, i64Ty, true);
                            }
                            std::vector<llvm::Value*> args{handle, ms};
                            prLayoutArgs(args);
                            llvm::Value* result =
                                builder->CreateCall(fn, args, "proc.wait");
                            resolvedType = prClass;
                            return result;
                        }
                    }
                    if (wantKill) {
                        llvm::Function* fn =
                            module->getRuntimeFunction("__cajeta_proc_kill");
                        if (fn) {
                            builder->CreateCall(fn, {loadHandle()});
                            resolvedType = CajetaType::of("void");
                            return nullptr;
                        }
                    }
                    if (wantPid) {
                        llvm::Function* fn =
                            module->getRuntimeFunction("__cajeta_proc_pid");
                        if (fn) {
                            resolvedType = CajetaType::of("int32");
                            return builder->CreateCall(fn, {loadHandle()},
                                "proc.pid");
                        }
                    }
                    if (wantClose) {
                        llvm::Function* fn =
                            module->getRuntimeFunction("__cajeta_proc_release");
                        if (fn) {
                            builder->CreateCall(fn, {loadHandle()});
                            builder->CreateStore(i64c(0),
                                builder->CreateStructGEP(recvStructTy, thisValue,
                                    1, "proc.handle.clear"));
                            resolvedType = CajetaType::of("void");
                            return nullptr;
                        }
                    }
                }
            }
        }

        // ----- TcpStream / TcpListener instance-method intrinsic (NET-1.3 / NET-1.4) -----
        // The fd is struct index 1 (vtable@0), the index File.fd uses; a byte-buffer
        // arg GEPs past the array's 8-byte count header, then adds the caller offset.
        if (thisValue && targetClass && targetClass->getQName()) {
            const std::string netCanonical = targetClass->getQName()->toCanonical();
            bool isTcpStream   = netCanonical == "cajeta.io.net.TcpStream";
            bool isTcpListener = netCanonical == "cajeta.io.net.TcpListener";
            // ----- UdpSocket / socket-option intrinsic (b2) -----
            bool isUdpSocket   = netCanonical == "cajeta.io.net.UdpSocket";
            if (isTcpStream || isTcpListener || isUdpSocket) {
                auto* netStructTy =
                    llvm::cast<llvm::StructType>(targetClass->getLlvmType());
                llvm::Type* ptrTy = llvm::PointerType::get(llvmCtx, 0);
                llvm::Type* i32Ty = llvm::Type::getInt32Ty(llvmCtx);
                llvm::Type* i64Ty = llvm::Type::getInt64Ty(llvmCtx);
                llvm::Type* i8Ty  = llvm::Type::getInt8Ty(llvmCtx);
                auto loadNetFd = [&]() -> llvm::Value* {
                    llvm::Value* fdSlot = builder->CreateStructGEP(
                        netStructTy, thisValue, 1, "net.fd_slot");
                    return builder->CreateLoad(i32Ty, fdSlot, "net.fd");
                };
                // &buf[8 + offset]: skip the array header, then add the offset.
                auto bufPtrAtOffset = [&](size_t argIdx, llvm::Value* offV) -> llvm::Value* {
                    llvm::Value* arr = parameters[argIdx].expression->generateCode(module);
                    if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(arr)) {
                        arr = builder->CreateLoad(a->getAllocatedType(), a);
                    }
                    llvm::Value* dataStart = builder->CreateInBoundsGEP(
                        i8Ty, arr, llvm::ConstantInt::get(i64Ty, 8),
                        "net.buf_start");
                    return builder->CreateInBoundsGEP(
                        i8Ty, dataStart, offV, "net.buf_off");
                };
                auto loadI64Arg = [&](size_t argIdx) -> llvm::Value* {
                    llvm::Value* v = parameters[argIdx].expression->generateCode(module);
                    if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(v)) {
                        v = builder->CreateLoad(a->getAllocatedType(), a);
                    }
                    if (v && v->getType() != i64Ty && v->getType()->isIntegerTy()) {
                        v = builder->CreateIntCast(v, i64Ty, true);
                    }
                    return v;
                };

                if (isTcpStream && methodCallName == "read"
                        && parameters.size() == 3) {
                    llvm::Function* fn = module->getRuntimeFunction(
                        "__cajeta_net_recv");
                    if (fn) {
                        llvm::Value* fd = loadNetFd();
                        llvm::Value* off = loadI64Arg(1);
                        llvm::Value* len = loadI64Arg(2);
                        llvm::Value* buf = bufPtrAtOffset(0, off);
                        llvm::Value* n = builder->CreateCall(fn,
                            {fd, buf, len, llvm::ConstantInt::get(i32Ty, 0)},
                            "net.recv");
                        resolvedType = CajetaType::of("int64");
                        return n;
                    }
                }
                if (isTcpStream && methodCallName == "write"
                        && parameters.size() == 3) {
                    llvm::Function* fn = module->getRuntimeFunction(
                        "__cajeta_net_send");
                    if (fn) {
                        llvm::Value* fd = loadNetFd();
                        llvm::Value* off = loadI64Arg(1);
                        llvm::Value* len = loadI64Arg(2);
                        llvm::Value* buf = bufPtrAtOffset(0, off);
                        llvm::Value* n = builder->CreateCall(fn,
                            {fd, buf, len, llvm::ConstantInt::get(i32Ty, 0)},
                            "net.send");
                        resolvedType = CajetaType::of("int64");
                        return n;
                    }
                }
                if (isTcpStream && methodCallName == "shutdown"
                        && parameters.size() == 1) {
                    llvm::Function* fn = module->getRuntimeFunction(
                        "__cajeta_net_shutdown");
                    if (fn) {
                        llvm::Value* fd = loadNetFd();
                        llvm::Value* how = parameters[0].expression->generateCode(module);
                        if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(how)) {
                            how = builder->CreateLoad(a->getAllocatedType(), a);
                        }
                        if (how && how->getType() != i32Ty
                                && how->getType()->isIntegerTy()) {
                            how = builder->CreateIntCast(how, i32Ty, true);
                        }
                        builder->CreateCall(fn, {fd, how});
                        resolvedType = CajetaType::of("void");
                        return nullptr;
                    }
                }
                if ((isTcpStream || isTcpListener) && methodCallName == "close"
                        && parameters.empty()) {
                    llvm::Function* fn = module->getRuntimeFunction(
                        "__cajeta_net_close");
                    if (fn) {
                        llvm::Value* fd = loadNetFd();
                        builder->CreateCall(fn, {fd});
                        llvm::Value* fdSlot = builder->CreateStructGEP(
                            netStructTy, thisValue, 1, "net.fd_slot");
                        builder->CreateStore(
                            llvm::ConstantInt::get(i32Ty, -1), fdSlot);
                        resolvedType = CajetaType::of("void");
                        return nullptr;
                    }
                }
                if (isTcpListener && methodCallName == "acceptFd"
                        && parameters.empty()) {
                    llvm::Function* fn = module->getRuntimeFunction(
                        "__cajeta_net_accept");
                    if (fn) {
                        llvm::Value* fd = loadNetFd();
                        llvm::Value* nullPtr =
                            llvm::ConstantPointerNull::get(
                                llvm::cast<llvm::PointerType>(ptrTy));
                        llvm::Value* connFd = builder->CreateCall(fn,
                            {fd, nullPtr, nullPtr}, "net.accept");
                        resolvedType = CajetaType::of("int32");
                        return connFd;
                    }
                }
                if (isTcpListener && methodCallName == "boundPort"
                        && parameters.empty()) {
                    llvm::Function* nameFn = module->getRuntimeFunction(
                        "__cajeta_net_getsockname");
                    llvm::Function* unpackFn = module->getRuntimeFunction(
                        "__cajeta_net_sockaddr_unpack");
                    if (nameFn && unpackFn) {
                        llvm::Value* fd = loadNetFd();
                        // sockaddr scratch (128 = the sockaddr_storage upper bound), a len in/out i32,
                        // an octets[16] out and a port i32 out.
                        llvm::Value* scratch = builder->CreateAlloca(
                            llvm::ArrayType::get(i8Ty, 128), nullptr, "net.scratch");
                        llvm::Value* lenSlot = builder->CreateAlloca(
                            i32Ty, nullptr, "net.addrlen");
                        builder->CreateStore(
                            llvm::ConstantInt::get(i32Ty, 128), lenSlot);
                        llvm::Value* octets = builder->CreateAlloca(
                            llvm::ArrayType::get(i8Ty, 16), nullptr, "net.octets");
                        llvm::Value* portSlot = builder->CreateAlloca(
                            i32Ty, nullptr, "net.port");
                        builder->CreateStore(
                            llvm::ConstantInt::get(i32Ty, 0), portSlot);
                        builder->CreateCall(nameFn, {fd, scratch, lenSlot});
                        llvm::Value* addrlen = builder->CreateLoad(
                            i32Ty, lenSlot, "net.addrlen.v");
                        builder->CreateCall(unpackFn,
                            {scratch, addrlen, octets, portSlot});
                        resolvedType = CajetaType::of("int32");
                        return builder->CreateLoad(i32Ty, portSlot, "net.boundPort");
                    }
                }

                // ----- typed socket-option pairs (NET-1.6) on TcpStream + UdpSocket -----

                auto loadI32Arg = [&](size_t argIdx) -> llvm::Value* {
                    llvm::Value* v =
                        parameters[argIdx].expression->generateCode(module);
                    if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(v)) {
                        v = builder->CreateLoad(a->getAllocatedType(), a);
                    }
                    if (v && v->getType() != i32Ty && v->getType()->isIntegerTy()) {
                        v = builder->CreateIntCast(v, i32Ty, true);
                    }
                    return v;
                };
                auto boolArgAsOnOff = [&](size_t argIdx) -> llvm::Value* {
                    llvm::Value* v = loadI32Arg(argIdx);
                    llvm::Value* nz = builder->CreateICmpNE(
                        v, llvm::ConstantInt::get(v->getType(), 0), "opt.nz");
                    return builder->CreateSelect(nz,
                        llvm::ConstantInt::get(i32Ty, 1),
                        llvm::ConstantInt::get(i32Ty, 0), "opt.onoff");
                };
                auto lowerBoolSetter = [&](const char* sym) -> bool {
                    llvm::Function* fn = module->getRuntimeFunction(sym);
                    if (!fn) return false;
                    llvm::Value* fd = loadNetFd();
                    llvm::Value* on = boolArgAsOnOff(0);
                    builder->CreateCall(fn, {fd, on});
                    resolvedType = CajetaType::of("void");
                    return true;
                };
                auto lowerBoolGetter = [&](const char* sym,
                                           llvm::Value*& out) -> bool {
                    llvm::Function* fn = module->getRuntimeFunction(sym);
                    if (!fn) return false;
                    llvm::Value* fd = loadNetFd();
                    llvm::Value* r = builder->CreateCall(fn, {fd}, "opt.get");
                    out = builder->CreateICmpNE(
                        r, llvm::ConstantInt::get(i32Ty, 0), "opt.bool");
                    resolvedType = CajetaType::of("boolean");
                    return true;
                };
                auto lowerIntSetter = [&](const char* sym) -> bool {
                    llvm::Function* fn = module->getRuntimeFunction(sym);
                    if (!fn) return false;
                    llvm::Value* fd = loadNetFd();
                    llvm::Value* val = loadI32Arg(0);
                    builder->CreateCall(fn, {fd, val});
                    resolvedType = CajetaType::of("void");
                    return true;
                };
                auto lowerIntGetter = [&](const char* sym,
                                          llvm::Value*& out) -> bool {
                    llvm::Function* fn = module->getRuntimeFunction(sym);
                    if (!fn) return false;
                    llvm::Value* fd = loadNetFd();
                    out = builder->CreateCall(fn, {fd}, "opt.iget");
                    resolvedType = CajetaType::of("int32");
                    return true;
                };

                bool isOptHolder = isTcpStream || isUdpSocket;
                if (isOptHolder) {
                    struct BoolOpt { const char* m; const char* setSym;
                                     const char* getSym; bool tcp; bool udp; };
                    static const BoolOpt boolOpts[] = {
                        {"NoDelay",   "__cajeta_net_set_nodelay",
                                      "__cajeta_net_get_nodelay",   true,  false},
                        {"KeepAlive", "__cajeta_net_set_keepalive",
                                      "__cajeta_net_get_keepalive", true,  false},
                        {"Broadcast", "__cajeta_net_set_broadcast",
                                      "__cajeta_net_get_broadcast", false, true},
                    };
                    for (const auto& o : boolOpts) {
                        bool applies = (isTcpStream && o.tcp)
                                    || (isUdpSocket && o.udp);
                        if (!applies) continue;
                        if (methodCallName == std::string("set") + o.m
                                && parameters.size() == 1) {
                            if (lowerBoolSetter(o.setSym)) return nullptr;
                        }
                        if (methodCallName == std::string("get") + o.m
                                && parameters.empty()) {
                            llvm::Value* out = nullptr;
                            if (lowerBoolGetter(o.getSym, out)) return out;
                        }
                    }
                    struct IntOpt { const char* m; const char* setSym;
                                    const char* getSym; };
                    static const IntOpt intOpts[] = {
                        {"RecvBufferSize", "__cajeta_net_set_recvbuf",
                                           "__cajeta_net_get_recvbuf"},
                        {"SendBufferSize", "__cajeta_net_set_sendbuf",
                                           "__cajeta_net_get_sendbuf"},
                    };
                    for (const auto& o : intOpts) {
                        if (methodCallName == std::string("set") + o.m
                                && parameters.size() == 1) {
                            if (lowerIntSetter(o.setSym)) return nullptr;
                        }
                        if (methodCallName == std::string("get") + o.m
                                && parameters.empty()) {
                            llvm::Value* out = nullptr;
                            if (lowerIntGetter(o.getSym, out)) return out;
                        }
                    }
                    if (methodCallName == "setTtl" && parameters.size() == 1) {
                        llvm::Function* fn =
                            module->getRuntimeFunction("__cajeta_net_set_ttl");
                        if (fn) {
                            llvm::Value* fd = loadNetFd();
                            llvm::Value* ttl = loadI32Arg(0);
                            builder->CreateCall(fn,
                                {fd, llvm::ConstantInt::get(i32Ty, 0), ttl});
                            resolvedType = CajetaType::of("void");
                            return nullptr;
                        }
                    }
                    if (methodCallName == "getTtl" && parameters.empty()) {
                        llvm::Function* fn =
                            module->getRuntimeFunction("__cajeta_net_get_ttl");
                        if (fn) {
                            llvm::Value* fd = loadNetFd();
                            llvm::Value* r = builder->CreateCall(fn,
                                {fd, llvm::ConstantInt::get(i32Ty, 0)},
                                "opt.ttl");
                            resolvedType = CajetaType::of("int32");
                            return r;
                        }
                    }
                }
                if (isTcpStream && methodCallName == "setLinger"
                        && parameters.size() == 2) {
                    llvm::Function* fn =
                        module->getRuntimeFunction("__cajeta_net_set_linger");
                    if (fn) {
                        llvm::Value* fd = loadNetFd();
                        llvm::Value* on = boolArgAsOnOff(0);
                        llvm::Value* secs = loadI32Arg(1);
                        builder->CreateCall(fn, {fd, on, secs});
                        resolvedType = CajetaType::of("void");
                        return nullptr;
                    }
                }
                if (isTcpStream && methodCallName == "getLinger"
                        && parameters.empty()) {
                    llvm::Function* fn =
                        module->getRuntimeFunction("__cajeta_net_get_linger");
                    if (fn) {
                        llvm::Value* fd = loadNetFd();
                        llvm::Value* onOut = builder->CreateAlloca(
                            i32Ty, nullptr, "linger.on");
                        builder->CreateStore(
                            llvm::ConstantInt::get(i32Ty, 0), onOut);
                        llvm::Value* secsOut = builder->CreateAlloca(
                            i32Ty, nullptr, "linger.secs");
                        builder->CreateStore(
                            llvm::ConstantInt::get(i32Ty, 0), secsOut);
                        builder->CreateCall(fn, {fd, onOut, secsOut});
                        llvm::Value* onV = builder->CreateLoad(
                            i32Ty, onOut, "linger.on.v");
                        resolvedType = CajetaType::of("boolean");
                        return builder->CreateICmpNE(onV,
                            llvm::ConstantInt::get(i32Ty, 0), "linger.bool");
                    }
                }

                // ----- UdpSocket datagram I/O (b2) -----
                if (isUdpSocket) {
                    auto packSockAddrArg =
                        [&](size_t argIdx, llvm::Value*& scratchOut,
                            llvm::Value*& addrlenOut) -> bool {
                        auto& cmapN = CajetaType::getCanonicalMap();
                        CajetaClassPtr saCls, ipCls;
                        auto sIt = cmapN.find("cajeta.io.net.SocketAddress");
                        if (sIt == cmapN.end()) sIt = cmapN.find("SocketAddress");
                        if (sIt != cmapN.end())
                            saCls = std::dynamic_pointer_cast<CajetaClass>(sIt->second);
                        auto iIt = cmapN.find("cajeta.io.net.IpAddress");
                        if (iIt == cmapN.end()) iIt = cmapN.find("IpAddress");
                        if (iIt != cmapN.end())
                            ipCls = std::dynamic_pointer_cast<CajetaClass>(iIt->second);
                        llvm::Function* packFn = module->getRuntimeFunction(
                            "__cajeta_net_sockaddr_pack");
                        if (!packFn || !saCls || !ipCls
                                || !llvm::isa<llvm::StructType>(saCls->getLlvmType())
                                || !llvm::isa<llvm::StructType>(ipCls->getLlvmType()))
                            return false;
                        auto* saTy = llvm::cast<llvm::StructType>(saCls->getLlvmType());
                        auto* ipTy = llvm::cast<llvm::StructType>(ipCls->getLlvmType());
                        llvm::Value* sa =
                            parameters[argIdx].expression->generateCode(module);
                        if (auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(sa)) {
                            sa = builder->CreateLoad(a->getAllocatedType(), a);
                        }
                        llvm::Value* ip = builder->CreateLoad(ptrTy,
                            builder->CreateStructGEP(saTy, sa, 1, "udp.ip_slot"),
                            "udp.ip");
                        llvm::Value* port = builder->CreateLoad(i32Ty,
                            builder->CreateStructGEP(saTy, sa, 2, "udp.port_slot"),
                            "udp.port");
                        llvm::Value* family = builder->CreateLoad(i32Ty,
                            builder->CreateStructGEP(ipTy, ip, 1, "udp.fam_slot"),
                            "udp.family");
                        llvm::Value* octArr = builder->CreateLoad(ptrTy,
                            builder->CreateStructGEP(ipTy, ip, 2, "udp.oct_slot"),
                            "udp.octets_arr");
                        llvm::Value* octData = builder->CreateInBoundsGEP(
                            i8Ty, octArr, llvm::ConstantInt::get(i64Ty, 8),
                            "udp.octets");
                        scratchOut = builder->CreateAlloca(
                            llvm::ArrayType::get(i8Ty, 128), nullptr, "udp.sa");
                        addrlenOut = builder->CreateCall(packFn,
                            {family, octData, port, scratchOut,
                             llvm::ConstantInt::get(i32Ty, 128)}, "udp.addrlen");
                        return true;
                    };

                    auto buildSockAddr =
                        [&](llvm::Value* octets16, llvm::Value* portV,
                            llvm::Value* familyV) -> llvm::Value* {
                        auto& cmapN = CajetaType::getCanonicalMap();
                        CajetaClassPtr saCls, ipCls;
                        auto sIt = cmapN.find("cajeta.io.net.SocketAddress");
                        if (sIt == cmapN.end()) sIt = cmapN.find("SocketAddress");
                        if (sIt != cmapN.end())
                            saCls = std::dynamic_pointer_cast<CajetaClass>(sIt->second);
                        auto iIt = cmapN.find("cajeta.io.net.IpAddress");
                        if (iIt == cmapN.end()) iIt = cmapN.find("IpAddress");
                        if (iIt != cmapN.end())
                            ipCls = std::dynamic_pointer_cast<CajetaClass>(iIt->second);
                        llvm::Function* newArr = module->getRuntimeFunction(
                            "__cajeta_new_array_header");
                        if (!saCls || !ipCls || !newArr
                                || !llvm::isa<llvm::StructType>(saCls->getLlvmType())
                                || !llvm::isa<llvm::StructType>(ipCls->getLlvmType()))
                            return nullptr;
                        auto* saTy = llvm::cast<llvm::StructType>(saCls->getLlvmType());
                        auto* ipTy = llvm::cast<llvm::StructType>(ipCls->getLlvmType());
                        const llvm::DataLayout& dl =
                            module->getLlvmModule()->getDataLayout();
                        auto allocObj = [&](CajetaClassPtr cls,
                                            llvm::StructType* ty) -> llvm::Value* {
                            llvm::Constant* size = llvm::ConstantInt::get(
                                i64Ty, dl.getTypeAllocSize(ty));
                            llvm::Value* inst =
                                MemoryManager::createMallocInstruction(
                                    module, size, builder->GetInsertBlock());
                            builder->CreateMemSet(inst,
                                llvm::ConstantInt::get(i8Ty, 0), size,
                                llvm::MaybeAlign(8));
                            llvm::Constant* vt =
                                llvm::ConstantPointerNull::get(
                                    llvm::cast<llvm::PointerType>(ptrTy));
                            if (auto* g = cls->getVirtualTableGlobal()) {
                                vt = CajetaModule::ensureGlobalInModule(
                                    module->getLlvmModule(), g);
                            }
                            builder->CreateStore(vt,
                                builder->CreateStructGEP(ty, inst, 0, "sa.vt"));
                            return inst;
                        };
                        llvm::Value* arr = builder->CreateCall(newArr,
                            {llvm::ConstantInt::get(i64Ty, 8),
                             llvm::ConstantInt::get(i64Ty, 1),
                             llvm::ConstantInt::get(i64Ty, 16)}, "sa.octarr");
                        llvm::Value* arrData = builder->CreateInBoundsGEP(
                            i8Ty, arr, llvm::ConstantInt::get(i64Ty, 8),
                            "sa.octdata");
                        builder->CreateMemCpy(arrData, llvm::MaybeAlign(1),
                            octets16, llvm::MaybeAlign(1),
                            llvm::ConstantInt::get(i64Ty, 16));
                        // IpAddress { vtable@0, family@1, octets@2 }.
                        llvm::Value* ipInst = allocObj(ipCls, ipTy);
                        builder->CreateStore(familyV,
                            builder->CreateStructGEP(ipTy, ipInst, 1, "ip.fam"));
                        builder->CreateStore(arr,
                            builder->CreateStructGEP(ipTy, ipInst, 2, "ip.oct"));
                        // SocketAddress { vtable@0, ip@1, port@2 }.
                        llvm::Value* saInst = allocObj(saCls, saTy);
                        builder->CreateStore(ipInst,
                            builder->CreateStructGEP(saTy, saInst, 1, "sa.ip"));
                        builder->CreateStore(portV,
                            builder->CreateStructGEP(saTy, saInst, 2, "sa.port"));
                        return saInst;
                    };

                    if (methodCallName == "sendTo" && parameters.size() == 4) {
                        llvm::Function* fn = module->getRuntimeFunction(
                            "__cajeta_net_sendto");
                        llvm::Value* scratch = nullptr;
                        llvm::Value* addrlen = nullptr;
                        if (fn && packSockAddrArg(3, scratch, addrlen)) {
                            llvm::Value* fd = loadNetFd();
                            llvm::Value* off = loadI64Arg(1);
                            llvm::Value* len = loadI64Arg(2);
                            llvm::Value* buf = bufPtrAtOffset(0, off);
                            llvm::Value* n = builder->CreateCall(fn,
                                {fd, buf, len,
                                 llvm::ConstantInt::get(i32Ty, 0),
                                 scratch, addrlen}, "udp.sendto");
                            resolvedType = CajetaType::of("int32");
                            return builder->CreateIntCast(n, i32Ty, true);
                        }
                    }
                    if (methodCallName == "recvFrom" && parameters.size() == 3) {
                        llvm::Function* fn = module->getRuntimeFunction(
                            "__cajeta_net_recvfrom");
                        llvm::Function* unpackFn = module->getRuntimeFunction(
                            "__cajeta_net_sockaddr_unpack");
                        auto& cmapN = CajetaType::getCanonicalMap();
                        auto rrIt = cmapN.find("cajeta.io.net.RecvResult");
                        if (rrIt == cmapN.end()) rrIt = cmapN.find("RecvResult");
                        CajetaClassPtr rrCls = rrIt != cmapN.end()
                            ? std::dynamic_pointer_cast<CajetaClass>(rrIt->second)
                            : nullptr;
                        if (fn && unpackFn && rrCls
                                && llvm::isa<llvm::StructType>(rrCls->getLlvmType())) {
                            llvm::Value* fd = loadNetFd();
                            llvm::Value* off = loadI64Arg(1);
                            llvm::Value* cap = loadI64Arg(2);
                            llvm::Value* buf = bufPtrAtOffset(0, off);
                            llvm::Value* scratch = builder->CreateAlloca(
                                llvm::ArrayType::get(i8Ty, 128), nullptr,
                                "udp.rf.sa");
                            llvm::Value* lenSlot = builder->CreateAlloca(
                                i32Ty, nullptr, "udp.rf.len");
                            builder->CreateStore(
                                llvm::ConstantInt::get(i32Ty, 128), lenSlot);
                            llvm::Value* n = builder->CreateCall(fn,
                                {fd, buf, cap,
                                 llvm::ConstantInt::get(i32Ty, 0),
                                 scratch, lenSlot}, "udp.recvfrom");
                            llvm::Value* count =
                                builder->CreateIntCast(n, i32Ty, true);
                            llvm::Value* addrlen = builder->CreateLoad(
                                i32Ty, lenSlot, "udp.rf.len.v");
                            llvm::Value* octets = builder->CreateAlloca(
                                llvm::ArrayType::get(i8Ty, 16), nullptr,
                                "udp.rf.oct");
                            builder->CreateMemSet(octets,
                                llvm::ConstantInt::get(i8Ty, 0),
                                llvm::ConstantInt::get(i64Ty, 16),
                                llvm::MaybeAlign(1));
                            llvm::Value* portSlot = builder->CreateAlloca(
                                i32Ty, nullptr, "udp.rf.port");
                            builder->CreateStore(
                                llvm::ConstantInt::get(i32Ty, 0), portSlot);
                            llvm::Value* fam = builder->CreateCall(unpackFn,
                                {scratch, addrlen, octets, portSlot},
                                "udp.rf.fam");
                            llvm::Value* famOk = builder->CreateICmpSLT(fam,
                                llvm::ConstantInt::get(i32Ty, 0), "udp.rf.famneg");
                            llvm::Value* famClamped = builder->CreateSelect(
                                famOk, llvm::ConstantInt::get(i32Ty, 0), fam,
                                "udp.rf.fam.c");
                            llvm::Value* portV = builder->CreateLoad(
                                i32Ty, portSlot, "udp.rf.port.v");
                            llvm::Value* from =
                                buildSockAddr(octets, portV, famClamped);
                            // RecvResult { vtable@0, count@1, from@2 }.
                            auto* rrTy = llvm::cast<llvm::StructType>(
                                rrCls->getLlvmType());
                            const llvm::DataLayout& dl =
                                module->getLlvmModule()->getDataLayout();
                            llvm::Constant* size = llvm::ConstantInt::get(
                                i64Ty, dl.getTypeAllocSize(rrTy));
                            llvm::Value* rr =
                                MemoryManager::createMallocInstruction(
                                    module, size, builder->GetInsertBlock());
                            builder->CreateMemSet(rr,
                                llvm::ConstantInt::get(i8Ty, 0), size,
                                llvm::MaybeAlign(8));
                            llvm::Constant* vt =
                                llvm::ConstantPointerNull::get(
                                    llvm::cast<llvm::PointerType>(ptrTy));
                            if (auto* g = rrCls->getVirtualTableGlobal()) {
                                vt = CajetaModule::ensureGlobalInModule(
                                    module->getLlvmModule(), g);
                            }
                            builder->CreateStore(vt,
                                builder->CreateStructGEP(rrTy, rr, 0, "rr.vt"));
                            builder->CreateStore(count,
                                builder->CreateStructGEP(rrTy, rr, 1, "rr.count"));
                            if (from) {
                                builder->CreateStore(from,
                                    builder->CreateStructGEP(rrTy, rr, 2,
                                        "rr.from"));
                            }
                            resolvedType = rrCls;
                            return rr;
                        }
                    }
                    if (methodCallName == "connect" && parameters.size() == 1) {
                        llvm::Function* fn = module->getRuntimeFunction(
                            "__cajeta_net_connect");
                        llvm::Value* scratch = nullptr;
                        llvm::Value* addrlen = nullptr;
                        if (fn && packSockAddrArg(0, scratch, addrlen)) {
                            llvm::Value* fd = loadNetFd();
                            builder->CreateCall(fn, {fd, scratch, addrlen});
                            resolvedType = CajetaType::of("void");
                            return nullptr;
                        }
                    }
                    if (methodCallName == "send" && parameters.size() == 3) {
                        llvm::Function* fn = module->getRuntimeFunction(
                            "__cajeta_net_send");
                        if (fn) {
                            llvm::Value* fd = loadNetFd();
                            llvm::Value* off = loadI64Arg(1);
                            llvm::Value* len = loadI64Arg(2);
                            llvm::Value* buf = bufPtrAtOffset(0, off);
                            llvm::Value* n = builder->CreateCall(fn,
                                {fd, buf, len,
                                 llvm::ConstantInt::get(i32Ty, 0)}, "udp.send");
                            resolvedType = CajetaType::of("int32");
                            return builder->CreateIntCast(n, i32Ty, true);
                        }
                    }
                    if (methodCallName == "recv" && parameters.size() == 3) {
                        llvm::Function* fn = module->getRuntimeFunction(
                            "__cajeta_net_recv");
                        if (fn) {
                            llvm::Value* fd = loadNetFd();
                            llvm::Value* off = loadI64Arg(1);
                            llvm::Value* cap = loadI64Arg(2);
                            llvm::Value* buf = bufPtrAtOffset(0, off);
                            llvm::Value* n = builder->CreateCall(fn,
                                {fd, buf, cap,
                                 llvm::ConstantInt::get(i32Ty, 0)}, "udp.recv");
                            resolvedType = CajetaType::of("int32");
                            return builder->CreateIntCast(n, i32Ty, true);
                        }
                    }
                    if (methodCallName == "localAddress" && parameters.empty()) {
                        llvm::Function* nameFn = module->getRuntimeFunction(
                            "__cajeta_net_getsockname");
                        llvm::Function* unpackFn = module->getRuntimeFunction(
                            "__cajeta_net_sockaddr_unpack");
                        if (nameFn && unpackFn) {
                            llvm::Value* fd = loadNetFd();
                            llvm::Value* scratch = builder->CreateAlloca(
                                llvm::ArrayType::get(i8Ty, 128), nullptr,
                                "udp.la.sa");
                            llvm::Value* lenSlot = builder->CreateAlloca(
                                i32Ty, nullptr, "udp.la.len");
                            builder->CreateStore(
                                llvm::ConstantInt::get(i32Ty, 128), lenSlot);
                            llvm::Value* octets = builder->CreateAlloca(
                                llvm::ArrayType::get(i8Ty, 16), nullptr,
                                "udp.la.oct");
                            builder->CreateMemSet(octets,
                                llvm::ConstantInt::get(i8Ty, 0),
                                llvm::ConstantInt::get(i64Ty, 16),
                                llvm::MaybeAlign(1));
                            llvm::Value* portSlot = builder->CreateAlloca(
                                i32Ty, nullptr, "udp.la.port");
                            builder->CreateStore(
                                llvm::ConstantInt::get(i32Ty, 0), portSlot);
                            builder->CreateCall(nameFn, {fd, scratch, lenSlot});
                            llvm::Value* addrlen = builder->CreateLoad(
                                i32Ty, lenSlot, "udp.la.len.v");
                            llvm::Value* fam = builder->CreateCall(unpackFn,
                                {scratch, addrlen, octets, portSlot},
                                "udp.la.fam");
                            llvm::Value* famNeg = builder->CreateICmpSLT(fam,
                                llvm::ConstantInt::get(i32Ty, 0), "udp.la.famneg");
                            llvm::Value* famClamped = builder->CreateSelect(
                                famNeg, llvm::ConstantInt::get(i32Ty, 0), fam,
                                "udp.la.fam.c");
                            llvm::Value* portV = builder->CreateLoad(
                                i32Ty, portSlot, "udp.la.port.v");
                            llvm::Value* sa =
                                buildSockAddr(octets, portV, famClamped);
                            if (sa) {
                                auto& cmapN2 = CajetaType::getCanonicalMap();
                                auto sIt = cmapN2.find("cajeta.io.net.SocketAddress");
                                if (sIt == cmapN2.end())
                                    sIt = cmapN2.find("SocketAddress");
                                if (sIt != cmapN2.end())
                                    resolvedType =
                                        std::dynamic_pointer_cast<CajetaClass>(
                                            sIt->second);
                                return sa;
                            }
                        }
                    }
                }
            }
        }

        bool nullSafeStringMethod = false;
        llvm::Type* nullSafeReturnTy = nullptr;
        llvm::Constant* nullSafeDefault = nullptr;
        if (thisValue && thisValue->getType()->isPointerTy() && targetClass
                && targetClass->getQName()
                && targetClass->getQName()->getTypeName() == "String"
                && targetClass->getQName()->getPackageName() == "cajeta.lang") {
            auto& ctx = *module->getLlvmContext();
            llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
            llvm::Type* i1Ty = llvm::Type::getInt1Ty(ctx);
            if (methodCallName == "size" || methodCallName == "count") {
                nullSafeStringMethod = true;
                nullSafeReturnTy = i64Ty;
                nullSafeDefault = llvm::ConstantInt::get(i64Ty, 0);
            } else if (methodCallName == "isEmpty") {
                nullSafeStringMethod = true;
                nullSafeReturnTy = i1Ty;
                nullSafeDefault = llvm::ConstantInt::get(i1Ty, 1);
            } else if (methodCallName == "equals") {
                nullSafeStringMethod = true;
                nullSafeReturnTy = i1Ty;
                nullSafeDefault = llvm::ConstantInt::get(i1Ty, 0);
            }
        }

        llvm::BasicBlock* nullSafeNullBB = nullptr;
        llvm::BasicBlock* nullSafeCallBB = nullptr;
        llvm::BasicBlock* nullSafeJoinBB = nullptr;
        if (nullSafeStringMethod) {
            auto& ctx = *module->getLlvmContext();
            llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
            nullSafeNullBB = llvm::BasicBlock::Create(ctx, "str.nullsafe.null", parentFn);
            nullSafeCallBB = llvm::BasicBlock::Create(ctx, "str.nullsafe.call", parentFn);
            nullSafeJoinBB = llvm::BasicBlock::Create(ctx, "str.nullsafe.join", parentFn);
            llvm::Value* isNull = builder->CreateICmpEQ(thisValue,
                llvm::ConstantPointerNull::get(
                    llvm::cast<llvm::PointerType>(thisValue->getType())),
                "str.null");
            builder->CreateCondBr(isNull, nullSafeNullBB, nullSafeCallBB);
            builder->SetInsertPoint(nullSafeCallBB);
        }

        // A caller's `#x` deactivates that argument's drop entry - the callee takes the
        // title. A `#T` formal alone deactivates nothing; the callee-side pass below
        // instead rejects a plain argument so the caller has to spell `#`.
        {
            for (size_t i = 0; i < parameters.size(); ++i) {
                if (parameters[i].callerTransferred) continue;
                auto advId = std::dynamic_pointer_cast<IdentifierExpression>(
                    parameters[i].expression);
                if (!advId) continue;
                auto advScope = module->getScopeStack().peek();
                if (!advScope) continue;
                FieldPtr advF = advScope->getField(advId->getTextValue());
                bool advIsLocalOwner = advF && advF->getDropEntry()
                    && !std::dynamic_pointer_cast<ParameterField>(advF);
                if (!advIsLocalOwner) continue;
                auto advM = module->getCurrentMethod();
                if (!advM) continue;
                if (!advM->isFinalUseOfLocal(advId->getTextValue(),
                        advId->getSourceLine(), advId->getSourceColumn())) {
                    continue;
                }
                if (auto* eng = DiagnosticEngine::active()) {
                    eng->report("warning", "CAJETA_WARN_LAST_USE_TRANSFER",
                        "`" + advId->getTextValue() + "` is lent here at its "
                        "final use, so nothing in this scope reads it again — "
                        "if the callee is meant to KEEP it, spell `#"
                            + advId->getTextValue() + "` to transfer the title. "
                        "As written the title stays with the local and is "
                        "released at scope exit (a plain argument lends).",
                        module->getSourcePath(),
                        advId->getSourceLine(), advId->getSourceColumn() + 1);
                }
            }
            if (!children.empty()) {
                if (auto recvId = std::dynamic_pointer_cast<IdentifierExpression>(
                        children[0])) {
                    if (auto sc = module->getScopeStack().peek()) {
                        FieldPtr recvF = sc->getField(recvId->getTextValue());
                        MethodPtr lendCallee;
                        if (recvF) {
                            auto recvCls = std::dynamic_pointer_cast<CajetaClass>(
                                recvF->getType());
                            if (recvCls) {
                                try {
                                    lendCallee = recvCls->resolveMethod(
                                        methodCallName, entries,
                                        /*isConstructor=*/false,
                                        /*floatingParams=*/false);
                                } catch (...) {
                                    lendCallee = nullptr;
                                }
                            }
                        }
                        if (recvF) {
                            for (size_t i = 0; i < parameters.size(); ++i) {
                                if (parameters[i].callerTransferred) continue;
                                auto argId = std::dynamic_pointer_cast<
                                    IdentifierExpression>(
                                        parameters[i].expression);
                                if (!argId) continue;
                                FieldPtr argF = sc->getField(argId->getTextValue());
                                bool argIsLocalOwner = argF && argF->getDropEntry()
                                    && !std::dynamic_pointer_cast<ParameterField>(
                                           argF);
                                if (!argIsLocalOwner) continue;
                                if (!lendCallee) continue;
                                auto lfps = lendCallee->getParameterList();
                                size_t lfi = i + (lendCallee->isStatic() ? 0 : 1);
                                if (lfi >= lfps.size() || !lfps[lfi]) continue;
                                if (lendCallee->retainsFormal(
                                        lfps[lfi]->getName())) {
                                    sc->recordLend(recvId->getTextValue(),
                                                   argId->getTextValue());
                                }
                            }
                        }
                    }
                }
            }
            auto deactivateIfClassLocal = [&](size_t argIdx) {
                if (argIdx >= parameters.size()) return;
                auto argExprBase = parameters[argIdx].expression;
                auto idExpr = std::dynamic_pointer_cast<IdentifierExpression>(
                    argExprBase);
                if (!idExpr) return;
                auto scope = module->getScopeStack().peek();
                if (!scope) return;
                FieldPtr field = scope->getField(idExpr->getTextValue());
                if (!field) return;
                if (field->getDropEntry()) {
                    ownership::deactivateLocalEntry(module, field);
                }
                maybeEmitSessionDisarm(module, idExpr->getTextValue());
            };
            for (size_t i = 0; i < parameters.size(); ++i) {
                if (!parameters[i].callerTransferred) continue;
                auto idExpr = std::dynamic_pointer_cast<IdentifierExpression>(
                    parameters[i].expression);
                if (idExpr) {
                    if (auto scope = module->getScopeStack().peek()) {
                        const string& nm = idExpr->getTextValue();
                        FieldPtr field = scope->getField(nm);
                        if (field && !std::dynamic_pointer_cast<ParameterField>(field)) {
                            scope->rejectTransferOfBorrow(nm, /*modeCarrying=*/false);
                            auto klass = std::dynamic_pointer_cast<CajetaClass>(
                                field->getType());
                            if (klass && !klass->isValueType()
                                    && !klass->isSharedCapableValue()
                                    && !klass->isInterface()) {
                                scope->demoteToBorrow(nm,
                                    "transferred to `" + methodCallName
                                        + "` at line "
                                        + std::to_string(getSourceLine()));
                            }
                        }
                    }
                }
                deactivateIfClassLocal(i);
            }
            bool callFloatingX = true;
            for (auto& e : entries) {
                if (e.label.empty()) { callFloatingX = false; break; }
            }
            MethodPtr xferTarget = targetClass->resolveMethod(
                methodCallName, entries, /*isConstructor=*/false,
                callFloatingX);
            if (xferTarget) {
                auto formalParams = xferTarget->getParameterList();
                bool isStaticTgt = xferTarget->getModifiers().find(STATIC)
                    != xferTarget->getModifiers().end();
                bool hasThisP = !formalParams.empty()
                    && formalParams.front()->getName() == "this";
                int xferParamOffset = (isStaticTgt || !hasThisP) ? 0 : 1;
                size_t fIdx = 0;
                for (auto& fp : formalParams) {
                    if ((int) fIdx < xferParamOffset) { ++fIdx; continue; }
                    size_t argIdx = fIdx - xferParamOffset;
                    if (argIdx >= parameters.size()) break;
                    ++fIdx;
                    if (!fp->isTransferred()) continue;
                    auto escArg = parameters[argIdx].expression;
                    if (!escArg) continue;
                    if (escArg->kind() == ExprKind::Move
                            || parameters[argIdx].callerTransferred) {
                        ownership::rejectEscape(escArg,
                            ownership::ConsumerRole::ArgOwned, module,
                            "a `#T` argument");
                    }
                    ownership::rejectOwnedFormalArgument(escArg,
                        parameters[argIdx].callerTransferred, module,
                        methodCallName, fp->getName(), (int) getSourceLine());
                }
            }
        }

        if (targetClass && targetClass->isWildcardInstantiation()
                && !isSuperCall) {
            auto outerRecvId = !children.empty()
                ? dynamic_pointer_cast<IdentifierExpression>(children[0])
                : nullptr;
            for (auto& methodEntry : targetClass->getMethods()) {
                MethodPtr m = methodEntry.second;
                if (!m || m->getName() != methodCallName) continue;
                auto plist = m->getParameterList();
                bool isStatic = m->getModifiers().find(STATIC)
                    != m->getModifiers().end();
                size_t thisShift = isStatic ? 0 : 1;
                if (parameters.size() + thisShift > plist.size()) continue;
                for (size_t i = 0; i < parameters.size(); ++i) {
                    auto& formal = plist[i + thisShift];
                    if (!formal || !formal->getType()) continue;
                    if (formal->getType()->wildcardKind()
                            != CajetaType::WildcardKind::Extends) {
                        continue;
                    }
                    auto argExpr = parameters[i].expression;
                    bool sameReceiverMce = false;
                    if (outerRecvId) {
                        auto argMce = dynamic_pointer_cast<MethodCallExpression>(argExpr);
                        if (argMce && !argMce->getChildren().empty()) {
                            auto argRecvId = dynamic_pointer_cast<IdentifierExpression>(
                                argMce->getChildren()[0]);
                            if (argRecvId
                                    && argRecvId->getTextValue()
                                        == outerRecvId->getTextValue()) {
                                sameReceiverMce = true;
                            }
                        }
                    }
                    if (!sameReceiverMce) {
                        throw Exception(
                            "cannot pass value to wildcard parameter '"
                            + formal->getName() + "' of "
                            + targetClass->toCanonical() + "::"
                            + methodCallName + " — the receiver's "
                            "type-parameter T is unknown beyond its bound, "
                            "so any value not sourced from this same "
                            "receiver might violate the container's "
                            "element-type contract (PECS: producer "
                            "extends, consumer super). To make the write "
                            "safe, either narrow the receiver type or "
                            "thread the value through the same receiver "
                            "(e.g. `b.set(b.get())`).",
                            "CAJETA_ERROR_PECS_WRITE_VIOLATION");
                    }
                }
                break;
            }
        }

        bool targetIsFinalClass = targetClass
            && !targetClass->isInterface()
            && targetClass->getModifiers().count(FINAL) > 0;
        // The transfer word is RUNTIME-COMPOSED: a `#x` whose source is itself a
        // runtime owner forwards the flag it held, and only sources with no entry
        // contribute a static 1. The args generated above stashed their flags.
        int64_t moveMask = 0;
        llvm::Value* transferWordVal = nullptr;
        for (size_t mmi = 0; mmi < parameters.size(); ++mmi) {
            llvm::Value* rf = mmi < argTitleFlags.size() ? argTitleFlags[mmi] : nullptr;
            if (!rf) continue;
            if (auto* k = llvm::dyn_cast<llvm::ConstantInt>(rf)) {
                if (!k->isZero()) moveMask |= ((int64_t) 1) << mmi;
                continue;
            }
            llvm::Value* bit = builder->CreateShl(
                builder->CreateAnd(rf, builder->getInt64(1)),
                builder->getInt64((uint64_t) mmi));
            transferWordVal = transferWordVal
                ? builder->CreateOr(transferWordVal, bit) : bit;
        }
        if (transferWordVal) {
            if (moveMask != 0) {
                transferWordVal = builder->CreateOr(transferWordVal,
                    builder->getInt64((uint64_t) moveMask));
            }
        } else {
            transferWordVal = builder->getInt64((uint64_t) moveMask);
        }
        llvm::Value* callResult = targetClass->invokeMethod(methodCallName, entries,
            /*isConstructor=*/false, thisValue, /*callerModule=*/module,
            /*forceDirectCall=*/(isSuperCall || targetIsFinalClass),
            /*explicitMethodTypeArgs=*/explicitMethodTypeArgs,
            /*sretTarget=*/nullptr,
            /*transferWord=*/transferWordVal,
            /*errorIfUnresolved=*/true,
            getSourceLine(), getSourceColumn() + 1);

        if (nullSafeStringMethod) {
            // Close all three null-safety blocks unconditionally: invokeMethod may return
            // null for a method the class does not define, and leaving the cond-br's blocks
            // open fails JIT verification with "Basic Block does not have terminator".
            llvm::Value* normalizedCall = callResult;
            if (callResult && callResult->getType() != nullSafeReturnTy
                    && callResult->getType()->isIntegerTy()
                    && nullSafeReturnTy->isIntegerTy()) {
                normalizedCall = builder->CreateIntCast(callResult,
                    nullSafeReturnTy, /*isSigned=*/false, "str.nullsafe.cast");
            }
            llvm::BasicBlock* callTerm = builder->GetInsertBlock();
            builder->CreateBr(nullSafeJoinBB);
            builder->SetInsertPoint(nullSafeNullBB);
            builder->CreateBr(nullSafeJoinBB);
            builder->SetInsertPoint(nullSafeJoinBB);
            llvm::PHINode* phi = builder->CreatePHI(nullSafeReturnTy, 2,
                "str.nullsafe.result");
            phi->addIncoming(
                normalizedCall ? normalizedCall : nullSafeDefault, callTerm);
            phi->addIncoming(nullSafeDefault, nullSafeNullBB);
            callResult = phi;
        }

        {
            bool callFloatingT = true;
            for (auto& e : entries) {
                if (e.label.empty()) { callFloatingT = false; break; }
            }
            MethodPtr tempTarget = targetClass
                ? targetClass->resolveMethod(methodCallName, entries,
                      /*isConstructor=*/false, callFloatingT,
                      /*explicitMethodTypeArgs=*/explicitMethodTypeArgs)
                : nullptr;
            llvm::Function* strDropFn = module->getRuntimeFunction(
                "__cajeta_string_drop");
            if (tempTarget && strDropFn
                    && entries.size() == parameters.size()) {
                resolvedReturnsOwnership = tempTarget->isReturnsOwnership();
                resolvedReturnsOwnershipKnown = true;
                resolvedMethod = tempTarget;
                auto fpl = tempTarget->getParameterList();
                bool isStaticT = tempTarget->getModifiers().find(STATIC)
                    != tempTarget->getModifiers().end();
                bool hasThisT = !fpl.empty()
                    && fpl.front()->getName() == "this";
                int offT = (isStaticT || !hasThisT) ? 0 : 1;
                for (size_t ai = 0; ai < parameters.size(); ++ai) {
                    size_t fi = ai + (size_t) offT;
                    if (fi >= fpl.size()) break;
                    if (!fpl[fi] || fpl[fi]->isTransferred()) continue;
                    if (parameters[ai].callerTransferred) continue;
                    llvm::Value* tempV = entries[ai].value;
                    if (!tempV) continue;
                    if (ai < argTitles.size()
                            && argTitles[ai].shape.has(ownership::TitleShape::kString)
                            && argTitles[ai].shape.family != ownership::TitleFamily::Literal
                            && tempV->getType()->isPointerTy()) {
                        const auto& at = argTitles[ai];
                        if (at.shape.answer == ownership::TitleAnswer::Owned) {
                            builder->CreateCall(strDropFn, {tempV});
                        } else if (at.shape.answer == ownership::TitleAnswer::Runtime
                                && at.flag) {
                            if (auto* cf = llvm::dyn_cast<llvm::ConstantInt>(at.flag)) {
                                if (!cf->isZero()) builder->CreateCall(strDropFn, {tempV});
                            } else {
                                auto& tctx = *module->getLlvmContext();
                                llvm::Function* tfn = builder->GetInsertBlock()->getParent();
                                auto* dropBB = llvm::BasicBlock::Create(tctx, "arg_temp_drop", tfn);
                                auto* contBB = llvm::BasicBlock::Create(tctx, "arg_temp_cont", tfn);
                                builder->CreateCondBr(
                                    builder->CreateICmpNE(at.flag,
                                        llvm::ConstantInt::get(at.flag->getType(), 0),
                                        "arg_temp_owned"),
                                    dropBB, contBB);
                                builder->SetInsertPoint(dropBB);
                                builder->CreateCall(strDropFn, {tempV});
                                builder->CreateBr(contBB);
                                builder->SetInsertPoint(contBB);
                            }
                        }
                        continue;
                    }
                    if (auto vCls = freshSharedValueTempClass(
                            parameters[ai].expression)) {
                        llvm::Value* slot = tempV;
                        if (!tempV->getType()->isPointerTy()) {
                            slot = builder->CreateAlloca(tempV->getType(),
                                nullptr, "temp.value.rel");
                            builder->CreateStore(tempV, slot);
                        }
                        llvm::Function* relFn = CajetaModule::ensureFunctionInModule(
                            module->getLlvmModule(),
                            vCls->getOrCreateValueReleaseFunction());
                        if (relFn) builder->CreateCall(relFn, {slot});
                    }
                }
                if (offT == 1 && !fpl.empty() && fpl.front()
                        && !fpl.front()->isTransferred()
                        && !children.empty() && thisValue
                        && thisValue->getType()->isPointerTy()) {
                    if (freshOwnedStringTemp(children[0])) {
                        builder->CreateCall(strDropFn, {thisValue});
                    } else if (auto rCls = freshSharedValueTempClass(
                            children[0])) {
                        llvm::Function* relFn =
                            CajetaModule::ensureFunctionInModule(
                                module->getLlvmModule(),
                                rCls->getOrCreateValueReleaseFunction());
                        if (relFn) builder->CreateCall(relFn, {thisValue});
                    } else if (recvTempClass
                            && (recvTempStatic || recvTempFlag)) {
                        // Reclaim an anonymous owned receiver ONLY for a void/primitive return: a
                        // class-pointer result may be a wrapper that borrowed it. The dtors inside
                        // virtual_drop clobber the return-flag TLS, so save and restore it here.
                        CajetaTypePtr rrt = tempTarget->getReturnType();
                        auto rrtClass = dynamic_pointer_cast<CajetaClass>(rrt);
                        bool retIsSafeScalar = !rrt
                            || (!tempTarget->returnsClassPointer()
                                && !dynamic_pointer_cast<CajetaArray>(rrt)
                                && !(rrtClass && rrtClass->isInterface())
                                && !dynamic_pointer_cast<CajetaFunctionType>(rrt)
                                && !dynamic_pointer_cast<CajetaView>(rrt));
                        llvm::Function* vdropFn = module->getRuntimeFunction(
                            "__cajeta_class_virtual_drop");
                        llvm::Function* fgFn = module->getRuntimeFunction(
                            "__cajeta_return_flag_get");
                        llvm::Function* fsFn = module->getRuntimeFunction(
                            "__cajeta_return_flag_set");
                        if (vdropFn && fgFn && fsFn && retIsSafeScalar) {
                            llvm::Value* savedFl = builder->CreateCall(
                                fgFn, {}, "recv_reclaim_savefl");
                            llvm::Value* ownedRecv = recvTempStatic
                                ? (llvm::Value*) builder->getInt1(true)
                                : builder->CreateICmpNE(recvTempFlag,
                                      builder->getInt64(0), "recv_owned");
                            llvm::Function* fn =
                                builder->GetInsertBlock()->getParent();
                            llvm::BasicBlock* dropBB =
                                llvm::BasicBlock::Create(llvmCtx,
                                    "recv_reclaim_drop", fn);
                            llvm::BasicBlock* contBB =
                                llvm::BasicBlock::Create(llvmCtx,
                                    "recv_reclaim_cont", fn);
                            builder->CreateCondBr(ownedRecv, dropBB, contBB);
                            builder->SetInsertPoint(dropBB);
                            builder->CreateCall(vdropFn, {thisValue});
                            builder->CreateBr(contBB);
                            builder->SetInsertPoint(contBB);
                            builder->CreateCall(fsFn, {savedFl});
                        }
                    }
                }
            }
        }

        if (!resolvedType && targetClass) {
            bool callFloating = true;
            for (auto& e : entries) {
                if (e.label.empty()) { callFloating = false; break; }
            }
            MethodPtr resolved = targetClass->resolveMethod(
                methodCallName, entries, /*isConstructor=*/false, callFloating,
                /*explicitMethodTypeArgs=*/explicitMethodTypeArgs);
            if (resolved) {
                resolvedReturnsOwnership = resolved->isReturnsOwnership();
                resolvedReturnsOwnershipKnown = true;
                resolvedMethod = resolved;
            }
            if (resolved && resolved->getReturnType()) {
                preProjectionReturnType = resolved->getReturnType();
                resolvedType = CajetaType::captureProject(
                    preProjectionReturnType);
            }
        }

        if (callResult && targetClass) {
            for (auto& mEntry : targetClass->getMethods()) {
                auto& m = mEntry.second;
                if (!m || m->getName() != methodCallName) continue;
                auto rt = m->getReturnType();
                if (auto retClass = dynamic_pointer_cast<CajetaClass>(rt)) {
                    if (retClass->isInterface()) {
                        if (llvm::Type* bodyTy = retClass->getLlvmType()) {
                            if (callResult->getType() == bodyTy) {
                                llvm::Value* bodyAlloca =
                                    builder->CreateAlloca(bodyTy);
                                builder->CreateStore(callResult, bodyAlloca);
                                return bodyAlloca;
                            }
                        }
                        break;
                    }
                }
                break;
            }
        }
        return callResult;
    }


}
