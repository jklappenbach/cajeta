# minimum required CMAKE version
CMAKE_MINIMUM_REQUIRED(VERSION 3.26 FATAL_ERROR)
project(cajeta VERSION 1.0.0)

cmake_policy(SET CMP0135 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0135 NEW)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

cmake_minimum_required(VERSION 3.16)
project(cajeta VERSION 1.0)

find_package(glog 0.6.0 REQUIRED)

## compiler must be 11 or 14
set(CMAKE_CXX_STANDARD 17)
set(Java_JAVA_EXECUTABLE "java")
set(ANTLR4_TAG 7ed420f)

## required if linking to static library
add_definitions(-DANTLR4CPP_STATIC)

# using /MD flag for antlr4_runtime (for Visual C++ compilers only)
set(ANTLR4_WITH_STATIC_CRT OFF)

# add external build for antlrcpp
# add antrl4cpp artifacts to project environment
include(ExternalAntlr4Cpp)
include_directories(${ANTLR4_INCLUDE_DIRS})
message("-- Defined include directores as: ${ANTLR4_INCLUDE_DIRS}")

## set variable pointing to the antlr tool that supports C++
## this is not required if the jar file can be found under PATH environment
set(ANTLR_EXECUTABLE $ENV{ANTLR_EXECUTABLE})

## add macros to codegen ANTLR Cpp code of grammar
find_package(ANTLR REQUIRED)
find_package(LLVM REQUIRED CONFIG)
include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

