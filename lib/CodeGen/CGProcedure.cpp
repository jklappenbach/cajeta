#include "tinylang/CodeGen/CGProcedure.h"
#include "llvm/IR/CFG.h"
#include "llvm/Support/Casting.h"

using namespace cajeta;

void CGProcedure::writeLocalVariable(llvm::BasicBlock* basicBlock,
                                     Decl* decl,
                                     llvm::Value* value) {
    assert(basicBlock && "Basic block is nullptr");
    assert((llvm::isa<VariableDeclaration>(decl) ||
             llvm::isa<FormalParameterDeclaration>(decl)) &&
            "Declaration must be variable or formal parameter");
    assert(value && "Value is nullptr");
    CurrentDef[basicBlock].Defs[decl] = value;
}

llvm::Value*
CGProcedure::readLocalVariable(llvm::BasicBlock* basicBlock,
                               Decl* decl) {
    assert(basicBlock && "Basic block is nullptr");
    assert((llvm::isa<VariableDeclaration>(Decl) || llvm::isa<FormalParameterDeclaration>(Decl)) &&
            "Declaration must be variable or formal parameter");
    auto value = CurrentDef[basicBlock].Defs.find(Decl);
    if (value != CurrentDef[basicBlock].Defs.end())
        return value->second;
    return readLocalVariableRecursive(basicBlock, decl);
}

llvm::Value* CGProcedure::readLocalVariableRecursive(llvm::BasicBlock* basicBlock, Decl* decl) {
    llvm::Value* value = nullptr;
    if (!currentDef[basicBlock].sealed) {
        // Add incomplete phi for variable.
        llvm::PHINode* phi = addEmptyPhi(basicBlock, decl);
        currentDef[basicBlock].incompletePhis[phi] = decl;
        value = phi;
    } else if (auto* predBB = basicBlock->getSinglePredecessor()) {
        // Only one predecessor.
        value = readLocalVariable(predBB, decl);
    } else {
        // Create empty phi instruction to break potential
        // cycles.
        llvm::PHINode* phi = addEmptyPhi(basicBlock, decl);
        writeLocalVariable(basicBlock, decl, phi);
        value = addPhiOperands(basicBlock, decl, phi);
    }
    writeLocalVariable(basicBlock, decl, value);
    return value;
}

llvm::PHINode* CGProcedure::addEmptyPhi(llvm::BasicBlock* basicBlock, Decl* decl) {
    return basicBlock->empty() ? llvm::PHINode::Create(mapType(decl), 0, "", basicBlock)
           : llvm::PHINode::Create(mapType(decl), 0, "", &basicBlock->front());
}

llvm::Value* CGProcedure::addPhiOperands(llvm::BasicBlock* basicBlock, Decl* decl, llvm::PHINode* phi) {
    for (auto itr = llvm::pred_begin(basicBlock), end = llvm::pred_end(basicBlock); itr != end; ++itr) {
        Phi->addIncoming(readLocalVariable(*itr, decl), *itr);
    }
    return optimizePhi(phi);
}

llvm::Value* CGProcedure::optimizePhi(llvm::PHINode* phi) {
    llvm::Value* same = nullptr;
    for (llvm::Value* value : Phi->incoming_values()) {
        if (value == same || value == phi)
            continue;
        if (same && value != same)
            return phi;
        same = value;
    }
    if (same == nullptr)
        same = llvm::UndefValue::get(phi->getType());
    // Collect phi instructions using this one.
    llvm::SmallVector<llvm::PHINode*, 8> candidatePhis;
    for (llvm::Use& use : phi->uses()) {
        if (auto* candidate = llvm::dyn_cast<llvm::PHINode>(use.getUser()))
            if (candidate != phi)
                candidatePhis.push_back(candidate);
    }
    phi->replaceAllUsesWith(Same);
    phi->eraseFromParent();
    for (auto* candidate : candidatePhis)
        optimizePhi(candidate);
    return Same;
}

void CGProcedure::sealBlock(llvm::BasicBlock* basicBlock) {
    assert(!CurrentDef[basicBlock].Sealed &&
           "Attempt to seal already sealed block");
    for (auto PhiDecl : CurrentDef[basicBlock].IncompletePhis) {
        addPhiOperands(basicBlock, PhiDecl.second, PhiDecl.first);
    }
    CurrentDef[basicBlock].IncompletePhis.clear();
    CurrentDef[basicBlock].Sealed = true;
}

