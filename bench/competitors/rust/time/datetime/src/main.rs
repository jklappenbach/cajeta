// Rust time competitors (benchmark-reference unit 8): every time/ id the Cajeta
// side runs in bench/src/main/cajeta/bench/time/, same sizes, same inputs, same
// checks (the constants are the ones the Cajeta benchmarks and every other
// language check). Libraries: jiff and chrono; std::time::Instant for
// clock-now. Fallible library calls propagate Result (`?`); a failure is a
// `failed` row with the error, never a panic. Every checksum is passed
// through black_box and checked, so nothing can be elided.
use std::alloc::{GlobalAlloc, Layout, System};
use std::error::Error;
use std::hint::black_box;
use std::sync::atomic::{AtomicU64, Ordering as Ord_};
use std::time::Instant;

static ALLOCED: AtomicU64 = AtomicU64::new(0);
struct Counting;
unsafe impl GlobalAlloc for Counting {
    unsafe fn alloc(&self, l: Layout) -> *mut u8 { ALLOCED.fetch_add(l.size() as u64, Ord_::Relaxed); System.alloc(l) }
    unsafe fn dealloc(&self, p: *mut u8, l: Layout) { System.dealloc(p, l) }
}
#[global_allocator]
static GA: Counting = Counting;

type R<T> = Result<T, Box<dyn Error>>;

const N: i64 = 10_000_000;
const N_ZONE: i64 = 1_000_000;
const N_FMT: i64 = 1_000_000;
const INSTANT_REF: i64 = 22_090_666_995_000_000;
const LOCALDATE_REF: i64 = 504_057_395_700;
const DURATION_REF: i64 = 159_999_997_999_999;
const LOCALTIME_REF: i64 = 432_435_641_193;
const LOCALDATETIME_REF: i64 = 20_264_337_423_627_200;
const ZONE_REF: i64 = -25_266_031_177;
const FIRST_ISO: &str = "2024-01-01T00:00:00.000000001Z";

fn env(k: &str, d: &str) -> String { std::env::var(k).unwrap_or_else(|_| d.to_string()) }

fn peak_rss_kb() -> i64 {
    std::fs::read_to_string("/proc/self/status").ok().and_then(|s| {
        s.lines().find(|l| l.starts_with("VmHWM:"))
            .and_then(|l| l.split_whitespace().nth(1)).and_then(|v| v.parse().ok())
    }).unwrap_or(-1)
}

struct Cfg { run_id: String, ts: String, warmup: usize, trials: usize }

fn clean(s: &str) -> String { s.replace(',', ";").replace('"', "'").replace('\n', " ") }

fn row(c: &Cfg, bench: &str, n: i64, lib: &str, libver: &str, s: &mut Vec<u128>, alloc: u64,
       status: &str, ok: bool, notes: &str) {
    let (mn, med, mean, p95) = if s.is_empty() { (-1i128, -1i128, -1i128, -1i128) } else {
        s.sort_unstable();
        let k = s.len();
        let sum: u128 = s.iter().sum();
        (s[0] as i128, s[k / 2] as i128, (sum / k as u128) as i128, s[(k * 95 / 100).min(k - 1)] as i128)
    };
    let mops = if med > 0 { n as f64 / med as f64 * 1e3 } else { 0.0 };
    println!("1,{},{},{},time,,{},,{},rust,{},{},{},-O3 lto target-cpu=native,{},{},{},{},{},{},{:.2},Mop/s,{},-1,{},-1,-1,{},{},{},",
        c.run_id, c.ts, bench, n, n, env("PROFILE_LANG_VERSION", ""), lib, libver,
        c.warmup, c.trials, mn, med, mean, p95, mops, peak_rss_kb(), alloc, status, ok, clean(notes));
}

// Warm up, time `trials` calls, then one untimed call for the allocation count.
// The workload returns its checksum; `check` decides the row's check_ok.
fn measure<F: FnMut() -> R<i64>, C: Fn(i64) -> bool>(c: &Cfg, bench: &str, n: i64, lib: &str, libver: &str,
                                                       notes: &str, mut f: F, check: C) {
    let mut go = || -> R<(Vec<u128>, bool, u64)> {
        for _ in 0..c.warmup { black_box(f()?); }
        let mut s = Vec::with_capacity(c.trials);
        let mut ok = true;
        for _ in 0..c.trials {
            let t = Instant::now();
            let r = black_box(f()?);
            s.push(t.elapsed().as_nanos());
            ok &= check(r);
        }
        ALLOCED.store(0, Ord_::Relaxed);
        black_box(f()?);
        Ok((s, ok, ALLOCED.load(Ord_::Relaxed)))
    };
    match go() {
        Ok((mut s, ok, a)) => row(c, bench, n, lib, libver, &mut s, a, if ok { "ok" } else { "invalid" }, ok, notes),
        Err(e) => row(c, bench, n, lib, libver, &mut Vec::new(), 0, "failed", false, &format!("{}", e)),
    }
}

