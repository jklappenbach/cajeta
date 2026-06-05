# cajetadoc — cajeta documentation generator (plan: plans/docs/cajetadoc-tool.md).
#
# The doc engine is a reusable OBJECT library (`cajetadoc_core`) so the same
# objects embed into BOTH the standalone `cajetadoc` binary and the main
# `cajeta` binary (as the `cajeta doc` subcommand) without duplicate-symbol or
# static-archive ordering problems. `cajetadoc_core` does NOT compile the antlr
# parser — the consumer provides it: the standalone binary links
# `cajetadoc_parser` (the checked-in generated lexer/parser), while `cajeta`
# reuses the parser already inside `cajeta_lib`. Either way it links only the
# antlr4 runtime — no LLVM is needed for the doc model.
cmake_minimum_required(VERSION 3.25)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- antlr4 runtime (headers + library) -----------------------------------
find_path(CAJETADOC_ANTLR4_INCLUDE_DIR
    NAMES antlr4-runtime.h
    PATH_SUFFIXES antlr4-runtime
    HINTS
        /opt/homebrew/opt/antlr4-cpp-runtime/include
        /usr/local/opt/antlr4-cpp-runtime/include
    PATHS /usr/include /usr/local/include)
find_library(CAJETADOC_ANTLR4_RUNTIME_LIB
    NAMES antlr4-runtime
    HINTS
        /opt/homebrew/opt/antlr4-cpp-runtime/lib
        /usr/local/opt/antlr4-cpp-runtime/lib
    PATHS /usr/lib/${CMAKE_LIBRARY_ARCHITECTURE} /usr/lib /usr/local/lib)

if(NOT CAJETADOC_ANTLR4_INCLUDE_DIR OR NOT CAJETADOC_ANTLR4_RUNTIME_LIB)
    message(WARNING "cajetadoc: antlr4 runtime not found — skipping cajetadoc target")
    return()
endif()

# --- core doc engine (OBJECT lib; no parser sources) -----------------------
add_library(cajetadoc_core OBJECT
    src/Model.cpp
    src/DocComment.cpp
    src/Ingest.cpp
    src/Json.cpp
    src/Markdown.cpp
    src/Render.cpp
    src/Stylesheet.cpp
    src/Cli.cpp)
target_compile_definitions(cajetadoc_core PUBLIC ANTLR4CPP_STATIC)
# PUBLIC so consumers (standalone binary, cajeta_lib) inherit the include paths
# needed to compile against cajetadoc/* headers and the generated parser headers.
target_include_directories(cajetadoc_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CAJETADOC_ANTLR4_INCLUDE_DIR}
    ${PROJECT_SOURCE_DIR}/generated/lex
    ${PROJECT_SOURCE_DIR}/generated/parser)
# Needed so OBJECT-lib objects are position-independent when linked into the
# shared/static graph of the cajeta binary.
set_target_properties(cajetadoc_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- generated parser/lexer (for the standalone binary + tests only) -------
set(CAJETADOC_GENERATED
    ${PROJECT_SOURCE_DIR}/generated/lex/CajetaLexer.cpp
    ${PROJECT_SOURCE_DIR}/generated/parser/CajetaParser.cpp
    ${PROJECT_SOURCE_DIR}/generated/parser/CajetaParserBaseVisitor.cpp
    ${PROJECT_SOURCE_DIR}/generated/parser/CajetaParserVisitor.cpp)
set_source_files_properties(${CAJETADOC_GENERATED} PROPERTIES GENERATED TRUE)
add_library(cajetadoc_parser STATIC ${CAJETADOC_GENERATED})
target_compile_definitions(cajetadoc_parser PUBLIC ANTLR4CPP_STATIC)
target_include_directories(cajetadoc_parser PUBLIC
    ${CAJETADOC_ANTLR4_INCLUDE_DIR}
    ${PROJECT_SOURCE_DIR}/generated/lex
    ${PROJECT_SOURCE_DIR}/generated/parser)
target_link_libraries(cajetadoc_parser PUBLIC ${CAJETADOC_ANTLR4_RUNTIME_LIB})

# --- standalone binary -----------------------------------------------------
add_executable(cajetadoc main.cpp)
target_link_libraries(cajetadoc PRIVATE cajetadoc_core cajetadoc_parser
    ${CAJETADOC_ANTLR4_RUNTIME_LIB})

# --- tests -----------------------------------------------------------------
find_package(GTest QUIET)
if(GTest_FOUND)
    add_executable(cajetadoc_test test/CajetadocTest.cpp)
    target_compile_definitions(cajetadoc_test PRIVATE
        CAJETADOC_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures")
    target_link_libraries(cajetadoc_test PRIVATE cajetadoc_core cajetadoc_parser
        ${CAJETADOC_ANTLR4_RUNTIME_LIB} GTest::gtest GTest::gtest_main)
    include(GoogleTest)
    gtest_discover_tests(cajetadoc_test PROPERTIES LABELS cajetadoc)
endif()
