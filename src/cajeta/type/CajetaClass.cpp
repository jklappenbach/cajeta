//
// Created by James Klappenbach on 10/24/22.
//

#include "CajetaClass.h"
#include "CajetaView.h"
#include "CajetaStruct.h"
#include "StructureMetadata.h"
#include "../field/Field.h"
#include "../method/Method.h"
#include "../asn/ClassBodyDeclaration.h"
#include "../method/DefaultConstructorMethod.h"
#include "../method/SynthesizedHashMethod.h"
#include "CajetaArray.h"
#include "../field/HeapField.h"
#include "../error/Exception.h"
#include "../asn/expression/LiteralExpression.h"
#include "../asn/VariableDeclarator.h"
#include <cstdlib>

#include <algorithm>
#include <functional>
#include <cstdint>

namespace {
    // FNV-1a 64-bit — must match the runtime's __cajeta_signature_hash
    // exactly so compile-time and runtime hashes of the same canonical
    // signature agree.
    int64_t signatureHash(const std::string& s) {
        uint64_t h = 0xcbf29ce484222325ULL;
        for (unsigned char c : s) {
            h ^= c;
            h *= 0x100000001b3ULL;
        }
        return (int64_t) h;
    }
}

using namespace std;

namespace cajeta {
    CajetaClass::CajetaClass(CajetaModulePtr module, QualifiedNamePtr qName, list<QualifiedNamePtr> qImplemented) : CajetaType(qName) {
        this->qImplemented = qImplemented;
        this->module = module;
    }
    CajetaClass::CajetaClass(CajetaModulePtr module, QualifiedNamePtr qName, list<QualifiedNamePtr> qExtended, list<QualifiedNamePtr> qImplemented)
            : CajetaType(qName) {
        this->qExtended = qExtended;
        this->qImplemented = qImplemented;
        this->module = module;
    }

    llvm::Type* CajetaClass::getLlvmType() {
        if (llvmType) return llvmType;
        if (placeholderFlag && module) {
            return llvm::PointerType::get(*module->getLlvmContext(), 0);
        }
        return llvmType;
    }

    llvm::Type* CajetaClass::getLlvmReferenceType() {
        if (llvmReferenceType == nullptr) {
            vector<llvm::Type*> types;
            types.push_back(llvm::Type::getInt1Ty(*module->getLlvmContext()));
            types.push_back(llvm::PointerType::get(*module->getLlvmContext(), 0));
            llvmReferenceType = llvm::StructType::create(*module->getLlvmContext(), llvm::ArrayRef<llvm::Type*>(types));
        }
        return llvmReferenceType;
    }

    bool CajetaClass::isParentOrKind(CajetaClassPtr source) {
        if (source->getQName()->operator==(qName)) {
            return true;
        }
        for (auto& parent : superClasses) {
            if (parent->isParentOrKind(source)) {
                return true;
            }
        }
        return false;
    }

    int getMethodCount(map<string, map<string, MethodPtr>>& map) {
        int count = 0;
        for (auto& entry : map) {
            count += entry.second.size();
        }
        return count;
    }

    void mapMethod(MethodPtr method, map<string, map<string, MethodPtr>>& map, bool labeled) {
        string generic = method->toGeneric(labeled);
        string canonical = method->toCanonical(labeled);

        auto itrGeneric = map.find(generic);
        if (itrGeneric != map.end()) {
            auto itrExact = (*itrGeneric).second.find(canonical);
            if (itrExact != (*itrGeneric).second.end()) {
                method->setVirtualTableIndex((*itrExact).second->getVirtualTableIndex());
            } else {
                int id = getMethodCount(map);
                method->setVirtualTableIndex(id);
            }
            map[generic][canonical] = method;
        } else {
            int id = getMethodCount(map);
            map[generic][canonical] = method;
            method->setVirtualTableIndex(id);
        }
    }

    void CajetaClass::addMethod(MethodPtr method) {
        methods[method->toCanonical()] = method;

        if (method->isConstructor()) {
            map<string, MethodPtr> canonical = unlabeledConstructorMap[method->toGeneric(false)];
            if (canonical.find(method->toCanonical(false)) != canonical.end()) {
                throw "Constructor already exists";
            }
            mapMethod(method, labeledConstructorMap, true);
            mapMethod(method, unlabeledConstructorMap, false);
        } else {
            if (method->isStatic()) {
                map<string, MethodPtr> canonical = unlabeledMethodMap[method->toGeneric(false)];
                if (canonical.find(method->toCanonical(false)) != canonical.end()) {
                    throw "A static method with this signature already exists.  Static methods can not be overridden.";
                }
                staticMethods[method->toCanonical()] = method;
            }
            methodList.push_back(method);
            methods[method->toCanonical()] = method;
            mapMethod(method, labeledMethodMap, true);
            mapMethod(method, unlabeledMethodMap, false);
        }
    }

    void CajetaClass::addMethods(list<MethodPtr> methods) {
        for (MethodPtr method: methods) {
            addMethod(method);
        }
    }

    void CajetaClass::addProperty(StructurePropertyPtr field) {
        properties[field->getName()] = field;
        propertyList.push_back(field);
    }

