// search/ competitors in Rust (bench/search/*.md). Inputs from the shared
// description (bench/scripts/search_ref.py: LCG31, seeds 12345/777/999);
// constants are search_ref.py's. Errors propagate as Result from main.
use nucleo_matcher::pattern::{AtomKind, CaseMatching, Normalization, Pattern};
use nucleo_matcher::{Config, Matcher, Utf32String};
use std::collections::{HashMap, HashSet};
use std::error::Error;
use std::hint::black_box;
use std::time::Instant;

fn env(k: &str, d: &str) -> String {
    std::env::var(k).ok().filter(|v| !v.is_empty()).unwrap_or_else(|| d.to_string())
}
fn env_n(k: &str, d: usize) -> usize { env(k, "").parse().unwrap_or(d) }
fn smoke() -> bool { matches!(env("CAJETA_BENCH_SMOKE", "").as_str(), "1" | "true" | "yes") }
fn peak_rss_kb() -> i64 {
    std::fs::read_to_string("/proc/self/status").ok()
        .and_then(|s| s.lines().find(|l| l.starts_with("VmHWM:"))
            .and_then(|l| l.split_whitespace().nth(1)).and_then(|v| v.parse().ok()))
        .unwrap_or(-1)
}

struct Stats { min: u128, med: u128, mean: u128, p95: u128 }
fn measure<T, F: FnMut() -> T>(warm: usize, trials: usize, mut f: F) -> (Stats, T) {
    for _ in 0..warm { black_box(f()); }
    let mut v = Vec::new();
    let mut last = None;
    for _ in 0..trials.max(1) {
        let t = Instant::now();
        let r = f();
        v.push(t.elapsed().as_nanos());
        last = Some(black_box(r));
    }
    v.sort_unstable();
    let n = v.len();
    (Stats { min: v[0], med: v[n / 2], mean: v.iter().sum::<u128>() / n as u128, p95: v[(n * 95 / 100).min(n - 1)] },
     last.unwrap())
}

const FLAGS: &str = "-O3 lto target-cpu=native";
#[allow(clippy::too_many_arguments)]
fn row(slug: &str, input: usize, lib: &str, ver: &str, w: usize, t: usize, s: &Stats, ok: bool, notes: &str, unit: &str) {
    let thr = if s.med > 0 { format!("{:.1}", input as f64 / s.med as f64 * 1e9) } else { String::new() };
    println!("1,{},{},{},search,,0,,{},rust,{},{},{},{},{},{},{},{},{},{},{},{},{},-1,-1,-1,-1,{},{},{},",
        env("PROFILE_RUN_ID", "local"), env("PROFILE_RUN_TS", ""), slug, input,
        env("PROFILE_LANG_VERSION", "").replace(',', ";"), lib, ver, FLAGS, w, t,
        s.min, s.med, s.mean, s.p95, thr, unit, peak_rss_kb(),
        if ok { "ok" } else { "invalid" }, ok, notes.replace(',', ";"));
}

