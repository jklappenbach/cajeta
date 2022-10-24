//
// Created by James Klappenbach on 10/22/22.
//

#include <utility>

#include "cajeta/module/CompilationUnit.h"

namespace cajeta {
    map<QualifiedName*, CompilationUnit*> CompilationUnit::archive;

    CompilationUnit* CompilationUnit::create(llvm::LLVMContext* ctxLlvm,
                                             string path,
                                             string sourceRoot,
                                             string archiveRoot,
                                             llvm::TargetMachine* targetMachine,
                                             string targetTriple) {
        CompilationUnit* compilationUnit = new CompilationUnit(ctxLlvm,
                                                               std::move(path),
                                                               std::move(sourceRoot),
                                                               std::move(archiveRoot),
                                                               targetMachine,
                                                               std::move(targetTriple));
        archive[compilationUnit->qName] = compilationUnit;
        return compilationUnit;
    }
}
