// codec/csv-parse and codec/csv-write competitor rows for the `csv` crate:
// parse the generated file (datasets/gen_csv.py) into typed records, and
// write those records back with a header, minimal quoting and "\n" line
// endings. Checks: row count and qty sum against the .meta; output byte
// count against the file size. Errors propagate as Result.
mod common;
use common::*;
use serde::{Deserialize, Serialize};
use std::hint::black_box;

#[global_allocator]
static GA: Counting = Counting;

#[derive(Deserialize, Serialize)]
struct Rec { id: i64, name: String, qty: i32, price: f64, note: String }

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

fn parse(b: &[u8]) -> Result<Vec<Rec>, csv::Error> {
    csv::ReaderBuilder::new().has_headers(true).from_reader(b).deserialize().collect()
}

fn write(rows: &[Rec]) -> Result<Vec<u8>, Box<dyn std::error::Error>> {
    let mut w = csv::WriterBuilder::new().terminator(csv::Terminator::Any(b'\n'))
        .from_writer(Vec::with_capacity(1 << 20));
    for r in rows { w.serialize(r)?; }
    Ok(w.into_inner()?)
}

fn main() {
    let dir = match std::env::var("PROFILE_DATA_DIR") { Ok(d) => d, Err(_) => {
        eprintln!("rust/codec/csv: PROFILE_DATA_DIR unset"); return; } };
    let warmup = env_n("PROFILE_WARMUP", 3);
    let trials = env_n("PROFILE_TRIALS", 10);
    let scale = if smoke() { "smoke" } else { "full" };
    let dataset = format!("codec-csv-{scale}");
    let ver = lock_version("csv");
    let mut notes = String::from("serde-typed records; errors propagate as Result");
    if smoke() { notes += "; smoke input 4 MB"; }

    let bytes = std::fs::read(format!("{dir}/{dataset}.csv"));
    let meta = std::fs::read_to_string(format!("{dir}/{dataset}.meta"));
    let (bytes, meta) = match (bytes, meta) { (Ok(b), Ok(m)) => (b, m), _ => {
        for slug in ["csv-parse", "csv-write"] {
            emit_skip(&Row { slug, dataset: &dataset, bytes: -1, input: -1, lib: "csv", ver: &ver, warmup, trials,
                             throughput: true, variant: "" }, "input absent (run datasets/gen_csv.py)");
        }
        return; } };
    let m: Vec<i64> = meta.split_whitespace().filter_map(|x| x.parse().ok()).collect();
    let (exp_rows, exp_qty, exp_bytes) = (m[0], m[1], m[2]);
    let n = bytes.len() as i64;
    let row = |slug| Row { slug, dataset: &dataset, bytes: n, input: n, lib: "csv", ver: &ver, warmup, trials,
                           throughput: true, variant: "" };

    // csv-parse
    let mut last: Result<Vec<Rec>, csv::Error> = Ok(Vec::new());
    let s = measure(warmup, trials, || { last = black_box(parse(black_box(&bytes))); });
    let a = alloc_of(|| { black_box(parse(&bytes).ok()); });
    let (rows, qty) = last.as_ref().map(|v| (v.len() as i64, v.iter().map(|r| r.qty as i64).sum::<i64>())).unwrap_or((-1, -1));
    emit(&row("csv-parse"), &s, a, rows == exp_rows && qty == exp_qty,
         &format!("rows={rows} fields={} qty_sum={qty}; {notes}", rows * 5));

    // csv-write (rows parsed untimed)
    let recs = match parse(&bytes) { Ok(r) => r, Err(e) => { emit_skip(&row("csv-write"), &format!("parse failed: {e}")); return; } };
    let mut out: Vec<u8> = Vec::new();
    let s = measure(warmup, trials, || { out = black_box(write(black_box(&recs)).unwrap_or_default()); });
    let a = alloc_of(|| { black_box(write(&recs).ok()); });
    emit(&row("csv-write"), &s, a, out.len() as i64 == exp_bytes,
         &format!("out={} bytes; identical={}; {notes}", out.len(), out == bytes));
}
