//
// Created by James Klappenbach on 10/22/22.
//

#pragma once

#include "../asn/AbstractSyntaxNode.h"
#include "llvm/IR/Module.h"
#include "../type/QualifiedName.h"
#include "../type/CajetaClass.h"
#include "../method/Method.h"
#include "support/Any.h"
#include <string>
#include <fstream>
#include <filesystem>
#include <queue>
#include <llvm/Support/raw_os_ostream.h>
#include "../type/ScopeStack.h"

using namespace std;
using std::ofstream;

#define PATH_SEPARATOR              '/'
#define PACKAGE_SEPARATOR           '.'
#define CAJETA_EXTENSION            ".cajeta"
#define CAJETA_IR_EXTENSION         ".ll"

namespace cajeta {
    class StructureMetadata;
    typedef shared_ptr<StructureMetadata> StructureMetadataPtr;

    class CajetaClass;

    class CajetaModule : public enable_shared_from_this<CajetaModule> {
    private:
        static map<string, MethodPtr> methods;
        static map<string, CajetaModulePtr> strutureToModule;
        static map<string, CajetaModulePtr> moduleVariables;

        // The module currently being walked (parse pass or template-
        // instantiation walk). Used as a fallback by call sites that don't
        // thread `module` through their APIs — notably the parse-time
        // Expression / Type construction path, which is several layers deep
        // off the visitor. Set by `Compiler::parse` and by
        // `CajetaClass::instantiate` around their respective walks.
        // Single-threaded compilation, so a plain pointer is enough; if we
        // ever parallelize parsing this becomes thread_local.
        static CajetaModulePtr activeModule;


        map<string, map<string, QualifiedNamePtr>> imports;
        QualifiedNamePtr qName;
        string sourcePath;
        string sourceRoot;
        string archiveRoot;
        string archivePath;

        map<string, CajetaClassPtr> structures;
        MethodPtr currentMethod;
        StructureMetadataPtr structureMetadata;

        // Compiler-level options that codegen consults. Set on the module by the
        // Compiler at creation time (so each module produces IR consistent with the
        // current invocation's CLI flags).
        bool boundsCheckEnabled = true;

    public:
        // Active loop targets for break/continue. Each loop pushes its targets on
        // entry to its body codegen and pops on exit; break/continue read the
        // innermost frame.
        struct LoopContext {
            llvm::BasicBlock* continueTarget;
            llvm::BasicBlock* breakTarget;
        };

    private:
        std::vector<LoopContext> loopContextStack;

        // Type-parameter substitution stack for template instantiation. Each
        // frame is a map from parameter name (T, K, V, ...) to the concrete
        // CajetaTypePtr it resolves to during the current instantiation walk.
        // Pushed on entry to `CajetaClass::instantiate`, popped on exit.
        //
        // Only the TOP frame is consulted by type resolution — parameters
        // never leak across instantiations. When `Pair<A,B> extends Tuple<A>`
        // is materialized, `A` is substituted to `int32` *before* recursing
        // into Tuple's instantiation; Tuple's frame contains `T → int32`,
        // not Pair's `A`. v1 doesn't support nested template classes, so a
        // single-level lookup is sufficient.
        std::vector<std::map<std::string, CajetaTypePtr>> typeSubstitutionStack;

        // Current state
        ScopeStack scopeStack;
        list<CajetaClassPtr> structureStack;
        list<MethodPtr> toGenerate;
        llvm::Module* llvmModule;
        llvm::IRBuilder<>* builder;
        llvm::LLVMContext* llvmContext;
        llvm::TargetMachine* targetMachine;
        CajetaTypePtr initializerType;
        string targetTriple;


    public:
        CajetaModule(llvm::LLVMContext* llvmContext,
            string sourcePath,
            string sourceRoot,
            string archiveRoot,
            string targetTriple,
            llvm::TargetMachine* targetMachine);

        QualifiedNamePtr getQName() const {
            return qName;
        }

        void setQName(QualifiedNamePtr qName) {
            CajetaModule::qName = qName;
        }

        list<CajetaClassPtr>& getStructureStack() { return structureStack; }

        llvm::LLVMContext* getLlvmContext() {
            return llvmContext;
        }

        llvm::Module* getLlvmModule() const {
            return llvmModule;
        }

        void setBuilder(llvm::IRBuilder<>* builder) {
            this->builder = builder;
        }

        llvm::IRBuilder<>* getBuilder() {
            return this->builder;
        }

        CajetaTypePtr getInitializerType() const;

        void setInitializerType(CajetaTypePtr initializerType);

        const string& getSourcePath() const {
            return sourcePath;
        }

        void setSourcePath(const string& sourcePath) {
            this->sourcePath = sourcePath;
        }

        const string& getArchiveRoot() const {
            return archiveRoot;
        }

        void setArchiveRoot(const string& archiveRoot) {
            this->archiveRoot = archiveRoot;
        }

