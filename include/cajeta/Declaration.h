//
// Created by James Klappenbach on 1/22/22.
//

#pragma once

#include <llvm/Support/SMLoc.h>

using namespace llvm;

namespace cajeta {
    class Declaration {
    public:
        enum DeclarationKind {
            DK_Module, DK_Const, DK_Type, DK_Var, DK_Param, DK_Proc
        };
    private:
        const DeclarationKind kind;
    protected:
        Declaration* enclosingDecl;
        SMLoc loc;
        StringRef name;
    public:
        Declaration(DeclarationKind kind, Declaration* enclosingDecl, SMLoc loc, StringRef name) :
                kind(kind),
                enclosingDecl(enclosingDecl),
                loc(loc),
                name(name) {}

        DeclarationKind getKind() const { return kind; }

        SMLoc getLocation() { return loc; }

        StringRef getName() { return name; }

        Declaration* getEnclosingDecl() { return enclosingDecl; }
    };
}