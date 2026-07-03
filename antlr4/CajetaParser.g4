/*
 [The "BSD licence"]
 Copyright (c) 2013 Terence Parr, Sam Harwell
 Copyright (c) 2017 Ivan Kochurkin (upgrade to Java 8)
 Copyright (c) 2021 Michał Lorek (upgrade to Java 11)
 Copyright (c) 2022 Michał Lorek (upgrade to Java 17)
 All rights reserved.

 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions
 are met:
 1. Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
 2. Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.
 3. The name of the author may not be used to endorse or promote products
    derived of this software without specific prior written permission.

 THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

parser grammar CajetaParser;

options { tokenVocab=CajetaLexer; }

compilationUnit
    : packageDeclaration? importDeclaration* typeDeclaration*
    | EOF
    ;

packageDeclaration
    : annotation* PACKAGE qualifiedName ';'
    ;

importDeclaration
    : IMPORT STATIC? qualifiedName ('.' '*')? ';'
    ;

typeDeclaration
    : classOrInterfaceModifier*
      (classDeclaration | recordDeclaration | viewDeclaration | enumDeclaration | interfaceDeclaration | annotationTypeDeclaration)
    | ';'
    ;

modifier
    : classOrInterfaceModifier
    | NATIVE
    | TRANSIENT
    | VOLATILE
    | MUT  // record per-field mutation opt-in (records-spec §3.4)
    ;

classOrInterfaceModifier
    : annotation
    | PUBLIC
    | PROTECTED
    | PRIVATE
    | STATIC
    | ABSTRACT
    | CONST
    | FINAL    // FINAL for class only -- does not apply to interfaces
    | STRICTFP
    | SEALED // Java17
    | NON_SEALED // Java17
    | ASYNC // ThreadModel.md — async fn returns Task<T>
    ;

variableModifier
    : FINAL
    | annotation
    ;

classDeclaration
    : CLASS identifier typeParameters?
      (EXTENDS typeList)?
      (IMPLEMENTS typeList)?
      (PERMITS typeList)? // Java17
      classBody
    ;

// Zero-copy memory overlay onto a byte buffer (Views.md). Fields restricted to
// types directly encodable in bytes (primitives, fixed/variable arrays, nested
// views). An endianness annotation (@BigEndian/@LittleEndian/@HostEndian) is
// optional at the declaration; CajetaView defaults to host endianness when
// absent. Body reuses classBody.
viewDeclaration
    : VIEW identifier typeParameters?
      classBody
    ;

// Value-type record (docs/specification/nucleo/records-spec.md): lowers to a
// @ValueType final class with no vtable. EXTENDS = static non-virtual
// inheritance (single base, Unit 4). IMPLEMENTS parses only so the visitor
// can reject it with a proper diagnostic — records never carry a
// vtable/itable. Body reuses classBody.
recordDeclaration
    : RECORD identifier typeParameters?
      (EXTENDS typeList)?
      (IMPLEMENTS typeList)?
      classBody
    ;

typeParameters
    : '<' typeParameter (',' typeParameter)* '>'
    ;

typeParameter
    : annotation* identifier (EXTENDS annotation* typeBound)? (ASSIGN typeType)?
    | primitiveType identifier
    ;

typeBound
    : typeType ('&' typeType)*
    ;

enumDeclaration
    : ENUM identifier (IMPLEMENTS typeList)? '{' enumConstants? ','? enumBodyDeclarations? '}'
    ;

enumConstants
    : enumConstant (',' enumConstant)*
    ;

enumConstant
    : annotation* identifier arguments? classBody?
    ;

enumBodyDeclarations
    : ';' classBodyDeclaration*
    ;

interfaceDeclaration
    : INTERFACE identifier typeParameters? (EXTENDS typeList)? interfaceBody
    ;

classBody
    : '{' classBodyDeclaration* '}'
    ;

interfaceBody
    : '{' interfaceBodyDeclaration* '}'
    ;

classBodyDeclaration
    : ';'
    | STATIC? block
    | modifier* memberDeclaration
    ;

// Method-level templates: instance + static methods may introduce
// their own typeParameters via `<R>` immediately before the return
// type (see methodDeclaration). Such methods are inherently NON-
// virtual — the templating itself excludes them from the vtable
// (the vtable would need one slot per (method, type-arg-list) and
// the set of arg lists isn't knowable at vtable-build time, which
// contradicts the monomorphization-per-instantiation design; C++
// forbids the construct for the same reason).
//
// Calls to method-templated methods resolve statically on the
// receiver's static type and emit direct calls to the monomorphized
// symbol. Subclass declarations with the same name as a method-
// templated parent method shadow rather than override (warning
// emitted; same model Java applies to `static` shadowing). See
// docs/specification/lang/MethodLevelTemplate.md for the dispatch model
// and constraints.
//
// Constructor and operator declarations remain non-templated at the
// method level (the construct/operator entry shapes don't compose
// with per-call monomorphization).
memberDeclaration
    : methodDeclaration
    | operatorOverloadDeclaration
    | fieldDeclaration
    | constructorDeclaration
    | destructorDeclaration
    | interfaceDeclaration
    | annotationTypeDeclaration
    | classDeclaration
    | enumDeclaration
    ;

// Return type is `typeTypeOrVoid` (= `REFERENCE? typeType | VOID`),
// so each alternative can declare either a concrete return (`#T` for
// ownership transfer or bare `T` for value/borrow) or `void`.
// `void` is the natural return for mutating-unary (`++`, `--`),
// compound-assignment (`+=` etc.), and indexed-write (`[]=`) — the
// expression value isn't the operator's "result," the receiver is.
// Per-operator staticness + arity + return-type constraints are
// enforced semantically in CajetaLlvmVisitor's
// visitClassBodyDeclaration post-pass (see
// docs/OperatorOverloading.md §10 — Option B).
operatorOverloadDeclaration
    : typeTypeOrVoid OPERATOR ASSIGN formalParameters methodBody
    | typeTypeOrVoid OPERATOR GT formalParameters methodBody
    | typeTypeOrVoid OPERATOR LT formalParameters methodBody
    | typeTypeOrVoid OPERATOR EQUAL formalParameters methodBody
    | typeTypeOrVoid OPERATOR LE formalParameters methodBody
    | typeTypeOrVoid OPERATOR GE formalParameters methodBody
    | typeTypeOrVoid OPERATOR NOTEQUAL formalParameters methodBody
    | typeTypeOrVoid OPERATOR AND formalParameters methodBody
    | typeTypeOrVoid OPERATOR OR formalParameters methodBody
    | typeTypeOrVoid OPERATOR INC formalParameters methodBody
    | typeTypeOrVoid OPERATOR DEC formalParameters methodBody
    | typeTypeOrVoid OPERATOR ADD formalParameters methodBody
    | typeTypeOrVoid OPERATOR SUB formalParameters methodBody
    | typeTypeOrVoid OPERATOR MUL formalParameters methodBody
    | typeTypeOrVoid OPERATOR DIV formalParameters methodBody
    | typeTypeOrVoid OPERATOR BITAND formalParameters methodBody
    | typeTypeOrVoid OPERATOR BITOR formalParameters methodBody
    | typeTypeOrVoid OPERATOR CARET formalParameters methodBody
    | typeTypeOrVoid OPERATOR MOD formalParameters methodBody
    | typeTypeOrVoid OPERATOR ADD_ASSIGN formalParameters methodBody
    | typeTypeOrVoid OPERATOR SUB_ASSIGN formalParameters methodBody
    | typeTypeOrVoid OPERATOR MUL_ASSIGN formalParameters methodBody
    | typeTypeOrVoid OPERATOR DIV_ASSIGN formalParameters methodBody
    | typeTypeOrVoid OPERATOR AND_ASSIGN formalParameters methodBody
    | typeTypeOrVoid OPERATOR OR_ASSIGN formalParameters methodBody
    | typeTypeOrVoid OPERATOR XOR_ASSIGN formalParameters methodBody
    | typeTypeOrVoid OPERATOR MOD_ASSIGN formalParameters methodBody
    | typeTypeOrVoid OPERATOR LSHIFT_ASSIGN formalParameters methodBody
    | typeTypeOrVoid OPERATOR RSHIFT_ASSIGN formalParameters methodBody
    | typeTypeOrVoid OPERATOR URSHIFT_ASSIGN formalParameters methodBody
    // Indexing operators (read + write) collapsed into a single
    // alternative with optional ASSIGN — ANTLR4 LL(*) prediction
    // gets confused by two separate alternatives both starting with
    // `OPERATOR LBRACK RBRACK` and prints "mismatched input
    // 'operator'" diagnostics even though it eventually recovers.
    // The visitor branches on ctx->ASSIGN() to pick the method name
    // (`operator[]` vs `operator[]=`).
    //
    // Return type is `typeTypeOrVoid` (not bare `typeType`) so the
    // setter form can declare `void` — `void operator[]= (...)`.
    // The read form's return type can't be void in practice
    // (subscript-read producing nothing is nonsensical) but the
    // grammar permits it; the type-check enforces the semantic
    // shape elsewhere.
    | typeTypeOrVoid OPERATOR LBRACK RBRACK ASSIGN? formalParameters methodBody
    ;

/* We use rule this even for void methods which cannot have [] after parameters.
   This simplifies grammar and we can consider void to be a type, which
   renders the [] matching as a llvmContext-sensitive issue or a semantic check
   for invalid return type after parsing.

   Optional `typeParameters` after the identifier introduces
   method-level type parameters (docs/specification/lang/MethodLevelTemplate.md).
   When present, the method is non-virtual and monomorphized per call
   site over (receiver-class args x method-level args). The same
   typeParameters nonterminal used for classDeclaration is reused
   here. Position is post-identifier rather than pre-return-type
   (Java-style) — mirrors call-site syntax `expr.method<T>(args)` and
   matches C++ template-on-method convention.
 */
