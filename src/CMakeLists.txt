CMAKE_MINIMUM_REQUIRED(VERSION 3.7 FATAL_ERROR)
message(-- CMAKE_SOURCE_DIR: ${CMAKE_SOURCE_DIR})
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

# compiler must be 17
set(CMAKE_CXX_STANDARD 17)

# required if linking to static library
add_definitions(-DANTLR4CPP_STATIC)

# using /MD flag for antlr4_runtime (for Visual C++ compilers only)
set(ANTLR4_WITH_STATIC_CRT OFF)

# Specify the version of the antlr4 library needed for this project.
# By default the latest version of antlr4 will be used.  You can specify a
# specific, stable version by setting a repository tag value or a link
# to a zip file containing the libary source.
# set(ANTLR4_TAG 4.13.1)
# set(ANTLR4_ZIP_REPOSITORY https://github.com/antlr/antlr4/archive/refs/tags/4.13.1.zip)

# add external build for antlrcpp
# add antrl4cpp artifacts to project environment
# set variable pointing to the antlr tool that supports C++
# this is not required if the jar file can be found under PATH environment
set(ANTLR_EXECUTABLE ${ANTLR_EXECUTABLE})
# add macros to generate ANTLR Cpp code from grammar

#
#
#
## minimum required CMAKE version
#list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
#set(CMAKE_CXX_STANDARD 17)
#
####
#
## compiler must be 17
#
#
## Specify the version of the antlr4 library needed for this project.
## By default the latest version of antlr4 will be used.  You can specify a
## specific, stable version by setting a repository tag value or a link
## to a zip file containing the libary source.
## set(ANTLR4_TAG 4.13.1)
## set(ANTLR4_ZIP_REPOSITORY https://github.com/antlr/antlr4/archive/refs/tags/4.13.1.zip)
#
## add external build for antlrcpp
#include(ExternalAntlr4Cpp)
## add antrl4cpp artifacts to project environment
#include_directories(${ANTLR4_INCLUDE_DIRS})
#
## set variable pointing to the antlr tool that supports C++
## this is not required if the jar file can be found under PATH environment
#set(ANTLR_EXECUTABLE /home/user/antlr-4.13.1-complete.jar)
## add macros to generate ANTLR Cpp code from grammar
#find_package(ANTLR REQUIRED)

#        COMPILE_FLAGS -lib ${ANTLR_CajetaGrammarLexer_OUTPUT_DIR}
#        OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/generated/parser")

# include generated files in project environment

set(ANTLR_EXECUTABLE $ENV{ANTLR_EXECUTABLE})
message("-- ANTLR_EXCECUTABLE: ${ANTLR_EXECUTABLE}")

#set(ANTLR4_TAG 7ed420f)

# required if linking to static library
add_definitions(-DANTLR4CPP_STATIC)
set(ANTLR4_WITH_STATIC_CRT OFF)
set(ANTLR4_INSTALL ON)
include(ExternalAntlr4Cpp)
message("-- ANTLR4_INCLUDE_DIRS: ${ANTLR4_INCLUDE_DIRS}")

find_package(glog 0.6.0 REQUIRED)
find_package(ANTLR REQUIRED)
## find_package(antlr4_generator REQUIRED)
find_package(LLVM REQUIRED CONFIG)

add_definitions(${LLVM_DEFINITIONS})
include_directories(${LLVM_INCLUDE_DIRS})
include_directories(${ANTLR4_INCLUDE_DIRS})
include_directories("/usr/local/include/antlr4-runtime")
include_directories("${CMAKE_SOURCE_DIR}/src")

# Call macro to add lexer and grammar to your build dependencies.
#antlr4_generate(CajetaGrammarLexer ${CMAKE_SOURCE_DIR}/antlr4/CajetaLexer.g4 LEXER
#        true false cajeta)
#
#antlr4_generate(CajetaGrammarParser ${CMAKE_SOURCE_DIR}/antlr4/CajetaParser.g4 PARSER
#        true false cajeta CajetaGrammarLexer)

## Call macro to add lexer and grammar to your build dependencies.
antlr_target(CajetaGrammarLexer ${CMAKE_SOURCE_DIR}/antlr4/CajetaLexer.g4 LEXER
             PACKAGE cajeta OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/generated/lex")
antlr_target(CajetaGrammarParser ${CMAKE_SOURCE_DIR}/antlr4/CajetaParser.g4 PARSER VISITOR
             PACKAGE cajeta
             DEPENDS_ANTLR CajetaGrammarLexer
             COMPILE_FLAGS -lib ${ANTLR_CajetaGrammarLexer_OUTPUT_DIR}
             OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/generated/parser")

## include generated files in project environment
include_directories(${ANTLR_CajetaGrammarLexer_OUTPUT_DIR})
include_directories(${ANTLR_CajetaGrammarParser_OUTPUT_DIR})

message("-- ANTLR CajetaGrammar Lex Output: ${ANTLR_CajetaGrammarLexer_OUTPUT_DIR}")
message("-- ANTLR CajetaGrammer Parser Output: ${ANTLR_CajetaGrammarParser_OUTPUT_DIR}")

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")
message(STATUS "Using LLVM includes at: ${LLVM_INCLUDE_DIRS}")
message(STATUS "Using project includes at: ${CMAKE_INCLUDE_SOURCE_DIR}/include")

set(LLVM_TARGETS_TO_BUILD ${LLVM_ALL_TARGETS})
foreach(target ${LLVM_TARGETS_TO_BUILD})
    list(APPEND targets "LLVM${target}CodeGen")
endforeach()

set(BINARY ${CMAKE_PROJECT_NAME})
file(GLOB_RECURSE SOURCES LIST_DIRECTORIES true *.h *.cpp)
set(SOURCES ${ANTLR_CajetaGrammarLexer_CXX_OUTPUTS} ${ANTLR_CajetaGrammarParser_CXX_OUTPUTS} ${SOURCES})
add_executable(${CMAKE_PROJECT_NAME} ${SOURCES})
add_library(${CMAKE_PROJECT_NAME}_lib STATIC ${SOURCES})

target_link_libraries(${CMAKE_PROJECT_NAME}
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

