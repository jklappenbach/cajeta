#include "CrossModuleInline.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/ValueMapper.h"

#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace cajeta {

    namespace {

        bool tooBig(const llvm::Function& f) {
            unsigned n = 0;
            for (const llvm::BasicBlock& bb : f) {
                n += bb.size();
                if (n > kCrossModuleInlineMaxInstructions) return true;
            }
            return false;
        }

        bool isLineFrameCall(const llvm::Instruction& i) {
            auto* cb = llvm::dyn_cast<llvm::CallBase>(&i);
            llvm::Function* callee = cb ? cb->getCalledFunction() : nullptr;
            return callee && callee->getName().starts_with("__cajeta_line_");
        }

        unsigned hotSize(const llvm::Function& f) {
            unsigned n = 0;
            for (const llvm::BasicBlock& bb : f) {
                if (llvm::isa<llvm::UnreachableInst>(bb.getTerminator())) continue;
                for (const llvm::Instruction& i : bb)
                    if (!isLineFrameCall(i) && !i.isDebugOrPseudoInst()) ++n;
            }
            return n;
        }

        bool namesShadowGlobal(const llvm::Value* v, int depth = 0) {
            if (auto* g = llvm::dyn_cast<llvm::GlobalVariable>(v)) {
                llvm::StringRef n = g->getName();
                return n.contains("__cajeta_main_shadow") || n.contains("__cajeta_current_fiber")
                    || n.contains("__cajeta_shadow_cur");
            }
            auto* c = llvm::dyn_cast<llvm::Constant>(v);
            if (!c || llvm::isa<llvm::GlobalValue>(c) || depth > 8) return false;
            for (const llvm::Use& op : c->operands())
                if (namesShadowGlobal(op.get(), depth + 1)) return true;
            return false;
        }

        /** True for the runtime's shadow-stack code, which the deferred line calls hide from the optimizer. */
        bool touchesShadow(const llvm::Function& f) {
            llvm::StringRef n = f.getName();
            if (n.starts_with("__cajeta_line_") || n.starts_with("__cajeta_shadow_")) return true;
            for (const llvm::BasicBlock& bb : f)
                for (const llvm::Instruction& i : bb)
                    for (const llvm::Use& op : i.operands())
                        if (namesShadowGlobal(op.get())) return true;
            return false;
        }

        bool copyable(const llvm::Function& f) {
            if (f.isDeclaration() || f.hasLocalLinkage() || f.isVarArg() || tooBig(f)) return false;
            if (touchesShadow(f)) return false;
            if (f.hasFnAttribute(llvm::Attribute::NoInline) || f.hasFnAttribute(llvm::Attribute::OptimizeNone))
                return false;
            if (f.hasPersonalityFn()) return false;
            for (const llvm::BasicBlock& bb : f)
                for (const llvm::Instruction& i : bb)
                    if (auto* cb = llvm::dyn_cast<llvm::CallBase>(&i))
                        if (cb->hasFnAttr(llvm::Attribute::ReturnsTwice)) return false;
            return true;
        }

        /** Collects the local globals and functions `c` reaches, through constant expressions only. */
        void localsIn(llvm::Constant* c, std::vector<llvm::GlobalValue*>& out, int depth = 0) {
            if (depth > 8) return;
            if (auto* gv = llvm::dyn_cast<llvm::GlobalValue>(c)) {
                if (gv->hasLocalLinkage()) out.push_back(gv);
                return;
            }
            for (llvm::Use& op : c->operands())
                if (auto* oc = llvm::dyn_cast<llvm::Constant>(op.get())) localsIn(oc, out, depth + 1);
        }

        /** Gives each local symbol `f` references a program-unique hidden external name, so a
         *  copy in another module can refer to the one original, as ThinLTO promotion does. */
        void promoteLocals(llvm::Function& f) {
            std::vector<llvm::GlobalValue*> locals;
            for (llvm::BasicBlock& bb : f)
                for (llvm::Instruction& i : bb)
                    for (llvm::Use& op : i.operands())
                        if (auto* c = llvm::dyn_cast<llvm::Constant>(op.get())) localsIn(c, locals);
            std::string tag = std::to_string(std::hash<std::string>{}(f.getParent()->getModuleIdentifier()));
            for (llvm::GlobalValue* gv : locals) {
                if (!gv->hasLocalLinkage()) continue;
                gv->setName("__xm." + tag + "." + gv->getName().str());
                gv->setLinkage(llvm::GlobalValue::ExternalLinkage);
                gv->setVisibility(llvm::GlobalValue::HiddenVisibility);
                if (auto* g = llvm::dyn_cast<llvm::GlobalVariable>(gv)) g->setDSOLocal(true);
            }
        }

        /** Maps each global a copied body references into `dst` as an external declaration. */
        class IntoModule : public llvm::ValueMaterializer {
        public:
            explicit IntoModule(llvm::Module& dst) : dst(dst) {}
            llvm::Value* materialize(llvm::Value* v) override {
                auto* gv = llvm::dyn_cast<llvm::GlobalValue>(v);
                if (!gv || gv->getParent() == &dst) return nullptr;
                if (auto* f = llvm::dyn_cast<llvm::Function>(gv)) {
                    if (llvm::Function* have = dst.getFunction(f->getName())) return have;
                    llvm::Function* nf = llvm::Function::Create(f->getFunctionType(),
                        llvm::GlobalValue::ExternalLinkage, f->getAddressSpace(), f->getName(), &dst);
                    nf->setAttributes(f->getAttributes());
                    nf->setCallingConv(f->getCallingConv());
                    return nf;
                }
                auto* g = llvm::dyn_cast<llvm::GlobalVariable>(gv);
                if (!g) return nullptr;
                if (llvm::GlobalVariable* have = dst.getGlobalVariable(g->getName(), true)) return have;
                return new llvm::GlobalVariable(dst, g->getValueType(), g->isConstant(),
                    llvm::GlobalValue::ExternalLinkage, nullptr, g->getName(), nullptr,
                    g->getThreadLocalMode(), g->getAddressSpace());
            }
            llvm::ValueToValueMapTy map;
        private:
            llvm::Module& dst;
        };

    } // namespace

    int copySmallBodiesIntoCallers(const std::vector<llvm::Module*>& mods) {
        const char* env = std::getenv("CAJETA_XMODULE_INLINE");
        if (env && std::strcmp(env, "0") == 0) return 0;
        std::unordered_map<std::string, llvm::Function*> defs;
        for (llvm::Module* m : mods)
            for (llvm::Function& f : *m)
                if (!f.isDeclaration()) defs.emplace(f.getName().str(), &f);
        int copies = 0;
        for (llvm::Module* m : mods) {
            IntoModule mat(*m);
            std::deque<llvm::Function*> work;
            for (llvm::Function& f : *m)
                if (f.isDeclaration()) work.push_back(&f);
            while (!work.empty()) {
                llvm::Function* decl = work.front();
                work.pop_front();
                if (!decl->isDeclaration() || decl->use_empty()) continue;
                auto it = defs.find(decl->getName().str());
                if (it == defs.end() || it->second->getParent() == m) continue;
                llvm::Function* src = it->second;
                if (!copyable(*src) || src->getFunctionType() != decl->getFunctionType()) continue;
                promoteLocals(*src);
                auto argIt = decl->arg_begin();
                for (llvm::Argument& a : src->args()) mat.map[&a] = &*argIt++;
                llvm::SmallVector<llvm::ReturnInst*, 4> rets;
                llvm::CloneFunctionInto(decl, src, mat.map,
                    llvm::CloneFunctionChangeType::DifferentModule, rets, "", nullptr, nullptr, &mat);
                decl->setLinkage(llvm::GlobalValue::AvailableExternallyLinkage);
                decl->addFnAttr(hotSize(*decl) <= kCrossModuleAlwaysInlineHotInstructions
                    ? llvm::Attribute::AlwaysInline : llvm::Attribute::InlineHint);
                decl->setComdat(nullptr);
                ++copies;
                // Callees the copy declares are candidates too, so flatGet brings Storage.get.
                for (llvm::BasicBlock& bb : *decl)
                    for (llvm::Instruction& i : bb)
                        if (auto* cb = llvm::dyn_cast<llvm::CallBase>(&i))
                            if (llvm::Function* callee = cb->getCalledFunction())
                                if (callee->isDeclaration()) work.push_back(callee);
            }
        }
        return copies;
    }

}
