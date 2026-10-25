// Rust "hello-metric" runner — emits one schema-conformant CSV row (columns.txt
// order). Times a trivial loop and reports it as the timing distribution. The
// real per-area Rust runners (Cargo projects with serde_json/hashbrown/etc.)
// extend this contract in Units 5+.
use std::time::Instant;

fn main() {
    let run_id = std::env::var("PROFILE_RUN_ID").unwrap_or_else(|_| "local".to_string());
    let ts = std::env::var("PROFILE_RUN_TS").unwrap_or_default();
    let ver = std::env::var("PROFILE_LANG_VERSION").unwrap_or_default();

    let t0 = Instant::now();
    let mut acc: u64 = 0;
    for i in 0..1_000_000u64 {
        acc = acc.wrapping_add(i);
    }
    let ns = t0.elapsed().as_nanos();
    std::hint::black_box(acc);

    println!(
        "1,{run_id},{ts},hello,scaffold,,-1,,1000000,rust,{ver},stdlib,,-O,0,1,{ns},{ns},{ns},{ns},,,-1,-1,-1,-1,-1,ok,true,,"
    );
}
