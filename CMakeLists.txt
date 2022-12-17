# minimum required CMAKE version
CMAKE_MINIMUM_REQUIRED(VERSION 3.7 FATAL_ERROR)
project(cajeta VERSION 1.0.0)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

cmake_minimum_required (VERSION 3.16)
project (myproj VERSION 1.0)

find_package (glog 0.6.0 REQUIRED)

# compiler must be 11 or 14
set(CMAKE_CXX_STANDARD 17)
set(Java_JAVA_EXECUTABLE "java")
set(ANTLR4_TAG e4c1a74)

# required if linking to static library
add_definitions(-DANTLR4CPP_STATIC)

# using /MD flag for antlr4_runtime (for Visual C++ compilers only)
set(ANTLR4_WITH_STATIC_CRT OFF)
# add external build for antlrcpp
include(ExternalAntlr4Cpp)
# add antrl4cpp artifacts to project environment
include_directories(${ANTLR4_INCLUDE_DIRS})

# set variable pointing to the antlr tool that supports C++
# this is not required if the jar file can be found under PATH environment
set(ANTLR_EXECUTABLE /Users/julian/code/antlr4/tool/target/antlr4-4.9.4-SNAPSHOT-complete.jar)

# add macros to codegen ANTLR Cpp code of grammar
find_package(ANTLR REQUIRED)
find_package(LLVM REQUIRED CONFIG)
include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

# Call macro to add lexer and grammar to your build dependencies.
antlr_target(CajetaGrammarLexer ${CMAKE_CURRENT_SOURCE_DIR}/antlr4/CajetaLexer.g4 LEXER
             PACKAGE cajeta)
antlr_target(CajetaGrammarParser ${CMAKE_CURRENT_SOURCE_DIR}/antlr4/CajetaParser.g4 PARSER VISITOR
             PACKAGE cajeta
             DEPENDS_ANTLR CajetaGrammarLexer
             COMPILE_FLAGS -lib ${ANTLR_CajetaGrammarLexer_OUTPUT_DIR})

# include generated files in project environment
include_directories(${ANTLR_CajetaGrammarLexer_OUTPUT_DIR})
include_directories(${ANTLR_CajetaGrammarParser_OUTPUT_DIR})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# add generated grammar to demo binary target
add_executable(cajeta
        ${ANTLR_CajetaGrammarLexer_CXX_OUTPUTS}
        ${ANTLR_CajetaGrammarParser_CXX_OUTPUTS}
        src/main.cpp include/cajeta/type/Scope.h include/cajeta/type/Scope.cpp
        include/cajeta/asn/Statement.cpp include/cajeta/asn/Statement.h
        include/cajeta/type/Generics.cpp include/cajeta/type/Generics.h
        include/cajeta/method/Method.cpp include/cajeta/method/Method.h
        include/cajeta/type/Field.cpp include/cajeta/type/Field.h
        include/cajeta/asn/Expression.h include/cajeta/asn/Expression.cpp
        include/cajeta/type/CajetaType.cpp include/cajeta/type/CajetaType.h
        include/cajeta/type/CajetaInterface.h include/cajeta/type/CajetaInterface.cpp
        include/cajeta/type/CajetaClass.cpp include/cajeta/type/CajetaClass.h
        include/cajeta/type/QualifiedName.h include/cajeta/type/QualifiedName.cpp
        include/cajeta/asn/AbstractSyntaxNode.cpp include/cajeta/asn/AbstractSyntaxNode.h
        include/cajeta/compile/CajetaModule.cpp include/cajeta/compile/CajetaModule.h
        include/cajeta/compile/Compiler.cpp include/cajeta/compile/Compiler.h
        include/cajeta/type/CajetaStructure.cpp include/cajeta/type/CajetaStructure.h
        include/cajeta/type/FormalParameter.h include/cajeta/type/FormalParameter.cpp
        include/cajeta/asn/Block.h include/cajeta/asn/Block.cpp
        include/cajeta/asn/BlockStatement.h include/cajeta/asn/BlockStatement.cpp
        include/cajeta/asn/VariableDeclarator.cpp include/cajeta/asn/VariableDeclarator.h
        include/cajeta/asn/LocalVariableDeclaration.cpp include/cajeta/asn/LocalVariableDeclaration.h
        include/cajeta/asn/ClassBodyDeclaration.cpp include/cajeta/asn/ClassBodyDeclaration.h
        include/cajeta/exception/Exception.cpp include/cajeta/exception/Exception.h
        include/cajeta/exception/VariableAssignmentException.cpp include/cajeta/exception/VariableAssignmentException.h
        include/cajeta/exception/CajetaExceptions.h
        include/cajeta/logging/CajetaLogger.cpp include/cajeta/logging/CajetaLogger.h
        include/cajeta/util/LiteralUtils.cpp include/cajeta/util/LiteralUtils.h
        include/cajeta/asn/DefaultBlock.cpp include/cajeta/asn/DefaultBlock.h
        include/cajeta/util/MemoryManager.cpp include/cajeta/util/MemoryManager.h
        include/cajeta/type/ClassProperty.cpp include/cajeta/type/ClassProperty.h
        include/cajeta/type/LocalField.cpp include/cajeta/type/LocalField.h
        include/cajeta/type/LocalPropertyField.cpp include/cajeta/type/LocalPropertyField.h
        include/cajeta/util/FileStream.cpp include/cajeta/util/FileStream.h
        include/cajeta/type/CajetaArray.cpp include/cajeta/type/CajetaArray.h
        include/cajeta/method/DefaultConstructorMethod.h include/cajeta/method/DefaultConstructorMethod.cpp
        include/cajeta/type/StructureMetadata.cpp include/cajeta/type/StructureMetadata.h)

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
        LLVMSupport
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

