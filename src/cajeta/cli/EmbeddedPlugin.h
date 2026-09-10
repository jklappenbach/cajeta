#pragma once

// The IntelliJ plugin zip, embedded by the generated cajeta_plugin_embedded.cpp.
// Zero-length when the build carried no plugin zip, so the compiler still links.
extern "C" {
    extern unsigned char cajeta_plugin_zip[];
    extern unsigned int cajeta_plugin_zip_len;
}