// ---- inputs shared by format and parse ----
fn fmt_second(i: i64) -> i64 { 1_704_067_200 + i * 997 }
fn fmt_nano(i: i64) -> i32 { (((i * 1_000_003) % 100_000_000) * 10 + 1) as i32 }

// ---- clock-now: N readings of the monotonic clock; check monotonic ----
fn clock_now() -> R<i64> {
    let first = Instant::now();
    let mut prev = first;
    let mut back = 0i64;
    for _ in 1..N {
        let t = Instant::now();
        if t < prev { back += 1; }
        prev = black_box(t);
    }
    Ok(if back == 0 && prev > first { 1 } else { 0 })
}

// ---- jiff ----
fn jiff_instant() -> R<i64> {
    use jiff::{SignedDuration, Timestamp};
    let half = SignedDuration::from_millis(500);
    let (mut acc, mut cnt) = (0i64, 0i64);
    for i in 0..N {
        let i = black_box(i);
        let t0 = Timestamp::from_second(1_704_067_200 + i)?;
        let t2 = t0.checked_add(SignedDuration::from_nanos(((i * 7919) % 1_000_000) * 1000))?.checked_sub(half)?;
        acc += t2.as_second() + t2.subsec_nanosecond() as i64;
        if t2 < t0 { cnt += 1; }
    }
    Ok(acc + cnt)
}

fn jiff_localdate() -> R<i64> {
    use jiff::{civil::date, Span};
    let epoch = date(1970, 1, 1);
    let mut acc = 0i64;
    for i in 0..N {
        let i = black_box(i);
        let d = epoch.checked_add(Span::new().days(i % 100_000))?;
        let e = d.checked_add(Span::new().days(37))?.checked_add(Span::new().months(i % 25))?;
        acc += e.duration_since(epoch).as_hours() / 24 + e.weekday().to_monday_one_offset() as i64;
    }
    Ok(acc)
}

fn jiff_duration() -> R<i64> {
    use jiff::SignedDuration;
    let three = SignedDuration::from_millis(3);
    let limit = SignedDuration::from_millis(10);
    let (mut acc, mut cnt) = (0i64, 0i64);
    for i in 0..N {
        let d = (SignedDuration::from_nanos(black_box(i)) + three) * 2;
        acc += d.as_nanos() as i64;
        if d > limit { cnt += 1; }
    }
    Ok(acc + cnt)
}

fn jiff_localtime() -> R<i64> {
    use jiff::{civil::Time, SignedDuration};
    let noon = Time::new(12, 0, 0, 0)?;
    let step = SignedDuration::from_secs(18007);
    let (mut acc, mut cnt) = (0i64, 0i64);
    for i in 0..N {
        let t = Time::midnight().wrapping_add(SignedDuration::from_secs(black_box(i) % 86400));
        let u = t.wrapping_add(step);
        acc += u.hour() as i64 * 3600 + u.minute() as i64 * 60 + u.second() as i64;
        if u < noon { cnt += 1; }
    }
    Ok(acc + cnt)
}

fn jiff_localdatetime() -> R<i64> {
    use jiff::{civil::date, SignedDuration};
    let base = date(2024, 1, 1).at(0, 0, 0, 0);
    let mut acc = 0i64;
    for i in 0..N {
        let x = base.checked_add(SignedDuration::from_hours(black_box(i) % 50000))?;
        acc += x.year() as i64 * 1_000_000 + x.month() as i64 * 10_000 + x.day() as i64 * 100 + x.hour() as i64;
    }
    Ok(acc)
}

fn jiff_zoned() -> R<i64> {
    use jiff::{tz::TimeZone, Timestamp};
    let tz = TimeZone::get("America/Los_Angeles")?;
    let mut acc = 0i64;
    for i in 0..N_ZONE {
        let ts = Timestamp::from_second(1_577_836_800 + black_box(i) * 307)?;
        let z = ts.to_zoned(tz.clone());
        acc += z.offset().seconds() as i64 + z.hour() as i64 * 100 + z.day() as i64;
    }
    Ok(acc)
}