## Call macro to add lexer and grammar to your build dependencies.
antlr_target(CajetaGrammarLexer ${CMAKE_CURRENT_SOURCE_DIR}/antlr4/CajetaLexer.g4 LEXER
             PACKAGE cajeta OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/generated/lex")
antlr_target(CajetaGrammarParser ${CMAKE_CURRENT_SOURCE_DIR}/antlr4/CajetaParser.g4 PARSER VISITOR
             PACKAGE cajeta
             DEPENDS_ANTLR CajetaGrammarLexer
             COMPILE_FLAGS -lib ${ANTLR_CajetaGrammarLexer_OUTPUT_DIR}
             OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/generated/parser")

## include generated files in project environment
include_directories(${ANTLR_CajetaGrammarLexer_OUTPUT_DIR})
include_directories(${ANTLR_CajetaGrammarParser_OUTPUT_DIR})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)
message("-- ANTLR CajetaGrammar Lex Output: ${ANTLR_CajetaGrammarLexer_OUTPUT_DIR}")
message("-- ANTLR CajetaGrammer Parser Output: ${ANTLR_CajetaGrammarParser_OUTPUT_DIR}")

## add generated grammar to demo binary target
add_executable(cajeta
        ${ANTLR_CajetaGrammarLexer_CXX_OUTPUTS}
        ${ANTLR_CajetaGrammarParser_CXX_OUTPUTS}
        src/main.cpp src/cajeta/type/Scope.h src/cajeta/type/Scope.cpp
        src/cajeta/asn/Statement.cpp src/cajeta/asn/Statement.h
        src/cajeta/type/Generics.cpp src/cajeta/type/Generics.h
        src/cajeta/method/Method.cpp src/cajeta/method/Method.h
        src/cajeta/field/Field.cpp src/cajeta/field/Field.h
        src/cajeta/asn/expression/Expression.h src/cajeta/asn/expression/Expression.cpp
        src/cajeta/asn/expression/DotExpression.h src/cajeta/asn/expression/DotExpression.cpp
        src/cajeta/type/CajetaType.cpp src/cajeta/type/CajetaType.h
        src/cajeta/type/CajetaInterface.h src/cajeta/type/CajetaInterface.cpp
        src/cajeta/type/CajetaClass.cpp src/cajeta/type/CajetaClass.h
        src/cajeta/type/QualifiedName.h src/cajeta/type/QualifiedName.cpp
        src/cajeta/asn/AbstractSyntaxNode.cpp src/cajeta/asn/AbstractSyntaxNode.h
        src/cajeta/compile/CajetaModule.cpp src/cajeta/compile/CajetaModule.h
        src/cajeta/compile/Compiler.cpp src/cajeta/compile/Compiler.h
        src/cajeta/type/CajetaStructure.cpp src/cajeta/type/CajetaStructure.h
        src/cajeta/type/FormalParameter.h src/cajeta/type/FormalParameter.cpp
        src/cajeta/asn/Block.h src/cajeta/asn/Block.cpp
        src/cajeta/asn/BlockStatement.h src/cajeta/asn/BlockStatement.cpp
        src/cajeta/asn/VariableDeclarator.cpp src/cajeta/asn/VariableDeclarator.h
        src/cajeta/asn/LocalVariableDeclaration.cpp src/cajeta/asn/LocalVariableDeclaration.h
        src/cajeta/asn/ClassBodyDeclaration.cpp src/cajeta/asn/ClassBodyDeclaration.h
        src/cajeta/asn/expression/Expression.cpp src/cajeta/asn/expression/Expression.h
        src/cajeta/asn/expression/BinaryOpExpression.cpp src/cajeta/asn/expression/BinaryOpExpression.h
        src/cajeta/asn/expression/LiteralExpression.cpp src/cajeta/asn/expression/LiteralExpression.h
        src/cajeta/asn/expression/Identifier.cpp src/cajeta/asn/expression/Identifier.h
        src/cajeta/asn/expression/CreatorRest.cpp src/cajeta/asn/expression/CreatorRest.h
        src/cajeta/asn/expression/NewExpression.cpp src/cajeta/asn/expression/NewExpression.h
        src/cajeta/asn/expression/MethodCallExpression.cpp src/cajeta/asn/expression/MethodCallExpression.h
        src/cajeta/error/Exception.cpp src/cajeta/error/Exception.h
        src/cajeta/error/VariableAssignmentException.cpp src/cajeta/error/VariableAssignmentException.h
        src/cajeta/error/CajetaExceptions.h
        src/cajeta/logging/CajetaLogger.cpp src/cajeta/logging/CajetaLogger.h
        src/cajeta/util/LiteralUtils.cpp src/cajeta/util/LiteralUtils.h
        src/cajeta/asn/DefaultBlock.cpp src/cajeta/asn/DefaultBlock.h
        src/cajeta/util/MemoryManager.cpp src/cajeta/util/MemoryManager.h
        src/cajeta/type/StructureProperty.cpp src/cajeta/type/StructureProperty.h
        src/cajeta/field/HeapField.cpp src/cajeta/field/HeapField.h
        src/cajeta/field/StackField.cpp src/cajeta/field/StackField.h
        src/cajeta/field/StructureField.cpp src/cajeta/field/StructureField.h
        src/cajeta/util/FileStream.cpp src/cajeta/util/FileStream.h
        src/cajeta/type/CajetaArray.cpp src/cajeta/type/CajetaArray.h
        src/cajeta/method/DefaultConstructorMethod.h src/cajeta/method/DefaultConstructorMethod.cpp
        src/cajeta/type/StructureMetadata.cpp src/cajeta/type/StructureMetadata.h
        src/cajeta/field/ParameterField.cpp src/cajeta/field/ParameterField.h
        src/cajeta/util/Printer.cpp src/cajeta/util/Printer.h)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")
message(STATUS "Using LLVM includes at: ${LLVM_INCLUDE_DIRS}")
message(STATUS "Using project includes at: ${CMAKE_INCLUDE_SOURCE_DIR}/include")
set(LLVM_TARGETS_TO_BUILD ${LLVM_ALL_TARGETS})

foreach(target ${LLVM_TARGETS_TO_BUILD})
    list(APPEND targets "LLVM${target}CodeGen")
endforeach()

target_link_libraries(cajeta
        antlr4_static
        pthread
        LLVMX86CodeGen
        LLVMX86AsmParser # MC, MCParser, Support, X86Desc, X86Info
        LLVMX86Desc # MC, Support, X86AsmPrinter, X86Info
        LLVMX86Info # MC, Support, Target
        LLVMipo
        LLVMScalarOpts
        LLVMInstCombine
        LLVMTransformUtils
        LLVMAnalysis
        LLVMTarget
        ${targets}
        LLVMOption # Support
        LLVMMCParser # MC, Support
        LLVMMC # Object, Support
        LLVMObject # BitReader, Core, Support
        LLVMBitReader # Core, Support
        LLVMCore # Support
        LLVMSupport
        glog::glog)

