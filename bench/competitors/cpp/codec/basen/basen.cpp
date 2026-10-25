// C++ Base64 competitor for bench/codec/Base64.md: simdutf (the SIMD
// implementation the page reads Cajeta against). 1 MiB of data[i] =
// (i*31+7)&0xFF, standard alphabet, padded; each trial allocates its output
// (uninitialized new char[]) the way the other languages' encode/decode calls
// return a fresh buffer. The page has no C++ Base32 competitor, so this
// program emits base64 rows only.
#include "simdutf.h"
#include "row.h"

#include <cstring>
#include <memory>

static volatile size_t g_sink = 0;

int main() {
    const size_t N = 1048576;
    int warmup = row::env_int("PROFILE_WARMUP", 3);
    int trials = row::env_int("PROFILE_TRIALS", 1000);
    if (trials < 1) trials = 1;
    std::string ver = std::string(SIMDUTF_VERSION) + " @8d82299";
    const char* impl = simdutf::get_active_implementation()->name().data();

    std::unique_ptr<char[]> data(new char[N]);
    for (size_t i = 0; i < N; i++) data[i] = (char)((i * 31 + 7) & 0xFF);

    // base64-encode
    size_t elen = simdutf::base64_length_from_binary(N);
    std::unique_ptr<char[]> enc;
    size_t outLen = 0;
    auto se = row::measure(warmup, trials, [&] {
        std::unique_ptr<char[]> o(new char[elen]);
        outLen = simdutf::binary_to_base64(data.get(), N, o.get());
        g_sink = g_sink + (size_t)(unsigned char)o[outLen / 2];
        enc = std::move(o);
    });
    row::emit("base64-encode", "", -1, (long long)N, "simdutf", ver, warmup, trials, se,
              outLen == 1398104, std::string("binary_to_base64; kernel ") + impl, "");

    // base64-decode (of the encoding above)
    std::string text(enc.get(), outLen);
    size_t dmax = simdutf::maximal_binary_length_from_base64(text.data(), text.size());
    std::unique_ptr<char[]> dec;
    size_t decLen = 0;
    bool derr = false;
    auto sd = row::measure(warmup, trials, [&] {
        std::unique_ptr<char[]> o(new char[dmax]);
        simdutf::result r = simdutf::base64_to_binary(text.data(), text.size(), o.get());
        derr = r.error != simdutf::error_code::SUCCESS;
        decLen = r.count;
        g_sink = g_sink + (size_t)(unsigned char)o[decLen / 2];
        dec = std::move(o);
    });
    bool eq = !derr && decLen == N && std::memcmp(dec.get(), data.get(), N) == 0;
    row::emit("base64-decode", "", -1, (long long)N, "simdutf", ver, warmup, trials, sd, eq,
              std::string("base64_to_binary; kernel ") + impl, "");
    return 0;
}