    void CajetaClass::generatePrototype() {
        // Idempotent — the deferred-prototype machinery
        // (CajetaModule::buildPendingPrototypes) may attempt this multiple
        // times as parents fill in. The flag is set at the end so a
        // partial / aborted run doesn't claim done.
        if (prototypeBuilt) return;

        // Templates aren't types — `Box` alone has no layout, no methods to
        // lower, no vtable to build. Defer the structural work until a
        // concrete `Box<int32>` is referenced and `instantiate(...)` runs.
        //
        // We DO register the template in canonicalMap so type-use sites can
        // find it by name and route through `instantiate`. We deliberately
        // do NOT add it to module->getStructures() — that's the codegen
        // worklist, and the template's body methods reference unresolved
        // `T` placeholders which can't be lowered. Concrete instantiations
        // (e.g. Box<int32>) are added to structures and codegen normally.
        if (isTemplate()) {
            canonicalMap[qName->toCanonical()] = static_pointer_cast<CajetaType>(shared_from_this());
            canonicalMap[qName->getTypeName()] = static_pointer_cast<CajetaType>(shared_from_this());
            return;
        }
        string canonical = qName->toCanonical();

        // Interfaces participate as types (registered in canonicalMap so
        // variables can be declared at the interface type, and methods can
        // be looked up by signature) but have no instance layout, no vtable
        // of their own, and no default constructor. Their methods are
        // abstract — Method::generatePrototype skips LLVM-function creation
        // for them. Implementing classes' vtables carry concrete entries
        // keyed under the interface methods' canonical hashes (see
        // CajetaClass::buildVirtualTable).
        if (isInterface()) {
            // Interface fat pointer (S9.5.1): { ptr data, ptr vtable, i64 kind }
            // = 24 bytes. data points at the underlying class instance or
            // struct body; vtable points at the per-(impl, iface) global
            // synthesized by S9.2 / S9.5.2; kind is one of
            // IFACE_KIND_BORROWED_CLASS / OWNED_CLASS / BORROWED_STRUCT
            // and drives drop-chain dispatch at scope exit (S10.4).
            llvmType = CajetaType::getOrCreateLlvmType(module->getLlvmContext(), canonical);
            typeMap[TypeKey(llvmType)] = shared_from_this();
            llvm::Type* ptrTy = llvm::PointerType::get(*module->getLlvmContext(), 0);
            llvm::Type* i64Ty = llvm::Type::getInt64Ty(*module->getLlvmContext());
            vector<llvm::Type*> members{ ptrTy, ptrTy, i64Ty };
            ((llvm::StructType*) llvmType)->setBody(llvm::ArrayRef<llvm::Type*>(members), false);

            canonicalMap[canonical] = static_pointer_cast<CajetaType>(shared_from_this());
            canonicalMap[qName->getTypeName()] = static_pointer_cast<CajetaType>(shared_from_this());
            typeFlags = STRUCT_FLAG | USER_DEFINED_FLAG;
            module->getStructures()[canonical] = static_pointer_cast<CajetaClass>(shared_from_this());
            for (auto& m : methods) {
                m.second->generatePrototype();
            }
            CajetaModule::getStructureToModule()[canonical] = module;
            prototypeBuilt = true;
            return;
        }

        llvmType = CajetaType::getOrCreateLlvmType(module->getLlvmContext(), canonical);
        typeMap[TypeKey(llvmType)] = shared_from_this();
        // Overwrite the plain-CajetaType placeholder `getOrCreateLlvmType` put
        // in the canonical map so name lookups (e.g. `dynamic_pointer_cast<
        // CajetaClass>(receiverType)` in MethodCallExpression) actually see
        // this class. Also register the short typeName so unqualified
        // references like `new Counter()` find the right entry.
        canonicalMap[canonical] = static_pointer_cast<CajetaType>(shared_from_this());
        canonicalMap[qName->getTypeName()] = static_pointer_cast<CajetaType>(shared_from_this());
        typeFlags = STRUCT_FLAG | USER_DEFINED_FLAG;
        module->getScopeStack().add(make_shared<Scope>(toCanonical(), module));

        // Register self in the module's structure map BEFORE method/vtable
        // generation so any later-declared subclass that lists us in its
        // `extends` clause can find us by name. Also: resolve our own
        // parents now — they must have been registered by their own
        // prototype generation, which means declared earlier in the source.
        module->getStructures()[canonical] = static_pointer_cast<CajetaClass>(shared_from_this());
        resolveSuperClasses();
        resolveImplementedInterfaces();

        // Class instance layout: { ptr vtable, <inherited fields…>, <own
        // fields…> }. The vtable pointer at LLVM index 0 is set by `new
        // ClassName()` (see ClassCreatorRest). Inherited fields land
        // immediately after the vtable at the SAME indices they occupy
        // in their declaring parent's struct — so a GEP for an inherited
        // field works on a subclass instance the same way it works on
        // the parent (single-inheritance C++-style layout). Subclass own
        // fields come after the inherited block; getFieldLlvmIndex's
        // `countInheritedFields() + order + 1` formula accounts for the
        // shift.
        // Pick the LLVM type for a property when laying it out inside
        // the enclosing class struct. Reference-typed fields — arrays
        // AND plain class refs — are stored as **pointers** to the
        // heap-allocated body, not inline.
        //
        // Why: embedding the body inline (a) leaves nowhere for the
        // heap-returned pointer to land (the slot's bit-pattern would
        // be the inline body, not a reference), and (b) for template
        // instantiations whose body is a named/anonymous struct (e.g.
        // `Stream<int32>` lowers to a `{ ptr vtable }` `%union.anon`),
        // any call site passing the loaded field value to a `ptr`-typed
        // parameter trips the LLVM verifier ("Call parameter type does
        // not match function signature"). The methods-only / single-
        // word case happens to round-trip under opaque pointers because
        // the slot's bit-pattern is one pointer wide; classes with
        // additional fields or distinct named types would crash
        // unpredictably. Uniform `ptr` storage matches the Method
        // signature convention (class params/returns are `ptr` — see
        // Method.cpp:451) and the existing GEP+load code path that
        // already loads class-ref fields through as `ptr`.
        //
        // CajetaView (zero-copy struct) and CajetaInterface (24-byte
        // fat pointer) keep inline storage: views are by-design typed
        // overlays on a wire-format buffer, and interface values are
        // their fat pointer body.
        auto* lctx = module->getLlvmContext();
        auto fieldLayoutType = [&](const StructurePropertyPtr& p) -> llvm::Type* {
            CajetaTypePtr t = p->getType();
            if (dynamic_pointer_cast<CajetaArray>(t)) {
                return llvm::PointerType::get(*lctx, 0);
            }
            if (auto cls = dynamic_pointer_cast<CajetaClass>(t)) {
                if (dynamic_pointer_cast<CajetaView>(t)) {
                    return t->getLlvmType();  // inline view body
                }
                if (cls->isInterface()) {
                    return t->getLlvmType();  // inline 24-byte fat pointer
                }
                return llvm::PointerType::get(*lctx, 0);
            }
            return t->getLlvmType();
        };

        vector<llvm::Type*> llvmMembers;
        llvmMembers.push_back(llvm::PointerType::get(*lctx, 0));
        // Recursively prepend each ancestor's own fields, deepest-first.
        // The lambda walks superClasses to flatten the inheritance chain
        // into the order [grandparent fields, parent fields, this class's
        // own fields]. Iterate parents in declared order so multi-
        // inheritance (if it ever lands) produces a deterministic layout.
        std::function<void(CajetaClassPtr)> appendInherited;
        appendInherited = [&](CajetaClassPtr cls) {
            for (auto& parent : cls->superClasses) {
                appendInherited(parent);
                for (auto& p : parent->propertyList) {
                    if (p->isStatic()) continue;  // statics live in globals
                    llvmMembers.push_back(fieldLayoutType(p));
                }
            }
        };
        appendInherited(static_pointer_cast<CajetaClass>(shared_from_this()));
        // Then this class's own fields, in declaration order for
        // deterministic indices. Skip static properties — they're
        // backed by LLVM globals (CajetaClass::getOrCreateStaticFieldGlobal)
        // rather than instance slots.
        for (auto& property : propertyList) {
            if (property->isStatic()) continue;
            llvmMembers.push_back(fieldLayoutType(property));
        }
        ((llvm::StructType*) llvmType)->setBody(llvm::ArrayRef<llvm::Type*>(llvmMembers), false);

        ensureDefaultConstructor();
        synthesizeAutoHash();

        for (auto methodEntry: methods) {
            methodEntry.second->generatePrototype();
        }

        // Vtable build runs AFTER every method has its LLVM Function — the
        // constant needs `getLlvmFunction()` to return non-null for each slot.
        // Classes with only static methods/constructors produce a 2-slot
        // header-only vtable (`{ i16 version, i16 count = 0 }`); meaningful
        // dispatch arrives when inheritance and virtual calls are wired up.
        writeVirtualTable();

        // S9.5.2 — synthesize per-(class, interface) vtable globals for
        // every interface this class implements. Lands here, after method
        // prototypes + writeVirtualTable, so each implementing method's
        // LLVM function exists for fn-pointer harvesting and the per-pair
        // tables can sit alongside the legacy hash-keyed vtable.
        synthesizeInterfaceVTables();

        CajetaModule::getStructureToModule()[canonical] = module;
        prototypeBuilt = true;
    }

    void CajetaClass::synthesizeInterfaceVTables() {
        if (implementedInterfaces.empty()) return;

        auto& ctx = *module->getLlvmContext();
        auto* lmod = module->getLlvmModule();
        llvm::PointerType* ptrTy = llvm::PointerType::get(ctx, 0);
        std::string classCanonical = qName->toCanonical();

        auto sanitize = [](std::string s) {
            for (char& c : s) {
                if (c == ':' || c == '.' || c == '<' || c == '>'
                        || c == ',' || c == ' ') {
                    c = '_';
                }
            }
            return s;
        };

        for (auto& iface : implementedInterfaces) {
            std::string ifaceCanonical = iface->getQName()->toCanonical();

            // Walk interface methods in declaration order; for each one
            // find a same-name concrete method on this class. Signature
            // compatibility for class implementers is already enforced
            // at hash-vtable construction time (a mismatched signature
            // would hash differently and fail to bind), so the lookup
            // here is by name only — by the time we get here the user
            // either has a matching impl or the hash-vtable build has
            // already complained.
            auto findByName = [&](const std::string& name) -> MethodPtr {
                for (auto& [canon, m] : methods) {
                    if (!m || m->isConstructor()) continue;
                    if (m->getModifiers().find(STATIC) != m->getModifiers().end()) continue;
                    if (m->getName() == name) return m;
                }
                // Walk superclasses too — inherited implementations
                // count for satisfying an interface contract.
                for (auto& parent : superClasses) {
                    for (auto& [canon, m] : parent->getMethods()) {
                        if (!m || m->isConstructor()) continue;
                        if (m->getModifiers().find(STATIC) != m->getModifiers().end()) continue;
                        if (m->getName() == name) return m;
                    }
                }
                return nullptr;
            };

            std::vector<llvm::Constant*> entries;
            // S10.4 — slot 0 of every per-(impl, iface) vtable holds the
            // implementer's drop function. The interface drop helper
            // (__cajeta_iface_drop) reads this slot when kind ==
            // OWNED_CLASS. Method entries follow at slots 1..N; the
            // dispatch path adds +1 to its interface-method index.
            if (llvm::Function* dropFn = this->getOrCreateDropFunction()) {
                entries.push_back(dropFn);
            } else {
                entries.push_back(llvm::ConstantPointerNull::get(ptrTy));
            }
            for (auto& ifaceMethod : iface->getMethodList()) {
                if (!ifaceMethod || ifaceMethod->isConstructor()) continue;
                if (ifaceMethod->getModifiers().find(STATIC)
                        != ifaceMethod->getModifiers().end()) continue;
                MethodPtr concrete = findByName(ifaceMethod->getName());
                if (!concrete || !concrete->getLlvmFunction()) {
                    // Missing or unbuilt — leave a null slot. The hash-
                    // vtable build will have surfaced the real diagnostic
                    // earlier; null here just keeps the global well-formed.
                    entries.push_back(llvm::ConstantPointerNull::get(ptrTy));
                    continue;
                }
                entries.push_back(concrete->getLlvmFunction());
            }

            llvm::ArrayType* arrTy = llvm::ArrayType::get(
                ptrTy, entries.size());
            std::string globalName = std::string("class.")
                + sanitize(classCanonical) + "_iface_"
                + sanitize(ifaceCanonical) + "_VTable";
            if (llvm::GlobalVariable* existing = lmod->getNamedGlobal(globalName)) {
                interfaceVTables[ifaceCanonical] = existing;
                continue;
            }

            llvm::Constant* init = llvm::ConstantArray::get(arrTy, entries);
            auto* gv = new llvm::GlobalVariable(
                *lmod, arrTy, /*isConstant=*/true,
                llvm::GlobalValue::InternalLinkage, init, globalName);
            interfaceVTables[ifaceCanonical] = gv;
        }
    }