void CGProcedure::writeVariable(llvm::BasicBlock* basicBlock,
                                Decl* D, llvm::Value* Val) {
    if (auto* V = llvm::dyn_cast<VariableDeclaration>(D)) {
        if (V->getEnclosingDecl() == Proc)
            writeLocalVariable(basicBlock, D, Val);
        else if (V->getEnclosingDecl() ==
                 CGM.getModuleDeclaration()) {
            Builder.CreateStore(Val, CGM.getGlobal(D));
        } else
            llvm::report_fatal_error(
                    "Nested procedures not yet supported");
    } else if (auto* FP =
            llvm::dyn_cast<FormalParameterDeclaration>(
                    D)) {
        if (FP->isVar()) {
            Builder.CreateStore(Val, FormalParams[FP]);
        } else
            writeLocalVariable(basicBlock, D, Val);
    } else
        llvm::report_fatal_error("Unsupported declaration");
}

llvm::Value* CGProcedure::readVariable(llvm::BasicBlock* basicBlock,
                                       Decl* decl,
                                       bool loadVal) {
    if (auto* variableDecl = llvm::dyn_cast<VariableDeclaration>(decl)) {
        if (variableDecl->getEnclosingDecl() == proc)
            return readLocalVariable(basicBlock, decl);
        else if (variableDecl->getEnclosingDecl() == cgModule.getModuleDeclaration()) {
            auto* global = cgModule.getGlobal(decl);
            if (!loadVal) {
                return global;
            }
            return builder.CreateLoad(mapType(decl), Global);
        } else {
            llvm::report_fatal_error("Nested procedures not yet supported");
        }
    } else if (auto* formalParameter = llvm::dyn_cast<FormalParameterDeclaration>(decl)) {
        if (formalParameter->isVar()) {
            if (!loadVal) {
                return FormalParams[FP];
            }
            return Builder.CreateLoad(
                    mapType(formalParameter)->getPointerElementType(),
                    FormalParams[formalParameter]);
        } else {
            return readLocalVariable(basicBlock, decl);
        }
    } else
        llvm::report_fatal_error("Unsupported declaration");
}

llvm::Type* CGProcedure::mapType(Decl* decl) {
    if (auto* formalParameter = llvm::dyn_cast<FormalParameterDeclaration>(decl)) {
        llvm::Type* type = cgModule.convertType(formalParameter->getType());
        if (formalParameter->isVar()) {
            type = type->getPointerTo();
        }
        return type;
    }
    if (auto* variableDecl = llvm::dyn_cast<VariableDeclaration>(decl))
        return cgModule.convertType(variableDecl->getType());
    return cgModule.convertType(llvm::cast<TypeDeclaration>(decl));
}

llvm::FunctionType* CGProcedure::createFunctionType(ProcedureDeclaration* proc) {
    llvm::Type* resultType = cgModule.voidType;
    if (proc->getRetType()) {
        resultType = mapType(proc->getRetType());
    }
    auto formalParams = proc->getFormalParams();
    llvm::SmallVector<llvm::Type*, 8> paramTypes;
    for (auto formalParam : formalParams) {
        llvm::Type* type = mapType(formalParam);
        paramTypes.push_back(type);
    }
    return llvm::FunctionType::get(resultType, paramTypes, false);
}

llvm::Function*
CGProcedure::createFunction(ProcedureDeclaration* Proc,
                            llvm::FunctionType* FTy) {
    llvm::Function* Fn = llvm::Function::Create(
            Fty, llvm::GlobalValue::ExternalLinkage,
            CGM.mangleName(Proc), CGM.getModule());
    // Give parameters a name.
    size_t Idx = 0;
    for (auto I = Fn->arg_begin(), E = Fn->arg_end(); I != E;
         ++I, ++Idx) {
        llvm::Argument* Arg = I;
        FormalParameterDeclaration* FP =
                Proc->getFormalParams()[Idx];
        if (FP->isVar()) {
            llvm::AttrBuilder Attr;
            llvm::TypeSize Sz =
                    CGM.getModule()->getDataLayout().getTypeStoreSize(
                            CGM.convertType(FP->getType()));
            Attr.addDereferenceableAttr(Sz);
            Attr.addAttribute(llvm::Attribute::NoCapture);
            Arg->addAttrs(Attr);
        }
        Arg->setName(FP->getName());
    }
    return Fn;
}

