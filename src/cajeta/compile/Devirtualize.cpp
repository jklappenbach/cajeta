#include "Devirtualize.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

namespace cajeta {

    namespace {

        // #VTable layout (StructureMetadata::createVirtualTableType):
        // { i16 version, i16 count, ptr parent, ptr drop_fn, ptr classObject, [N x {i64 hash, ptr fn}] }.
        constexpr unsigned kEntriesField = 5;

        struct Slot {
            std::string fn;               // empty once two entries disagree
            llvm::FunctionType* type = nullptr;
            llvm::Function* def = nullptr;   // a definition, once one is seen
            bool conflict = false;
        };

        void record(std::unordered_map<int64_t, Slot>& slots, int64_t hash, llvm::Constant* fnC) {
            Slot& s = slots[hash];
            if (s.conflict) return;
            auto* fn = fnC ? llvm::dyn_cast<llvm::Function>(fnC->stripPointerCasts()) : nullptr;
            if (!fn) { s.conflict = true; return; }
            if (s.fn.empty() && !s.type) {
                s.fn = fn->getName().str();
                s.type = fn->getFunctionType();
            } else if (s.fn != fn->getName()) {
                s.conflict = true;
                return;
            }
            if (!fn->isDeclaration()) s.def = fn;
        }

        void collect(llvm::Module& m, std::unordered_map<int64_t, Slot>& slots) {
            for (llvm::GlobalVariable& g : m.globals()) {
                if (!g.hasInitializer() || !g.getName().ends_with("#VTable")) continue;
                auto* init = llvm::dyn_cast<llvm::ConstantStruct>(g.getInitializer());
                if (!init || init->getNumOperands() <= kEntriesField) continue;
                auto* entries = llvm::dyn_cast<llvm::ConstantArray>(init->getOperand(kEntriesField));
                if (!entries) continue;
                for (unsigned i = 0; i < entries->getNumOperands(); ++i) {
                    auto* e = llvm::dyn_cast<llvm::ConstantStruct>(entries->getOperand(i));
                    if (!e || e->getNumOperands() < 2) continue;
                    auto* h = llvm::dyn_cast<llvm::ConstantInt>(e->getOperand(0));
                    if (!h) continue;
                    record(slots, h->getSExtValue(), e->getOperand(1));
                }
            }
        }

    } // namespace

    int devirtualizeUniqueSlots(const std::vector<llvm::Module*>& mods) {
        const char* env = std::getenv("CAJETA_DEVIRT");
        if (env && std::strcmp(env, "0") == 0) return 0;
        std::unordered_map<int64_t, Slot> slots;
        for (llvm::Module* m : mods) collect(*m, slots);
        int replaced = 0;
        for (llvm::Module* m : mods) {
            llvm::Function* lookup = m->getFunction("__cajeta_vtable_lookup");
            if (!lookup) continue;
            std::vector<llvm::CallInst*> calls;
            for (llvm::User* u : lookup->users())
                if (auto* c = llvm::dyn_cast<llvm::CallInst>(u))
                    if (c->getCalledFunction() == lookup && c->arg_size() == 2) calls.push_back(c);
            for (llvm::CallInst* c : calls) {
                auto* h = llvm::dyn_cast<llvm::ConstantInt>(c->getArgOperand(1));
                if (!h) continue;
                auto it = slots.find(h->getSExtValue());
                // No definition anywhere means tree-shaking removed the body: keep the lookup.
                if (it == slots.end() || it->second.conflict || !it->second.def) continue;
                const Slot& s = it->second;
                if (&s.type->getContext() != &m->getContext()) continue;
                llvm::Value* callee = nullptr;
                if (s.def && s.def->hasLocalLinkage()) {
                    if (s.def->getParent() != m) continue;   // a local symbol is reachable only in its module
                    callee = s.def;
                } else {
                    callee = m->getOrInsertFunction(s.fn, s.type).getCallee();
                }
                c->replaceAllUsesWith(callee);
                c->eraseFromParent();
                ++replaced;
            }
        }
        return replaced;
    }

}
