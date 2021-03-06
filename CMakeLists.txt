# minimum required CMAKE version
CMAKE_MINIMUM_REQUIRED(VERSION 3.7 FATAL_ERROR)
project(cajeta VERSION 1.0.0)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

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

# add macros to generate ANTLR Cpp code from grammar
find_package(ANTLR REQUIRED)
find_package(LLVM REQUIRED CONFIG)
include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

# Call macro to add lexer and grammar to your build dependencies.
antlr_target(CajetaGrammarLexer ${CMAKE_CURRENT_SOURCE_DIR}/antlr4/CajetaLexer.g4 LEXER
             PACKAGE cajeta)
antlr_target(CajetaGrammarParser ${CMAKE_CURRENT_SOURCE_DIR}/antlr4/CajetaParser.g4 PARSER LISTENER
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
        src/main.cpp include/cajeta/CajetaParserIRVisitor.h include/cajeta/Scope.h src/Scope.cpp include/cajeta/Declaration.h include/cajeta/CajetaParserIRListener.h src/CajetaParserIRListener.cpp src/Statement.cpp include/cajeta/Statement.h src/Annotation.cpp include/cajeta/Annotation.h src/Generics.cpp include/cajeta/Generics.h src/Method.cpp include/cajeta/Method.h src/TypeDefinition.cpp include/cajeta/TypeDefinition.h include/cajeta/ParseContext.h include/cajeta/Field.cpp include/cajeta/Field.h include/cajeta/Expression.h src/Expression.cpp)

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
        LLVMSupport)
