//
// Created by James Klappenbach on 10/1/22.
//

#pragma once
#include <string>
#include "CajetaParser.h"
#include <map>
#include <llvm/ADT/Twine.h>

using namespace std;

namespace cajeta {

    class QualifiedName {
    private:
        static map<string, map<string, QualifiedName*>> cache;
        string packageName;
        string typeName;
        string canonical;

        QualifiedName(string typeName, string packageName) {
            this->typeName = typeName;
            this->packageName = packageName;
            canonical.append(packageName);
            canonical.append(".");
            canonical.append(typeName);
        }
        QualifiedName(const QualifiedName& src) {
            packageName = src.packageName;
            typeName = src.typeName;
            canonical.append(packageName);
            canonical.append(".");
            canonical.append(typeName);
        }
    public:
        const string& getPackageName() const {
            return packageName;
        }

        const string& getTypeName() const {
            return typeName;
        }

        const string toString() {
            return packageName + "." + typeName;
        }

        const string& toCanonical() { return canonical; }

        bool operator <(const QualifiedName& rhs) const {
            return typeName < rhs.typeName && packageName < rhs.packageName;
        }

        static map<string, map<string, QualifiedName*>> getCache();
        static QualifiedName* getOrInsert(string typeName, string packageName);
        static QualifiedName* fromContext(std::vector<CajetaParser::IdentifierContext*> identifiers);
        static QualifiedName* fromContext(CajetaParser::QualifiedNameContext* ctxQName);
        static QualifiedName* fromContext(CajetaParser::ClassOrInterfaceTypeContext* ctxTypeContext);
        static void free();
    };
}