llvm::Value*
CGProcedure::emitInfixExpr(InfixExpression* E) {
    llvm::Value* Left = emitExpr(E->getLeft());
    llvm::Value* Right = emitExpr(E->getRight());
    llvm::Value* Result = nullptr;
    switch (E->getOperatorInfo().getKind()) {
        case tok::plus:
            Result = Builder.CreateNSWAdd(Left, Right);
            break;
        case tok::minus:
            Result = Builder.CreateNSWSub(Left, Right);
            break;
        case tok::star:
            Result = Builder.CreateNSWMul(Left, Right);
            break;
        case tok::kw_DIV:
            Result = Builder.CreateSDiv(Left, Right);
            break;
        case tok::kw_MOD:
            Result = Builder.CreateSRem(Left, Right);
            break;
        case tok::equal:
            Result = Builder.CreateICmpEQ(Left, Right);
            break;
        case tok::hash:
            Result = Builder.CreateICmpNE(Left, Right);
            break;
        case tok::less:
            Result = Builder.CreateICmpSLT(Left, Right);
            break;
        case tok::lessequal:
            Result = Builder.CreateICmpSLE(Left, Right);
            break;
        case tok::greater:
            Result = Builder.CreateICmpSGT(Left, Right);
            break;
        case tok::greaterequal:
            Result = Builder.CreateICmpSGE(Left, Right);
            break;
        case tok::kw_AND:
            Result = Builder.CreateAnd(Left, Right);
            break;
        case tok::kw_OR:
            Result = Builder.CreateOr(Left, Right);
            break;
        case tok::slash:
            // Divide by real numbers not supported.
            LLVM_FALLTHROUGH;
        default:
            llvm_unreachable("Wrong operator");
    }
    return Result;
}

llvm::Value*
CGProcedure::emitPrefixExpr(PrefixExpression* E) {
    llvm::Value* Result = emitExpr(E->getExpr());
    switch (E->getOperatorInfo().getKind()) {
        case tok::plus:
            // Identity - nothing to do.
            break;
        case tok::minus:
            Result = Builder.CreateNeg(Result);
            break;
        case tok::kw_NOT:
            Result = Builder.CreateNot(Result);
            break;
        default:
            llvm_unreachable("Wrong operator");
    }
    return Result;
}

llvm::Value* CGProcedure::emitExpr(Expr* E) {
    if (auto* Infix = llvm::dyn_cast<InfixExpression>(E)) {
        return emitInfixExpr(Infix);
    } else if (auto* Prefix =
            llvm::dyn_cast<PrefixExpression>(E)) {
        return emitPrefixExpr(Prefix);
    } else if (auto* Var = llvm::dyn_cast<Designator>(E)) {
        auto* Decl = Var->getDecl();
        llvm::Value* Val = readVariable(Curr, Decl);
        // With more languages features in place, here you
        // need to add array and record support.
        auto& Selectors = Var->getSelectors();
        for (auto I = Selectors.begin(), E = Selectors.end();
             I != E;
            /* no increment */) {
            if (auto* IdxSel =
                    llvm::dyn_cast<IndexSelector>(*I)) {
                llvm::SmallVector<llvm::Value*, 4> IdxList;
                while (I != E) {
                    if (auto* Sel =
                            llvm::dyn_cast<IndexSelector>(*I)) {
                        IdxList.push_back(emitExpr(Sel->getIndex()));
                        ++I;
                    } else
                        break;
                }
                Val = Builder.CreateInBoundsGEP(Val, IdxList);
                Val = Builder.CreateLoad(
                        Val->getType()->getPointerElementType(), Val);
            } else if (auto* FieldSel =
                    llvm::dyn_cast<FieldSelector>(*I)) {
                llvm::SmallVector<llvm::Value*, 4> IdxList;
                while (I != E) {
                    if (auto* Sel =
                            llvm::dyn_cast<FieldSelector>(*I)) {
                        llvm::Value* V = llvm::ConstantInt::get(
                                CGM.Int64Ty, Sel->getIndex());
                        IdxList.push_back(V);
                        ++I;
                    } else
                        break;
                }
                Val = Builder.CreateInBoundsGEP(Val, IdxList);
                Val = Builder.CreateLoad(
                        Val->getType()->getPointerElementType(), Val);
            } else if (auto* DerefSel =
                    llvm::dyn_cast<DereferenceSelector>(
                            *I)) {
                Val = Builder.CreateLoad(
                        Val->getType()->getPointerElementType(), Val);
                ++I;
            } else {
                llvm::report_fatal_error("Unsupported selector");
            }
        }

        return Val;
    } else if (auto* Const =
            llvm::dyn_cast<ConstantAccess>(E)) {
        return emitExpr(Const->getDecl()->getExpr());
    } else if (auto* IntLit =
            llvm::dyn_cast<IntegerLiteral>(E)) {
        return llvm::ConstantInt::get(CGM.Int64Ty,
                                      IntLit->getValue());
    } else if (auto* BoolLit =
            llvm::dyn_cast<BooleanLiteral>(E)) {
        return llvm::ConstantInt::get(CGM.Int1Ty,
                                      BoolLit->getValue());
    }
    llvm::report_fatal_error("Unsupported expression");
}

