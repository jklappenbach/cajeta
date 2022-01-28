#pragma once

#include "tinylang/Basic/LLVM.h"
#include "tinylang/Basic/TokenKinds.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/SMLoc.h"
#include <string>
#include <vector>

namespace cajeta {

    class Declaration;

    class FormalParameterDeclaration;

    class Expression;

    class Selector;

    class Stmt;

    class TypeDeclaration;

    using DeclList = std::vector<Declaration*>;
    using FormalParamList = std::vector<FormalParameterDeclaration*>;
    using ExprList = std::vector<Expression*>;
    using SelectorList = std::vector<Selector*>;
    using StmtList = std::vector<Stmt*>;
    using IdentList = std::vector<std::pair<SMLoc, StringRef>>;

    class Field {
        SMLoc Loc;
        StringRef Name;
        TypeDeclaration* Type;

    public:
        Field(SMLoc Loc, const StringRef& Name,
              TypeDeclaration* Type)
                : Loc(Loc), Name(Name), Type(Type) {}

        SMLoc getLoc() const { return Loc; }

        const StringRef& getName() const { return Name; }

        TypeDeclaration* getType() const { return Type; }
    };

    using FieldList = std::vector<Field>;

    class Declaration {
    public:
        enum DeclarationKind {
            DK_Module,
            DK_Const,
            DK_AliasType,
            DK_ArrayType,
            DK_PervasiveType,
            DK_PointerType,
            DK_RecordType,
            DK_Var,
            DK_Param,
            DK_Proc
        };

    private:
        const DeclarationKind kind;

    protected:
        Declaration* enclosingDecl;
        SMLoc loc;
        StringRef name;

    public:
        Declaration(DeclarationKind Kind, Declaration* EnclosingDecL, SMLoc Loc,
                    StringRef Name)
                : kind(Kind), enclosingDecl(EnclosingDecL), loc(Loc),
                  name(Name) {}

        DeclarationKind getKind() const { return kind; }

        SMLoc getLocation() { return loc; }

        StringRef getName() { return name; }

        Declaration* getEnclosingDecl() { return enclosingDecl; }
    };

    class ModuleDeclaration : public Declaration {
        DeclList Decls;
        StmtList Stmts;

    public:
        ModuleDeclaration(Declaration* EnclosingDecL, SMLoc Loc,
                          StringRef Name)
                : Declaration(DK_Module, EnclosingDecL, Loc, Name) {}

        ModuleDeclaration(Declaration* EnclosingDecL, SMLoc Loc,
                          StringRef Name, DeclList& Decls,
                          StmtList& Stmts)
                : Declaration(DK_Module, EnclosingDecL, Loc, Name),
                  Decls(Decls), Stmts(Stmts) {}

        const DeclList& getDecls() { return Decls; }

        void setDecls(DeclList& D) { Decls = D; }

        const StmtList& getStmts() { return Stmts; }

        void setStmts(StmtList& L) { Stmts = L; }

        static bool classof(const Declaration* D) {
            return D->getKind() == DK_Module;
        }
    };

    class ConstantDeclaration : public Declaration {
        Expression* E;

    public:
        ConstantDeclaration(Declaration* EnclosingDecL, SMLoc Loc,
                            StringRef Name, Expression* E)
                : Declaration(DK_Const, EnclosingDecL, Loc, Name), E(E) {}

        Expression* getExpr() { return E; }

        static bool classof(const Declaration* D) {
            return D->getKind() == DK_Const;
        }
    };

    class TypeDeclaration : public Declaration {
    protected:
        TypeDeclaration(DeclarationKind Kind, Declaration* EnclosingDecL,
                        SMLoc Loc, StringRef Name)
                : Declaration(Kind, EnclosingDecL, Loc, Name) {}

    public:
        static bool classof(const Declaration* D) {
            return D->getKind() >= DK_AliasType &&
                   D->getKind() <= DK_RecordType;
        }
    };

    class AliasTypeDeclaration : public TypeDeclaration {
        TypeDeclaration* Type;

    public:
        AliasTypeDeclaration(Declaration* EnclosingDecL, SMLoc Loc,
                             StringRef Name,
                             TypeDeclaration* Type)
                : TypeDeclaration(DK_AliasType, EnclosingDecL, Loc,
                                  Name),
                  Type(Type) {}

        TypeDeclaration* getType() const { return Type; }

        static bool classof(const Declaration* D) {
            return D->getKind() == DK_AliasType;
        }
    };

    class ArrayTypeDeclaration : public TypeDeclaration {
        Expression* Nums;
        TypeDeclaration* Type;