methodDeclaration
    : typeTypeOrVoid identifier typeParameters? formalParameters ('[' ']')*
      (THROWS qualifiedNameList)?
      methodBody
    ;

methodBody
    : block
    | ';'
    ;

// Return type marker: an optional REFERENCE ('#') prefix on the return type
// declares that the function transfers ownership to its caller. See
// MemoryModel.md § Borrow / transfer rules and § Function signatures.
typeTypeOrVoid
    : REFERENCE? typeType
    | VOID
    ;

constructorDeclaration
    : identifier formalParameters (THROWS qualifiedNameList)? constructorBody=block
    ;

// Destructor — C++-style `~ClassName()`. Mirrors the constructor's
// shape (the identifier must match the enclosing class name; the
// compiler validates that during the visit). The body becomes the
// class's drop method internally — the synthesized __cajeta_<class>_drop
// wrapper invokes it before freeing the instance, and the destructor
// itself isn't callable from user code. See docs/MemoryModel.md
// § Destructors.
destructorDeclaration
    : TILDE identifier '(' ')' destructorBody=block
    ;

fieldDeclaration
    : typeType variableDeclarators ';'
    ;

interfaceBodyDeclaration
    : modifier* interfaceMemberDeclaration
    | ';'
    ;

// See the matching note above `memberDeclaration` — interface methods
// can't carry their own typeParameters either, since the concrete class
// implementing the interface has to populate a vtable slot per method
// and there's no place for per-call template instantiations to land.
interfaceMemberDeclaration
    : constDeclaration
    | interfaceMethodDeclaration
    | interfaceDeclaration
    | annotationTypeDeclaration
    | classDeclaration
    | enumDeclaration
    ;

