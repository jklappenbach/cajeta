//
// Created by James Klappenbach on 10/2/22.
//

#include <cstring>

#include "QualifiedName.h"

namespace cajeta {
    thread_local map<string, map<string, QualifiedNamePtr>> QualifiedName::cache;

    map<string, map<string, QualifiedNamePtr>>& QualifiedName::getCache() { return cache; }

    void QualifiedName::free() {
        for (auto& typeNameEntry: cache) {
            typeNameEntry.second.clear();
        }
        cache.clear();
    }

    QualifiedNamePtr QualifiedName::toArrayType() {
        return getOrInsert(typeName + "Array", packageName);
    }

    QualifiedNamePtr QualifiedName::getOrCreate(string typeName) {
        string package;
        string className;
        char* name = nullptr;
        int value = typeName.find('.');
        if (value >= 0) {
            char* str = (char*) typeName.c_str();
            // strtok_r (not strtok): strtok keeps tokenization state in a global,
            // so concurrent compiles race on it. The saveptr is local.
            char* saveptr = nullptr;
            char* val = strtok_r(str, ".", &saveptr);
            do {
                if (name) {
                    if (!package.empty()) {
                        package.append(".");
                    }
                    package.append(string(name));
                }
                name = val;
            } while ((val = strtok_r(nullptr, ".", &saveptr)));
            className = string(name);
        } else {
            className = typeName;
        }
        map<string, QualifiedNamePtr> packagesToTypeName = cache[package];
        QualifiedNamePtr qName = packagesToTypeName[className];
        if (qName == nullptr) {
            qName = make_shared<QualifiedName>(className, package);
            packagesToTypeName[className] = qName;
            cache[package] = packagesToTypeName;
        }
        return qName;
    }

    QualifiedNamePtr QualifiedName::getOrInsert(string typeName, string packageName) {
        // Key the shared intern cache as cache[package][typeName], same as
        // getOrCreate — the old (typeName-outer) keying meant the two entry
        // points never shared entries, so identical canonical names got
        // distinct shared_ptrs (breaks pointer-identity comparisons).
        map<string, QualifiedNamePtr> packagesToTypeName = cache[packageName];
        QualifiedNamePtr qName = packagesToTypeName[typeName];
        if (qName == nullptr) {
            qName = make_shared<QualifiedName>(typeName, packageName);
            packagesToTypeName[typeName] = qName;
            cache[packageName] = packagesToTypeName;
        }
        return qName;
    }

    QualifiedNamePtr QualifiedName::fromContext(std::vector<CajetaParser::IdentifierContext*> identifiers) {
        string typeName;
        string packageName;

        auto itr = identifiers.begin();
        string current = ((CajetaParser::IdentifierContext*) *itr)->getText();
        itr++;
        if (itr != identifiers.end()) {
            packageName = current;
            while (itr != identifiers.end()) {
                CajetaParser::IdentifierContext* ctxIdentifier = *itr;
                current = ctxIdentifier->getText();
                itr++;
                if (itr != identifiers.end()) {
                    packageName += "." + current;
                } else {
                    typeName = current;
                }
            }
            return getOrInsert(typeName, packageName);
        } else {
            return getOrInsert(current, "code");
        }
    }

    QualifiedNamePtr QualifiedName::fromContext(CajetaParser::QualifiedNameContext* ctxQName) {
        return fromContext(ctxQName->identifier());
    }

    QualifiedNamePtr QualifiedName::fromContext(CajetaParser::ClassOrInterfaceTypeContext* ctxTypeContext) {
        return fromContext(ctxTypeContext->identifier());
    }
}