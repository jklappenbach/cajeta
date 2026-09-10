#include "ScriptUnitSynthesis.h"

#include <cctype>
#include <filesystem>

#include "CommonTokenStream.h"

#include "CajetaModule.h"
#include "SessionState.h"
#include "../asn/expression/Expression.h"
#include "../asn/expression/BinaryOpExpression.h"
#include "../asn/expression/Identifier.h"
#include "../error/Exception.h"
#include "../field/HeapField.h"
#include "../field/StackField.h"
#include "../method/Method.h"
#include "../type/CajetaArray.h"
#include "../type/CajetaClass.h"
#include "../type/CajetaType.h"
#include "../type/Scope.h"

#include <llvm/IR/IRBuilder.h>

namespace cajeta {

    namespace {

        std::string textOf(antlr4::CommonTokenStream& tokens,
                           antlr4::ParserRuleContext* ctx) {
            return tokens.getText(ctx->getSourceInterval());
        }

        // Does this modifier list already carry an access modifier / static?
        bool hasAccessModifier(const std::vector<CajetaParser::ModifierContext*>& mods) {
            for (auto* m : mods) {
                auto t = m->getText();
                if (t == "public" || t == "private" || t == "protected") return true;
            }
            return false;
        }

        bool hasStaticModifier(const std::vector<CajetaParser::ModifierContext*>& mods) {
            for (auto* m : mods) {
                if (m->getText() == "static") return true;
            }
            return false;
        }

    }  // namespace

    bool isScriptUnit(CajetaParser::CompilationUnitContext* ctx) {
        return ctx != nullptr && !ctx->scriptMember().empty();
    }

    std::string scriptClassStem(const std::string& sourcePath) {
        std::string stem = std::filesystem::path(sourcePath).stem().string();
        std::string out;
        out.reserve(stem.size());
        for (char c : stem) {
            out.push_back((std::isalnum(static_cast<unsigned char>(c)) || c == '_')
                              ? c : '_');
        }
        if (out.empty()) out = "script";
        if (std::isdigit(static_cast<unsigned char>(out[0]))) out.insert(out.begin(), '_');
        return out;
    }

    namespace {

        // The names a top-level local declaration binds (the session bindings).
        void collectBindingNames(CajetaParser::BlockStatementContext* bs,
                                 std::vector<std::string>* out) {
            if (bs == nullptr || out == nullptr) return;
            auto* lvd = bs->localVariableDeclaration();
            if (lvd == nullptr) return;
            if (auto* vds = lvd->variableDeclarators()) {
                for (auto* vd : vds->variableDeclarator()) {
                    if (vd->variableDeclaratorId()
                            && vd->variableDeclaratorId()->identifier()) {
                        out->push_back(
                            vd->variableDeclaratorId()->identifier()->getText());
                    }
                }
            } else if (lvd->identifier()) {
                out->push_back(lvd->identifier()->getText());
            }
        }

    }  // namespace

    namespace {

        // Spliced host text (hostStart > 0) or synthetic glue; ends in '\n'.
        struct WrapperSeg {
            std::string text;
            int hostStart = 0;
        };

        int lineCountOf(const std::string& text) {
            int n = 0;
            for (char c : text) {
                if (c == '\n') ++n;
            }
            return n;
        }

    }  // namespace

