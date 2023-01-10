//
// Created by James Klappenbach on 10/2/22.
//

#include "QualifiedName.h"

namespace cajeta {
    map<string, map<string, QualifiedNamePtr>> QualifiedName::cache;

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
            char* val = std::strtok(str, ".");
            do {
                if (name) {
                    if (!package.empty()) {
                        package.append(".");
                    }
                    package.append(string(name));
                }
                name = val;
            } while ((val = std::strtok(nullptr, ".")));
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
        map<string, QualifiedNamePtr> packagesToTypeName = cache[typeName];
        QualifiedNamePtr qName = packagesToTypeName[packageName];
        if (qName == nullptr) {
            qName = make_shared<QualifiedName>(typeName, packageName);
            packagesToTypeName[packageName] = qName;
            cache[typeName] = packagesToTypeName;
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
            return getOrInsert(current, "cajeta");
        }
    }

    QualifiedNamePtr QualifiedName::fromContext(CajetaParser::QualifiedNameContext* ctxQName) {
        return fromContext(ctxQName->identifier());
    }

    QualifiedNamePtr QualifiedName::fromContext(CajetaParser::ClassOrInterfaceTypeContext* ctxTypeContext) {
        return fromContext(ctxTypeContext->identifier());
    }
}