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
      (classDeclaration | structDeclaration | enumDeclaration | interfaceDeclaration | annotationTypeDeclaration)
    | ';'
    ;

modifier
    : classOrInterfaceModifier
    | NATIVE
    | TRANSIENT
    | VOLATILE
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

// POD aggregate with declared layout. See WireFormats.md for layout, endianness,
// annotation semantics. The body reuses classBody for fields/methods, but the
// type system enforces no-vtable / no-inheritance / declared-layout semantics.
structDeclaration
    : STRUCT identifier typeParameters?
      classBody
    ;

typeParameters
    : '<' typeParameter (',' typeParameter)* '>'
    ;

typeParameter
    : annotation* identifier (EXTENDS annotation* typeBound)?
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

// Note: there are no method-, operator-, or constructor-level template
// declarations. Every method in Cajeta is virtual (like Java), and
// template-on-virtual would have no way to populate the vtable — the
// receiver's vtable would need a slot per (method, arg-type-list) and
// the call site can't know which to install without runtime type-arg
// metadata, which contradicts the monomorphization-per-instantiation
// design. C++ explicitly forbids template virtual methods for the same
// reason. Class templates remain supported (templates aren't methods).
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

operatorOverloadDeclaration
    : typeType OPERATOR ASSIGN formalParameters methodBody
    | typeType OPERATOR GT formalParameters methodBody
    | typeType OPERATOR LT formalParameters methodBody
    | typeType OPERATOR EQUAL formalParameters methodBody
    | typeType OPERATOR LE formalParameters methodBody
    | typeType OPERATOR GE formalParameters methodBody
    | typeType OPERATOR NOTEQUAL formalParameters methodBody
    | typeType OPERATOR AND formalParameters methodBody
    | typeType OPERATOR OR formalParameters methodBody
    | typeType OPERATOR INC formalParameters methodBody
    | typeType OPERATOR DEC formalParameters methodBody
    | typeType OPERATOR ADD formalParameters methodBody
    | typeType OPERATOR SUB formalParameters methodBody
    | typeType OPERATOR MUL formalParameters methodBody
    | typeType OPERATOR DIV formalParameters methodBody
    | typeType OPERATOR BITAND formalParameters methodBody
    | typeType OPERATOR BITOR formalParameters methodBody
    | typeType OPERATOR CARET formalParameters methodBody
    | typeType OPERATOR MOD formalParameters methodBody
    | typeType OPERATOR ADD_ASSIGN formalParameters methodBody
    | typeType OPERATOR SUB_ASSIGN formalParameters methodBody
    | typeType OPERATOR MUL_ASSIGN formalParameters methodBody
    | typeType OPERATOR DIV_ASSIGN formalParameters methodBody
    | typeType OPERATOR AND_ASSIGN formalParameters methodBody
    | typeType OPERATOR OR_ASSIGN formalParameters methodBody
    | typeType OPERATOR XOR_ASSIGN formalParameters methodBody
    | typeType OPERATOR MOD_ASSIGN formalParameters methodBody
    | typeType OPERATOR LSHIFT_ASSIGN formalParameters methodBody
    | typeType OPERATOR RSHIFT_ASSIGN formalParameters methodBody
    | typeType OPERATOR URSHIFT_ASSIGN formalParameters methodBody
    ;

/* We use rule this even for void methods which cannot have [] after parameters.
   This simplifies grammar and we can consider void to be a type, which
   renders the [] matching as a llvmContext-sensitive issue or a semantic check
   for invalid return type after parsing.
 */
methodDeclaration
    : typeTypeOrVoid identifier formalParameters ('[' ']')*
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
// itself isn't callable from user code. See cajeta-docs/MemoryModel.md
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

interfaceCommonBodyDeclaration
    : annotation* typeTypeOrVoid identifier formalParameters ('[' ']')* (THROWS qualifiedNameList)? methodBody
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
    : identifier '=' elementValue
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
    : '@' INTERFACE identifier annotationTypeBody
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
    | TRY block (catchClause+ finallyBlock? | finallyBlock)
    | TRY resourceSpecification block catchClause* finallyBlock?
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

resourceSpecification
    : '(' resources ';'? ')'
    ;

resources
    : resource (';' resource)*
    ;

resource
    : variableModifier* ( classOrInterfaceType variableDeclaratorId | VAR identifier ) '=' expression
    | identifier
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
    : parameterLabel? expression
    ;

parameterList
    : parameterEntry (',' parameterEntry)*
    ;

methodCall
    : identifier '(' parameterList? ')'
    | THIS '(' parameterList? ')'
    | SUPER '(' parameterList? ')'
    ;

expression
    : primary
    | expression bop='.'
      (
         identifier
       | methodCall
       | THIS
       | NEW nonWildcardTypeArguments? innerCreator
       | SUPER superSuffix
       | explicitTemplateInvocation
      )
    | expression '[' expression ']'
    | methodCall
    | NEW creator
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

    // Java 8 methodReference
    | expression '::' typeArguments? identifier
    | typeType '::' (typeArguments? identifier | NEW)
    | classType '::' typeArguments? NEW
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
    | THIS
    | SUPER
    | literal
    | identifier
    | typeTypeOrVoid '.' CLASS
    | nonWildcardTypeArguments (explicitTemplateInvocationSuffix | THIS arguments)
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

explicitTemplateInvocation
    : nonWildcardTypeArguments explicitTemplateInvocationSuffix
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
    | annotation* (classOrInterfaceType | primitiveType) (annotation* '[' ']')*
    | annotation* (classOrInterfaceType | primitiveType) (annotation* '[' expression ']')*
    ;

// First-class function type: `(T1, T2) -> R`. See cajeta-docs/Lambdas.md.
// Distinct from Java's @FunctionalInterface SAM conversion; this is a real
// type-former, callable directly, with no boxing for primitives.
functionType
    : '(' (typeType (',' typeType)*)? ')' '->' typeType
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

superSuffix
    : arguments
    | '.' typeArguments? identifier arguments?
    ;

explicitTemplateInvocationSuffix
    : SUPER superSuffix
    | identifier arguments
    ;

arguments
    : '(' parameterList? ')'
    ;
