#pragma once

// Symbols defined by the generated cajeta_runtime_embedded.cpp (built from
// runtime/native/cajeta_runtime.c via clang -emit-llvm + xxd -i). The bytes
// are an LLVM bitcode module containing the runtime helpers; CajetaModule
// parses and links it into each output module on first use.
extern "C" {
    extern unsigned char cajeta_runtime_bc[];
    extern unsigned int cajeta_runtime_bc_len;
}