    std::string synthesizeScriptUnit(antlr4::CommonTokenStream& tokens,
                                     CajetaParser::CompilationUnitContext* ctx,
                                     const std::string& stem,
                                     std::string* outCanonical,
                                     std::vector<std::string>* outBindings,
                                     ScriptLineMap* outLineMap,
                                     bool* outSyntheticTail) {
        std::string pkgName = scriptDefaultPackage();
        auto hostLineOf = [](antlr4::ParserRuleContext* c) {
            return (c && c->getStart())
                ? static_cast<int>(c->getStart()->getLine()) : 0;
        };

        std::vector<WrapperSeg> header;
        if (auto* pd = ctx->packageDeclaration()) {
            pkgName = pd->qualifiedName()->getText();
            header.push_back({textOf(tokens, pd) + "\n", hostLineOf(pd)});
        } else {
            header.push_back({"package " + pkgName + ";\n", 0});
        }
        for (auto* imp : ctx->importDeclaration()) {
            header.push_back({textOf(tokens, imp) + "\n", hostLineOf(imp)});
        }

        // Types hoist to top level, methods become static members, loose
        // statements form the entry body, all in source order.
        std::vector<WrapperSeg> hoistedTypes;
        std::vector<WrapperSeg> methods;
        std::vector<WrapperSeg> body;
        CajetaParser::BlockStatementContext* lastStatement = nullptr;
        for (auto* member : ctx->scriptMember()) {
            if (auto* td = member->typeDeclaration()) {
                hoistedTypes.push_back(
                    {textOf(tokens, td) + "\n", hostLineOf(td)});
            } else if (auto* md = member->methodDeclaration()) {
                auto mods = member->modifier();
                std::string line = "    ";
                if (!hasAccessModifier(mods)) line += "public ";
                if (!hasStaticModifier(mods)) line += "static ";
                for (auto* m : mods) {
                    line += m->getText();
                    line += " ";
                }
                // Injected modifiers ride the member's FIRST line, so the
                // segment still maps 1:1 from its host start.
                line += textOf(tokens, md);
                line += "\n";
                methods.push_back({std::move(line), hostLineOf(md)});
            } else if (auto* bs = member->blockStatement()) {
                body.push_back(
                    {"        " + textOf(tokens, bs) + "\n", hostLineOf(bs)});
                lastStatement = bs;
                collectBindingNames(bs, outBindings);
            }
        }

        // A statement appended after `return` would be unreachable.
        bool endsWithReturn = lastStatement != nullptr
            && lastStatement->getStart() != nullptr
            && lastStatement->getStart()->getType() == CajetaParser::RETURN;

        // Wrapper order, which is what keeps the line map's spans sorted.
        std::string out;
        int wrapperLine = 1;
        auto append = [&](const WrapperSeg& seg) {
            int lines = lineCountOf(seg.text);
            if (outLineMap && seg.hostStart > 0 && lines > 0) {
                outLineMap->push_back({wrapperLine, seg.hostStart, lines});
            }
            out += seg.text;
            wrapperLine += lines;
        };
        for (auto& seg : header) append(seg);
        for (auto& seg : hoistedTypes) append(seg);
        append({"public final class " + stem + " {\n", 0});
        append({"    public static int32 " + std::string(scriptEntryName())
                    + "() {\n", 0});
        for (auto& seg : body) append(seg);
        if (!endsWithReturn) append({"        return 0;\n", 0});
        append({"    }\n", 0});
        for (auto& seg : methods) append(seg);
        append({"}\n", 0});

        if (outCanonical) *outCanonical = pkgName + "." + stem;
        if (outSyntheticTail) *outSyntheticTail = !endsWithReturn;
        return out;
    }

    // --- U4: the session seam -------------------------------------------

    namespace {

        // The session when this module is a script unit compiling into one and
        // the current method is its synthesized entry; null otherwise.
        SessionState* sessionOfEntry(const CajetaModulePtr& module) {
            if (!module || !module->isScriptUnit()) return nullptr;
            SessionState* session = module->getSessionState();
            if (!session) return nullptr;
            auto method = module->getCurrentMethod();
            if (!method || method->getName() != scriptEntryName()) {
                return nullptr;
            }
            return session;
        }

        // Tag a transfer-site note with its unit, for later units' diagnostics.
        std::string decorateSite(const std::string& note,
                                 const std::string& hostName) {
            if (note.empty() || hostName.empty()) return note;
            return note + " in unit " + hostName;
        }