fn jiff_format() -> R<i64> {
    use jiff::Timestamp;
    let mut len = 0i64;
    let mut first_ok = false;
    for i in 0..N_FMT {
        let ts = Timestamp::new(fmt_second(black_box(i)), fmt_nano(i))?;
        let s = ts.to_string(); // RFC 3339; the fraction never ends in 0, so 9 digits
        if i == 0 { first_ok = s == FIRST_ISO; }
        len += black_box(s).len() as i64;
    }
    Ok(if first_ok { len } else { -1 })
}

fn iso_inputs() -> R<Vec<String>> {
    let mut v = Vec::with_capacity(N_FMT as usize);
    for i in 0..N_FMT { v.push(jiff::Timestamp::new(fmt_second(i), fmt_nano(i))?.to_string()); }
    Ok(v)
}

fn jiff_parse(inp: &[String]) -> R<i64> {
    use jiff::Timestamp;
    let mut good = 0i64;
    for (i, s) in inp.iter().enumerate() {
        let ts: Timestamp = black_box(s).parse()?;
        let i = i as i64;
        if ts.as_second() == fmt_second(i) && ts.subsec_nanosecond() == fmt_nano(i) { good += 1; }
    }
    Ok(good)
}

// ---- chrono ----
fn chrono_instant() -> R<i64> {
    use chrono::{DateTime, TimeDelta};
    let half = TimeDelta::milliseconds(500);
    let (mut acc, mut cnt) = (0i64, 0i64);
    for i in 0..N {
        let i = black_box(i);
        let t0 = DateTime::from_timestamp(1_704_067_200 + i, 0).ok_or("range")?;
        let t2 = t0.checked_add_signed(TimeDelta::nanoseconds(((i * 7919) % 1_000_000) * 1000)).ok_or("overflow")?
            .checked_sub_signed(half).ok_or("overflow")?;
        acc += t2.timestamp() + t2.timestamp_subsec_nanos() as i64;
        if t2 < t0 { cnt += 1; }
    }
    Ok(acc + cnt)
}

fn chrono_localdate() -> R<i64> {
    use chrono::{Datelike, Days, Months, NaiveDate};
    let mut acc = 0i64;
    for i in 0..N {
        let i = black_box(i);
        let d = NaiveDate::from_epoch_days((i % 100_000) as i32).ok_or("range")?;
        let e = d.checked_add_days(Days::new(37)).ok_or("range")?
            .checked_add_months(Months::new((i % 25) as u32)).ok_or("range")?;
        acc += (e.num_days_from_ce() - 719_163) as i64 + e.weekday().number_from_monday() as i64;
    }
    Ok(acc)
}

fn chrono_duration() -> R<i64> {
    use chrono::TimeDelta;
    let three = TimeDelta::milliseconds(3);
    let limit = TimeDelta::milliseconds(10);
    let (mut acc, mut cnt) = (0i64, 0i64);
    for i in 0..N {
        let d = (TimeDelta::nanoseconds(black_box(i)) + three) * 2;
        acc += d.num_nanoseconds().ok_or("TimeDelta overflow")?;
        if d > limit { cnt += 1; }
    }
    Ok(acc + cnt)
}

fn chrono_localtime() -> R<i64> {
    use chrono::{NaiveTime, TimeDelta, Timelike};
    let noon = NaiveTime::from_hms_opt(12, 0, 0).ok_or("noon")?;
    let step = TimeDelta::seconds(18007);
    let (mut acc, mut cnt) = (0i64, 0i64);
    for i in 0..N {
        let t = NaiveTime::from_num_seconds_from_midnight_opt((black_box(i) % 86400) as u32, 0).ok_or("time")?;
        let u = t + step; // wraps past midnight
        acc += u.num_seconds_from_midnight() as i64;
        if u < noon { cnt += 1; }
    }
    Ok(acc + cnt)
}

fn chrono_localdatetime() -> R<i64> {
    use chrono::{Datelike, NaiveDate, TimeDelta, Timelike};
    let base = NaiveDate::from_ymd_opt(2024, 1, 1).ok_or("date")?.and_hms_opt(0, 0, 0).ok_or("time")?;
    let mut acc = 0i64;
    for i in 0..N {
        let x = base.checked_add_signed(TimeDelta::hours(black_box(i) % 50000)).ok_or("overflow")?;
        acc += x.year() as i64 * 1_000_000 + x.month() as i64 * 10_000 + x.day() as i64 * 100 + x.hour() as i64;
    }
    Ok(acc)
}