    void CajetaClass::synthesizeAutoHash() {
        // Gated on @AutoHash class annotation. Inject a structural
        // hash() that walks fields, hashes each via the matching
        // __cajeta_hash_X primitive (or virtual dispatch for class-
        // typed fields), and combines via __cajeta_hash_combine
        // seeded with __cajeta_hash_seed.
        //
        // Skip when:
        //   - The annotation isn't present (default identity hash
        //     inherited from Object stands).
        //   - The class already declares hash() manually (user
        //     opt-out from the synthesizer for this class even with
        //     @AutoHash present — the manual version wins).
        //
        // The synthesizer throws on unsupported field kinds with a
        // diagnostic naming the class, field, and remediation. See
        // SynthesizedHashMethod::generateCode for the rules.
        if (!findAnnotation("AutoHash")) return;

        for (auto& m : methodList) {
            if (m->getName() == "hash" && m->getParameters().size() == 0) {
                return;
            }
        }
        addMethod(std::make_shared<SynthesizedHashMethod>(
            module,
            std::static_pointer_cast<CajetaClass>(shared_from_this())));
    }

    void CajetaClass::ensureDefaultConstructor() {
        // Previous version looked up `qName->getTypeName()` in `methods`,
        // but `methods` is keyed by the full canonical (`"pkg.Class::Class()"`),
        // never by the bare type name — so this check missed every
        // user-declared constructor and a synthesized default was added
        // after the user's, which then threw "Constructor already
        // exists" from addMethod when canonical signatures collided.
        // Consult the constructor map directly instead.
        if (!unlabeledConstructorMap.empty()) return;
        addMethod(make_shared<DefaultConstructorMethod>(
            module, static_pointer_cast<CajetaClass>(shared_from_this())));
    }

    void CajetaClass::setClassBody(cajeta::ClassBodyDeclarationPtr classBody) {
        for (auto memberDeclaration: classBody->getDeclarations()) {
            memberDeclaration->updateParent(static_pointer_cast<CajetaClass>(shared_from_this()));
        }
    }

    void CajetaClass::generateCode() {
        for (auto& method: methodList) {
            method->generateCode();
        }
    }

    // Static class fields. Each STATIC property gets a dedicated LLVM
    // global variable, defined in the class's home module and named
    // `<class.canonical>.<propertyName>`. Lazy — created on first
    // access from any module. Cross-module callers receive an extern
    // decl in their own llvm::Module that resolves to the home
    // definition at JIT link.
    //
    // Storage shape mirrors what StackField uses for the equivalent
    // local: primitives store their value directly (i32, i64, double,
    // ...); reference types (String, class instances, arrays) store
    // a ptr — they're heap-allocated and the slot holds the address.
    //
    // v1 initializer: zero (`llvm::Constant::getNullValue`). User-
    // supplied initializers (`public static int32 base = 100;`) are
    // a follow-on landing — until then the initial value is 0 / null
    // and `Class.field = literal` writes from method body do the
    // initialization at runtime.
    // Fold a static-field initializer expression to an llvm::Constant if
    // the shape is a known compile-time literal. Returns null for any
    // shape we don't statically evaluate yet — caller falls back to
    // zeroinitializer. Supported v1:
    //   - IntegerLiteralExpression (any radix); coerced to `storedType`
    //     when the field is an integer
    //   - FloatLiteralExpression; coerced to `storedType` when the
    //     field is a floating-point type
    //   - PrefixExpression with NEGATIVE applied to either of the above
    // Out of scope (would need a `<clinit>`-style runtime init function):
    //   computed expressions, method calls, references to other
    //   statics, string literals (which materialize through global
    //   string constants — supportable but deferred).
    static llvm::Constant* foldStaticInitializer(
            AbstractSyntaxNodePtr init, llvm::Type* storedType) {
        if (!init || !storedType) return nullptr;
        // VariableInitializer wraps the actual expression as its single
        // child. Unwrap once.
        if (auto vi = dynamic_pointer_cast<VariableInitializer>(init)) {
            auto& kids = vi->getChildren();
            if (kids.empty()) return nullptr;
            init = kids[0];
        }
        bool negate = false;
        if (auto pe = dynamic_pointer_cast<PrefixExpression>(init)) {
            if (pe->getOp() != PREFIX_OP_NEGATIVE) return nullptr;
            auto& kids = pe->getChildren();
            if (kids.empty()) return nullptr;
            init = kids[0];
            negate = true;
        }
        if (auto il = dynamic_pointer_cast<IntegerLiteralExpression>(init)) {
            if (!storedType->isIntegerTy()) return nullptr;
            uint8_t radix;
            switch (il->getIntegerLiteralType()) {
                case INTEGER_LITERAL_TYPE_BINARY: radix = 2;  break;
                case INTEGER_LITERAL_TYPE_OCT:    radix = 8;  break;
                case INTEGER_LITERAL_TYPE_HEX:    radix = 16; break;
                default:                          radix = 10; break;
            }
            // Strip the radix prefix (`0b`, `0x`) and any trailing `L`
            // / `l` suffix the grammar may have captured.
            std::string raw = il->getRawValue();
            if (raw.size() >= 2 && raw[0] == '0'
                    && (raw[1] == 'b' || raw[1] == 'B'
                        || raw[1] == 'x' || raw[1] == 'X')) {
                raw = raw.substr(2);
            }
            if (!raw.empty() && (raw.back() == 'L' || raw.back() == 'l')) {
                raw = raw.substr(0, raw.size() - 1);
            }
            unsigned bits = storedType->getIntegerBitWidth();
            llvm::APInt apint(64, raw, radix);
            if (negate) apint = -apint;
            if (bits < 64) {
                apint = apint.trunc(bits);
            } else if (bits > 64) {
                apint = apint.sext(bits);
            }
            return llvm::ConstantInt::get(storedType, apint);
        }
        if (auto fl = dynamic_pointer_cast<FloatLiteralExpression>(init)) {
            if (!storedType->isFloatingPointTy()) return nullptr;
            double v = std::strtod(fl->getRawValue().c_str(), nullptr);
            if (negate) v = -v;
            return llvm::ConstantFP::get(storedType, v);
        }
        return nullptr;
    }