    public:
        ArrayTypeDeclaration(Declaration* EnclosingDecL, SMLoc Loc,
                             StringRef Name, Expression* Nums,
                             TypeDeclaration* Type)
                : TypeDeclaration(DK_ArrayType, EnclosingDecL, Loc,
                                  Name),
                  Nums(Nums), Type(Type) {}

        Expression* getNums() const { return Nums; }

        TypeDeclaration* getType() const { return Type; }

        static bool classof(const Declaration* D) {
            return D->getKind() == DK_ArrayType;
        }
    };

    class PervasiveTypeDeclaration : public TypeDeclaration {
    public:
        PervasiveTypeDeclaration(Declaration* EnclosingDecL, SMLoc Loc,
                                 StringRef Name)
                : TypeDeclaration(DK_PervasiveType, EnclosingDecL,
                                  Loc, Name) {}

        static bool classof(const Declaration* D) {
            return D->getKind() == DK_PervasiveType;
        }
    };

    class PointerTypeDeclaration : public TypeDeclaration {
        TypeDeclaration* Type;

    public:
        PointerTypeDeclaration(Declaration* EnclosingDecL, SMLoc Loc,
                               StringRef Name,
                               TypeDeclaration* Type)
                : TypeDeclaration(DK_PointerType, EnclosingDecL, Loc,
                                  Name),
                  Type(Type) {}

        TypeDeclaration* getType() const { return Type; }

        static bool classof(const Declaration* D) {
            return D->getKind() == DK_PointerType;
        }
    };

    class RecordTypeDeclaration : public TypeDeclaration {
        FieldList Fields;

    public:
        RecordTypeDeclaration(Declaration* EnclosingDecL, SMLoc Loc,
                              StringRef Name,
                              const FieldList& Fields)
                : TypeDeclaration(DK_RecordType, EnclosingDecL, Loc,
                                  Name),
                  Fields(Fields) {}

        const FieldList& getFields() const { return Fields; }

        static bool classof(const Declaration* D) {
            return D->getKind() == DK_RecordType;
        }
    };

    class VariableDeclaration : public Declaration {
        TypeDeclaration* Ty;

    public:
        VariableDeclaration(Declaration* EnclosingDecL, SMLoc Loc,
                            StringRef Name, TypeDeclaration* Ty)
                : Declaration(DK_Var, EnclosingDecL, Loc, Name), Ty(Ty) {}

        TypeDeclaration* getType() { return Ty; }

        static bool classof(const Declaration* D) {
            return D->getKind() == DK_Var;
        }
    };

    class FormalParameterDeclaration : public Declaration {
        TypeDeclaration* Ty;
        bool IsVar;

    public:
        FormalParameterDeclaration(Declaration* EnclosingDecL, SMLoc Loc,
                                   StringRef Name,
                                   TypeDeclaration* Ty,
                                   bool IsVar)
                : Declaration(DK_Param, EnclosingDecL, Loc, Name), Ty(Ty),
                  IsVar(IsVar) {}

        TypeDeclaration* getType() const { return Ty; }

        bool isVar() const { return IsVar; }

        static bool classof(const Declaration* D) {
            return D->getKind() == DK_Param;
        }
    };

    class ProcedureDeclaration : public Declaration {
        FormalParamList Params;
        TypeDeclaration* RetType;
        DeclList Decls;
        StmtList Stmts;

    public:
        ProcedureDeclaration(Declaration* EnclosingDecL, SMLoc Loc,
                             StringRef Name)
                : Declaration(DK_Proc, EnclosingDecL, Loc, Name) {}

        ProcedureDeclaration(Declaration* EnclosingDecL, SMLoc Loc,
                             StringRef Name,
                             FormalParamList& Params,
                             TypeDeclaration* RetType,
                             DeclList& Decls, StmtList& Stmts)
                : Declaration(DK_Proc, EnclosingDecL, Loc, Name),
                  Params(Params), RetType(RetType), Decls(Decls),
                  Stmts(Stmts) {}

        const FormalParamList& getFormalParams() {
            return Params;
        }

        void setFormalParams(FormalParamList& FP) { Params = FP; }

        TypeDeclaration* getRetType() { return RetType; }

        void setRetType(TypeDeclaration* Ty) { RetType = Ty; }

        const DeclList& getDecls() { return Decls; }

        void setDecls(DeclList& D) { Decls = D; }

        const StmtList& getStmts() { return Stmts; }

        void setStmts(StmtList& L) { Stmts = L; }

        static bool classof(const Declaration* D) {
            return D->getKind() == DK_Proc;
        }
    };

    class OperatorInfo {
        SMLoc loc;
        uint32_t kind: 16;
        uint32_t isUnspecified: 1;

