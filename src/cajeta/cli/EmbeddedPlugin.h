#pragma once

// Symbols defined by the generated cajeta_plugin_embedded.cpp (the IntelliJ
// IDEA plugin distribution zip, embedded via xxd -i at compiler-build time —
// same mechanism as the embedded runtime bitcode). When no plugin zip is
// available at build time (e.g. a configure before the Gradle plugin build),
// the generated file defines a zero-length array so the compiler still links;
// `cajeta ide install` then reports that this build carries no bundled plugin.
extern "C" {
    extern unsigned char cajeta_plugin_zip[];
    extern unsigned int cajeta_plugin_zip_len;
}