constDeclaration
    : typeType constantDeclarator (',' constantDeclarator)* ';'
    ;

constantDeclarator
    : identifier ('[' ']')* '=' variableInitializer
    | identifier ('[' expression ']')* '=' variableInitializer
    ;

// Early versions of Java allows brackets after the curMethod canonical, eg.
// public int[] return2DArray() [] { ... }
// is the same as
// public int[][] return2DArray() { ... }
interfaceMethodDeclaration
    : interfaceMethodModifier* interfaceCommonBodyDeclaration
    ;

// Java8
interfaceMethodModifier
    : annotation
    | PUBLIC
    | ABSTRACT
    | DEFAULT
    | STATIC
    | STRICTFP
    ;

// Accepts an optional `typeParameters` prefix so the visitor can detect
// method-level generics on interface methods and reject them cleanly
// with CAJETA_ERROR_INTERFACE_METHOD_GENERIC (S9.4). Without the rule
// extension, the syntax would parse as a "no viable alternative" error
// at the `<` token and the rest of the file would partial-recover —
// giving the user an unhelpful diagnostic. v1 doesn't support method-
// level generics on interface methods (or on concrete struct/class
// methods either, but those don't go through this rule).
interfaceCommonBodyDeclaration
    : annotation* typeTypeOrVoid identifier typeParameters? formalParameters ('[' ']')* (THROWS qualifiedNameList)? methodBody
    ;