    public:
        OperatorInfo()
                : loc(), kind(tok::unknown), isUnspecified(true) {}

        OperatorInfo(SMLoc loc, tok::TokenKind kind,
                     bool isUnspecified = false)
                : loc(loc), kind(Kind), isUnspecified(isUnspecified) {
        }

        SMLoc getLocation() const { return loc; }

        tok::TokenKind getKind() const {
            return static_cast<tok::TokenKind>(kind);
        }

        bool isUnspecified() const { return isUnspecified; }
    };

    class Expression {
    public:
        enum ExpressionKind {
            EK_Infix,
            EK_Prefix,
            EK_Int,
            EK_Bool,
            EK_Designator,
            EK_Const,
            EK_Func,
        };

    private:
        const ExpressionKind Kind;
        TypeDeclaration* typeDeclaration;
        bool IsConstant;

    protected:
        Expression(ExpressionKind Kind, TypeDeclaration* Ty, bool IsConst)
                : Kind(Kind), typeDeclaration(Ty), IsConstant(IsConst) {}

    public:
        ExpressionKind getKind() const { return Kind; }

        TypeDeclaration* getType() { return typeDeclaration; }

        void setType(TypeDeclaration* T) { typeDeclaration = T; }

        bool isConst() { return IsConstant; }
    };

    class InfixExpression : public Expression {
        Expression* Left;
        Expression* Right;
        const OperatorInfo Op;

    public:
        InfixExpression(Expression* Left, Expression* Right, OperatorInfo Op,
                        TypeDeclaration* Ty, bool IsConst)
                : Expression(EK_Infix, Ty, IsConst), Left(Left),
                  Right(Right), Op(Op) {}

        Expression* getLeft() { return Left; }

        Expression* getRight() { return Right; }

        const OperatorInfo& getOperatorInfo() { return Op; }

        static bool classof(const Expression* E) {
            return E->getKind() == EK_Infix;
        }
    };

    class PrefixExpression : public Expression {
        Expression* E;
        const OperatorInfo Op;

    public:
        PrefixExpression(Expression* E, OperatorInfo Op,
                         TypeDeclaration* Ty, bool IsConst)
                : Expression(EK_Prefix, Ty, IsConst), E(E), Op(Op) {}

        Expression* getExpr() { return E; }

        const OperatorInfo& getOperatorInfo() { return Op; }

        static bool classof(const Expression* E) {
            return E->getKind() == EK_Prefix;
        }
    };

    class IntegerLiteral : public Expression {
        SMLoc Loc;
        llvm::APSInt Value;

    public:
        IntegerLiteral(SMLoc Loc, const llvm::APSInt& Value,
                       TypeDeclaration* Ty)
                : Expression(EK_Int, Ty, true), Loc(Loc), Value(Value) {}

        llvm::APSInt& getValue() { return Value; }

        static bool classof(const Expression* E) {
            return E->getKind() == EK_Int;
        }
    };

    class BooleanLiteral : public Expression {
        bool Value;

    public:
        BooleanLiteral(bool Value, TypeDeclaration* Ty)
                : Expression(EK_Bool, Ty, true), Value(Value) {}

        bool getValue() { return Value; }

        static bool classof(const Expression* E) {
            return E->getKind() == EK_Bool;
        }
    };

    class Selector {
    public:
        enum SelectorKind {
            SK_Index,
            SK_Field,
            SK_Dereference,
        };

    private:
        const SelectorKind Kind;

        // The type decribes the base type.
        // E.g. the component type of an index selector
        TypeDeclaration* Type;

    protected:
        Selector(SelectorKind Kind, TypeDeclaration* Type)
                : Kind(Kind), Type(Type) {}

    public:
        SelectorKind getKind() const { return Kind; }

        TypeDeclaration* getType() const { return Type; }
    };

    class IndexSelector : public Selector {
        Expression* Index;

    public:
        IndexSelector(Expression* Index, TypeDeclaration* Type)
                : Selector(SK_Index, Type), Index(Index) {}

        Expression* getIndex() const { return Index; }

        static bool classof(const Selector* Sel) {
            return Sel->getKind() == SK_Index;
        }
    };

    class FieldSelector : public Selector {
        uint32_t Index;
        StringRef Name;

    public:
        FieldSelector(uint32_t Index, StringRef Name,
                      TypeDeclaration* Type)
                : Selector(SK_Field, Type), Index(Index), Name(Name) {
        }

        uint32_t getIndex() const { return Index; }

        const StringRef& getname() const { return Name; }

        static bool classof(const Selector* Sel) {
            return Sel->getKind() == SK_Field;
        }
    };

