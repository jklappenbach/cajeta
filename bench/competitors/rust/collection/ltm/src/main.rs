// Rust collection/ltm competitors: ltm-insert and ltm-lookup against RocksDB
// (rocksdb crate) and LMDB (heed), the same workload as LtmInsertBench /
// LtmLookupBench: N random 16-byte keys (16 hex digits of key(i)) with 64-byte
// values (the key four times), then LOOKUPS point gets of key(draw % N); the
// sizes, the page budget and the directory come from the CAJETA_BENCH_LTM_*
// variables the Cajeta side reads (bench/src/main/cajeta/bench/collection/
// LtmEnv.cajeta). Errors propagate as Result to main, which turns them into a
// skipped row carrying the message (ltm.sh then marks the driver failed).
//   RocksDB: uncompressed, block cache = the budget, write buffer = budget/2,
//     default WriteOptions (WAL on, no per-write fsync), then flush + close.
//   LMDB: no page budget exists (the file is mmapped and the OS caches it);
//     all N puts in one write transaction, commit, then an fsync of the env.
#[path = "../../common/collgen.rs"]
mod collgen;
use collgen::*;
use std::error::Error;
use std::hint::black_box;
use std::path::{Path, PathBuf};
use std::process::Command;

type R<T> = Result<T, Box<dyn Error>>;

fn num(k: &str, d: u64) -> u64 { env(k, &d.to_string()).parse().unwrap_or(d) }

fn hex16(v: i64) -> [u8; 16] {
    let mut b = [0u8; 16];
    for i in 0..16 { let d = ((v as u64) >> (60 - 4 * i)) & 15; b[i] = if d < 10 { b'0' + d as u8 } else { b'a' + d as u8 - 10 }; }
    b
}

fn df_line(args: &[&str], dir: &Path) -> Option<Vec<String>> {
    let o = Command::new("df").args(args).arg(dir).output().ok()?;
    let s = String::from_utf8_lossy(&o.stdout).to_string();
    s.lines().nth(1).map(|l| l.split_whitespace().map(|x| x.to_string()).collect())
}

struct Ltm { dir: PathBuf, n: usize, lookups: usize, budget_mb: u64, note: String, why: Option<String> }

fn ltm_env() -> Ltm {
    let dir = PathBuf::from(env("CAJETA_BENCH_LTM_DIR", "ltm"));
    let n = num("CAJETA_BENCH_LTM_N", 1_000_000) as usize;
    let lookups = num("CAJETA_BENCH_LTM_LOOKUPS", 100_000) as usize;
    let budget_mb = num("CAJETA_BENCH_LTM_BUDGET_MB", 16);
    let min_free = num("CAJETA_BENCH_LTM_MIN_FREE_MB", 4096);
    let mut why = None;
    let mut storage = "unknown".to_string();
    if std::fs::create_dir_all(&dir).is_err() {
        why = Some(format!("disk budget unavailable: cannot create {}", dir.display()));
    } else if let Some(f) = df_line(&["-Pk"], &dir) {
        let free = f.get(3).and_then(|x| x.parse::<u64>().ok()).unwrap_or(0) / 1024;
        if let Some(s) = df_line(&["--output=source,fstype"], &dir) { storage = s.join(" "); }
        if free < min_free {
            why = Some(format!("disk budget unavailable: {free} MB free at {} ({storage}) is under CAJETA_BENCH_LTM_MIN_FREE_MB={min_free}", dir.display()));
        }
    } else {
        why = Some(format!("disk budget unavailable: df -Pk {} gave no answer", dir.display()));
    }
    let note = format!("storage={storage} dir={} n={n} lookups={lookups} budget={budget_mb}MB (page: 1e7 / 1e6 / 256MB)", dir.display());
    Ltm { dir, n, lookups, budget_mb, note, why }
}

fn rocks_opts(budget_mb: u64) -> rocksdb::Options {
    let mut o = rocksdb::Options::default();
    o.create_if_missing(true);
    o.set_compression_type(rocksdb::DBCompressionType::None);
    let budget = (budget_mb * 1024 * 1024) as usize;
    o.set_write_buffer_size(budget / 2);
    let cache = rocksdb::Cache::new_lru_cache(budget);
    let mut bo = rocksdb::BlockBasedOptions::default();
    bo.set_block_cache(&cache);
    o.set_block_based_table_factory(&bo);
    o
}

fn rocks_fill(path: &Path, l: &Ltm, keys: &[[u8; 16]], vals: &[Vec<u8>]) -> R<()> {
    let _ = std::fs::remove_dir_all(path);
    let db = rocksdb::DB::open(&rocks_opts(l.budget_mb), path)?;
    for (k, v) in keys.iter().zip(vals) { db.put(k, v)?; }
    db.flush()?;
    Ok(())
}

fn rocks_count(path: &Path) -> R<usize> {
    let db = rocksdb::DB::open_for_read_only(&rocksdb::Options::default(), path, false)?;
    Ok(db.iterator(rocksdb::IteratorMode::Start).count())
}