        // A type's generation suffix (`$g2`, ...); "" for a first declaration.
        std::string generationOf(const CajetaTypePtr& type) {
            auto klass = std::dynamic_pointer_cast<CajetaClass>(type);
            return klass ? klass->getGenerationSuffix() : std::string();
        }

        // Render a suffix as the ordinal a user reads: "" -> 1, "$g2" -> 2.
        std::string generationLabel(const std::string& suffix) {
            if (suffix.empty()) return "1";
            return suffix.substr(2);  // past "$g"
        }

    }  // namespace

    void rejectStaleGenerationUse(CajetaModulePtr module,
                                  const ExpressionPtr& expr,
                                  const std::string& position) {
        if (!module || !expr) return;
        // Cheapest gate first: only a script unit in a session can hold a stale
        // binding, so any other compile pays two null checks, not a walk.
        if (!module->isScriptUnit() || !module->getSessionState()) return;
        auto id = std::dynamic_pointer_cast<IdentifierExpression>(expr);
        if (!id) return;
        ScopePtr scope = module->getScopeStack().peek();
        if (!scope) return;
        FieldPtr field = scope->getField(id->getTextValue());
        if (!field || !field->isStaleGeneration()) return;
        const std::string was = generationLabel(field->getStaleGeneration());
        const std::string now = generationLabel(field->getCurrentGeneration());
        std::string canonical = field->getType() && field->getType()->getQName()
            ? field->getType()->getQName()->toCanonical()
            : std::string("<?>");
        throw Exception(
            "`" + id->getTextValue() + "` holds a generation-" + was + " `"
                + canonical + "`, but " + position + " expects generation-"
                + now + " — the class was redefined by a later cell. The two "
                "generations are different types with different layouts and "
                "different method bodies; using one as the other reads the "
                "old object through the new layout. Fix: rebuild the value "
                "under the current definition.",
            "CAJETA_ERROR_STALE_GENERATION");
    }

    void seedSessionScope(CajetaModulePtr module) {
        SessionState* session = sessionOfEntry(module);
        if (!session) return;
        ScopePtr scope = module->getScopeStack().peek();
        if (!scope) return;
        for (auto& fact : session->all()) {
            // Resolve by canonical, EXCEPT for a class in a shared-type-world
            // session, where the recorded type wins: the canonical now names
            // the newest generation, which is not what an older value is.
            CajetaTypePtr type = CajetaType::find(fact.typeCanonical);
            // Primitives are CajetaClass too, and excluding them explicitly is
            // what keeps the StackField choice below correct.
            if (session->hasSharedTypeWorld()
                    && fact.boundType
                    && !(fact.boundType->getTypeFlags() & PRIMITIVE_FLAG)
                    && std::dynamic_pointer_cast<CajetaClass>(fact.boundType)) {
                type = fact.boundType;
            }
            // The field KIND must match what the name holds: a HeapField slot is
            // pointer-shaped, so a primitive in one is read a pointer's width.
            FieldPtr field;
            if (type && (type->getTypeFlags() & PRIMITIVE_FLAG)) {
                field = std::make_shared<StackField>(module, fact.name, type);
            } else {
                field = std::make_shared<HeapField>(module, fact.name, type);
            }
            field->setSessionSeeded(true);
            // Was the class this name was bound under redefined since? The mark
            // rejects only positions that would reinterpret the value as the
            // newer one, and shared worlds only: suffixes must be comparable.
            if (session->hasSharedTypeWorld()
                    && std::dynamic_pointer_cast<CajetaClass>(type)) {
                const std::string current =
                    generationOf(CajetaType::find(fact.typeCanonical));
                if (current != fact.generation) {
                    field->setStaleGeneration(fact.generation, current);
                }
            }
            scope->putField(field);
            if (fact.moved) scope->demoteToBorrow(fact.name, fact.transferSite);
        }
    }