variableDeclarators
    : variableDeclarator (',' variableDeclarator)*
    ;

variableDeclarator
    : variableDeclaratorId ('=' variableInitializer)?
    ;

variableDeclaratorId
    : identifier ('[' ']')*
    | identifier ('[' expression ']')*
    ;

variableInitializer
    : arrayInitializer
    | expression
    ;

arrayInitializer
    : '{' (variableInitializer (',' variableInitializer)* (',')? )? '}'
    ;

classOrInterfaceType
    : identifier typeArguments? ('.' identifier typeArguments?)*
    ;

typeArgument
    : typeType
    | primitiveType
    | integerLiteral
    | annotation* '?' ((EXTENDS | SUPER) typeType)?
    ;

qualifiedNameList
    : qualifiedName (',' qualifiedName)*
    ;

formalParameters
    : '(' ( receiverParameter?
          | receiverParameter (',' formalParameterList)?
          | formalParameterList?
          ) ')'
    ;

receiverParameter
    : typeType (identifier '.')* THIS
    ;

formalParameterList
    : formalParameter (',' formalParameter)* (',' lastFormalParameter)?
    | lastFormalParameter
    ;

// Optional REFERENCE ('#') prefix on the parameter type declares that this
// parameter takes ownership of its argument. See MemoryModel.md § Borrow /
// transfer rules.
formalParameter
    : variableModifier* REFERENCE? typeType variableDeclaratorId (ASSIGN expression)?
    ;

lastFormalParameter
    : variableModifier* REFERENCE? typeType annotation* '...' variableDeclaratorId
    ;

// local variable type inference
lambdaLVTIList
    : lambdaLVTIParameter (',' lambdaLVTIParameter)*
    ;

lambdaLVTIParameter
    : variableModifier* VAR identifier
    ;

qualifiedName
    : identifier ('.' identifier)*
    ;

literal
    : integerLiteral
    | floatLiteral
    | CHAR_LITERAL
    | STRING_LITERAL
    | BOOL_LITERAL
    | NULL_LITERAL
    | TEXT_BLOCK // Java17
    ;

integerLiteral
    : DECIMAL_LITERAL
    | HEX_LITERAL
    | OCT_LITERAL
    | BINARY_LITERAL
    ;

floatLiteral
    : FLOAT_LITERAL
    | HEX_FLOAT_LITERAL
    ;

// ANNOTATIONS
altAnnotationQualifiedName
    : (identifier DOT)* '@' identifier
    ;

annotation
    : ('@' qualifiedName | altAnnotationQualifiedName) ('(' ( elementValuePairs | elementValue )? ')')?
    ;

elementValuePairs
    : elementValuePair (',' elementValuePair)*
    ;

elementValuePair
    : identifier ('=' | ':') elementValue
    ;

elementValue
    : expression
    | annotation
    | elementValueArrayInitializer
    ;

elementValueArrayInitializer
    : '{' (elementValue (',' elementValue)*)? (',')? '}'
    ;

annotationTypeDeclaration
    : ANNOTATION identifier annotationTypeBody
    ;

annotationTypeBody
    : '{' (annotationTypeElementDeclaration)* '}'
    ;

annotationTypeElementDeclaration
    : modifier* annotationTypeElementRest
    | ';' // this is not allowed by the grammar, but apparently allowed by the actual compiler
    ;

annotationTypeElementRest
    : typeType annotationMethodOrConstantRest ';'
    | classDeclaration ';'?
    | interfaceDeclaration ';'?
    | enumDeclaration ';'?
    | annotationTypeDeclaration ';'?
    ;

annotationMethodOrConstantRest
    : annotationMethodRest
    | annotationConstantRest
    ;

annotationMethodRest
    : identifier '(' ')' defaultValue?
    ;

annotationConstantRest
    : variableDeclarators
    ;

defaultValue
    : DEFAULT elementValue
    ;

requiresModifier
	: TRANSITIVE
	| STATIC
	;

