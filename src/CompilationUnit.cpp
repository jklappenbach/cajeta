//
// Created by James Klappenbach on 10/1/22.
//

#include "cajeta/CompilationUnit.h"

using namespace cajeta;

const string& CompilationUnit::getPackageName() const {
    return packageName;
}

void CompilationUnit::setPackageName(const string& packageName) {
    this->packageName = packageName;
}

set<QualifiedName*>& cajeta::CompilationUnit::getImports() {
    return imports;
}

void CompilationUnit::setImports(const set<QualifiedName*>& imports) {
    this->imports = imports;
}

map<QualifiedName*, Type*>& CompilationUnit::getTypes() {
    return types;
}

void CompilationUnit::setTypes(const map<QualifiedName*, Type*>& classes) {
    this->types = classes;
}