    llvm::GlobalVariable* CajetaClass::getOrCreateStaticFieldGlobal(
            StructurePropertyPtr prop, CajetaModulePtr callerModule) {
        if (!prop || !prop->isStatic()) return nullptr;
        if (!prop->getType() || !prop->getType()->getLlvmType()) return nullptr;

        auto* lmod = module->getLlvmModule();
        const std::string globalName =
            qName->toCanonical() + "." + prop->getName();

        auto it = staticFieldGlobals.find(prop->getName());
        llvm::GlobalVariable* g;
        if (it != staticFieldGlobals.end()) {
            g = it->second;
        } else if (auto* existing = lmod->getNamedGlobal(globalName)) {
            // Already declared in this module (e.g. an earlier extern
            // ref) — adopt it. If it lacks an initializer we'll
            // promote it to a definition below.
            g = existing;
        } else {
            llvm::Type* storedType = prop->getType()->getLlvmType();
            if (!(prop->getType()->getTypeFlags() & PRIMITIVE_FLAG)) {
                storedType = llvm::PointerType::get(
                    *module->getLlvmContext(), 0);
            }
            // Constant-fold the declared initializer if shape allows;
            // otherwise zero-init. Folding handles `= 100`, `= -7`, and
            // float literals — broad enough to cover what user code
            // routinely writes for class-level constants.
            llvm::Constant* init = foldStaticInitializer(
                prop->getInitializer(), storedType);
            if (!init) init = llvm::Constant::getNullValue(storedType);
            g = new llvm::GlobalVariable(
                *lmod, storedType, /*isConstant=*/false,
                llvm::GlobalValue::ExternalLinkage,
                init, globalName);
            staticFieldGlobals[prop->getName()] = g;
        }

        // Cross-module: the caller is emitting IR into a different
        // llvm::Module. Insert an extern decl there and return that.
        if (callerModule && callerModule->getLlvmModule() != lmod) {
            llvm::Constant* shim = CajetaModule::ensureGlobalInModule(
                callerModule->getLlvmModule(), g);
            return llvm::cast<llvm::GlobalVariable>(shim);
        }
        return g;
    }

    // Gap 1 (virtual dispatch on drop). Patch the vtable global's drop_fn
    // slot (index 3 in the vtable struct layout — see StructureMetadata::
    // createVirtualTableType) so __cajeta_class_virtual_drop's
    // vtable->drop_fn load returns this class's heap-drop wrapper.
    //
    // The slot was set to NULL by createVirtualTableConstant: emitting
    // the drop wrapper from inside vtable construction breaks stdlib
    // linkage (see comment there). Instead, every heap class local's
    // drop-registration site at LocalVariableDeclaration::generateCode
    // calls patch + ensures the wrapper has been generated.
    //
    // Both globals (vtable + drop wrapper) live in this class's home
    // module by construction, so the patch is module-local. Idempotent
    // via llvmDropFunctionPatched.
    void CajetaClass::patchVirtualTableDropFn() {
        if (llvmDropFunctionPatched) return;
        llvm::Function* dropFn = getOrCreateDropFunction();
        if (!dropFn) return;
        if (!llvmVirtualTableGlobal || !llvmVirtualTableType) return;
        llvm::Constant* init = llvmVirtualTableGlobal->getInitializer();
        if (!init) return;
        auto* structInit = llvm::dyn_cast<llvm::ConstantStruct>(init);
        if (!structInit) return;
        // Rebuild the constant with drop_fn replaced (slot index 3).
        std::vector<llvm::Constant*> elems;
        elems.reserve(structInit->getNumOperands());
        for (unsigned i = 0; i < structInit->getNumOperands(); ++i) {
            elems.push_back(structInit->getOperand(i));
        }
        if (elems.size() < 4) return;
        elems[3] = dropFn;
        llvmVirtualTableGlobal->setInitializer(
            llvm::ConstantStruct::get(llvmVirtualTableType,
                llvm::ArrayRef<llvm::Constant*>(elems)));
        llvmDropFunctionPatched = true;
    }

    llvm::Function* CajetaClass::getOrCreateStackDropFunction() {
        if (llvmStackDropFunction) return llvmStackDropFunction;
        if (interfaceFlag) return nullptr;
        auto& ctx = *module->getLlvmContext();
        auto* lmod = module->getLlvmModule();
        llvm::PointerType* ptrTy = llvm::PointerType::get(ctx, 0);
        llvm::FunctionType* fnTy = llvm::FunctionType::get(
            llvm::Type::getVoidTy(ctx), {(llvm::Type*) ptrTy},
            /*isVarArg=*/false);

        std::string dropName = std::string("__cajeta_stack_")
            + qName->toCanonical() + "_drop";
        for (char& c : dropName) {
            if (c == ':' || c == '.' || c == '<' || c == '>' || c == ',' || c == ' ') {
                c = '_';
            }
        }
        if (llvm::Function* existing = lmod->getFunction(dropName)) {
            llvmStackDropFunction = existing;
            return existing;
        }

        llvmStackDropFunction = llvm::Function::Create(fnTy,
            llvm::Function::ExternalLinkage, dropName, lmod);
        llvm::BasicBlock* bb = llvm::BasicBlock::Create(
            ctx, "entry", llvmStackDropFunction);
        llvm::IRBuilder<> b(bb);
        llvm::Value* instance = llvmStackDropFunction->getArg(0);

        llvm::BasicBlock* doDrop = llvm::BasicBlock::Create(
            ctx, "doDrop", llvmStackDropFunction);
        llvm::BasicBlock* done = llvm::BasicBlock::Create(
            ctx, "done", llvmStackDropFunction);
        llvm::Value* isNull = b.CreateICmpEQ(instance,
            llvm::ConstantPointerNull::get(ptrTy));
        b.CreateCondBr(isNull, done, doDrop);
        b.SetInsertPoint(doDrop);

        // Call user-defined drop() if present. Same lookup pattern as
        // the heap drop above.
        MethodPtr userDrop;
        for (auto& entry : methods) {
            MethodPtr m = entry.second;
            if (!m || m->isConstructor()) continue;
            if (m->getName() != "drop") continue;
            auto pl = m->getParameterList();
            if (pl.size() == 1) {
                userDrop = m;
                break;
            }
        }
        if (userDrop && userDrop->getLlvmFunction()) {
            b.CreateCall(userDrop->getLlvmFunctionType(),
                userDrop->getLlvmFunction(), {instance});
        }

        // Walk owned class-ref fields in REVERSE declaration order. For
        // each plain class-ref (not aggregate, not array, not interface),
        // GEP the slot, load the pointer, call the referent class's
        // own heap-drop fn. Mirrors CajetaStruct::getOrCreateDropFunction
        // — preserves the auto-walk-owned-fields behavior structs have
        // today, generalized to any class allocated on the stack.
        std::vector<StructurePropertyPtr> reversed(
            propertyList.begin(), propertyList.end());
        std::reverse(reversed.begin(), reversed.end());
        for (auto& property : reversed) {
            auto fieldType = property->getType();
            auto fieldClass = dynamic_pointer_cast<CajetaClass>(fieldType);
            if (!fieldClass) continue;
            if (dynamic_pointer_cast<CajetaView>(fieldType)) continue;
            if (dynamic_pointer_cast<CajetaArray>(fieldType)) continue;
            if (fieldClass->isInterface()) continue;

            unsigned fieldIdx = (unsigned) getFieldLlvmIndex(property);
            llvm::Value* slotPtr = b.CreateStructGEP(
                llvmType, instance, fieldIdx,
                std::string("stack_drop_field_") + property->getName());
            llvm::Value* refPtr = b.CreateLoad(ptrTy, slotPtr);

            llvm::Function* refDrop = fieldClass->getOrCreateDropFunction();
            if (!refDrop) continue;
            refDrop = CajetaModule::ensureFunctionInModule(lmod, refDrop);
            b.CreateCall(refDrop, {refPtr});
        }

        // Recurse into embedded struct fields (same as heap drop above).
        // Skips the trailing __cajeta_free — that's the only difference.
        for (auto& property : reversed) {
            auto fieldType = property->getType();
            auto fieldStruct = dynamic_pointer_cast<CajetaStruct>(fieldType);
            if (!fieldStruct) continue;
            llvm::Function* structDropFn =
                fieldStruct->getOrCreateDropFunction();
            if (!structDropFn) continue;
            structDropFn = CajetaModule::ensureFunctionInModule(
                lmod, structDropFn);
            unsigned fieldIdx = (unsigned) getFieldLlvmIndex(property);
            llvm::Value* embedPtr = b.CreateStructGEP(
                llvmType, instance, fieldIdx,
                std::string("stack_drop_embed_") + property->getName());
            b.CreateCall(structDropFn, {embedPtr});
        }

        // No __cajeta_free — stack body is reclaimed by the function
        // epilogue. This is the only structural difference from
        // getOrCreateDropFunction.

        b.CreateBr(done);
        b.SetInsertPoint(done);
        b.CreateRetVoid();
        return llvmStackDropFunction;
    }