        const string& getArchivePath() const {
            return archivePath;
        }

        void setArchivePath(const string& archivePath) {
            this->archivePath = archivePath;
        }

        map<string, map<string, QualifiedNamePtr>>& getImports() {
            return imports;
        }

        // Template-instantiation substitution stack — see field declaration
        // for semantics. Push before walking a template body with concrete
        // arguments; pop when done.
        void pushTypeSubstitution(std::map<std::string, CajetaTypePtr> frame) {
            typeSubstitutionStack.push_back(std::move(frame));
        }
        void popTypeSubstitution() {
            if (!typeSubstitutionStack.empty()) typeSubstitutionStack.pop_back();
        }
        // Returns nullptr if `name` isn't bound by the current top frame.
        // Only the top is consulted; parameters never leak across nested
        // instantiations (see field-comment rationale).
        CajetaTypePtr lookupTypeParameter(const std::string& name) const {
            if (typeSubstitutionStack.empty()) return nullptr;
            const auto& top = typeSubstitutionStack.back();
            auto it = top.find(name);
            return it == top.end() ? nullptr : it->second;
        }

        map<string, CajetaClassPtr>& getStructures() {
            return structures;
        }

        ScopeStack& getScopeStack() {
            return scopeStack;
        }

        static map<string, CajetaModulePtr>& getStructureToModule() {
            return strutureToModule;
        }

        // Active-module accessor. Returns the module currently being walked,
        // or nullptr outside any walk. Call sites that didn't thread a module
        // parameter through (parse-time Expression / Type construction) read
        // this. See the activeModule field for set/clear discipline.
        static CajetaModulePtr getActiveModule() { return activeModule; }
        static void setActiveModule(CajetaModulePtr m) { activeModule = m; }

        static map<string, CajetaModulePtr>& getModuleVariables() {
            return moduleVariables;
        }

        // Clear cross-Compiler module/method bookkeeping. Used by Compiler's ctor so
        // each fresh Compiler instance starts with empty static state.
        static void resetGlobals();

        llvm::IRBuilder<>* getBuilder() const;

        bool isBoundsCheckEnabled() const { return boundsCheckEnabled; }
        void setBoundsCheckEnabled(bool v) { boundsCheckEnabled = v; }

        void pushLoopContext(llvm::BasicBlock* cont, llvm::BasicBlock* brk) {
            loopContextStack.push_back({cont, brk});
        }
        void popLoopContext() { if (!loopContextStack.empty()) loopContextStack.pop_back(); }
        bool hasLoopContext() const { return !loopContextStack.empty(); }
        const LoopContext& currentLoopContext() const { return loopContextStack.back(); }

        void processMetadata(CajetaClassPtr structure);

        // Parse the embedded cajeta_runtime bitcode and Linker::linkModules-merge it
        // into this module. Idempotent — safe to call multiple times; subsequent calls
        // are no-ops. Returns true on success.
        bool linkRuntime();

        // Look up a runtime helper by name (must be linked first via linkRuntime()).
        // Returns nullptr if missing.
        llvm::Function* getRuntimeFunction(const std::string& name);

        void writeIRFileTarget() {
            string targetPath = archiveRoot + archivePath;
            std::error_code ec;

            string targetDirs = targetPath.substr(0, targetPath.rfind("/"));
            std::filesystem::create_directories(targetDirs);
            llvm::raw_ostream* out = new llvm::raw_fd_ostream(targetPath, ec, llvm::sys::fs::CD_CreateAlways);
            printf("\n\n");
            llvmModule->print(llvm::outs(), nullptr);
            llvmModule->print(*out, nullptr);
        }

        void setCurrentMethod(MethodPtr method) {
            this->currentMethod = method;
        }

        MethodPtr getCurrentMethod() {
            return currentMethod;
        }

        list<MethodPtr> getAllMethods() {
            list<MethodPtr> result;
            for (auto entry:  structures) {
                // Templates have no concrete type and their bodies reference
                // unresolved type parameters; their methods can't be lowered.
                // Concrete instantiations (e.g. Box<int32>) are codegen'd via
                // their own entries in `structures`.
                if (entry.second->isTemplate()) continue;
                for (auto methodEntry: entry.second->getMethods()) {
                    result.push_back(methodEntry.second);
                }
            }
            return result;
        }

        void onPackageDeclaration(CajetaParser::PackageDeclarationContext* ctx);

        void onImportDeclaration(CajetaParser::ImportDeclarationContext* ctx);

        void onStructureDeclaration(std::any any);

        static CajetaModulePtr create(llvm::LLVMContext* llvmContext,
            string sourcePath,
            string sourceRoot,
            string archiveRoot,
            string targetTriple,
            llvm::TargetMachine* targetMachine);
    };

    typedef shared_ptr<CajetaModule> CajetaModulePtr;
}

