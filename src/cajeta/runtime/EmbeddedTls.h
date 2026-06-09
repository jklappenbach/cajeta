#pragma once

// Symbols defined by the generated cajeta_tls_embedded.cpp (built from
// runtime/native/cajeta_tls.c via the host C compiler + xxd -i). Unlike the
// runtime bitcode (EmbeddedRuntime.h), these bytes are a *native relocatable
// object* — cajeta_tls.c is deliberately kept OUT of the embedded JIT bitcode
// (its OpenSSL headers/symbols would force every JIT test to resolve SSL_* at
// load), so the standalone `--emit=exe` link path materializes this object and
// links it in. The produced exe references `__cajeta_tls_*` from the always-
// linked stdlib TlsConnection thunks (cajeta roots class metadata via eager
// clinit global-ctors, so --gc-sections can't drop them on PE-COFF); the build
// machine has the mingw toolchain but not cajeta's runtime source, so the bytes
// ride along in the compiler binary. Windows only — that's where the standalone
// exe link set is wired and where the embedded host object format matches the
// produced exe. See Compiler::linkExecutable and src/CMakeLists.txt.
#if defined(_WIN32)
extern "C" {
    extern unsigned char cajeta_tls_o[];
    extern unsigned int cajeta_tls_o_len;
}
#endif
