//
// Created by James Klappenbach on 10/2/22.
//

#include "cajeta/type/QualifiedName.h"

namespace cajeta {
    map<string, map<string, QualifiedName*>> QualifiedName::cache;
    map<string, map<string, QualifiedName*>> QualifiedName::getCache() { return cache; }

    void QualifiedName::free() {
        for (auto & typeNameEntry : cache) {
            for (auto & packageEntry : typeNameEntry.second) {
                delete packageEntry.second;
            }
        }
    }

    QualifiedName* QualifiedName::getOrInsert(string typeName, string packageName) {
        map<string, QualifiedName*> packagesToTypeName = cache[typeName];
        QualifiedName* qName = packagesToTypeName[packageName];
        if (qName == nullptr) {
            qName = new QualifiedName(typeName, packageName);
            packagesToTypeName[packageName] = qName;
            cache[typeName] = packagesToTypeName;
        }
        return qName;
    }

    QualifiedName* QualifiedName::fromContext(std::vector<CajetaParser::IdentifierContext*> identifiers) {
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

    QualifiedName* QualifiedName::fromContext(CajetaParser::QualifiedNameContext* ctxQName) {
        return fromContext(ctxQName->identifier());
    }

    QualifiedName* QualifiedName::fromContext(CajetaParser::ClassOrInterfaceTypeContext* ctxTypeContext) {
        return fromContext(ctxTypeContext->identifier());
    }
}