fn chrono_format() -> R<i64> {
    use chrono::{DateTime, SecondsFormat};
    let mut len = 0i64;
    let mut first_ok = false;
    for i in 0..N_FMT {
        let t = DateTime::from_timestamp(fmt_second(black_box(i)), fmt_nano(i) as u32).ok_or("range")?;
        let s = t.to_rfc3339_opts(SecondsFormat::Nanos, true);
        if i == 0 { first_ok = s == FIRST_ISO; }
        len += black_box(s).len() as i64;
    }
    Ok(if first_ok { len } else { -1 })
}

fn chrono_parse(inp: &[String]) -> R<i64> {
    use chrono::DateTime;
    let mut good = 0i64;
    for (i, s) in inp.iter().enumerate() {
        let t = DateTime::parse_from_rfc3339(black_box(s))?;
        let i = i as i64;
        if t.timestamp() == fmt_second(i) && t.timestamp_subsec_nanos() as i32 == fmt_nano(i) { good += 1; }
    }
    Ok(good)
}

fn main() {
    let c = Cfg {
        run_id: env("PROFILE_RUN_ID", "local"),
        ts: env("PROFILE_RUN_TS", ""),
        warmup: env("PROFILE_WARMUP", "3").parse().unwrap_or(3),
        trials: env("PROFILE_TRIALS", "10").parse().unwrap_or(10),
    };
    let jv = "0.2.38";
    let cv = "0.4.45";
    let fmt_len = N_FMT * 30;

    measure(&c, "clock-now", N, "std", "", "std::time::Instant::now (CLOCK_MONOTONIC)", clock_now, |r| r == 1);

    measure(&c, "time-instant-arith", N, "jiff", jv, "Timestamp checked_add/checked_sub", jiff_instant, |r| r == INSTANT_REF);
    measure(&c, "time-instant-arith", N, "chrono", cv, "DateTime<Utc> checked_add_signed", chrono_instant, |r| r == INSTANT_REF);
    measure(&c, "time-localdate-arith", N, "jiff", jv, "civil::Date + Span days/months", jiff_localdate, |r| r == LOCALDATE_REF);
    measure(&c, "time-localdate-arith", N, "chrono", cv, "NaiveDate checked_add_days/months", chrono_localdate, |r| r == LOCALDATE_REF);
    measure(&c, "duration-arith", N, "jiff", jv, "SignedDuration", jiff_duration, |r| r == DURATION_REF);
    measure(&c, "duration-arith", N, "chrono", cv, "TimeDelta", chrono_duration, |r| r == DURATION_REF);
    measure(&c, "localtime-arith", N, "jiff", jv, "civil::Time wrapping_add", jiff_localtime, |r| r == LOCALTIME_REF);
    measure(&c, "localtime-arith", N, "chrono", cv, "NaiveTime + TimeDelta", chrono_localtime, |r| r == LOCALTIME_REF);
    measure(&c, "localdatetime-arith", N, "jiff", jv, "civil::DateTime checked_add", jiff_localdatetime, |r| r == LOCALDATETIME_REF);
    measure(&c, "localdatetime-arith", N, "chrono", cv, "NaiveDateTime checked_add_signed", chrono_localdatetime, |r| r == LOCALDATETIME_REF);
    measure(&c, "zoneddatetime-region", N_ZONE, "jiff", jv, "Timestamp::to_zoned (system tzdb)", jiff_zoned, |r| r == ZONE_REF);
    measure(&c, "datetime-format-iso", N_FMT, "jiff", jv, "Timestamp Display (RFC 3339)", jiff_format, |r| r == fmt_len);
    measure(&c, "datetime-format-iso", N_FMT, "chrono", cv, "to_rfc3339_opts(Nanos)", chrono_format, |r| r == fmt_len);
    match iso_inputs() {
        Ok(inp) => {
            measure(&c, "datetime-parse-iso", N_FMT, "jiff", jv, "str::parse::<Timestamp>", || jiff_parse(&inp), |r| r == N_FMT);
            measure(&c, "datetime-parse-iso", N_FMT, "chrono", cv, "DateTime::parse_from_rfc3339", || chrono_parse(&inp), |r| r == N_FMT);
        }
        Err(e) => {
            for lib in ["jiff", "chrono"] {
                row(&c, "datetime-parse-iso", N_FMT, lib, "", &mut Vec::new(), 0, "failed", false, &format!("inputs: {}", e));
            }
        }
    }
}
