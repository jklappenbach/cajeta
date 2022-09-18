//
// Created by James Klappenbach on 5/22/22.
//

#pragma once

#include <list>
#include <iostream>
#include <string>
#include <sstream>
#include <map>
#include "cajeta/TypeDefinition.h"
#include <mutex>

using namespace std;

#ifdef __WINDOWS__
#define PATH_SEPARATOR      '\\'
#else
#define PATH_SEPARATOR      '/'
#endif

#define PACKAGE_SEPARATOR   '.'
#define CAJETA_SUFFIX       ".cajeta"

using namespace cajeta;
using namespace std;

class Package {
private:
    static map<string, Package*> packages;
    static std::mutex mtxPackages;
    static std::mutex mtxTypes;
    string name;
    string packageName;
    TypeDefinition* typeDefinition;
    map<string, TypeDefinition*> typeDefinitions;
    Package(const string& rootRelativePath) {
        packageName = rootRelativePath;
        replace( packageName.begin(), packageName.end(),
                 PATH_SEPARATOR, PACKAGE_SEPARATOR);

        if (packageName.find(CAJETA_SUFFIX) >= 0) {
            int length = packageName.length();
            int cajetaSuffixIndex = length - strlen(CAJETA_SUFFIX);
            int classNameDotIndex = packageName.rfind('.', cajetaSuffixIndex - 1);
            packageName = rootRelativePath.substr(0, classNameDotIndex);
        }
    }
public:
    static Package* create(const string& rootRelativePath);

    static Package* get(const string& rootRelativePath);

    void addTypeDefinition(TypeDefinition* typeDefinition) {
        const std::lock_guard<std::mutex> lock(mtxTypes);
        typeDefinitions[typeDefinition->name] = typeDefinition;
    }

    TypeDefinition* getTypeDefinition(string name) {
        const std::lock_guard<std::mutex> lock(mtxTypes);
        return typeDefinitions[name];
    }
};