# cajeta-mcp — standalone MCP server that WRAPS the cajeta compiler.
#
# It does NOT link the compiler (cajeta_lib); it only reuses three small utility
# TUs from src/ (Json, Subprocess, TarZstd) and links LLVMSupport (base64 /
# Expected) + zstd. The server implements every tool by shelling out to a
# `cajeta` subcommand at runtime.
cmake_minimum_required(VERSION 3.25)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Own LLVM lookup (LLVM_DIR is cached by the top-level configure). Only the
# Support component is needed (base64, Error/Expected, StringRef).
find_package(LLVM REQUIRED CONFIG)
llvm_map_components_to_libnames(MCP_LLVM_LIBS support)

# zstd for the tar.zstd archive decode (TarZstd.cpp).
find_path(MCP_ZSTD_INCLUDE_DIR NAMES zstd.h)
find_library(MCP_ZSTD_LIB NAMES zstd)
if(NOT MCP_ZSTD_INCLUDE_DIR OR NOT MCP_ZSTD_LIB)
    message(WARNING "cajeta-mcp: zstd not found — skipping cajeta-mcp target")
    return()
endif()

# Reused compiler-side utility TUs (compiled in directly; no cajeta_lib link).
set(MCP_REUSED_TUS
    ${PROJECT_SOURCE_DIR}/src/cajeta/dap/Json.cpp
    ${PROJECT_SOURCE_DIR}/src/cajeta/buildtool/Subprocess.cpp
    ${PROJECT_SOURCE_DIR}/src/cajeta/buildtool/repo/TarZstd.cpp)

# Server logic as an OBJECT lib so the test binary can reuse the same objects
# (it provides the reused TUs itself via cajeta_lib).
add_library(cajeta_mcp_core OBJECT McpServer.cpp)
target_include_directories(cajeta_mcp_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/src
    ${LLVM_INCLUDE_DIRS}
    ${MCP_ZSTD_INCLUDE_DIR})
target_compile_definitions(cajeta_mcp_core PUBLIC ${LLVM_DEFINITIONS})
set_target_properties(cajeta_mcp_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_executable(cajeta-mcp
    main.cpp
    ${MCP_REUSED_TUS}
    $<TARGET_OBJECTS:cajeta_mcp_core>)
target_include_directories(cajeta-mcp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/src
    ${LLVM_INCLUDE_DIRS}
    ${MCP_ZSTD_INCLUDE_DIR})
target_link_libraries(cajeta-mcp PRIVATE ${MCP_LLVM_LIBS} ${MCP_ZSTD_LIB})