void CGProcedure::emitStmt(AssignmentStatement* Stmt) {
    auto* Val = emitExpr(Stmt->getExpr());
    Designator* Desig = Stmt->getVar();
    auto& Selectors = Desig->getSelectors();
    if (Selectors.empty())
        writeVariable(Curr, Desig->getDecl(), Val);
    else {
        llvm::SmallVector<llvm::Value*, 4> IdxList;
        // First index for GEP.
        IdxList.push_back(
                llvm::ConstantInt::get(CGM.Int32Ty, 0));
        auto* Base =
                readVariable(Curr, Desig->getDecl(), false);
        for (auto I = Selectors.begin(), E = Selectors.end();
             I != E; ++I) {
            if (auto* IdxSel =
                    llvm::dyn_cast<IndexSelector>(*I)) {
                IdxList.push_back(emitExpr(IdxSel->getIndex()));
            } else if (auto* FieldSel =
                    llvm::dyn_cast<FieldSelector>(*I)) {
                llvm::Value* V = llvm::ConstantInt::get(
                        CGM.Int32Ty, FieldSel->getIndex());
                IdxList.push_back(V);
            } else {
                llvm::report_fatal_error("not implemented");
            }
        }
        if (!IdxList.empty()) {
            if (Base->getType()->isPointerTy()) {
                Base = Builder.CreateInBoundsGEP(Base, IdxList);
                Builder.CreateStore(Val, Base);
            } else {
                llvm::report_fatal_error("not implemented");
                //Builder.CreateInsertValue(Base, Val, IdxList);
            }
        }
    }
}

void CGProcedure::emitStmt(ProcedureCallStatement* Stmt) {
    llvm::report_fatal_error("not implemented");
}

void CGProcedure::emitStmt(IfStatement* Stmt) {
    bool HasElse = Stmt->getElseStmts().size() > 0;

    // Create the required basic blocks.
    llvm::BasicBlock* IfbasicBlock = createBasicBlock("if.body");
    llvm::BasicBlock* ElsebasicBlock =
            HasElse ? createBasicBlock("else.body") : nullptr;
    llvm::BasicBlock* AfterIfbasicBlock =
            createBasicBlock("after.if");

    llvm::Value* Cond = emitExpr(Stmt->getCond());
    Builder.CreateCondBr(Cond, IfbasicBlock,
                         HasElse ? ElsebasicBlock : AfterIfbasicBlock);
    sealBlock(Curr);

    setCurr(IfbasicBlock);
    emit(Stmt->getIfStmts());
    if (!Curr->getTerminator()) {
        Builder.CreateBr(AfterIfbasicBlock);
    }
    sealBlock(Curr);

    if (HasElse) {
        setCurr(ElsebasicBlock);
        emit(Stmt->getElseStmts());
        if (!Curr->getTerminator()) {
            Builder.CreateBr(AfterIfbasicBlock);
        }
        sealBlock(Curr);
    }
    setCurr(AfterIfbasicBlock);
}