    class DereferenceSelector : public Selector {
    public:
        DereferenceSelector(TypeDeclaration* Type)
                : Selector(SK_Dereference, Type) {}

        static bool classof(const Selector* Sel) {
            return Sel->getKind() == SK_Dereference;
        }
    };

    class Designator : public Expression {
        Declaration* Var;
        SelectorList Selectors;

    public:
        Designator(VariableDeclaration* Var)
                : Expression(EK_Designator, Var->getType(), false),
                  Var(Var) {}

        Designator(FormalParameterDeclaration* Param)
                : Expression(EK_Designator, Param->getType(), false),
                  Var(Param) {}

        void addSelector(Selector* Sel) {
            Selectors.push_back(Sel);
            setType(Sel->getType());
        }

        Declaration* getDecl() { return Var; }

        const SelectorList& getSelectors() const {
            return Selectors;
        }

        static bool classof(const Expression* E) {
            return E->getKind() == EK_Designator;
        }
    };

    class ConstantAccess : public Expression {
        ConstantDeclaration* Const;

    public:
        ConstantAccess(ConstantDeclaration* Const)
                : Expression(EK_Const, Const->getExpr()->getType(), true),
                  Const(Const) {}

        ConstantDeclaration* getDecl() { return Const; }

        static bool classof(const Expression* E) {
            return E->getKind() == EK_Const;
        }
    };

    class FunctionCallExpr : public Expression {
        ProcedureDeclaration* Proc;
        ExprList Params;

    public:
        FunctionCallExpr(ProcedureDeclaration* Proc,
                         ExprList Params)
                : Expression(EK_Func, Proc->getRetType(), false),
                  Proc(Proc), Params(Params) {}

        ProcedureDeclaration* geDecl() { return Proc; }

        const ExprList& getParams() { return Params; }

        static bool classof(const Expression* E) {
            return E->getKind() == EK_Func;
        }
    };

    class Stmt {
    public:
        enum StmtKind {
            SK_Assign,
            SK_ProcCall,
            SK_If,
            SK_While,
            SK_Return
        };

    private:
        const StmtKind Kind;

    protected:
        Stmt(StmtKind Kind) : Kind(Kind) {}

    public:
        StmtKind getKind() const { return Kind; }
    };

    class AssignmentStatement : public Stmt {
        Designator* Var;
        Expression* E;

    public:
        AssignmentStatement(Designator* Var, Expression* E)
                : Stmt(SK_Assign), Var(Var), E(E) {}

        Designator* getVar() { return Var; }

        Expression* getExpr() { return E; }

        static bool classof(const Stmt* S) {
            return S->getKind() == SK_Assign;
        }
    };

    class ProcedureCallStatement : public Stmt {
        ProcedureDeclaration* Proc;
        ExprList Params;

    public:
        ProcedureCallStatement(ProcedureDeclaration* Proc,
                               ExprList& Params)
                : Stmt(SK_ProcCall), Proc(Proc), Params(Params) {}

        ProcedureDeclaration* getProc() { return Proc; }

        const ExprList& getParams() { return Params; }

        static bool classof(const Stmt* S) {
            return S->getKind() == SK_ProcCall;
        }
    };

    class IfStatement : public Stmt {
        Expression* Cond;
        StmtList IfStmts;
        StmtList ElseStmts;

    public:
        IfStatement(Expression* Cond, StmtList& IfStmts,
                    StmtList& ElseStmts)
                : Stmt(SK_If), Cond(Cond), IfStmts(IfStmts),
                  ElseStmts(ElseStmts) {}

        Expression* getCond() { return Cond; }

        const StmtList& getIfStmts() { return IfStmts; }

        const StmtList& getElseStmts() { return ElseStmts; }

        static bool classof(const Stmt* S) {
            return S->getKind() == SK_If;
        }
    };

    class WhileStatement : public Stmt {
        Expression* Cond;
        StmtList Stmts;

    public:
        WhileStatement(Expression* Cond, StmtList& Stmts)
                : Stmt(SK_While), Cond(Cond), Stmts(Stmts) {}

        Expression* getCond() { return Cond; }

        const StmtList& getWhileStmts() { return Stmts; }

        static bool classof(const Stmt* S) {
            return S->getKind() == SK_While;
        }
    };

    class ReturnStatement : public Stmt {
        Expression* RetVal;

    public:
        ReturnStatement(Expression* RetVal)
                : Stmt(SK_Return), RetVal(RetVal) {}

        Expression* getRetVal() { return RetVal; }

        static bool classof(const Stmt* S) {
            return S->getKind() == SK_Return;
        }
    };

} // namespace cajeta
