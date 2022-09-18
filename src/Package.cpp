//
// Created by James Klappenbach on 9/17/22.
//

#include "Parser/Package.h"

using namespace std;

mutex Package::mtxPackages;
mutex Package::mtxTypes;
map<string, Package*> Package::packages;

Package* Package::create(const string& rootRelativePath) {
    const std::lock_guard<std::mutex> lock(mtxPackages);
    Package* package = packages[rootRelativePath];
    if (package == nullptr) {
        package = new Package(rootRelativePath);
        packages[package->packageName] = package;
    }
    return package;
}

Package* Package::get(const string& rootRelativePath) {
    const std::lock_guard<std::mutex> lock(mtxPackages);
    return packages[rootRelativePath];
}
