// Rust wire/ competitors: bincode 1.3 (serde derive, default fixint config) for
// encoder-encode/decode; flate2 and zstd streams for compressstream-roundtrip.
// Record and constants from bench/scripts/gen_wire.py (expect.tsv at
// $WIRE_EXPECT). Errors propagate as Result: a failed encode/decode/stream
// returns Err and the row is not emitted (the driver's skip row reports it).
mod common;
mod record;

use common::*;
use record::WireRecord;
use std::collections::HashMap;
use std::io::{Read, Write};

#[global_allocator]
static GLOBAL: Counting = Counting;

const POOL: usize = 1024;
type Res<T> = Result<T, Box<dyn std::error::Error>>;

fn expect() -> Res<HashMap<String, i64>> {
    let t = std::fs::read_to_string(env("WIRE_EXPECT", "expect.tsv"))?;
    let mut m = HashMap::new();
    for l in t.lines() {
        if l.is_empty() || l.starts_with('#') { continue; }
        let mut p = l.split('\t');
        if let (Some(k), Some(v)) = (p.next(), p.next()) { m.insert(k.to_string(), v.parse()?); }
    }
    Ok(m)
}

fn stream_data(size: usize) -> Vec<u8> {
    let mut b = Vec::with_capacity(size + 720);
    let mut r = 0usize;
    while b.len() < size {
        let p = (r % POOL) as i64;
        for k in 1..=100i64 {
            if k % 5 == 0 { b.extend_from_slice(&(p as f64 * 0.5 + k as f64 * 0.25).to_le_bytes()); }
            else if k % 5 == 1 { b.extend_from_slice(&(((p * 31 + k * 17) % 65536) as i32).to_le_bytes()); }
            else { b.extend_from_slice(&((p + 1) * (k * k * k + 17)).to_le_bytes()); }
        }
        r += 1;
    }
    b.truncate(size);
    b
}

/// Measure a fallible body: the first Err aborts the run.
fn measure_res<F: FnMut() -> Res<()>>(warmup: usize, trials: usize, mut f: F) -> Res<Stats> {
    let mut err: Option<String> = None;
    let s = measure(warmup, trials, || { if err.is_none() { if let Err(e) = f() { err = Some(e.to_string()); } } });
    match err { Some(e) => Err(e.into()), None => Ok(s) }
}

fn main() -> Res<()> {
    let smoke = smoke();
    let tag = if smoke { "smoke" } else { "full" };
    let n: usize = if smoke { 100_000 } else { 1_000_000 };
    let warmup = env_n("PROFILE_WARMUP", 2);
    let trials = env_n("PROFILE_TRIALS", 5).max(1);
    let ex = expect()?;
    let e = |k: &str| *ex.get(k).unwrap_or(&-1);
    let sz = if smoke { "; smoke size N=1e5 in place of 1e6" } else { "" };
    let bver = "1.3.3";

    let pool: Vec<WireRecord> = (0..POOL as i64).map(WireRecord::fill).collect();
    let fixed = bincode::serialize(&pool[0])?.len() as i64;
    let mut total = 0i64;
    let mut same = true;
    let s = measure_res(warmup, trials, || {
        total = 0;
        for i in 0..n {
            let b = bincode::serialize(&pool[i % POOL])?;
            same &= b.len() as i64 == fixed;
            total += b.len() as i64;
            std::hint::black_box(&b);
        }
        Ok(())
    })?;
    let a = alloc_of(|| { let _ = bincode::serialize(&pool[0]); });
    emit("encoder-encode", n as i64, "bincode", bver, warmup, trials, &s, a * n as i64, same && total == n as i64 * fixed,
         &format!("bincode::serialize (serde derive; bincode 1 default config = fixint little-endian) to a fresh Vec; check: total bytes == N * {}{}", fixed, sz),
         "", "records/s");

    let enc: Vec<Vec<u8>> = pool.iter().map(|r| bincode::serialize(r)).collect::<Result<_, _>>()?;
    let mut sum = 0i64;
    let s = measure_res(warmup, trials, || {
        sum = 0;
        for i in 0..n {
            let r: WireRecord = bincode::deserialize(&enc[i % POOL])?;
            sum += r.checksum();
        }
        Ok(())
    })?;
    emit("encoder-decode", n as i64, "bincode", bver, warmup, trials, &s, -1, sum == e(&format!("decode_sum_{}", tag)),
         &format!("bincode::deserialize::<WireRecord>(&bytes)? (Result propagated); check: field checksum == decode_sum{}", sz),
         "", "records/s");

    // compressstream-roundtrip
    let size: usize = if smoke { 8 << 20 } else { 64 << 20 };
    const PIECE: usize = 65536;
    const FLUSH: usize = 1 << 20;
    let data = stream_data(size);
    let ssz = if smoke { "; smoke size 8 MiB in place of 64 MiB" } else { "" };
    let mut back: Vec<u8> = Vec::new();
    let mut clen = 0usize;
    let s = measure_res(warmup, trials, || {
        let mut w = flate2::write::DeflateEncoder::new(Vec::with_capacity(size / 2), flate2::Compression::new(1));
        let mut off = 0;
        while off < size {
            let m = PIECE.min(size - off);
            w.write_all(&data[off..off + m])?;
            if (off + m) % FLUSH == 0 && off + m < size { w.flush()?; }
            off += m;
        }
        let c = w.finish()?;
        clen = c.len();
        back = Vec::with_capacity(size);
        flate2::read::DeflateDecoder::new(&c[..]).read_to_end(&mut back)?;
        Ok(())
    })?;
    emit("compressstream-roundtrip", size as i64, "flate2", "1.1.10 (miniz_oxide 0.9.1)", warmup, trials, &s, -1, back == data,
         &format!("flate2::write::DeflateEncoder (raw DEFLATE) level 1; 64 KiB write_all; flush (Z_SYNC_FLUSH) every 1 MiB; finish; DeflateDecoder read_to_end; compressed {} B; check: byte equality{}", clen, ssz),
         "zlib", "MB/s");
    let s = measure_res(warmup, trials, || {
        let mut w = zstd::stream::write::Encoder::new(Vec::with_capacity(size / 2), 1)?;
        let mut off = 0;
        while off < size {
            let m = PIECE.min(size - off);
            w.write_all(&data[off..off + m])?;
            if (off + m) % FLUSH == 0 && off + m < size { w.flush()?; }
            off += m;
        }
        let c = w.finish()?;
        clen = c.len();
        back = Vec::with_capacity(size);
        zstd::stream::read::Decoder::new(&c[..])?.read_to_end(&mut back)?;
        Ok(())
    })?;
    emit("compressstream-roundtrip", size as i64, "zstd", &format!("0.13.3 (libzstd {})", zstd::zstd_safe::version_string()), warmup, trials, &s, -1, back == data,
         &format!("zstd::stream::write::Encoder level 1; 64 KiB write_all; flush (ZSTD_e_flush) every 1 MiB; finish; read::Decoder read_to_end; compressed {} B; check: byte equality{}", clen, ssz),
         "zstd", "MB/s");
    Ok(())
}