    llvm::Function* CajetaClass::getOrCreateDropFunction() {
        if (llvmDropFunction) return llvmDropFunction;
        if (interfaceFlag) return nullptr;
        auto& ctx = *module->getLlvmContext();
        auto* lmod = module->getLlvmModule();
        llvm::PointerType* ptrTy = llvm::PointerType::get(ctx, 0);
        llvm::FunctionType* fnTy = llvm::FunctionType::get(
            llvm::Type::getVoidTy(ctx), {(llvm::Type*) ptrTy},
            /*isVarArg=*/false);

        // Build the symbol name from the canonical class name with
        // separators sanitized to underscores so it's a valid C
        // identifier (and reads sensibly in stack traces).
        std::string dropName = std::string("__cajeta_") + qName->toCanonical() + "_drop";
        for (char& c : dropName) {
            if (c == ':' || c == '.' || c == '<' || c == '>' || c == ',' || c == ' ') {
                c = '_';
            }
        }

        // If the same JIT module has built this drop fn before (e.g. a
        // template instantiation revisited), reuse it.
        if (llvm::Function* existing = lmod->getFunction(dropName)) {
            llvmDropFunction = existing;
            return existing;
        }

        llvmDropFunction = llvm::Function::Create(fnTy,
            llvm::Function::ExternalLinkage, dropName, lmod);
        llvm::BasicBlock* bb = llvm::BasicBlock::Create(
            ctx, "entry", llvmDropFunction);
        llvm::IRBuilder<> b(bb);
        llvm::Value* instance = llvmDropFunction->getArg(0);

        // Null guard. The drop chain only ever fires on values pushed
        // by __cajeta_drop_push, but a user-declared `drop()` method
        // might run inside another drop (chain teardown) on a borrowed
        // null receiver. Cheap to add, prevents a crash on the unhappy
        // path.
        llvm::BasicBlock* doDrop = llvm::BasicBlock::Create(
            ctx, "doDrop", llvmDropFunction);
        llvm::BasicBlock* done = llvm::BasicBlock::Create(
            ctx, "done", llvmDropFunction);
        llvm::Value* isNull = b.CreateICmpEQ(instance,
            llvm::ConstantPointerNull::get(ptrTy));
        b.CreateCondBr(isNull, done, doDrop);

        b.SetInsertPoint(doDrop);

        // Call the user's drop() method if defined. Single-param
        // (just `this`), non-constructor, named exactly "drop". We
        // look up directly rather than going through resolveMethod so
        // the drop wrapper doesn't depend on overload resolution that
        // hasn't run yet at this stage of codegen.
        MethodPtr userDrop;
        for (auto& entry : methods) {
            MethodPtr m = entry.second;
            if (!m || m->isConstructor()) continue;
            if (m->getName() != "drop") continue;
            auto pl = m->getParameterList();
            // After Method::generatePrototype, instance methods have
            // `this` as parameterList[0]. drop() has no other params.
            if (pl.size() == 1) {
                userDrop = m;
                break;
            }
        }
        if (userDrop && userDrop->getLlvmFunction()) {
            b.CreateCall(userDrop->getLlvmFunctionType(),
                userDrop->getLlvmFunction(), {instance});
        }

        // Auto field drops (MemoryModel.md § Known gaps — automatic
        // field drops). In REVERSE declaration order, walk every
        // owned heap field and route to its appropriate drop helper.
        // The stack-drop wrapper already does this (see
        // getOrCreateStackDropFunction); the heap-drop wrapper was
        // the gap. Done BEFORE __cajeta_free so the field slots
        // still address the live body — freeing the block first
        // would leave the dropped pointers dangling, unobservable
        // from outside but still UB to dereference.
        //
        // Per-field-type emission (see cajeta-docs/FieldOwnership.md
        // § Solution B for the doctrine):
        //
        //   - CajetaStruct: GEP the embedded body, call the struct's
        //     synthesized drop fn (which walks its own owned fields).
        //     The body itself isn't separately freed — it lives
        //     inline in this instance's block.
        //   - CajetaArray: load the heap-array pointer and call
        //     __cajeta_free_array. The runtime helper is idempotent
        //     (claims through the live-allocation set), so when the
        //     field aliases a local-owned buffer (e.g. ArrayStream.data
        //     aliasing ArrayList.data) the first drop wins and the
        //     second is a silent no-op rather than a double-free.
        //   - CajetaInterface: the slot IS the 24-byte fat-pointer
        //     body. Pass its address to __cajeta_iface_drop, which
        //     reads the kind word and only fires the underlying drop
        //     for OWNED_CLASS variants (BORROWED_* kinds no-op).
        //   - Plain CajetaClass ref: load the instance pointer and call
        //     __cajeta_class_virtual_drop, which atomically claims the
        //     address out of the live-allocation set and dispatches
        //     through the dynamic type's vtable.drop_fn. Same
        //     idempotency story as arrays — aliased fields (e.g.
        //     Optional<Hello>.value aliasing a local) drop once.
        //
        // Null-safe by construction: instance bytes were memset to
        // zero by ClassCreatorRest, so an unassigned class-ref or
        // array field reads as a null pointer; each runtime helper
        // null-checks.
        std::vector<StructurePropertyPtr> reversed(
            propertyList.begin(), propertyList.end());
        std::reverse(reversed.begin(), reversed.end());

        llvm::Function* freeArrayFn = nullptr;
        llvm::Function* virtualDropFn = nullptr;
        llvm::Function* ifaceDropFn = nullptr;

        for (auto& property : reversed) {
            if (property->isStatic()) continue;  // statics live in globals
            auto fieldType = property->getType();
            if (!fieldType) continue;
            unsigned fieldIdx = (unsigned) getFieldLlvmIndex(property);

            // Struct field — recurse into the embedded body in place.
            if (auto fieldStruct = dynamic_pointer_cast<CajetaStruct>(fieldType)) {
                llvm::Function* structDropFn =
                    fieldStruct->getOrCreateDropFunction();
                if (!structDropFn) continue;
                structDropFn = CajetaModule::ensureFunctionInModule(
                    lmod, structDropFn);
                llvm::Value* embedPtr = b.CreateStructGEP(
                    llvmType, instance, fieldIdx,
                    std::string("drop_embed_") + property->getName());
                b.CreateCall(structDropFn, {embedPtr});
                continue;
            }

            // Array field — idempotent free via the live-allocation set.
            if (dynamic_pointer_cast<CajetaArray>(fieldType)) {
                if (!freeArrayFn) {
                    freeArrayFn = module->getRuntimeFunction("__cajeta_free_array");
                }
                if (!freeArrayFn) continue;
                llvm::Value* slot = b.CreateStructGEP(
                    llvmType, instance, fieldIdx,
                    std::string("drop_arr_slot_") + property->getName());
                llvm::Value* arrPtr = b.CreateLoad(ptrTy, slot,
                    std::string("drop_arr_ptr_") + property->getName());
                b.CreateCall(freeArrayFn, {arrPtr});
                continue;
            }

            if (auto fieldClass = dynamic_pointer_cast<CajetaClass>(fieldType)) {
                if (dynamic_pointer_cast<CajetaView>(fieldType)) continue;

                // Interface field — kind-tagged drop already discriminates.
                if (fieldClass->isInterface()) {
                    if (!ifaceDropFn) {
                        ifaceDropFn = module->getRuntimeFunction("__cajeta_iface_drop");
                    }
                    if (!ifaceDropFn) continue;
                    llvm::Value* bodyPtr = b.CreateStructGEP(
                        llvmType, instance, fieldIdx,
                        std::string("drop_iface_body_") + property->getName());
                    b.CreateCall(ifaceDropFn, {bodyPtr});
                    continue;
                }

                // Plain class-ref — virtual dispatch through the
                // referent's vtable. Skip types with no vtable slot at
                // offset 0 (Task<T> et al.) — virtual_drop assumes a
                // vtable at instance[0]. Their handful of users hold
                // them as locals, not class fields.
                if (!fieldClass->hasVtablePointerAtSlotZero()) continue;
                if (!virtualDropFn) {
                    virtualDropFn = module->getRuntimeFunction(
                        "__cajeta_class_virtual_drop");
                }
                if (!virtualDropFn) continue;
                // Materialize the field class's drop wrapper so the
                // vtable.drop_fn slot is patched before dispatch.
                fieldClass->patchVirtualTableDropFn();
                llvm::Value* slot = b.CreateStructGEP(
                    llvmType, instance, fieldIdx,
                    std::string("drop_ref_slot_") + property->getName());
                llvm::Value* refPtr = b.CreateLoad(ptrTy, slot,
                    std::string("drop_ref_ptr_") + property->getName());
                b.CreateCall(virtualDropFn, {refPtr});
                continue;
            }
            // Primitive / pointer / function-typed / etc. — no drop.
        }

        // Free the heap allocation. __cajeta_free is part of the
        // closure-drop runtime added in L3-3; reuse it here so all
        // generic heap blocks share one free symbol.
        llvm::Function* freeFn = module->getRuntimeFunction("__cajeta_free");
        if (freeFn) {
            b.CreateCall(freeFn, {instance});
        }

        b.CreateBr(done);
        b.SetInsertPoint(done);
        b.CreateRetVoid();
        return llvmDropFunction;
    }