fn lmdb_env(path: &Path, l: &Ltm) -> R<heed::Env> {
    std::fs::create_dir_all(path)?;
    let map = (((l.n as u64) * 400).max(64 << 20) + 4095) / 4096 * 4096;
    let map = map as usize;
    Ok(unsafe { heed::EnvOpenOptions::new().map_size(map).max_dbs(1).open(path)? })
}

fn lmdb_fill(path: &Path, l: &Ltm, keys: &[[u8; 16]], vals: &[Vec<u8>]) -> R<usize> {
    let _ = std::fs::remove_dir_all(path);
    let e = lmdb_env(path, l)?;
    let mut w = e.write_txn()?;
    let db: heed::Database<heed::types::Bytes, heed::types::Bytes> = e.create_database(&mut w, None)?;
    for (k, v) in keys.iter().zip(vals) { db.put(&mut w, &k[..], &v[..])?; }
    w.commit()?;
    e.force_sync()?;
    let r = e.read_txn()?;
    Ok(db.len(&r)? as usize)
}

struct Input { keys: Vec<[u8; 16]>, vals: Vec<Vec<u8>>, q: Vec<[u8; 16]> }

fn run_rocks(l: &Ltm, inp: &Input) -> R<()> {
    let (keys, vals, q, note) = (&inp.keys, &inp.vals, &inp.q, l.note.clone());
    // ---- RocksDB ----
    let rp = l.dir.join("rust-rocksdb");
    if selected("ltm-insert") {
        let m = bench(&mut || rocks_fill(&rp, l, black_box(&keys), &vals).map_err(|e| e.to_string()),
            |r| r.is_ok() && rocks_count(&rp).ok() == Some(l.n));
        row("ltm-insert", "rocksdb", "0.25.0", l.n as u64, m, &format!("{note}; uncompressed; block cache = budget; flush at the end"));
    }
    if selected("ltm-lookup") {
        rocks_fill(&rp, l, &keys, &vals)?;
        let db = rocksdb::DB::open(&rocks_opts(l.budget_mb), &rp)?;
        let m = bench(&mut || {
            let mut s = 0i64;
            for k in black_box(&q).iter() { if let Ok(Some(v)) = db.get_pinned(k) { s += v.len() as i64; } }
            s
        }, |&s| s == 64 * l.lookups as i64);
        row("ltm-lookup", "rocksdb", "0.25.0", l.lookups as u64, m, &format!("{note}; get_pinned"));
    }
    let _ = std::fs::remove_dir_all(&rp);
    Ok(())
}

fn run_lmdb(l: &Ltm, inp: &Input) -> R<()> {
    let (keys, vals, q, note) = (&inp.keys, &inp.vals, &inp.q, l.note.clone());
    // ---- LMDB (heed) ----
    let lp = l.dir.join("rust-lmdb");
    if selected("ltm-insert") {
        let m = bench(&mut || lmdb_fill(&lp, l, black_box(&keys), &vals).map_err(|e| e.to_string()),
            |r| r.as_ref().ok() == Some(&l.n));
        row("ltm-insert", "heed", "0.22.1", l.n as u64, m, &format!("{note}; LMDB has no page budget (mmap); one write txn then fsync"));
    }
    if selected("ltm-lookup") {
        lmdb_fill(&lp, l, &keys, &vals)?;
        let e = lmdb_env(&lp, l)?;
        let r = e.read_txn()?;
        let db: heed::Database<heed::types::Bytes, heed::types::Bytes> = e.open_database(&r, None)?.ok_or("no db")?;
        let m = bench(&mut || {
            let mut s = 0i64;
            for k in black_box(&q).iter() { if let Ok(Some(v)) = db.get(&r, &k[..]) { s += v.len() as i64; } }
            s
        }, |&s| s == 64 * l.lookups as i64);
        row("ltm-lookup", "heed", "0.22.1", l.lookups as u64, m, &format!("{note}; LMDB has no page budget (mmap)"));
    }
    let _ = std::fs::remove_dir_all(&lp);
    Ok(())
}

fn main() {
    let l = ltm_env();
    let ids = [("ltm-insert", l.n as u64), ("ltm-lookup", l.lookups as u64)];
    if let Some(why) = &l.why {
        for lib in ["rocksdb", "heed"] { for (id, n) in ids { if selected(id) { skip(id, lib, n, why); } } }
        return;
    }
    let keys: Vec<[u8; 16]> = (0..l.n as i64).map(|i| hex16(key(i))).collect();
    let vals: Vec<Vec<u8>> = keys.iter().map(|k| k.repeat(4)).collect();
    let q: Vec<[u8; 16]> = Gen::new().indices(l.lookups, l.n as i32).iter().map(|&i| keys[i as usize]).collect();
    let inp = Input { keys, vals, q };
    let mut rc = 0;
    // A store error (Result propagated up from either store) is a skipped row
    // carrying the message for each of that store's ids, and exit status 1.
    for (lib, r) in [("rocksdb", run_rocks(&l, &inp)), ("heed", run_lmdb(&l, &inp))] {
        if let Err(e) = r {
            for (id, n) in ids { if selected(id) { skip(id, lib, n, &format!("error: {e}")); } }
            rc = 1;
        }
    }
    std::process::exit(rc);
}
