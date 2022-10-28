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
        llvm::Twine canonical;

        QualifiedName(string typeName, string packageName) {
            this->typeName = typeName;
            this->packageName = packageName;
            canonical.concat(packageName);
            canonical.concat(".");
            canonical.concat(typeName);
        }
        QualifiedName(const QualifiedName& src) {
            packageName = src.packageName;
            typeName = src.typeName;
            canonical.concat(packageName);
            canonical.concat(".");
            canonical.concat(typeName);
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

        const llvm::Twine& toCanonical() { return canonical; }

        bool operator <(const QualifiedName& rhs) const {
            return typeName < rhs.typeName && packageName < rhs.packageName;
        }

        static map<string, map<string, QualifiedName*>> getCache();
        static QualifiedName* create(string typeName, string packageName);
        static QualifiedName* fromContext(std::vector<CajetaParser::IdentifierContext*> identifiers);
        static QualifiedName* fromContext(CajetaParser::QualifiedNameContext* ctxQName);
        static QualifiedName* fromContext(CajetaParser::ClassOrInterfaceTypeContext* ctxTypeContext);
        static void free();
    };
}