    struct MethodEntry {
        MethodPtr method;
        int score;
        MethodEntry(MethodPtr method) { this->method = method; score = 0; }
    };

    MethodPtr CajetaClass::getClosestMethod(string methodName, vector<ParameterEntry> parameters, map<string, MethodPtr> canonical) {
        vector<MethodEntry> entries;

        for (auto& entry : canonical) {
            MethodPtr method = entry.second;
            map<string, FormalParameterPtr> methodParameters = method->getParameters();
            bool valid = true;
            MethodEntry methodEntry(method);
            for (auto& parameter : parameters) {
                if (methodParameters.find(parameter.label) != methodParameters.end()) {
                    int score = methodParameters[parameter.label]->getType()->getRank() - parameter.type->getRank();
                    if (score < 0) {
                        valid = false;
                        break;
                    } else {
                        methodEntry.score += score;
                    }
                }
            }
            if (valid) {
                entries.push_back(methodEntry);
            }
        }
        if (entries.empty()) {
            return nullptr;
        }
        sort(entries.begin(), entries.end(), [](const MethodEntry& a, const MethodEntry& b) { return a.score < b.score; });
        return entries[0].method;
    }

    // TODO: Need to call this before addMethods are called, when any parent classes are loaded.  Rewrite based on parent classes already having their methods mapped.
    void CajetaClass::createInheritanceMethodMap(CajetaClassPtr structure) {
        if (structure == nullptr) {
            structure = static_pointer_cast<CajetaClass>(shared_from_this());
        }

        for (auto& superClass: structure->getSuperClasses()) {
            createInheritanceMethodMap(superClass);
        }

        for (auto& method: structure->getMethodList()) {
            mapMethod(method, labeledMethodMap, true);
            mapMethod(method, unlabeledMethodMap, false);
        }
    }

    void CajetaClass::resolveSuperClasses() {
        superClasses.clear();
        for (auto& qName : qExtended) {
            // First try the qName's full canonical (correct when the user
            // wrote `extends some.pkg.Animal`).
            auto& structures = module->getStructures();
            auto it = structures.find(qName->toCanonical());
            if (it != structures.end()) {
                superClasses.push_back(it->second);
                continue;
            }
            // Fallback for single-identifier names like `extends Animal`:
            // QualifiedName::fromContext picks "code" as the package, which
            // won't match the class's actual canonical. Walk by short name.
            bool found = false;
            for (auto& entry : structures) {
                if (entry.second->getQName()->getTypeName() == qName->getTypeName()) {
                    superClasses.push_back(entry.second);
                    found = true;
                    break;
                }
            }
            if (found) continue;
            // Cross-module / forward-reference fallback: consult the
            // process-global canonicalMap. The parent might be in a
            // different module's structures, or it might still be a
            // placeholder created by fromContext. Picking up the
            // placeholder here keeps superClasses non-empty so
            // tryGeneratePrototype can spot the placeholder and defer
            // until the parent fills in.
            auto& canon = canonicalMap;
            auto canonIt = canon.find(qName->toCanonical());
            if (canonIt == canon.end()) {
                canonIt = canon.find(qName->getTypeName());
            }
            if (canonIt != canon.end()) {
                if (auto klass = std::dynamic_pointer_cast<CajetaClass>(canonIt->second)) {
                    superClasses.push_back(klass);
                }
            }
            // Truly unresolved (no archive entry, no placeholder) parents
            // silently skip today; CajetaModule::validatePlaceholders
            // catches the post-parse case where a placeholder was created
            // but never filled in.
        }
    }

    bool CajetaClass::tryGeneratePrototype() {
        if (prototypeBuilt) return true;
        // Resolve parents first so we can inspect them. resolveSuperClasses
        // is idempotent and reflects whatever canonicalMap shows now.
        resolveSuperClasses();
        resolveImplementedInterfaces();
        // Defer until every superclass has its struct laid out. A
        // placeholder isn't enough — an already-filled-in but
        // not-yet-prototyped parent's propertyList might still be empty
        // OR the inheritance chain above it might not be built, which
        // would leave OUR layout missing inherited fields. Wait until
        // the parent's prototypeBuilt latches true (which only happens
        // at the END of generatePrototype, after the parent's full
        // ancestor walk has run).
        for (auto& parent : superClasses) {
            if (!parent) continue;
            // Templates (uninstantiated) never set prototypeBuilt — they
            // short-circuit in generatePrototype without laying out a
            // struct. Treat them as "done" so a templated subclass like
            // `class List<T> extends Container<T>` doesn't defer forever
            // waiting on its template parent.
            if (parent->isTemplate()) continue;
            if (parent->isPlaceholder()) return false;
            if (!parent->prototypeBuilt) return false;
        }
        for (auto& iface : implementedInterfaces) {
            if (!iface) continue;
            if (iface->isTemplate()) continue;
            if (iface->isPlaceholder()) return false;
            if (!iface->prototypeBuilt) return false;
        }
        generatePrototype();
        return true;
    }

    void CajetaClass::resolveImplementedInterfaces() {
        // Mirrors resolveSuperClasses but for `implements I1, I2`. Each name
        // is looked up in the module's structures map; entries flagged
        // isInterface() are pushed into implementedInterfaces. Non-interface
        // names in the implements list are silently skipped today (a future
        // version should raise CAJETA_ERROR_NOT_AN_INTERFACE).
        implementedInterfaces.clear();
        auto& structures = module->getStructures();
        for (auto& qn : qImplemented) {
            CajetaClassPtr found;
            auto it = structures.find(qn->toCanonical());
            if (it != structures.end()) {
                found = it->second;
            } else {
                for (auto& entry : structures) {
                    if (entry.second->getQName()->getTypeName() == qn->getTypeName()) {
                        found = entry.second;
                        break;
                    }
                }
            }
            if (found && found->isInterface()) {
                implementedInterfaces.push_back(found);
            }
        }
    }

