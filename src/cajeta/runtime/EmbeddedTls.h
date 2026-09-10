#pragma once

// Symbols defined by the generated cajeta_tls_embedded.cpp: a native relocatable
// object, not JIT bitcode, since cajeta_tls.c's OpenSSL symbols would force every JIT
// test to resolve SSL_*. The `--emit=exe` link path materializes it on all platforms.
extern "C" {
    extern unsigned char cajeta_tls_o[];
    extern unsigned int cajeta_tls_o_len;
}
