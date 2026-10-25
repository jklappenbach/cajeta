// codec/base64-{encode,decode} and codec/base32-{encode,decode} competitor
// rows: 1 MiB of data[i] = (i*31+7) & 0xFF, encoded padded; trials default
// to the page's 1e3. Checks: encoded length, decoded bytes equal the input.
// Decode errors propagate as Result (checked, never a panic).
mod common;
use base64::Engine;
use common::*;
use data_encoding::BASE32;
use std::hint::black_box;

#[global_allocator]
static GA: Counting = Counting;

const N: usize = 1048576;

fn lock_version(name: &str) -> String {
    let lock = include_str!("../Cargo.lock");
    let mut it = lock.lines();
    while let Some(l) = it.next() {
        if l == format!("name = \"{name}\"") {
            if let Some(v) = it.next() {
                return v.trim_start_matches("version = ").trim_matches('"').to_string();
            }
        }
    }
    String::new()
}

fn main() {
    let warmup = env_n("PROFILE_WARMUP", 3);
    let trials = env_n("PROFILE_TRIALS", 1000);
    let data: Vec<u8> = (0..N).map(|i| ((i * 31 + 7) & 0xFF) as u8).collect();
    const NOTE: &str = "decode errors propagate as Result";

    // base64 (STANDARD = RFC 4648 alphabet with padding)
    let v64 = lock_version("base64");
    let row = |slug| Row { slug, dataset: "", bytes: N as i64, input: N as i64, lib: "base64",
                           ver: &v64, warmup, trials, throughput: true, variant: "" };
    let eng = base64::engine::general_purpose::STANDARD;
    let mut enc = String::new();
    let s = measure(warmup, trials, || { enc = black_box(eng.encode(black_box(&data))); });
    let a = alloc_of(|| { black_box(eng.encode(&data)); });
    emit(&row("base64-encode"), &s, a, enc.len() == 1398104, &format!("len={}; engine STANDARD", enc.len()));
    let encoded = eng.encode(&data);
    let mut dec: Result<Vec<u8>, base64::DecodeError> = Ok(Vec::new());
    let s = measure(warmup, trials, || { dec = black_box(eng.decode(black_box(&encoded))); });
    let a = alloc_of(|| { black_box(eng.decode(&encoded).ok()); });
    let eq = dec.as_ref().map(|d| *d == data).unwrap_or(false);
    emit(&row("base64-decode"), &s, a, eq, &format!("equal={eq}; engine STANDARD; {NOTE}"));

    // base32 (data-encoding BASE32 = RFC 4648 with padding)
    let v32 = lock_version("data-encoding");
    let row = |slug| Row { slug, dataset: "", bytes: N as i64, input: N as i64, lib: "data-encoding",
                           ver: &v32, warmup, trials, throughput: true, variant: "" };
    let mut enc = String::new();
    let s = measure(warmup, trials, || { enc = black_box(BASE32.encode(black_box(&data))); });
    let a = alloc_of(|| { black_box(BASE32.encode(&data)); });
    emit(&row("base32-encode"), &s, a, enc.len() == 1677728, &format!("len={}; BASE32 padded", enc.len()));
    let encoded = BASE32.encode(&data);
    let mut dec: Result<Vec<u8>, data_encoding::DecodeError> = Ok(Vec::new());
    let s = measure(warmup, trials, || { dec = black_box(BASE32.decode(black_box(encoded.as_bytes()))); });
    let a = alloc_of(|| { black_box(BASE32.decode(encoded.as_bytes()).ok()); });
    let eq = dec.as_ref().map(|d| *d == data).unwrap_or(false);
    emit(&row("base32-decode"), &s, a, eq, &format!("equal={eq}; BASE32 padded; {NOTE}"));
}
