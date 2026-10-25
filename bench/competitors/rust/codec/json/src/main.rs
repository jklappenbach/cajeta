// codec/json-* competitor rows for serde_json (bench/codec/json/Json.md).
// One row per (benchmark, corpus file), plus one json-conformance row; the
// checks are the page's (token count, node count, re-parse, equality,
// accept/reject counts). Errors propagate as Result: a parse error in a
// timed body is recorded by the check, never a panic.
mod common;
use common::*;
use serde::de::{DeserializeSeed, Deserializer, IgnoredAny, MapAccess, SeqAccess, Visitor};
use serde::Deserialize;
use serde_json::Value;
use std::fmt;
use std::hint::black_box;

#[global_allocator]
static GA: Counting = Counting;

const FILES: &[(&str, u64, u64)] = &[
    ("twitter", 29573, 13914),
    ("citm_catalog", 85035, 37778),
    ("canada", 223236, 167179),
];

// ---- tokenize: a counting DeserializeSeed that walks the document without
// building anything (serde_json has no public token stream; this is its
// streaming deserializer driven by a visitor that counts container start,
// container end, key and scalar events).
struct Count<'a>(&'a mut u64);
impl<'de, 'a> DeserializeSeed<'de> for Count<'a> {
    type Value = ();
    fn deserialize<D: Deserializer<'de>>(self, d: D) -> Result<(), D::Error> { d.deserialize_any(self) }
}
impl<'de, 'a> Visitor<'de> for Count<'a> {
    type Value = ();
    fn expecting(&self, f: &mut fmt::Formatter) -> fmt::Result { f.write_str("any JSON value") }
    fn visit_bool<E>(self, _: bool) -> Result<(), E> { *self.0 += 1; Ok(()) }
    fn visit_i64<E>(self, _: i64) -> Result<(), E> { *self.0 += 1; Ok(()) }
    fn visit_u64<E>(self, _: u64) -> Result<(), E> { *self.0 += 1; Ok(()) }
    fn visit_f64<E>(self, _: f64) -> Result<(), E> { *self.0 += 1; Ok(()) }
    fn visit_str<E>(self, _: &str) -> Result<(), E> { *self.0 += 1; Ok(()) }
    fn visit_borrowed_str<E>(self, _: &'de str) -> Result<(), E> { *self.0 += 1; Ok(()) }
    fn visit_string<E>(self, _: String) -> Result<(), E> { *self.0 += 1; Ok(()) }
    fn visit_unit<E>(self) -> Result<(), E> { *self.0 += 1; Ok(()) }
    fn visit_seq<A: SeqAccess<'de>>(self, mut s: A) -> Result<(), A::Error> {
        *self.0 += 2;
        while s.next_element_seed(Count(&mut *self.0))?.is_some() {}
        Ok(())
    }
    fn visit_map<A: MapAccess<'de>>(self, mut m: A) -> Result<(), A::Error> {
        *self.0 += 2;
        while m.next_key::<IgnoredAny>()?.is_some() {
            *self.0 += 1;
            m.next_value_seed(Count(&mut *self.0))?;
        }
        Ok(())
    }
}

fn tokens(b: &[u8]) -> Result<u64, serde_json::Error> {
    let mut n = 0u64;
    let mut de = serde_json::Deserializer::from_slice(b);
    Count(&mut n).deserialize(&mut de)?;
    de.end()?;
    Ok(n)
}

// ---- bind-skip: a typed record that declares no fields; serde's derive
// skips every unknown key (IgnoredAny underneath).
#[derive(Deserialize)]
struct Empty {}

