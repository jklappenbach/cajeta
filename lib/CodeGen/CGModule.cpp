#include "tinylang/CodeGen/CGModule.h"
#include "tinylang/CodeGen/CGProcedure.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/CommandLine.h"

using namespace cajeta;

static llvm::cl::opt<bool>
        Debug("g", llvm::cl::desc("Generate debug information"),
              llvm::cl::init(false));

CGModule::CGModule(ASTContext& ASTCtx, llvm::Module* M)
        : ASTCtx(ASTCtx), M(M), TBAA(CGTBAA(*this)) {
    initialize();
}

void CGModule::initialize() {
    voidType = llvm::Type::getVoidType(getLLVMCtx());
    int1Type = llvm::Type::getInt1Type(getLLVMCtx());
    int32Type = llvm::Type::getInt32Type(getLLVMCtx());
    int64Type = llvm::Type::getInt64Type(getLLVMCtx());
    int32Zero = llvm::ConstantInt::get(int32Type, 0, /*isSigned*/ true);
    if (Debug)
        DebugInfo.reset(new CGDebugInfo(*this));
}

llvm::Type* CGModule::convertType(TypeDeclaration* typeDeclaration) {
    if (llvm::Type* type = typeCache[typeDeclaration])
        return type;

    if (llvm::isa<PervasiveTypeDeclaration>(typeDeclaration)) {
        if (typeDeclaration->getName() == "INTEGER")
            return int64Type;
        if (typeDeclaration->getName() == "BOOLEAN")
            return int1Type;
    } else if (auto* aliasType =
            llvm::dyn_cast<AliasTypeDeclaration>(typeDeclaration)) {
        llvm::Type* type = convertType(aliasType->getType());
        return typeCache[typeDeclaration] = type;
    } else if (auto* arrayType =
            llvm::dyn_cast<ArrayTypeDeclaration>(typeDeclaration)) {
        llvm::Type* Component = convertType(arrayType->getType());
        Expr* Nums = arrayType->getNums();
        uint64_t numElements = 5; // TODO Eval Nums
        llvm::Type* type = llvm::ArrayType::get(component, numElements);
        return typeCache[typeDeclaration] = type;
    } else if (auto* recordType = llvm::dyn_cast<RecordTypeDeclaration>(typeDeclaration)) {
        llvm::SmallVector<llvm::Type*, 4> elements;
        for (const auto& field : RecordTy->getFields()) {
            elements.push_back(convertType(field.getType()));
        }
        llvm::Type* type = llvm::StructType::create(
                Elements, recordType->getName(), false);
        return typeCache[typeDeclaration] = type;
    }
    llvm::report_fatal_error("Unsupported type");
}

std::string CGModule::mangleName(Decl* decl) {
    std::string mangled;
    llvm::SmallString<16> tmp;
    while (decl) {
        llvm::StringRef name = decl->getName();
        tmp.clear();
        tmp.append(llvm::itostr(name.size()));
        tmp.append(name);
        mangled.insert(0, tmp.c_str());
        decl = decl->getEnclosingDecl();
    }
    mangled.insert(0, "_t");
    return mangled;
}

void CGModule::decorateInst(llvm::Instruction* Inst,
                            TypeDeclaration* Type) {
    if (auto*  = TBAA.getAccessTagInfo(Type))
        Inst->setMetadata(llvm::LLVMContext::MD_tbaa, N);
}

llvm::GlobalObject* CGModule::getGlobal(Decl* D) {
    return Globals[D];
}

void CGModule::applyLocation(llvm::Instruction* Inst,
                             llvm::SMLoc Loc) {
    if (CGDebugInfo * Dbg = getDbgInfo())
        Inst->setDebugLoc(Dbg->getDebugLoc(Loc));
}

void CGModule::run(ModuleDeclaration* Mod) {
    this->Mod = Mod;
    for (auto* Decl : Mod->getDecls()) {
        if (auto* Var =
                llvm::dyn_cast<VariableDeclaration>(Decl)) {
            // Create global variables
            llvm::GlobalVariable* V = new llvm::GlobalVariable(
                    *M, convertType(Var->getType()),
                    /*isConstant=*/false,
                    llvm::GlobalValue::PrivateLinkage, nullptr,
                    mangleName(Var));
            Globals[Var] = V;
            if (CGDebugInfo * Dbg = getDbgInfo())
                Dbg->emitGlobalVariable(Var, V);
        } else if (auto* Proc =
                llvm::dyn_cast<ProcedureDeclaration>(
                        Decl)) {
            CGProcedure CGP(*this);
            CGP.run(Proc);
        }
    }
    if (CGDebugInfo * Dbg = getDbgInfo())
        Dbg->finalize();
}