// STATEMENTS / BLOCKS

block
    : '{' blockStatement* '}'
    ;

blockStatement
    : localVariableDeclaration ';'
    | statement
    | localTypeDeclaration
    ;

localVariableDeclaration
    : variableModifier* (typeType variableDeclarators | VAR identifier '=' expression)
    ;

identifier
    : IDENTIFIER
    | VAR
    // Module-system soft keywords (docs/specification/Modules.md
    // — pending). These tokens are reserved by the lexer for the
    // future module declaration syntax but the parser never uses
    // them in any rule today. Until the module surface lands,
    // they're freely usable as ordinary identifiers (e.g.
    // `File.open(...)`, `String s = "open"`). When module
    // declarations gain grammar rules, they'll consume these
    // tokens in their specific positions; the identifier rule
    // keeps accepting them so user method names like `open` /
    // `requires` / `provides` continue to parse.
    | MODULE
    | OPEN
    | REQUIRES
    | EXPORTS
    | OPENS
    | TO
    | USES
    | PROVIDES
    | WITH
    | TRANSITIVE
    // `shared` is a contextual keyword: it has special meaning only in the
    // token-led GPU placement form `SHARED (creator | aggregateInitializer)`
    // (workgroup-shared memory inside an @Kernel body). Everywhere else it must
    // remain a plain identifier so pre-existing code with a `shared`
    // method/field/variable still parses — reserving it outright produced a
    // parse error ("no viable alternative at input '... shared'") -> malformed
    // tree -> bad any_cast at AST-build time. (Unlike its placement siblings
    // HEAP/STACK, which are reserved, `shared` collides with real user code.)
    | SHARED
    // `mut` is likewise contextual: meaningful only as a record field
    // modifier; everywhere else it stays a plain identifier.
    | MUT
    ;

localTypeDeclaration
    : classOrInterfaceModifier*
      (classDeclaration | interfaceDeclaration)
    | ';'
    ;

statement
    : blockLabel=block
    | IF parExpression statement (ELSE statement)?
    | FOR '(' forControl ')' statement
    | WHILE parExpression statement
    | DO statement WHILE parExpression ';'
    // No try-with-resources rule. Destructors fire deterministically
    // at the closing `}` of the resource's declaring block, so
    // `try (R r = …) { … }` is strictly redundant with
    // `{ R r = …; … }`. See docs/MemoryModel.md § Destructors
    // and docs/specification/io/file/Readme.md § Design tenets for
    // the rationale.
    | TRY block (catchClause+ finallyBlock? | finallyBlock)
    | SWITCH parExpression '{' switchBlockStatementGroup* switchLabel* '}'
    | RETURN expression? ';'
    | THROW expression ';'
    | BREAK identifier? ';'
    | CONTINUE identifier? ';'
    | YIELD expression ';' // Java17
    | SCOPE block  // ThreadModel.md — joins all child tasks before exiting
    | SEMI
    | statementExpression=expression ';'
    | switchExpression ';'? // Java17
    | identifierLabel=identifier ':' statement
    ;

catchClause
    : CATCH '(' variableModifier* catchType identifier ')' block
    ;

catchType
    : qualifiedName ('|' qualifiedName)*
    ;

finallyBlock
    : FINALLY block
    ;

/** Matches cases then statements, both of which are mandatory.
 *  To handle empty cases at the end, we add switchLabel* to statement.
 */
switchBlockStatementGroup
    : switchLabel+ blockStatement+
    ;

switchLabel
    : CASE (constantExpression=expression | enumConstantName=IDENTIFIER | typeType varName=identifier) ':'
    | DEFAULT ':'
    ;

forControl
    : enhancedForControl
    | forInit? ';' expression? ';' forUpdate=expressionList?
    ;

forInit
    : localVariableDeclaration
    | expressionList
    ;

// Standard Java enhanced-for `for (T x : iterable)`. The optional `loopIterator ,`
// prefix is a Cajeta extension that exposes the running index/iteration variable
// alongside the element binding.
enhancedForControl
    : (loopIterator ',')? loopVariable
    ;

loopVariable
    : variableModifier* (typeType | VAR) variableDeclaratorId ':' expression
    ;

loopIterator
    : variableModifier* (typeType | VAR) variableDeclaratorId
    ;

