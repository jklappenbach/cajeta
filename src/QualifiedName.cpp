//
// Created by James Klappenbach on 10/2/22.
//

#include "cajeta/module/QualifiedName.h"

namespace cajeta {
    map<string, map<string, QualifiedName*>> QualifiedName::global;
    map<string, map<string, QualifiedName*>> QualifiedName::getGlobal() { return global; }

    void QualifiedName::free() {
        for (auto & typeNameEntry : global) {
            for (auto & packageEntry : typeNameEntry.second) {
                delete packageEntry.second;
            }
        }
    }

    QualifiedName* QualifiedName::toQualifiedName(string typeName) {
        map<string, QualifiedName*> packagesToTypeName = global[typeName];
        auto itr = packagesToTypeName.begin();
        QualifiedName* qName = nullptr;
        if (itr != packagesToTypeName.end()) {
            qName = itr->second;
        }

        return qName;
    }

    QualifiedName* QualifiedName::toQualifiedName(string typeName, string packageName) {
        map<string, QualifiedName*> packagesToTypeName = global[typeName];
        QualifiedName* qName = packagesToTypeName[packageName];
        if (qName == nullptr) {
            qName = new QualifiedName(typeName, packageName);
            packagesToTypeName[packageName] = qName;
            global[typeName] = packagesToTypeName;
        }
        return qName;
    }

    QualifiedName* QualifiedName::toQualifiedName(std::vector<CajetaParser::IdentifierContext*> identifiers) {
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
            return toQualifiedName(typeName, packageName);
        } else {
            typeName = current;
            return toQualifiedName(typeName);
        }
    }

    QualifiedName* QualifiedName::toQualifiedName(CajetaParser::QualifiedNameContext* ctxQName) {
        return toQualifiedName(ctxQName->identifier());
    }

    QualifiedName* QualifiedName::toQualifiedName(CajetaParser::ClassOrInterfaceTypeContext* ctxTypeContext) {
        return toQualifiedName(ctxTypeContext->identifier());
    }
}