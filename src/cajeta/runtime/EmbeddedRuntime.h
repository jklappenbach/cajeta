#pragma once

// The runtime helpers as an LLVM bitcode module, embedded by the generated
// cajeta_runtime_embedded.cpp; CajetaModule links it into each output module.
extern "C" {
    extern unsigned char cajeta_runtime_bc[];
    extern unsigned int cajeta_runtime_bc_len;
}