// EXPRESSIONS

parExpression
    : '(' expression ')'
    ;

expressionList
    : expression (',' expression)*
    ;

parameterLabel
    : IDENTIFIER ':'
    ;

parameterEntry
    : parameterLabel? REFERENCE? expression
    ;

parameterList
    : parameterEntry (',' parameterEntry)*
    ;

// XPU launch dimensions and any list literal: `[e1, e2, ...]` (and `[]`).
// Reachable from `primary`, so it only matches where a value is expected —
// never colliding with the postfix index form `expression '[' expression ']'`,
// which requires a preceding expression. See CajetaXPU.md §3.1.3 (`grid: [...]`).
arrayLiteral
    : '[' expressionList? ']'
    ;

// Method-level template call-site form: `identifier<TypeArgs>(args)`.
// The optional `<typeList>` between the name and `(` carries explicit
// type arguments for method-templated callees. Inference (no type
// args) is the common case; explicit args are only required when
// inference can't bind every type parameter (e.g. T appears only in
// the return type). See docs/specification/lang/MethodLevelTemplate.md.
//
// This is the only call-site form: there is no Java-style
// `Type.<TypeArgs>name(args)` alternative — that form was removed in
// favor of one syntax that mirrors `Type<args>` at the type-use site.
methodCall
    // The optional type-arg list uses `typeArguments` (the same production as a
    // type-use site `Foo<...>`), not `typeList`, so a method-level non-type
    // (integer-constant) argument parses: `m<8>(args)` — `typeArgument` admits
    // `integerLiteral` where `typeList`'s `typeType` does not. The visitor
    // resolves an integerLiteral arg to a CajetaConstantType.
    : identifier typeArguments? '(' parameterList? ')'
    | THIS '(' parameterList? ')'
    | SUPER '(' parameterList? ')'
    ;

