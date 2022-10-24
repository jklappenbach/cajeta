//
// Created by James Klappenbach on 2/19/22.
//

#pragma once

#include <string>
#include <llvm/IR/Type.h>


using namespace std;

namespace cajeta {
    struct TypeParameter {
        string parameterName;
        TypeParameter(string parameterName) {
            this->parameterName = parameterName;
        }
    };


    struct ExtendedTypeParameter : TypeParameter {
        string extends;
        llvm::Type* type;
        ExtendedTypeParameter(string parameterName, string extends) : TypeParameter(parameterName) {
            this->extends = extends;
        }
    };

}