struct Lcg(i64);
impl Lcg {
    fn rnd(&mut self, m: i64) -> i64 {
        self.0 = (self.0 * 1103515245 + 12345) & 0x7fffffff;
        ((self.0 >> 15) * m) >> 16
    }
}
fn rand_str(g: &mut Lcg, n: usize, alpha: i64) -> String {
    (0..n).map(|_| (b'a' + g.rnd(alpha) as u8) as char).collect()
}
fn keys(k: usize) -> Vec<String> {
    let mut g = Lcg(777);
    (0..k).map(|_| { let n = 8 + g.rnd(9) as usize; rand_str(&mut g, n, 26) }).collect()
}
fn queries(ks: &[String], q: usize) -> Vec<String> {
    let mut g = Lcg(999);
    (0..q).map(|_| {
        let key = ks[g.rnd(ks.len() as i64) as usize].as_bytes();
        let op = g.rnd(3);
        let n = key.len() as i64;
        let mut s = key.to_vec();
        match op {
            0 => { let p = g.rnd(n) as usize; let c = b'a' + g.rnd(26) as u8; s[p] = c; }
            1 => { let p = g.rnd(n - 1) as usize; s.swap(p, p + 1); }
            _ => { let p = g.rnd(n) as usize; s.remove(p); }
        }
        String::from_utf8(s).unwrap()
    }).collect()
}
fn grams(s: &str) -> Vec<String> {
    let l = s.to_lowercase();
    let b = l.as_bytes();
    if b.is_empty() { return vec![]; }
    if b.len() < 3 { return vec![l]; }
    (0..=b.len() - 3).map(|i| l[i..i + 3].to_string()).collect()
}
/// OSA truth for `q`: up to 10 (distance, key) within `t`, sorted.
fn osa_truth(q: &str, ks: &[String], t: usize) -> Vec<(usize, String)> {
    let mut v: Vec<(usize, String)> = ks.iter()
        .filter_map(|k| { let d = strsim::osa_distance(q, k); if d <= t { Some((d, k.clone())) } else { None } })
        .collect();
    v.sort();
    v.truncate(10);
    v
}