fn nodes(v: &Value) -> u64 {
    match v {
        Value::Array(a) => 1 + a.iter().map(nodes).sum::<u64>(),
        Value::Object(o) => 1 + o.values().map(nodes).sum::<u64>(),
        _ => 1,
    }
}

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
    let dir = match std::env::var("PROFILE_DATA_DIR") { Ok(d) => d, Err(_) => {
        eprintln!("rust/codec/json: PROFILE_DATA_DIR unset"); return; } };
    let warmup = env_n("PROFILE_WARMUP", 3);
    let trials = env_n("PROFILE_TRIALS", 10);
    let ver = lock_version("serde_json");
    let lib = "serde_json";
    const NOTE: &str = "feature float_roundtrip (correctly rounded floats); errors propagate as Result";

    for &(name, exp_tok, exp_nodes) in FILES {
        let path = format!("{dir}/{name}.json");
        let bytes = match std::fs::read(&path) { Ok(b) => b, Err(_) => {
            for slug in ["json-tokenize", "json-bind-skip", "json-dom", "json-serialize", "json-roundtrip"] {
                emit_skip(&Row { slug, dataset: name, bytes: -1, input: -1, lib, ver: &ver, warmup, trials,
                                 throughput: true, variant: name }, "dataset absent (run datasets/fetch.sh)");
            }
            continue; } };
        let n = bytes.len() as i64;
        let row = |slug| Row { slug, dataset: name, bytes: n, input: n, lib, ver: &ver, warmup, trials,
                               throughput: true, variant: name };

        // json-tokenize
        let mut last = Err(());
        let s = measure(warmup, trials, || { last = black_box(tokens(black_box(&bytes))).map_err(|_| ()); });
        let a = alloc_of(|| { black_box(tokens(&bytes).ok()); });
        let got = last.unwrap_or(0);
        emit(&row("json-tokenize"), &s, a, got == exp_tok,
             &format!("tokens={got}; serde_json has no token stream: its streaming deserializer driven by a counting visitor (no tree built); {NOTE}"));

        // json-bind-skip
        let mut ok = false;
        let s = measure(warmup, trials, || { ok = black_box(serde_json::from_slice::<Empty>(black_box(&bytes))).is_ok(); });
        let a = alloc_of(|| { black_box(serde_json::from_slice::<Empty>(&bytes).ok()); });
        emit(&row("json-bind-skip"), &s, a, ok,
             &format!("serde derive struct with no fields (unknown keys skipped via IgnoredAny); {NOTE}"));

        // json-dom
        let mut v: Result<Value, serde_json::Error> = serde_json::from_slice(b"null");
        let s = measure(warmup, trials, || { v = black_box(serde_json::from_slice::<Value>(black_box(&bytes))); });
        let a = alloc_of(|| { black_box(serde_json::from_slice::<Value>(&bytes).ok()); });
        let cnt = v.as_ref().map(nodes).unwrap_or(0);
        emit(&row("json-dom"), &s, a, cnt == exp_nodes, &format!("nodes={cnt}; {NOTE}"));

        // json-serialize (tree built untimed)
        let tree: Value = match serde_json::from_slice(&bytes) { Ok(t) => t, Err(e) => {
            emit_skip(&row("json-serialize"), &format!("parse failed: {e}")); continue; } };
        let mut out: Vec<u8> = Vec::new();
        let s = measure(warmup, trials, || { out = black_box(serde_json::to_vec(black_box(&tree)).unwrap_or_default()); });
        let a = alloc_of(|| { black_box(serde_json::to_vec(&tree).ok()); });
        let re = serde_json::from_slice::<Value>(&out).map(|t| nodes(&t)).unwrap_or(0);
        emit(&row("json-serialize"), &s, a, !out.is_empty() && re == exp_nodes,
             &format!("out={} bytes; re-parsed nodes={re}; {NOTE}", out.len()));

        // json-roundtrip: parse -> serialize -> parse; deep equality
        let rt = |b: &[u8]| -> Result<(Value, Value), serde_json::Error> {
            let v1: Value = serde_json::from_slice(b)?;
            let o = serde_json::to_vec(&v1)?;
            let v2: Value = serde_json::from_slice(&o)?;
            Ok((v1, v2))
        };
        let mut pair = Err(());
        let s = measure(warmup, trials, || { pair = black_box(rt(black_box(&bytes))).map_err(|_| ()); });
        let a = alloc_of(|| { black_box(rt(&bytes).ok()); });
        let eq = pair.as_ref().map(|(x, y)| x == y).unwrap_or(false);
        emit(&row("json-roundtrip"), &s, a, eq, &format!("deep equality (Value ==) {eq}; {NOTE}"));
    }

    // json-conformance
    let mut cases: Vec<(String, bool, Vec<u8>)> = Vec::new();
    for i in 1..=3 { cases.push((format!("pass{i:02}"), true, Vec::new())); }
    for i in 2..=33 { if i != 18 { cases.push((format!("fail{i:02}"), false, Vec::new())); } }
    let mut missing = false;
    for c in cases.iter_mut() {
        match std::fs::read(format!("{dir}/jsonchecker_{}.json", c.0)) { Ok(b) => c.2 = b, Err(_) => missing = true }
    }
    let crow = Row { slug: "json-conformance", dataset: "jsonchecker", bytes: cases.iter().map(|c| c.2.len() as i64).sum(),
                     input: cases.len() as i64, lib, ver: &ver, warmup, trials, throughput: false, variant: "" };
    if missing { emit_skip(&crow, "jsonchecker files absent (run datasets/fetch.sh)"); return; }
    let mut verdict: Vec<bool> = vec![false; cases.len()];
    let s = measure(warmup, trials, || {
        for (i, c) in cases.iter().enumerate() {
            verdict[i] = black_box(serde_json::from_slice::<Value>(black_box(&c.2))).is_ok();
        }
    });
    let a = alloc_of(|| { for c in &cases { black_box(serde_json::from_slice::<Value>(&c.2).ok()); } });
    let acc = cases.iter().zip(&verdict).filter(|(c, v)| c.1 && **v).count();
    let rej = cases.iter().zip(&verdict).filter(|(c, v)| !c.1 && !**v).count();
    let bad: Vec<&str> = cases.iter().zip(&verdict).filter(|(c, v)| c.1 != **v).map(|(c, _)| c.0.as_str()).collect();
    let mut note = format!("pass {acc}/3 fail {rej}/31");
    if !bad.is_empty() { note += &format!("; misclassified {}", bad.join(" ")); }
    note += &format!("; {NOTE}");
    emit(&crow, &s, a, acc == 3 && rej == 31, &note);
}