    void CajetaClass::buildVirtualTable() {
        // Build the (canonical → MethodPtr) mapping by walking the hierarchy
        // parent-first. Overrides in derived classes replace the parent's
        // entry; brand-new methods append. Statics and constructors are not
        // virtual and don't participate.
        //
        // Once the unique-method set is determined, we sort by the canonical
        // signature's FNV-1a hash. Dispatch is hash-based (see runtime's
        // __cajeta_vtable_lookup) which sidesteps the slot-index collision
        // problem that multi-inheritance would otherwise hit with a
        // position-only layout.
        virtualMethodList.clear();
        virtualSlotHashList.clear();
        map<string, MethodPtr> uniqueByCanonical;

        // Override detection: a child's override has a DIFFERENT canonical
        // than the parent's method because canonical includes `parent::`
        // (e.g. `test.Animal::speak()` vs `test.Dog::speak()`). To make
        // overrides replace the parent's vtable slot, we match by SUFFIX
        // (name + params, stripping the leading `parent::`). When the
        // suffix is already in `uniqueByCanonical`, remove the existing
        // entry (the parent's) and insert the child's keyed by the
        // child's canonical. Result: dispatch by either the parent's or
        // child's canonical-hash lands on the child's method.
        auto suffixOf = [](const string& canon) -> string {
            auto pos = canon.rfind("::");
            return (pos == string::npos) ? canon : canon.substr(pos + 2);
        };
        std::function<void(CajetaClassPtr)> walk = [&](CajetaClassPtr c) {
            for (auto& sup : c->getSuperClasses()) walk(sup);
            for (auto& m : c->getMethodList()) {
                if (m->isConstructor()) continue;
                if (m->getModifiers().find(STATIC) != m->getModifiers().end()) continue;
                if (m->isAbstract()) continue;   // interface markers don't sit here
                string canon = m->toCanonical(/*labeled=*/false);
                string suffix = suffixOf(canon);
                // Find + replace any existing entry with the same suffix
                // (the parent's method this overrides).
                for (auto it = uniqueByCanonical.begin();
                        it != uniqueByCanonical.end(); ) {
                    if (suffixOf(it->first) == suffix) {
                        it = uniqueByCanonical.erase(it);
                    } else {
                        ++it;
                    }
                }
                uniqueByCanonical[canon] = m;
            }
        };
        walk(static_pointer_cast<CajetaClass>(shared_from_this()));

        // Also re-key the resulting entries so a dispatch using the
        // BASE class's canonical-hash also finds the override. Build a
        // parallel map keyed by suffix; for each entry, walk all its
        // superclasses' equivalent (same-suffix) methods and re-publish
        // under THEIR canonical hashes pointing at the same impl.
        {
            map<string, MethodPtr> bySuffix;
            for (auto& [canon, m] : uniqueByCanonical) {
                bySuffix[suffixOf(canon)] = m;
            }
            // Walk superclasses and add entries under any base-canonical
            // that has a matching suffix. Multiple superclasses with the
            // same-suffix method all alias to the most-derived impl.
            std::function<void(CajetaClassPtr)> aliasWalk = [&](CajetaClassPtr c) {
                for (auto& sup : c->getSuperClasses()) {
                    for (auto& m : sup->getMethodList()) {
                        if (m->isConstructor()) continue;
                        if (m->getModifiers().find(STATIC) != m->getModifiers().end()) continue;
                        if (m->isAbstract()) continue;
                        string supCanon = m->toCanonical(/*labeled=*/false);
                        auto it = bySuffix.find(suffixOf(supCanon));
                        if (it != bySuffix.end()) {
                            // Add alias entry under the parent's canonical
                            // pointing at the most-derived impl.
                            uniqueByCanonical[supCanon] = it->second;
                        }
                    }
                    aliasWalk(sup);
                }
            };
            aliasWalk(static_pointer_cast<CajetaClass>(shared_from_this()));
        }

        // Interface dispatch: for each implemented interface method, find
        // the class's matching concrete method (by short name + param
        // signature) and register it in the vtable under the *interface*
        // method's canonical. The receiver's static type at the call site
        // determines which hash gets used — a call through an interface-
        // typed variable uses the interface's canonical and lands on the
        // entry we add here.
        auto findConcreteFor = [&](MethodPtr abstractM) -> MethodPtr {
            string targetSig = abstractM->toCanonical(/*labeled=*/false);
            // Strip the leading `parent::` (anything before the last "::")
            // so we match by method name + params only.
            auto pos = targetSig.rfind("::");
            string suffix = (pos == string::npos) ? targetSig : targetSig.substr(pos + 2);
            for (auto& [canon, m] : uniqueByCanonical) {
                auto p = canon.rfind("::");
                if (p == string::npos) continue;
                if (canon.substr(p + 2) == suffix) return m;
            }
            return nullptr;
        };
        for (auto& iface : implementedInterfaces) {
            for (auto& m : iface->getMethodList()) {
                if (m->isConstructor()) continue;
                if (m->getModifiers().find(STATIC) != m->getModifiers().end()) continue;
                if (auto concrete = findConcreteFor(m)) {
                    // Key by the interface method's canonical; value points
                    // to the class's concrete implementation. The vtable
                    // entry's function is concrete->getLlvmFunction().
                    uniqueByCanonical[m->toCanonical(/*labeled=*/false)] = concrete;
                }
                // No concrete impl = unsatisfied interface obligation. A
                // future version should raise CAJETA_ERROR_INTERFACE_NOT_IMPLEMENTED
                // here; today we skip and the vtable simply omits the entry.
            }
        }

        // Sort by hash for binary search at dispatch time. Hash is stable
        // across runs (FNV-1a) so the compiler and runtime agree.
        vector<pair<int64_t, MethodPtr>> sorted;
        sorted.reserve(uniqueByCanonical.size());
        for (auto& entry : uniqueByCanonical) {
            sorted.emplace_back(signatureHash(entry.first), entry.second);
        }
        std::sort(sorted.begin(), sorted.end(),
            [](const pair<int64_t, MethodPtr>& a,
               const pair<int64_t, MethodPtr>& b) {
                return a.first < b.first;
            });

        int idx = 0;
        for (auto& [hash, method] : sorted) {
            method->setVirtualTableIndex(idx++);
            virtualMethodList.push_back(method);
            virtualSlotHashList.push_back(hash);
        }
    }

    void CajetaClass::writeVirtualTable() {
        // Idempotent: bail if we've already produced a vtable global. Callers
        // can invoke this at any point in prototype generation without
        // worrying about duplicate work.
        if (llvmVirtualTableGlobal != nullptr) return;
        buildVirtualTable();
        StructureMetadata(module).populate(
            static_pointer_cast<CajetaClass>(shared_from_this()));
    }

    MethodPtr CajetaClass::resolveMethod(string& methodName, vector<ParameterEntry>& parameters, bool isConstructor, bool floatingParams) {
        // Each class indexes its declared methods under keys built with its
        // own class name (Method::buildGeneric/buildCanonical embed the
        // parent class). Inherited methods are NOT re-keyed into derived
        // class maps; instead we walk the hierarchy at lookup time so the
        // recursive call hits the parent's maps with the right key flavor.
        string generic = Method::buildGeneric(static_pointer_cast<CajetaClass>(shared_from_this()), methodName, parameters, floatingParams);
        string canonical = Method::buildCanonical(static_pointer_cast<CajetaClass>(shared_from_this()), methodName, parameters, floatingParams);

        map<string, map<string, MethodPtr>>* genericMap;
        if (isConstructor) {
            genericMap = floatingParams ? &labeledConstructorMap : &unlabeledConstructorMap;
        } else {
            genericMap = floatingParams ? &labeledMethodMap : &unlabeledMethodMap;
        }

        if (genericMap->find(generic) != genericMap->end()) {
            map<string, MethodPtr>& canonicalMap = (*genericMap)[generic];
            auto it = canonicalMap.find(canonical);
            if (it != canonicalMap.end()) {
                return it->second;
            }
            MethodPtr m = getClosestMethod(methodName, parameters, canonicalMap);
            if (m) return m;
        }

        // Constructors are NOT inherited; only walk parents for instance methods.
        if (!isConstructor) {
            for (auto& parent : superClasses) {
                MethodPtr m = parent->resolveMethod(methodName, parameters, isConstructor, floatingParams);
                if (m) return m;
            }
        }
        return nullptr;
    }