    void writeBackSessionState(CajetaModulePtr module) {
        SessionState* session = sessionOfEntry(module);
        if (!session) return;
        ScopePtr scope = module->getScopeStack().peek();
        if (!scope) return;
        // Earlier units' bindings: only a fresh move re-records the site, so a
        // still-moved one keeps the note of the unit that moved it.
        for (auto& fact : session->all()) {
            bool nowMoved = scope->isBorrow(fact.name);
            if (nowMoved && !fact.moved) {
                fact.transferSite = decorateSite(
                    scope->transferSiteOf(fact.name),
                    module->getScriptHostName());
            } else if (!nowMoved) {
                fact.transferSite.clear();
            }
            fact.moved = nowMoved;
        }
        // This unit's own top-level bindings, a redeclared seeded name included.
        for (auto& name : module->getScriptBindingNames()) {
            FieldPtr field = scope->localBinding(name);
            if (!field || field->isSessionSeeded()) continue;
            SessionBindingFact fact;
            fact.name = name;
            if (field->getType() && field->getType()->getQName()) {
                fact.typeCanonical =
                    field->getType()->getQName()->toCanonical();
            }
            fact.boundType = field->getType();
            // Read now: a later redefinition overwrites the suffix in place.
            fact.generation = generationOf(field->getType());
            fact.moved = scope->isBorrow(name);
            fact.transferSite = decorateSite(scope->transferSiteOf(name),
                                             module->getScriptHostName());
            session->put(std::move(fact));
        }
    }

    // --- U3: the unit result (Out[N]) -----------------------------------

    namespace {

        // FNV-1a 64, the vtable lookup key: these constants MUST stay those of
        // `__cajeta_vtable_lookup` and SynthesizedToStringMethod.
        int64_t scriptSignatureHash(const std::string& s) {
            uint64_t h = 0xcbf29ce484222325ULL;
            for (unsigned char c : s) {
                h ^= c;
                h *= 0x100000001b3ULL;
            }
            return (int64_t) h;
        }

