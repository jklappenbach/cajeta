#include "cajetadoc/Ingest.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "antlr4-runtime.h"
#include "CajetaLexer.h"
#include "CajetaParser.h"

#include "cajetadoc/DocComment.h"

namespace fs = std::filesystem;
using cajeta::CajetaLexer;
using cajeta::CajetaParser;

namespace cajetadoc {

namespace {

std::string ltrim(const std::string& s) {
    size_t b = 0;
    while (b < s.size() && std::isspace((unsigned char)s[b])) ++b;
    return s.substr(b);
}

std::string lower(const std::string& s) {
    std::string o = s;
    std::transform(o.begin(), o.end(), o.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return o;
}

// Original source text of a context, preserving spacing (vs. getText() which
// concatenates token text with no separators). Drives faithful signatures.
std::string sourceText(antlr4::ParserRuleContext* ctx) {
    if (!ctx || !ctx->getStart() || !ctx->getStop()) return "";
    size_t a = ctx->getStart()->getStartIndex();
    size_t b = ctx->getStop()->getStopIndex();
    if (b < a) return "";
    antlr4::misc::Interval interval((ssize_t)a, (ssize_t)b);
    return ctx->getStart()->getInputStream()->getText(interval);
}

// The nearest preceding `/** */` doc comment for a declaration context, read
// from the hidden token channel (cajeta lexes all comments to channel(HIDDEN)).
std::string docFor(antlr4::CommonTokenStream& tokens, antlr4::ParserRuleContext* ctx) {
    if (!ctx || !ctx->getStart()) return "";
    size_t idx = ctx->getStart()->getTokenIndex();
    std::string doc;
    for (antlr4::Token* t : tokens.getHiddenTokensToLeft(idx)) {
        const std::string& txt = t->getText();
        if (isDocComment(txt)) doc = txt; // keep the last (closest) one
    }
    return doc;
}

template <typename VecT>
void collectMods(const VecT& mods, Visibility& vis, std::set<std::string>& set,
                 std::vector<std::string>& annos) {
    for (auto* m : mods) {
        std::string txt = ltrim(sourceText(m));
        if (!txt.empty() && txt[0] == '@') {
            annos.push_back(txt);
            continue;
        }
        std::string low = lower(txt);
        if (low == "public") vis = Visibility::Public;
        else if (low == "protected") vis = Visibility::Protected;
        else if (low == "private") vis = Visibility::Private;
        else if (!low.empty()) set.insert(low);
    }
}

std::vector<TypeParam> collectTypeParams(CajetaParser::TypeParametersContext* tp) {
    std::vector<TypeParam> out;
    if (!tp) return out;
    for (auto* p : tp->typeParameter()) {
        TypeParam param;
        param.name = p->identifier() ? p->identifier()->getText() : "";
        if (p->typeBound()) param.bound = sourceText(p->typeBound());
        out.push_back(param);
    }
    return out;
}

void fillSourceRef(antlr4::ParserRuleContext* ctx, const std::string& relPath, SourceRef& ref) {
    ref.file = relPath;
    if (ctx && ctx->getStart()) {
        ref.line = (int)ctx->getStart()->getLine();
        ref.col = (int)ctx->getStart()->getCharPositionInLine();
    }
}

std::vector<Param> collectParams(CajetaParser::FormalParametersContext* fp) {
    std::vector<Param> out;
    if (!fp || !fp->formalParameterList()) return out;
    auto* fpl = fp->formalParameterList();
    for (auto* p : fpl->formalParameter()) {
        Param param;
        param.ownershipTransfer = p->REFERENCE() != nullptr;
        param.type = p->typeType() ? sourceText(p->typeType()) : "";
        if (p->variableDeclaratorId() && p->variableDeclaratorId()->identifier())
            param.name = p->variableDeclaratorId()->identifier()->getText();
        out.push_back(param);
    }
    if (auto* last = fpl->lastFormalParameter()) {
        Param param;
        param.ownershipTransfer = last->REFERENCE() != nullptr;
        param.type = last->typeType() ? sourceText(last->typeType()) : "";
        param.variadic = true;
        if (last->variableDeclaratorId() && last->variableDeclaratorId()->identifier())
            param.name = last->variableDeclaratorId()->identifier()->getText();
        out.push_back(param);
    }
    return out;
}

std::vector<std::string> collectThrows(CajetaParser::QualifiedNameListContext* ql) {
    std::vector<std::string> out;
    if (!ql) return out;
    for (auto* q : ql->qualifiedName()) out.push_back(sourceText(q));
    return out;
}

// Build a Method member from the (typeTypeOrVoid, identifier, typeParameters,
// formalParameters, throws) shape shared by class methods and interface methods.
Member buildMethod(CajetaParser::TypeTypeOrVoidContext* ttov,
                   CajetaParser::IdentifierContext* id,
                   CajetaParser::TypeParametersContext* tp,
                   CajetaParser::FormalParametersContext* fp,
                   CajetaParser::QualifiedNameListContext* throwsList) {
    Member m;
    m.kind = MemberKind::Method;
    m.name = id ? id->getText() : "";
    if (ttov) {
        if (ttov->VOID()) {
            m.returnType = "void";
        } else if (ttov->typeType()) {
            m.returnTransfer = ttov->REFERENCE() != nullptr;
            m.returnType = sourceText(ttov->typeType());
        }
    }
    m.typeParams = collectTypeParams(tp);
    m.params = collectParams(fp);
    m.throws = collectThrows(throwsList);
    return m;
}

class Builder {
public:
    Builder(antlr4::CommonTokenStream& tokens, const std::string& relPath,
            const IngestOptions& opts, IngestResult& out)
        : tokens_(tokens), relPath_(relPath), opts_(opts), out_(out) {}

    void run(CajetaParser::CompilationUnitContext* cu) {
        std::string pkg;
        if (cu->packageDeclaration() && cu->packageDeclaration()->qualifiedName())
            pkg = sourceText(cu->packageDeclaration()->qualifiedName());
        Package& package = out_.model.ensurePackage(pkg);
        // A `/** */` immediately preceding the `package` declaration is the
        // package-level doc (JavaDoc's package-info convention). Last one wins.
        if (cu->packageDeclaration()) {
            std::string pdoc = docFor(tokens_, cu->packageDeclaration());
            if (!pdoc.empty()) {
                package.rawDoc = pdoc;
                package.doc = std::make_shared<DocComment>(parseDocComment(pdoc));
            }
        }
        for (auto* td : cu->typeDeclaration()) {
            Type t;
            if (buildType(td, pkg, t)) package.types.push_back(std::move(t));
        }
    }

private:
    bool included(Visibility v) const {
        if (v == Visibility::Private) return opts_.includePrivate;
        if (v == Visibility::Package || v == Visibility::Protected)
            return opts_.includeInternal ? true : (v == Visibility::Protected || true);
        return true;
    }

    bool buildType(CajetaParser::TypeDeclarationContext* td, const std::string& pkg, Type& out) {
        Visibility vis = Visibility::Package;
        std::set<std::string> mods;
        std::vector<std::string> annos;
        collectMods(td->classOrInterfaceModifier(), vis, mods, annos);

        if (td->classDeclaration()) {
            buildClassLike(td->classDeclaration(), pkg, TypeKind::Class, out);
        } else if (td->interfaceDeclaration()) {
            buildInterface(td->interfaceDeclaration(), pkg, out);
        } else if (td->enumDeclaration()) {
            buildEnum(td->enumDeclaration(), pkg, out);
        } else if (td->viewDeclaration()) {
            buildView(td->viewDeclaration(), pkg, out);
        } else if (td->annotationTypeDeclaration()) {
            out.kind = TypeKind::Annotation;
            auto* a = td->annotationTypeDeclaration();
            if (a->identifier()) out.name = a->identifier()->getText();
            fillSourceRef(a, relPath_, out.loc);
        } else {
            return false;
        }
        out.packageName = pkg;
        out.visibility = vis;
        out.modifiers = mods;
        out.annotations = annos;
        out.rawDoc = docFor(tokens_, td);
        if (!out.rawDoc.empty()) out.doc = std::make_shared<DocComment>(parseDocComment(out.rawDoc));
        return !out.name.empty();
    }

    void buildClassLike(CajetaParser::ClassDeclarationContext* cls, const std::string&,
                        TypeKind kind, Type& out) {
        out.kind = kind;
        out.name = cls->identifier() ? cls->identifier()->getText() : "";
        out.typeParams = collectTypeParams(cls->typeParameters());
        fillSourceRef(cls, relPath_, out.loc);
        collectExtendsImplements(cls, out);
        if (cls->classBody()) buildClassBody(cls->classBody(), out);
    }

    void buildView(CajetaParser::ViewDeclarationContext* v, const std::string&, Type& out) {
        out.kind = TypeKind::View;
        out.name = v->identifier() ? v->identifier()->getText() : "";
        out.typeParams = collectTypeParams(v->typeParameters());
        fillSourceRef(v, relPath_, out.loc);
        if (v->classBody()) buildClassBody(v->classBody(), out);
    }

    void buildEnum(CajetaParser::EnumDeclarationContext* e, const std::string&, Type& out) {
        out.kind = TypeKind::Enum;
        out.name = e->identifier() ? e->identifier()->getText() : "";
        fillSourceRef(e, relPath_, out.loc);
        if (e->typeList()) {
            for (auto* tt : e->typeList()->typeType()) out.implements.push_back(sourceText(tt));
        }
        if (e->enumConstants()) {
            for (auto* c : e->enumConstants()->enumConstant()) {
                Member m;
                m.kind = MemberKind::EnumConstant;
                m.name = c->identifier() ? c->identifier()->getText() : "";
                fillSourceRef(c, relPath_, m.loc);
                m.rawDoc = docFor(tokens_, c);
                if (!m.rawDoc.empty())
                    m.doc = std::make_shared<DocComment>(parseDocComment(m.rawDoc));
                out.members.push_back(std::move(m));
            }
        }
        if (e->enumBodyDeclarations()) {
            for (auto* cbd : e->enumBodyDeclarations()->classBodyDeclaration())
                buildClassBodyDecl(cbd, out);
        }
    }

    void collectExtendsImplements(CajetaParser::ClassDeclarationContext* cls, Type& out) {
        std::string mode;
        for (auto* child : cls->children) {
            if (auto* term = dynamic_cast<antlr4::tree::TerminalNode*>(child)) {
                size_t ty = term->getSymbol()->getType();
                if (ty == CajetaParser::EXTENDS) mode = "extends";
                else if (ty == CajetaParser::IMPLEMENTS) mode = "implements";
                else if (ty == CajetaParser::PERMITS) mode = "permits";
            } else if (auto* tl = dynamic_cast<CajetaParser::TypeListContext*>(child)) {
                for (auto* tt : tl->typeType()) {
                    if (mode == "extends") out.extends.push_back(sourceText(tt));
                    else if (mode == "implements") out.implements.push_back(sourceText(tt));
                }
            }
        }
    }

    void buildInterface(CajetaParser::InterfaceDeclarationContext* itf, const std::string&,
                        Type& out) {
        out.kind = TypeKind::Interface;
        out.name = itf->identifier() ? itf->identifier()->getText() : "";
        out.typeParams = collectTypeParams(itf->typeParameters());
        fillSourceRef(itf, relPath_, out.loc);
        if (itf->typeList()) {
            for (auto* tt : itf->typeList()->typeType()) out.extends.push_back(sourceText(tt));
        }
        if (!itf->interfaceBody()) return;
        for (auto* ibd : itf->interfaceBody()->interfaceBodyDeclaration()) {
            auto* imd = ibd->interfaceMemberDeclaration();
            if (!imd) continue;
            Visibility vis = Visibility::Public; // interface members default public
            std::set<std::string> mods;
            std::vector<std::string> annos;
            collectMods(ibd->modifier(), vis, mods, annos);

            if (imd->interfaceMethodDeclaration() &&
                imd->interfaceMethodDeclaration()->interfaceCommonBodyDeclaration()) {
                auto* cb = imd->interfaceMethodDeclaration()->interfaceCommonBodyDeclaration();
                Member m = buildMethod(cb->typeTypeOrVoid(), cb->identifier(),
                                       cb->typeParameters(), cb->formalParameters(),
                                       cb->qualifiedNameList());
                finishMember(ibd, m, vis, mods, annos, out);
            } else if (imd->constDeclaration()) {
                auto* cd = imd->constDeclaration();
                std::string ty = cd->typeType() ? sourceText(cd->typeType()) : "";
                for (auto* decl : cd->constantDeclarator()) {
                    Member m;
                    m.kind = MemberKind::Field;
                    m.returnType = ty;
                    m.name = decl->identifier() ? decl->identifier()->getText() : "";
                    m.modifiers.insert("const");
                    finishMember(ibd, m, vis, mods, annos, out);
                }
            } else {
                buildNested(imd->classDeclaration(), imd->interfaceDeclaration(),
                            imd->enumDeclaration(), out.packageName, out);
            }
        }
    }

    void buildClassBody(CajetaParser::ClassBodyContext* body, Type& out) {
        for (auto* cbd : body->classBodyDeclaration()) buildClassBodyDecl(cbd, out);
    }

    void buildClassBodyDecl(CajetaParser::ClassBodyDeclarationContext* cbd, Type& out) {
        auto* md = cbd->memberDeclaration();
        if (!md) return; // static block / ';'
        Visibility vis = Visibility::Package;
        std::set<std::string> mods;
        std::vector<std::string> annos;
        collectMods(cbd->modifier(), vis, mods, annos);

        if (md->methodDeclaration()) {
            auto* mdc = md->methodDeclaration();
            Member m = buildMethod(mdc->typeTypeOrVoid(), mdc->identifier(),
                                   mdc->typeParameters(), mdc->formalParameters(),
                                   mdc->qualifiedNameList());
            finishMember(cbd, m, vis, mods, annos, out);
        } else if (md->constructorDeclaration()) {
            auto* cd = md->constructorDeclaration();
            Member m;
            m.kind = MemberKind::Constructor;
            m.name = cd->identifier() ? cd->identifier()->getText() : "";
            m.params = collectParams(cd->formalParameters());
            m.throws = collectThrows(cd->qualifiedNameList());
            finishMember(cbd, m, vis, mods, annos, out);
        } else if (md->destructorDeclaration()) {
            Member m;
            m.kind = MemberKind::Destructor;
            auto* dd = md->destructorDeclaration();
            m.name = "~" + (dd->identifier() ? dd->identifier()->getText() : "");
            finishMember(cbd, m, vis, mods, annos, out);
        } else if (md->fieldDeclaration()) {
            auto* fd = md->fieldDeclaration();
            std::string ty = fd->typeType() ? sourceText(fd->typeType()) : "";
            if (fd->variableDeclarators()) {
                std::string doc = docFor(tokens_, cbd);
                for (auto* vd : fd->variableDeclarators()->variableDeclarator()) {
                    Member m;
                    m.kind = MemberKind::Field;
                    m.returnType = ty;
                    if (vd->variableDeclaratorId() && vd->variableDeclaratorId()->identifier())
                        m.name = vd->variableDeclaratorId()->identifier()->getText();
                    m.visibility = vis;
                    m.modifiers = mods;
                    m.annotations = annos;
                    fillSourceRef(cbd, relPath_, m.loc);
                    m.rawDoc = doc;
                    if (!doc.empty()) m.doc = std::make_shared<DocComment>(parseDocComment(doc));
                    if (keepMember(m)) out.members.push_back(std::move(m));
                }
            }
        } else {
            buildNested(md->classDeclaration(), md->interfaceDeclaration(),
                        md->enumDeclaration(), out.packageName, out);
        }
    }

    void buildNested(CajetaParser::ClassDeclarationContext* cls,
                     CajetaParser::InterfaceDeclarationContext* itf,
                     CajetaParser::EnumDeclarationContext* en,
                     const std::string& pkg, Type& out) {
        Type nested;
        if (cls) buildClassLike(cls, pkg, TypeKind::Class, nested);
        else if (itf) buildInterface(itf, pkg, nested);
        else if (en) buildEnum(en, pkg, nested);
        else return;
        nested.packageName = pkg;
        if (!nested.name.empty()) out.nested.push_back(std::move(nested));
    }

    bool keepMember(const Member& m) const {
        if (m.visibility == Visibility::Private) return opts_.includePrivate;
        if (m.visibility == Visibility::Package) return opts_.includeInternal ? true : true;
        return true;
    }

    void finishMember(antlr4::ParserRuleContext* declCtx, Member& m, Visibility vis,
                      const std::set<std::string>& mods, const std::vector<std::string>& annos,
                      Type& out) {
        m.visibility = vis;
        for (auto& mod : mods) m.modifiers.insert(mod);
        m.annotations = annos;
        fillSourceRef(declCtx, relPath_, m.loc);
        m.rawDoc = docFor(tokens_, declCtx);
        if (!m.rawDoc.empty()) m.doc = std::make_shared<DocComment>(parseDocComment(m.rawDoc));
        if (keepMember(m)) out.members.push_back(std::move(m));
    }

    antlr4::CommonTokenStream& tokens_;
    std::string relPath_;
    const IngestOptions& opts_;
    IngestResult& out_;
};

// Error listener that collects syntax errors instead of printing to stderr.
class CollectingErrors : public antlr4::BaseErrorListener {
public:
    CollectingErrors(const std::string& file, IngestResult& out) : file_(file), out_(out) {}
    void syntaxError(antlr4::Recognizer*, antlr4::Token*, size_t line, size_t,
                     const std::string& msg, std::exception_ptr) override {
        out_.diagnostics.push_back(IngestDiagnostic{file_, (int)line, msg});
    }

private:
    std::string file_;
    IngestResult& out_;
};

} // namespace

void ingestSource(const std::string& text, const std::string& relPath,
                  const IngestOptions& opts, IngestResult& out) {
    antlr4::ANTLRInputStream input(text);
    CajetaLexer lexer(&input);
    antlr4::CommonTokenStream tokens(&lexer);
    CajetaParser parser(&tokens);

    CollectingErrors errs(relPath, out);
    lexer.removeErrorListeners();
    parser.removeErrorListeners();
    parser.addErrorListener(&errs);

    CajetaParser::CompilationUnitContext* cu = parser.compilationUnit();
    Builder builder(tokens, relPath, opts, out);
    builder.run(cu);
    out.filesParsed += 1;
}

IngestResult ingestTree(const std::string& rootDir, const IngestOptions& opts) {
    IngestResult out;
    std::error_code ec;
    fs::path root(rootDir);
    if (!fs::exists(root, ec)) {
        out.diagnostics.push_back(IngestDiagnostic{rootDir, 0, "source root does not exist"});
        return out;
    }
    std::vector<fs::path> files;
    for (auto it = fs::recursive_directory_iterator(root, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        const fs::path& p = it->path();
        // skip excluded directories
        if (it->is_directory(ec)) {
            std::string name = p.filename().string();
            if (std::find(opts.excludeDirs.begin(), opts.excludeDirs.end(), name) !=
                opts.excludeDirs.end()) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (p.extension() == ".cajeta") files.push_back(p);
    }
    std::sort(files.begin(), files.end()); // deterministic order

    for (const fs::path& p : files) {
        std::ifstream in(p, std::ios::binary);
        if (!in) {
            out.diagnostics.push_back(IngestDiagnostic{p.string(), 0, "cannot open file"});
            continue;
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        std::string rel = fs::relative(p, root, ec).string();
        if (ec || rel.empty()) rel = p.string();
        ingestSource(ss.str(), rel, opts, out);
    }
    return out;
}

} // namespace cajetadoc