expression
    // methodCall is listed FIRST (ahead of `primary`) so a base call
    // `foo(x)` matches the whole methodCall form before the general
    // postfix-call suffix below can reinterpret it as primary(foo) applied
    // to `(x)`. Without this ordering, adding `expression '(' parameterList?
    // ')'` silently turns every ordinary call into a postfix call. A bare
    // identifier (no `(`) cleanly falls through to `primary`; `a < b`
    // comparisons still fall through because methodCall needs a trailing `(`.
    : methodCall
    | primary
    | expression bop='.'
      (
         // methodCall MUST come before bare identifier — for inputs
         // like `expr.name<TypeArg>(arg)`, both alternatives can
         // produce a complete overall parse (identifier consumes just
         // `name` and lets the outer expression rule absorb
         // `<TypeArg>(arg)` as chained `<`/`>` comparison; methodCall
         // consumes the whole call form). ANTLR's tie-break picks the
         // first listed alternative, so listing identifier first
         // ambiguates `<ClassName>(singleArg)` shapes into bogus
         // comparison-chain ASTs and segfaults during codegen with a
         // null operand. (Two-arg / zero-arg / primitive-type-arg
         // forms work because the comma / empty parens / primitive
         // keyword block the comparison alternative on independent
         // grounds.) Trying methodCall first cleanly fails to match
         // when no `(` follows and falls through to identifier — so
         // the swap doesn't affect plain field-access syntax.
         methodCall
       | identifier
       | THIS
       | HEAP nonWildcardTypeArguments? innerCreator
       | SUPER superSuffix
      )
    | expression '[' expression ']'
    // XPU: postfix call applied to the result of an expression — the
    // `(args)` that follows `kernel.launch(stream, grid:, block:)`. General
    // by design: any expression yielding a callable can be invoked this way.
    // Listed after the DOT/methodCall/index forms so established call shapes
    // (`foo(x)`, `obj.foo(x)`) keep winning; this alternative only fires for a
    // bare `(args)` trailing a non-identifier expression result. The
    // classic cast-vs-call ambiguity on `(T)(x)` resolves to the earlier
    // cast alternative below, which is the conventional choice.
    | expression '(' parameterList? ')'
    // Unified-class allocation prefixes (UnifiedClasses.md). `heap` and
    // `stack` are mandatory at the allocation site — bare `MyClass(args)`
    // is a compile error, and there is no `new` allocator (removed). Both
    // forms wrap either a constructor call (via `creator`) or an aggregate-
    // init expression. `heap` is the sole heap allocator; `stack` lowers to
    // a stack alloca + ctor call.
    | HEAP  (creator | aggregateInitializer)
    | STACK (creator | aggregateInitializer)
    // `shared` is a third placement (GPU workgroup-shared memory, NV addrspace
    // 3). Device-only: legal only inside an @Kernel body, where the device
    // lowerer (NvptxKernelLowering) turns `shared T[N]` into one per-block
    // addrspace(3) global. The host codegen path rejects it. See CajetaXPU.md.
    | SHARED (creator | aggregateInitializer)
    | '(' annotation* typeType ('&' typeType)* ')' expression
    | expression postfix=('++' | '--')
    | prefix=('+'|'-'|'++'|'--') expression
    | prefix=('~'|'!') expression
    // Move/transfer operator: '#expr' transfers ownership of expr to the
    // receiving site (assignment LHS, argument slot, return slot). See
    // MemoryModel.md for full semantics.
    | REFERENCE expression
    // Structured concurrency (ThreadModel.md): await unwraps a Task<T> to
    // T; spawn launches a Task<T> bound to the enclosing scope; detach
    // launches one that outlives the current frame. Semantic checks
    // (await only inside async, spawn only inside scope, detach requires
    // # captures) live in the AST resolution pass.
    | AWAIT expression
    | SPAWN expression
    | DETACH expression
    | expression bop=('*'|'/'|'%') expression
    | expression bop=('+'|'-') expression
    | expression ('<' '<' | '>' '>' '>' | '>' '>') expression
    | expression bop=('<=' | '>=' | '>' | '<') expression
    | expression bop=INSTANCEOF (typeType | pattern)
    | expression bop=('==' | '!=') expression
    | expression bop='&' expression
    | expression bop='^' expression
    | expression bop='|' expression
    | expression bop='&&' expression
    | expression bop='||' expression
    | <assoc=right> expression bop='?' expression ':' expression
    | <assoc=right> expression
      bop=('=' | '+=' | '-=' | '*=' | '/=' | '&=' | '|=' | '^=' | '>>=' | '>>>=' | '<<=' | '%=')
      expression
    | lambdaExpression // Java8
    | switchExpression // Java17

    // Java 8 methodReference. A constructor reference is spelled `Type::heap`
    // (it is a deferred, escaping factory returning an owned `#Type`, so the
    // placement is necessarily `heap` — never `stack`/`shared`).
    | expression '::' typeArguments? identifier
    | typeType '::' (typeArguments? identifier | HEAP)
    | classType '::' typeArguments? HEAP
    ;

// Java17
pattern
    : variableModifier* typeType annotation* identifier
    ;

// Java8
lambdaExpression
    : lambdaParameters '->' lambdaBody
    ;

// Java8
lambdaParameters
    : identifier
    | '(' formalParameterList? ')'
    | '(' identifier (',' identifier)* ')'
    | '(' lambdaLVTIList? ')'
    ;

// Java8
lambdaBody
    : expression
    | block
    ;

primary
    : '(' expression ')'
    // MultiClassing Phase 2 (docs/specification/lang/MultiClassing.md § P-2):
    // parent-view selectors on THIS / SUPER use angle brackets,
    // reusing template-instantiation syntax: `this<Base>.field`
    // reaches the slot belonging to ancestor `Base`;
    // `super<Base>.method()` direct-calls `Base`'s body bypassing the
    // vtable. ANTLR commits to the selector alt only when it can
    // match `<typeType>` followed by the trailing `.member`, so plain
    // `<` comparisons against THIS / SUPER (which neither typecheck
    // as numeric values anyway) continue to fall through to the bare
    // alternatives. Listed BEFORE the bare THIS / SUPER alternatives
    // so ANTLR takes the longer match when the next token is `<`.
    | THIS '<' typeType '>'
    | SUPER '<' typeType '>'
    | THIS
    | SUPER
    | literal
    | arrayLiteral
    | aggregateInitializer
    | identifier
    | typeTypeOrVoid '.' CLASS
    ;