        // A private constant C string in the emit module, as an i8*.
        llvm::Value* scriptLiteralPtr(llvm::IRBuilder<>* b, llvm::Module* lmod,
                                      const std::string& s) {
            auto& ctx = lmod->getContext();
            llvm::Constant* data =
                llvm::ConstantDataArray::getString(ctx, s, true);
            auto* g = new llvm::GlobalVariable(
                *lmod, data->getType(), /*isConstant=*/true,
                llvm::GlobalValue::PrivateLinkage, data, ".script.res.lit");
            g->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
            llvm::Value* zero =
                llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx), 0);
            return b->CreateInBoundsGEP(data->getType(), g, {zero, zero});
        }

        // The no-arg `toString` on `klass` or an ancestor, `this` at parameter 0
        // included. Object's own returns null and does not count as a rendering.
        MethodPtr findScriptToString(const CajetaClassPtr& klass) {
            if (!klass) return nullptr;
            if (auto qn = klass->getQName()) {
                if (qn->toCanonical() == "cajeta.lang.Object") return nullptr;
            }
            for (auto& m : klass->getMethodList()) {
                if (!m || m->isConstructor()) continue;
                if (m->getName() != "toString") continue;
                auto params = m->getParameterList();
                if (params.empty()) return m;
                if (params.size() == 1 && params.front()
                        && params.front()->getName() == "this") {
                    return m;
                }
            }
            for (auto& sup : klass->getSuperClasses()) {
                if (auto found = findScriptToString(sup)) return found;
            }
            return nullptr;
        }

    }  // namespace

    void emitScriptUnitResult(CajetaModulePtr module, const ExpressionPtr& expr,
                              llvm::Value* value) {
        if (!module || !expr || !value) return;
        if (!sessionOfEntry(module)) return;
        auto* builder = module->getBuilder();
        if (!builder) return;
        llvm::BasicBlock* insertBB = builder->GetInsertBlock();
        if (!insertBB || insertBB->hasTerminator()) return;

        // An assignment is a statement, not a result: its arms hand back
        // several shapes (r-value, staked copy, slot), not one renderable value.
        if (auto binOp = std::dynamic_pointer_cast<BinaryOpExpression>(expr)) {
            if (binOp->isAssignment()) return;
        }

        CajetaTypePtr type = expr->getResolvedType();
        if (!type) {
            expr->resolveTypes(module);
            type = expr->getResolvedType();
        }
        if (!type) return;
        // `void` carries PRIMITIVE_FLAG too, so it must go before that branch.
        if ((type->getTypeFlags() & TYPE_ID_MASK) == VOID_ID) return;

        llvm::Function* store = module->getRuntimeFunction(
            "__cajeta_script_result");
        if (!store) return;

        llvm::Module* lmod = module->emitTargetLlvmModule();
        auto& ctx = *module->getLlvmContext();
        llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
        llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
        llvm::Type* f64Ty = llvm::Type::getDoubleTy(ctx);
        llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);

        std::string canonical = type->getQName()
            ? type->getQName()->toCanonical() : std::string();
        bool isString = canonical == "cajeta.lang.String" || canonical == "String";
        auto klass = std::dynamic_pointer_cast<CajetaClass>(type);
        bool isArray = std::dynamic_pointer_cast<CajetaArray>(type) != nullptr;

        // Decide from the VALUE's shape, not loadIfLValue's TYPE test: alloca,
        // global and GEP are storage, anything else is already the value.
        llvm::Value* rv = value;
        if (value->getType()->isPointerTy()
                && (llvm::isa<llvm::AllocaInst>(value)
                    || llvm::isa<llvm::GlobalVariable>(value)
                    || llvm::isa<llvm::GetElementPtrInst>(value))) {
            rv = loadIfLValue(module, value, expr);
        }
        if (!rv) return;

        // Park the placeholder FIRST: a throwing `toString` unwinds past the
        // store below, and the host must not collect the last cell's result.
        std::string placeholder = canonical.empty() ? "<value>" : canonical;
        builder->CreateCall(store,
                            {scriptLiteralPtr(builder, lmod, placeholder)});

        llvm::Value* text = nullptr;
        if (!isString && !isArray && (type->getTypeFlags() & PRIMITIVE_FLAG)) {
            llvm::Function* i64ToStr =
                module->getRuntimeFunction("__cajeta_i64_to_str");
            llvm::Function* f64ToStr =
                module->getRuntimeFunction("__cajeta_f64_to_str");
            llvm::Function* boolToStr =
                module->getRuntimeFunction("__cajeta_bool_to_str");
            switch (type->getTypeFlags() & TYPE_ID_MASK) {
                case BOOLEAN_ID:
                    if (boolToStr) {
                        text = builder->CreateCall(
                            boolToStr, {builder->CreateZExt(rv, i32Ty)});
                    }
                    break;
                case INT8_ID: case INT16_ID: case INT32_ID:
                    if (i64ToStr) {
                        text = builder->CreateCall(
                            i64ToStr, {builder->CreateSExt(rv, i64Ty)});
                    }
                    break;
                case UINT8_ID: case UINT16_ID: case UINT32_ID: case UINT64_ID:
                    if (i64ToStr) {
                        text = builder->CreateCall(
                            i64ToStr, {builder->CreateZExt(rv, i64Ty)});
                    }
                    break;
                case INT64_ID:
                    if (i64ToStr) text = builder->CreateCall(i64ToStr, {rv});
                    break;
                case FLOAT32_ID:
                    if (f64ToStr) {
                        text = builder->CreateCall(
                            f64ToStr, {builder->CreateFPExt(rv, f64Ty)});
                    }
                    break;
                case FLOAT64_ID:
                    if (f64ToStr) text = builder->CreateCall(f64ToStr, {rv});
                    break;
                default:
                    break;  // 128-bit, extended float, bare pointer: degrade
            }
        } else if (isString) {
            // A String's bytes are behind the accessor, not at the pointer.
            if (llvm::Function* cstr =
                    module->getRuntimeFunction("__cajeta_string_cstr")) {
                text = builder->CreateCall(cstr, {rv});
            }
        } else if (klass && !klass->isInterface()) {
            MethodPtr ts = findScriptToString(klass);
            llvm::Function* vtLookup =
                module->getRuntimeFunction("__cajeta_vtable_lookup");
            llvm::Function* cstr =
                module->getRuntimeFunction("__cajeta_string_cstr");
            if (ts && vtLookup && cstr) {
                // Virtual dispatch, as @ToString emits it; null renders "null".
                llvm::Function* curFn = insertBB->getParent();
                auto* nullBB = llvm::BasicBlock::Create(ctx, "res.null", curFn);
                auto* callBB = llvm::BasicBlock::Create(ctx, "res.call", curFn);
                auto* mergeBB = llvm::BasicBlock::Create(ctx, "res.mrg", curFn);
                llvm::Value* isNull = builder->CreateICmpEQ(
                    rv, llvm::ConstantPointerNull::get(
                            llvm::cast<llvm::PointerType>(ptrTy)));
                builder->CreateCondBr(isNull, nullBB, callBB);

                builder->SetInsertPoint(nullBB);
                llvm::Value* nullLit = scriptLiteralPtr(builder, lmod, "null");
                builder->CreateBr(mergeBB);

                builder->SetInsertPoint(callBB);
                auto* callTy = llvm::FunctionType::get(ptrTy, {ptrTy}, false);
                llvm::Value* vtPtr = builder->CreateLoad(ptrTy, rv);
                llvm::Value* fnPtr = builder->CreateCall(
                    vtLookup, {vtPtr,
                               llvm::ConstantInt::get(
                                   i64Ty, llvm::APInt(64,
                                       (uint64_t) scriptSignatureHash(
                                           ts->toCanonical(false)), false))});
                llvm::Value* str = builder->CreateCall(callTy, fnPtr, {rv});
                llvm::Value* strC = builder->CreateCall(cstr, {str});
                builder->CreateBr(mergeBB);

                builder->SetInsertPoint(mergeBB);
                llvm::PHINode* phi = builder->CreatePHI(ptrTy, 2);
                phi->addIncoming(nullLit, nullBB);
                phi->addIncoming(strC, callBB);
                text = phi;
            }
        }

        // Nothing rendered it; the placeholder stands.
        if (text) builder->CreateCall(store, {text});
    }

    void remapScriptException(CajetaModulePtr module, Exception& e) {
        if (!module || !module->isScriptUnit()) return;
        if (e.isScriptRemapped()) return;
        e.markScriptRemapped();
        std::string file = module->scriptDiagFile();
        if (e.hasLocation()) {
            e.setLocation(file, module->mapScriptLine(e.getLine()),
                          e.getColumn());
        } else {
            e.setLocation(file, module->getScriptCurrentHostLine(), 0);
        }
    }

    void maybeEmitSessionDisarm(CajetaModulePtr module,
                                const std::string& name) {
        if (!module || !module->isScriptUnit()) return;
        if (!module->isScriptBindingName(name)) return;
        auto* builder = module->getBuilder();
        if (!builder || !builder->GetInsertBlock()) return;
        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
        if (!parentFn
            || parentFn->getName().find(scriptEntryName())
                   == llvm::StringRef::npos) {
            return;
        }
        llvm::Function* disarmFn =
            module->getRuntimeFunction("__cajeta_session_disarm");
        if (!disarmFn) return;
        builder->CreateCall(disarmFn, {builder->CreateGlobalString(name)});
    }

}  // namespace cajeta