    llvm::Value* CajetaClass::invokeMethod(string& methodName, vector<ParameterEntry> parameters, bool isConstructor, llvm::Value* thisValue,
                                            CajetaModulePtr callerModule) {
        bool floatingParams = true;
        for (auto &param : parameters) {
            if (param.label.empty()) {
                floatingParams &= false;
            }
        }

        if (floatingParams) {
            sort(parameters.begin(), parameters.end(), [](const ParameterEntry& a, const ParameterEntry& b) -> bool { return a.label < b.label; });
        }

        MethodPtr method = resolveMethod(methodName, parameters, isConstructor, floatingParams);
        if (!method) {
            return nullptr;
        }
        // Method::generatePrototype injects `this` as the first parameter for non-static
        // methods; prepend the instance pointer here so the call's argument list matches.
        bool isStatic = method->getModifiers().find(STATIC) != method->getModifiers().end();
        vector<llvm::Value*> methodArgs;
        if (thisValue && !isStatic) {
            methodArgs.push_back(thisValue);
        }
        // The caller's module owns the IRBuilder we need to insert into.
        // Pre-refactor every CajetaModule re-parsed the stdlib, so the
        // receiver class's own pModule == the caller's module and
        // `this->module` was a safe fallback. With one shared stdlib
        // module, that fallback would emit cross-module calls into the
        // *stdlib* module (whose builder still points at whichever
        // stdlib method was generateCode'd last) — appending instructions
        // after a foreign function's terminator. Callers in user code
        // pass their own module; the fallback is preserved for sites
        // that haven't been threaded through yet.
        CajetaModulePtr emitMod = callerModule ? callerModule : module;
        // Coerce each arg to match the function's parameter type. Integer
        // literals default to i64, but the function may expect i32 / i8 / etc.
        // Without coercion the JIT verifier rejects the call as a type
        // mismatch. Use the function's parameter types as the source of truth.
        auto* coerceBuilder = emitMod->getBuilder();
        llvm::FunctionType* mft = method->getLlvmFunctionType();
        int thisOffset = (thisValue && !isStatic) ? 1 : 0;
        for (int i = 0; i < (int) parameters.size(); i++) {
            llvm::Value* v = parameters[i].value;
            if (mft && (int) mft->getNumParams() > i + thisOffset) {
                llvm::Type* expected = mft->getParamType(i + thisOffset);
                if (v && v->getType() != expected) {
                    if (expected->isIntegerTy() && v->getType()->isIntegerTy()) {
                        v = coerceBuilder->CreateIntCast(v, expected, /*isSigned=*/true);
                    } else if (expected->isFloatingPointTy() && v->getType()->isFloatingPointTy()) {
                        v = coerceBuilder->CreateFPCast(v, expected);
                    }
                }
            }
            methodArgs.push_back(v);
        }

        // Dynamic dispatch via the receiver's vtable: hash the method's
        // canonical signature, load the receiver's vtable pointer (instance
        // slot 0), call __cajeta_vtable_lookup to find the function pointer,
        // and indirect-call it. Statics and constructors stay direct — they
        // don't participate in the vtable. The compile-time `method` is the
        // statically-resolved entry; the runtime hash-based lookup is what
        // actually picks the override-correct function in a subclass.
        auto* builder = emitMod->getBuilder();
        auto& llvmCtx = *emitMod->getLlvmContext();
        // Cross-module dispatch: when the receiver class lives in a
        // different llvm::Module than where the call is being
        // emitted (e.g. App's run() calling Provider's __cajeta_inject
        // after multi-source compile), use a module-local extern
        // declaration as the callee rather than a Function* whose
        // parent is a foreign module. ensureFunctionVisible returns
        // method->getLlvmFunction() unchanged when caller and target
        // are co-resident.
        llvm::Value* callee = CajetaModule::ensureFunctionVisible(
            builder, method->getLlvmFunction(),
            method->getLlvmFunctionType());
        // Views have no vtable header (they are typed overlays onto
        // byte buffers; the byte buffer is the value). Methods on
        // views are statically dispatched — the receiver's concrete
        // type is known at the call site. Skip the vtable path
        // entirely; LLVM gets a direct call to the resolved method.
        // P7.6: this used to also cover CajetaStruct; under the
        // unified-class model `struct` is just `class` and gets the
        // normal vtable-dispatch treatment.
        bool isView = dynamic_cast<CajetaView*>(this) != nullptr;
        bool useVtable = thisValue && !isStatic && !isConstructor && !isView;
        bool isInterfaceRecv = this->isInterface();
        if (useVtable && isInterfaceRecv) {
            // S11.2 — reject same-concrete-type return through dyn
            // dispatch. The interface value's fat pointer doesn't carry
            // the underlying struct's size, so a method whose return
            // type is its own implementing struct has nowhere to land
            // the sret slot at the call site. The same call on the
            // concrete struct type (which bypasses this branch entirely
            // via the `isView` skip above) keeps working — the
            // restriction is dyn-dispatch-only. Trigger: return type is
            // a CajetaStruct that itself implements this receiver
            // interface. Implementers that are classes (1-word) or
            // structs returning a different concrete type aren't
            // affected.
            CajetaTypePtr resolvedRet = method->getReturnType();
            if (resolvedRet && resolvedRet->getQName()) {
                // The interface's method may carry a placeholder for a
                // type registered after the interface itself (interfaces
                // tend to forward-reference their implementers). Method::
                // generatePrototype's S8.4 refresh runs once at proto
                // build time, but a CajetaStruct registered later
                // replaces the canonicalMap entry without back-patching
                // existing MethodPtrs. Refresh just-in-time here so the
                // CajetaStruct cast below succeeds.
                auto& cmap = CajetaType::getCanonicalMap();
                auto it = cmap.find(resolvedRet->getQName()->toCanonical());
                if (it != cmap.end() && it->second) {
                    resolvedRet = it->second;
                }
            }
            if (auto structRet = std::dynamic_pointer_cast<CajetaStruct>(
                    resolvedRet)) {
                std::string ifaceCanonical = qName->toCanonical();
                for (auto& iface : structRet->getImplementedInterfaces()) {
                    if (!iface || !iface->getQName()) continue;
                    if (iface->getQName()->toCanonical() == ifaceCanonical) {
                        char buf[640];
                        snprintf(buf, sizeof(buf),
                            "method '%s' on interface '%s' returns its "
                            "implementing struct's own concrete type '%s' "
                            "and cannot be dispatched through an interface "
                            "value; the fat pointer doesn't carry the "
                            "concrete size needed for an sret slot. Call "
                            "the method directly on the concrete struct "
                            "type instead.",
                            method->getName().c_str(),
                            ifaceCanonical.c_str(),
                            structRet->getQName()->toCanonical().c_str());
                        throw Exception(buf,
                            "CAJETA_ERROR_DYN_DISPATCH_OWN_TYPE_RETURN");
                    }
                }
            }

            // S9.5.6 — interface receiver via fat pointer body. Load the
            // per-(impl, iface) vtable from word 1 (the runtime-resolved
            // pointer set by the assignment site that built the fat
            // pointer); load the underlying data from word 0 and use it
            // as the actual `this` for the implementer's function. The
            // vtable is a flat [N x ptr] array in interface-declaration
            // order (per S9.2 / S9.5.2), so dispatch is a GEP-to-index
            // load — no hash lookup needed.
            llvm::Type* ptrTy = llvm::PointerType::get(llvmCtx, 0);
            llvm::Type* bodyTy = this->getLlvmType();

            llvm::Value* dataSlot = builder->CreateStructGEP(
                bodyTy, thisValue, 0, "iface_data_slot");
            llvm::Value* vtableSlot = builder->CreateStructGEP(
                bodyTy, thisValue, 1, "iface_vtable_slot");
            llvm::Value* dataPtr = builder->CreateLoad(
                ptrTy, dataSlot, "iface_data");
            llvm::Value* vtablePtr = builder->CreateLoad(
                ptrTy, vtableSlot, "iface_vtable");

            // Method's index in the interface's method list. Skips
            // constructors and statics for parity with vtable emission
            // (which also skips them per S9.2 and S9.5.2).
            int methodIdx = -1;
            int idx = 0;
            for (auto& im : this->getMethodList()) {
                if (!im || im->isConstructor()) { continue; }
                if (im->getModifiers().find(STATIC) != im->getModifiers().end()) {
                    continue;
                }
                if (im->getName() == method->getName()) {
                    methodIdx = idx;
                    break;
                }
                ++idx;
            }

            if (methodIdx >= 0) {
                // +1 to skip the drop-fn slot at vtable[0] (S10.4).
                llvm::Value* methodSlot = builder->CreateInBoundsGEP(
                    ptrTy, vtablePtr,
                    llvm::ConstantInt::get(llvm::Type::getInt64Ty(llvmCtx),
                        (uint64_t) (methodIdx + 1)),
                    "iface_method_slot");
                callee = builder->CreateLoad(ptrTy, methodSlot, "iface_method_fn");
            }

            // Swap the body pointer for the data pointer at arg position
            // 0 — the implementer's function expects its concrete class
            // instance as `this`, not the interface body.
            if (!methodArgs.empty()) {
                methodArgs[0] = dataPtr;
            }
        } else if (useVtable) {
            llvm::Function* lookupFn = emitMod->getRuntimeFunction("__cajeta_vtable_lookup");
            if (lookupFn) {
                llvm::Type* ptrTy = llvm::PointerType::get(llvmCtx, 0);
                llvm::Type* i64Ty = llvm::Type::getInt64Ty(llvmCtx);
                // The instance's vtable pointer lives at slot 0 — load it.
                // `thisValue` is the receiver heap pointer; first 8 bytes
                // are the vtable* (per CajetaClass::generatePrototype's
                // layout decision).
                llvm::Value* vtable = builder->CreateLoad(ptrTy, thisValue, "vtable");
                int64_t hash = signatureHash(method->toCanonical(/*labeled=*/false));
                llvm::Value* fnPtr = builder->CreateCall(lookupFn,
                    {vtable,
                     llvm::ConstantInt::get(i64Ty, llvm::APInt(64, (uint64_t) hash, false))},
                    "vmethod_fn");
                callee = fnPtr;
            }
        }
        return builder->CreateCall(method->getLlvmFunctionType(),
            callee, llvm::ArrayRef<llvm::Value*>(methodArgs));
    }

    /**
     * Structure of metadata:
     * MetaData[]
     * First, start with the root level classes and work our way up:
     * - ([2B String Length] + [Canonical Class Name UTF8])
     * - Number of methods * ([8B Method Ptr] + [2B String Length] + [Method Name UTF8] + [2B Argument Count] + (A)
     *
     * @param module
     */
    void CajetaClass::generateMetadata() {
        //pModule->getLlvmModule()->getOrInsertGlobal();
    }
} // code