void CGProcedure::emitStmt(WhileStatement* Stmt) {
    // The basic block for the condition.
    llvm::BasicBlock* WhileCondbasicBlock;
    // The basic block for the while body.
    llvm::BasicBlock* WhileBodybasicBlock =
            createBasicBlock("while.body");
    // The basic block after the while statement.
    llvm::BasicBlock* AfterWhilebasicBlock =
            createBasicBlock("after.while");

    if (Curr->empty()) {
        Curr->setName("while.cond");
        WhileCondbasicBlock = Curr;
    } else {
        WhileCondbasicBlock = createBasicBlock("while.cond");
        Builder.CreateBr(WhileCondbasicBlock);
        sealBlock(Curr);
        setCurr(WhileCondbasicBlock);
    }

    llvm::Value* Cond = emitExpr(Stmt->getCond());
    Builder.CreateCondBr(Cond, WhileBodybasicBlock, AfterWhilebasicBlock);

    setCurr(WhileBodybasicBlock);
    emit(Stmt->getWhileStmts());
    Builder.CreateBr(WhileCondbasicBlock);
    sealBlock(WhileCondbasicBlock);
    sealBlock(Curr);

    setCurr(AfterWhilebasicBlock);
}

void CGProcedure::emitStmt(ReturnStatement* Stmt) {
    if (Stmt->getRetVal()) {
        llvm::Value* RetVal = emitExpr(Stmt->getRetVal());
        Builder.CreateRet(RetVal);
    } else {
        Builder.CreateRetVoid();
    }
}

void CGProcedure::emit(const StmtList& Stmts) {
    for (auto* S : Stmts) {
        if (auto* Stmt = llvm::dyn_cast<AssignmentStatement>(S))
            emitStmt(Stmt);
        else if (auto* Stmt =
                llvm::dyn_cast<ProcedureCallStatement>(S))
            emitStmt(Stmt);
        else if (auto* Stmt = llvm::dyn_cast<IfStatement>(S))
            emitStmt(Stmt);
        else if (auto* Stmt = llvm::dyn_cast<WhileStatement>(S))
            emitStmt(Stmt);
        else if (auto* Stmt =
                llvm::dyn_cast<ReturnStatement>(S))
            emitStmt(Stmt);
        else
            llvm_unreachable("Unknown statement");
    }
}

void CGProcedure::run(ProcedureDeclaration* Proc) {
    this->Proc = Proc;
    Fty = createFunctionType(Proc);
    Fn = createFunction(Proc, Fty);
    if (CGDebugInfo * Dbg = CGM.getDbgInfo())
        Dbg->emitProcedure(Proc, Fn);

    llvm::BasicBlock* basicBlock = createBasicBlock("entry");
    setCurr(basicBlock);

    size_t Idx = 0;
    auto& Defs = CurrentDef[basicBlock];
    for (auto I = Fn->arg_begin(), E = Fn->arg_end(); I != E;
         ++I, ++Idx) {
        llvm::Argument* Arg = I;
        FormalParameterDeclaration* FP =
                Proc->getFormalParams()[Idx];
        // Create mapping FormalParameter -> llvm::Argument
        // for VAR parameters.
        FormalParams[FP] = Arg;
        writeLocalVariable(Curr, FP, Arg);
        if (CGDebugInfo * Dbg = CGM.getDbgInfo())
            DIVariables[FP] =
                    Dbg->emitParameterVariable(FP, Idx + 1, Arg, basicBlock);
    }

    for (auto* D : Proc->getDecls()) {
        if (auto* Var =
                llvm::dyn_cast<VariableDeclaration>(D)) {
            llvm::Type* Ty = mapType(Var);
            if (Ty->isAggregateType()) {
                llvm::Value* Val = Builder.CreateAlloca(Ty);
                writeLocalVariable(Curr, Var, Val);
            }
        }
    }

    auto Block = Proc->getStmts();
    emit(Proc->getStmts());
    if (!Curr->getTerminator()) {
        Builder.CreateRetVoid();
    }
    sealBlock(Curr);
    if (CGDebugInfo * Dbg = CGM.getDbgInfo())
        Dbg->emitProcedureEnd(Proc, Fn);
}

void CGProcedure::run() {}