// S6.2 — struct aggregate initializer (Structs.md). Syntax mirrors Rust:
// `Foo { field: expr, field: expr }`. Reuses parameterList (the same
// `parameterLabel? expression` shape methodCall uses for keyword args)
// so labeled bindings parse with no new lex tokens.
//
// Listed before `identifier` in the primary alternatives so ANTLR's
// adaptive lookahead prefers the longer match — a bare `Foo` falls
// back to the identifier alternative cleanly.
aggregateInitializer
    : identifier '{' parameterList? '}'
    ;

// Java17
switchExpression
    : SWITCH parExpression '{' switchLabeledRule* '}'
    ;

// Java17
switchLabeledRule
    : CASE (expressionList | NULL_LITERAL | guardedPattern) (ARROW | COLON) switchRuleOutcome
    | DEFAULT (ARROW | COLON) switchRuleOutcome
    ;

// Java17
guardedPattern
    : '(' guardedPattern ')'
    | variableModifier* typeType annotation* identifier ('&&' expression)*
    | guardedPattern '&&' expression
    ;

// Java17
switchRuleOutcome
    : block
    | blockStatement*
    ;

classType
    : (classOrInterfaceType '.')? annotation* identifier typeArguments?
    ;

creator
    : nonWildcardTypeArguments createdName classCreatorRest
    | createdName (arrayCreatorRest | classCreatorRest)
    ;

createdName
    : identifier typeArgumentsOrDiamond? ('.' identifier typeArgumentsOrDiamond?)*
    | primitiveType
    ;

innerCreator
    : identifier nonWildcardTypeArgumentsOrDiamond? classCreatorRest
    ;

arrayCreatorRest
    : '[' (']' ('[' ']')* arrayInitializer | expression ']' ('[' expression ']')* ('[' ']')*)
    ;

classCreatorRest
    : arguments classBody?
    ;

typeArgumentsOrDiamond
    : '<' '>'
    | typeArguments
    ;

nonWildcardTypeArgumentsOrDiamond
    : '<' '>'
    | nonWildcardTypeArguments
    ;

nonWildcardTypeArguments
    : '<' typeList '>'
    ;

typeList
    : typeType (',' typeType)*
    ;

typeType
    : functionType
    // Parenthesized function type, optionally arrayed: `((T)->R)[]`. The grouping
    // parens are what make an array-OF-function-type expressible — without them
    // `(T)->R[]` binds the `[]` to the RETURN (a function returning `R[]`). The
    // inner is restricted to `functionType` (not a general `'(' typeType ')'`) so
    // the `(`...`)` here never collides with the C-style cast `'(' typeType ')' expr`:
    // it only matches when a `->` proves the parens wrap a function type.
    | '(' functionType ')' (annotation* '[' ']')*
    | annotation* (classOrInterfaceType | primitiveType) (annotation* '[' ']')*
    | annotation* (classOrInterfaceType | primitiveType) (annotation* '[' expression ']')*
    ;

// First-class function type: `(T1, T2) -> R`. See docs/Lambdas.md.
// Distinct from Java's @FunctionalInterface SAM conversion; this is a real
// type-former, callable directly, with no boxing for primitives. Return is
// typeTypeOrVoid so `(T) -> void` is a legal type (Stream.forEach etc.).
functionType
    : '(' (typeType (',' typeType)*)? ')' '->' typeTypeOrVoid
    ;

primitiveType
    : BOOLEAN
    | CHAR
    | INT8
    | UINT8
    | INT16
    | UINT16
    | INT32
    | UINT32
    | INT64
    | UINT64
    | INT128
    | UINT128
    | FLOAT4E2M1
    | FLOAT6E2M3
    | FLOAT6E3M2
    | FLOAT8E4M3
    | FLOAT8E5M2
    | FLOAT8E4M3FNUZ
    | FLOAT8E5M2FNUZ
    | FLOAT16
    | FLOAT32
    | FLOAT64
    | FLOAT128
    ;

typeArguments
    : '<' typeArgument (',' typeArgument)* '>'
    ;

// `super.foo(args)` or Form C templated `super.foo<T>(args)`. The
// type-args go AFTER the identifier (mirrors methodCall). Super
// calls land in UnsupportedExpression in this release, so the
// shape is grammar-only consistency.
superSuffix
    : arguments
    | '.' identifier ('<' typeList '>')? arguments?
    ;

arguments
    : '(' parameterList? ')'
    ;