fn main() -> Result<(), Box<dyn Error>> {
    let sm = smoke();
    let w = env_n("PROFILE_WARMUP", 1);
    let t = env_n("PROFILE_TRIALS", 5);
    let sn = |s: &str| if sm { format!("; smoke size ({s})") } else { String::new() };

    // ---- distance ----
    let m = if sm { 100_000 } else { 1_000_000 };
    let mut g = Lcg(12345);
    let mut pairs = Vec::with_capacity(m);
    for _ in 0..m { let a = rand_str(&mut g, 16, 8); let b = rand_str(&mut g, 16, 8); pairs.push((a, b)); }
    let (st, s) = measure(w, t, || pairs.iter().map(|(a, b)| strsim::levenshtein(a, b) as i64).sum::<i64>());
    let want = if sm { 1276356 } else { 12760743 };
    row("distance-levenshtein", m, "strsim", "0.11.1", w, t, &st, s == want,
        &format!("strsim::levenshtein per pair; sum {s} == {want}{}", sn(&format!("{m} pairs in place of 1e6"))), "pairs/s");
    let (st, s) = measure(w, t, || pairs.iter().map(|(a, b)| strsim::jaro_winkler(a, b)).sum::<f64>());
    let want = if sm { 61342.531185943684 } else { 613325.3143160543 };
    row("distance-jaro-winkler", m, "strsim", "0.11.1", w, t, &st, (s - want).abs() <= 1e-6 * m as f64,
        &format!("strsim::jaro_winkler per pair; sum {s:.6} within 1e-6*M of {want}{}", sn(&format!("{m} pairs in place of 1e6"))), "pairs/s");

    // ---- fuzzy top-k: nucleo-matcher ----
    let k = if sm { 10_000 } else { 100_000 };
    let ks = keys(k);
    let q = if sm { 1_000 } else { 10_000 };
    let qs = queries(&ks, q);
    let hay: Vec<Utf32String> = ks.iter().map(|s| Utf32String::from(s.as_str())).collect();
    let mut matcher = Matcher::new(Config::DEFAULT);
    let pats: Vec<Pattern> = qs.iter()
        .map(|s| Pattern::new(s, CaseMatching::Ignore, Normalization::Smart, AtomKind::Fuzzy)).collect();
    // Top 10 by (score desc, key asc), bounded selection over every key.
    let topk = |matcher: &mut Matcher, p: &Pattern| -> Vec<(u32, usize)> {
        let mut top: Vec<(u32, usize)> = Vec::with_capacity(11);
        for (i, h) in hay.iter().enumerate() {
            if let Some(sc) = p.score(h.slice(..), matcher) {
                let better = |a: &(u32, usize), b: &(u32, usize)| a.0 > b.0 || (a.0 == b.0 && ks[a.1] < ks[b.1]);
                if top.len() < 10 || better(&(sc, i), &top[top.len() - 1]) {
                    let pos = top.iter().position(|e| better(&(sc, i), e)).unwrap_or(top.len());
                    top.insert(pos, (sc, i));
                    top.truncate(10);
                }
            }
        }
        top
    };
    let (st, res) = measure(w, t, || pats.iter().map(|p| topk(&mut matcher, p)).collect::<Vec<_>>());
    // Check: the bounded selection equals nucleo's own brute force (score every key, full sort) on the first 100.
    let mut same = true;
    let (mut osa_hits, mut osa_n) = (0usize, 0usize);
    for i in 0..100 {
        let mut all: Vec<(u32, usize)> = hay.iter().enumerate()
            .filter_map(|(j, h)| pats[i].score(h.slice(..), &mut matcher).map(|sc| (sc, j))).collect();
        all.sort_by(|a, b| b.0.cmp(&a.0).then(ks[a.1].cmp(&ks[b.1])));
        all.truncate(10);
        if all != res[i] { same = false; }
        let tq = (qs[i].len() / 2).max(1);
        let truth = osa_truth(&qs[i], &ks, tq);
        osa_n += truth.len();
        osa_hits += truth.iter().filter(|(_, kk)| res[i].iter().any(|&(_, j)| &ks[j] == kk)).count();
    }
    let total: usize = res.iter().map(|r| r.len()).sum();
    row("fuzzy-topk", q, "nucleo-matcher", "0.3.1", w, t, &st, same,
        &format!("nucleo-matcher Pattern(AtomKind::Fuzzy; CaseMatching::Ignore).score over all {k} keys; top 10 by (score desc; key); \
recall@10 1.0 = {same} vs nucleo's own brute force (score all; full sort) on the first 100 queries: nucleo ranks by fzf-style \
subsequence score; not OSA distance; so its OSA recall@10 is {:.4} ({osa_hits}/{osa_n}); {total} results{}",
            if osa_n > 0 { osa_hits as f64 / osa_n as f64 } else { 0.0 }, sn(&format!("{q} queries over {k} keys in place of 1e4 over 1e5"))),
        "queries/s");

    // ---- ngram (std HashMap; duplicates kept like cajeta.search.ngram.Index) ----
    let (st, idx) = measure(w, t, || {
        let mut idx: HashMap<String, Vec<u32>> = HashMap::new();
        for (i, s) in ks.iter().enumerate() {
            for gm in grams(s) { idx.entry(gm).or_default().push(i as u32); }
        }
        idx
    });
    let post: usize = idx.values().map(|v| v.len()).sum();
    let want = if sm { 100205 } else { 998746 };
    row("ngram-build", k, "std::collections::HashMap", &env("PROFILE_LANG_VERSION", "").replace(',', ";"), w, t, &st, post == want,
        &format!("HashMap<String; Vec<u32>> trigram -> key ids (std only: the page names no Rust library); postings {post} == {want}{}",
            sn(&format!("{k} keys in place of 1e5"))), "keys/s");
    let nq = if sm { 10_000 } else { 100_000 };
    let qq = queries(&ks, nq);
    let (st, c) = measure(w, t, || {
        let mut c = 0usize;
        let mut seen: HashSet<u32> = HashSet::new();
        for s in &qq {
            seen.clear();
            for gm in grams(s) {
                if let Some(v) = idx.get(&gm) { seen.extend(v.iter().copied()); }
            }
            c += seen.len();
        }
        c
    });
    let want = if sm { 545614 } else { 53088630 };
    row("ngram-query", nq, "std::collections::HashMap", &env("PROFILE_LANG_VERSION", "").replace(',', ";"), w, t, &st, c == want,
        &format!("distinct key ids per query via a HashSet over the query's trigram postings; candidates {c} == {want}{}",
            sn(&format!("{nq} queries over {k} keys in place of 1e5 over 1e5"))), "queries/s");
    Ok(())
}
