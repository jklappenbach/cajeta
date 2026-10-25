#!/usr/bin/env python3
# Python time competitors (benchmark-reference unit 8): every time/ id the Cajeta
# side runs in bench/src/main/cajeta/bench/time/, same sizes, inputs and check
# constants. Libraries: the stdlib (datetime; zoneinfo for the region zone;
# time.perf_counter_ns for clock-now) and `whenever` (pinned in
# competitors/python/requirements.txt).
#
# Where a library cannot do the workload as written the row says so in notes:
# datetime has microsecond resolution (duration-arith runs the same integers as
# microseconds; format writes 6 fraction digits, 27 bytes; parse truncates the
# 9-digit inputs to microseconds and compares against the truncated instant);
# datetime.time and whenever.Time have no arithmetic (localtime-arith goes
# through a datetime / PlainDateTime on a fixed date).
#
# alloc_bytes is tracemalloc's peak over a 1/100-size pass: no loop keeps
# state, so the peak does not grow with N, and a full traced pass would
# multiply the runner's wall time.
import os
import resource
import sys
import time
import tracemalloc
import warnings
import calendar
from datetime import date, datetime, timedelta, timezone
from zoneinfo import ZoneInfo

import whenever as W

N = 10_000_000
N_ZONE = 1_000_000
N_FMT = 1_000_000
INSTANT_REF = 22_090_666_995_000_000
LOCALDATE_REF = 504_057_395_700
DURATION_REF = 159_999_997_999_999
LOCALTIME_REF = 432_435_641_193
LOCALDATETIME_REF = 20_264_337_423_627_200
ZONE_REF = -25_266_031_177
FIRST_ISO = "2024-01-01T00:00:00.000000001Z"
LA = "America/Los_Angeles"


def env(k, d):
    v = os.environ.get(k)
    return v if v else d


def clean(s):
    return str(s).replace(",", ";").replace("\n", " ").replace('"', "'")


RUN_ID = env("PROFILE_RUN_ID", "local")
TS = env("PROFILE_RUN_TS", "")
VER = env("PROFILE_LANG_VERSION", "")
WARMUP = int(env("PROFILE_WARMUP", "3"))
TRIALS = int(env("PROFILE_TRIALS", "10"))
WHENEVER_VER = getattr(W, "__version__", "")
PY_VER = sys.version.split()[0]


def row(bench, n, lib, libver, samples, alloc, status, ok, notes):
    if samples:
        s = sorted(samples)
        k = len(s)
        mn, med, mean, p95 = s[0], s[k // 2], sum(s) // k, s[min(k - 1, k * 95 // 100)]
    else:
        mn = med = mean = p95 = -1
    mops = (n / med * 1e3) if med > 0 else 0.0
    rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    print(f"1,{RUN_ID},{TS},{bench},time,,{n},,{n},python,{VER},{lib},{libver},cpython,"
          f"{WARMUP},{TRIALS},{mn},{med},{mean},{p95},{mops:.2f},Mop/s,{rss},-1,{alloc},-1,-1,"
          f"{status},{str(ok).lower()},{clean(notes)},", flush=True)


def measure(bench, n, lib, libver, notes, f, check):
    """f(n) returns the checksum of an n-step run."""
    try:
        for _ in range(WARMUP):
            f(n)
        samples, ok = [], True
        for _ in range(TRIALS):
            t0 = time.perf_counter_ns()
            r = f(n)
            samples.append(time.perf_counter_ns() - t0)
            ok = ok and check(r)
        tracemalloc.start()
        f(max(1, n // 100))
        alloc = tracemalloc.get_traced_memory()[1]
        tracemalloc.stop()
        row(bench, n, lib, libver, samples, alloc, "ok" if ok else "invalid", ok, notes)
    except Exception as e:  # a library error is a failed row, never a crash
        row(bench, n, lib, libver, [], -1, "failed", False, f"{type(e).__name__}: {e}")


def fmt_second(i):
    return 1_704_067_200 + i * 997


def fmt_nano(i):
    return ((i * 1_000_003) % 100_000_000) * 10 + 1


# ---- clock-now ----
def clock_now(n):
    pc = time.perf_counter_ns
    first = prev = pc()
    back = 0
    for _ in range(1, n):
        t = pc()
        if t < prev:
            back += 1
        prev = t
    return 1 if back == 0 and prev > first else 0


# ---- datetime (stdlib) ----
def dt_instant(n):
    utc = timezone.utc
    epoch = datetime(1970, 1, 1, tzinfo=utc)
    half = timedelta(milliseconds=500)
    acc = cnt = 0
    for i in range(n):
        t0 = datetime.fromtimestamp(1_704_067_200 + i, utc)
        t2 = t0 + timedelta(microseconds=(i * 7919) % 1_000_000) - half
        d = t2 - epoch
        acc += d.days * 86400 + d.seconds + d.microseconds * 1000
        if t2 < t0:
            cnt += 1
    return acc + cnt


def dt_localdate(n):
    eo = date(1970, 1, 1).toordinal()
    plus37 = timedelta(days=37)
    mr = calendar.monthrange
    acc = 0
    for i in range(n):
        a = date.fromordinal(eo + i % 100_000) + plus37
        mi = a.month - 1 + i % 25
        y, m = a.year + mi // 12, mi % 12 + 1
        e = date(y, m, min(a.day, mr(y, m)[1]))
        acc += e.toordinal() - eo + e.isoweekday()
    return acc


def dt_duration(n):
    # the ns workload's integers as microseconds: 3 ms -> 3e6 us, 10 ms -> 1e7 us
    three, limit = timedelta(microseconds=3_000_000), timedelta(microseconds=10_000_000)
    acc = cnt = 0
    for i in range(n):
        d = (timedelta(microseconds=i) + three) * 2
        acc += (d.days * 86400 + d.seconds) * 1_000_000 + d.microseconds
        if d > limit:
            cnt += 1
    return acc + cnt


def dt_localtime(n):
    day = datetime(2000, 1, 1)
    noon = datetime(2000, 1, 1, 12).time()
    step = timedelta(seconds=18007)
    acc = cnt = 0
    for i in range(n):
        u = (day + timedelta(seconds=i % 86400) + step).time()
        acc += u.hour * 3600 + u.minute * 60 + u.second
        if u < noon:
            cnt += 1
    return acc + cnt


def dt_localdatetime(n):
    base = datetime(2024, 1, 1)
    acc = 0
    for i in range(n):
        x = base + timedelta(hours=i % 50000)
        acc += x.year * 1_000_000 + x.month * 10_000 + x.day * 100 + x.hour
    return acc


def zi_zoned(n):
    la = ZoneInfo(LA)
    acc = 0
    for i in range(n):
        z = datetime.fromtimestamp(1_577_836_800 + i * 307, la)
        acc += int(z.utcoffset().total_seconds()) + z.hour * 100 + z.day
    return acc


def dt_format(n):
    utc = timezone.utc
    ln, first_ok = 0, False
    for i in range(n):
        t = datetime.fromtimestamp(fmt_second(i), utc).replace(microsecond=fmt_nano(i) // 1000)
        s = t.replace(tzinfo=None).isoformat(timespec="microseconds") + "Z"
        if i == 0:
            first_ok = s == "2024-01-01T00:00:00.000000Z"
        ln += len(s)
    return ln if first_ok else -1


def dt_parse(inputs):
    def run(n):
        good = 0
        for i in range(n):
            t = datetime.fromisoformat(inputs[i])
            if int(t.timestamp()) == fmt_second(i) and t.microsecond == fmt_nano(i) // 1000:
                good += 1
        return good
    return run


# ---- whenever ----
def w_instant(n):
    I = W.Instant.from_timestamp
    acc = cnt = 0
    for i in range(n):
        t0 = I(1_704_067_200 + i)
        t2 = t0.add(nanoseconds=((i * 7919) % 1_000_000) * 1000).subtract(milliseconds=500)
        s, ns = divmod(t2.timestamp(unit="nanosecond"), 1_000_000_000)
        acc += s + ns
        if t2 < t0:
            cnt += 1
    return acc + cnt


def w_localdate(n):
    epoch = W.Date(1970, 1, 1)
    acc = 0
    for i in range(n):
        e = epoch.add(days=i % 100_000).add(days=37).add(months=i % 25)
        acc += int(e.since(epoch, total="days")) + e.day_of_week().value
    return acc


def w_duration(n):
    TD = W.TimeDelta
    three, limit = TD(milliseconds=3), TD(milliseconds=10)
    acc = cnt = 0
    for i in range(n):
        d = (TD(nanoseconds=i) + three) * 2
        acc += d.total("nanoseconds")
        if d > limit:
            cnt += 1
    return acc + cnt


def w_localtime(n):
    day = W.PlainDateTime(2000, 1, 1)
    noon = W.Time.NOON
    acc = cnt = 0
    for i in range(n):
        u = day.add(seconds=i % 86400 + 18007, naive_arithmetic_ok=True).time()
        acc += u.hour * 3600 + u.minute * 60 + u.second
        if u < noon:
            cnt += 1
    return acc + cnt


def w_localdatetime(n):
    base = W.PlainDateTime(2024, 1, 1)
    acc = 0
    for i in range(n):
        x = base.add(hours=i % 50000, naive_arithmetic_ok=True)
        acc += x.year * 1_000_000 + x.month * 10_000 + x.day * 100 + x.hour
    return acc


def w_format(n):
    I = W.Instant.from_timestamp
    ln, first_ok = 0, False
    for i in range(n):
        s = I(fmt_second(i) * 1_000_000_000 + fmt_nano(i), unit="nanosecond").format_iso()
        if i == 0:
            first_ok = s == FIRST_ISO
        ln += len(s)
    return ln if first_ok else -1


def w_parse(inputs):
    def run(n):
        P = W.Instant.parse_iso
        good = 0
        for i in range(n):
            if P(inputs[i]).timestamp(unit="nanosecond") == fmt_second(i) * 1_000_000_000 + fmt_nano(i):
                good += 1
        return good
    return run


def main():
    warnings.simplefilter("ignore", W.WheneverDeprecationWarning)
    eq = lambda want: (lambda r: r == want)
    measure("clock-now", N, "time", PY_VER, "time.perf_counter_ns", clock_now, eq(1))
    measure("time-instant-arith", N, "datetime", PY_VER, "aware datetime + timedelta (microsecond resolution; the nanosecond offsets are multiples of 1000)",
            dt_instant, eq(INSTANT_REF))
    measure("time-instant-arith", N, "whenever", WHENEVER_VER, "Instant.add/subtract", w_instant, eq(INSTANT_REF))
    measure("time-localdate-arith", N, "datetime", PY_VER, "date + timedelta; date has no month arithmetic: explicit clamp",
            dt_localdate, eq(LOCALDATE_REF))
    measure("time-localdate-arith", N, "whenever", WHENEVER_VER, "Date.add(days/months) and day_of_week", w_localdate, eq(LOCALDATE_REF))
    measure("duration-arith", N, "datetime", PY_VER, "timedelta has microsecond resolution: same integers as microseconds",
            dt_duration, eq(DURATION_REF))
    measure("duration-arith", N, "whenever", WHENEVER_VER, "TimeDelta", w_duration, eq(DURATION_REF))
    measure("localtime-arith", N, "datetime", PY_VER, "datetime.time has no arithmetic: datetime on a fixed date then .time()",
            dt_localtime, eq(LOCALTIME_REF))
    measure("localtime-arith", N, "whenever", WHENEVER_VER, "whenever.Time has no arithmetic: PlainDateTime.add then .time()",
            w_localtime, eq(LOCALTIME_REF))
    measure("localdatetime-arith", N, "datetime", PY_VER, "datetime + timedelta(hours)", dt_localdatetime, eq(LOCALDATETIME_REF))
    measure("localdatetime-arith", N, "whenever", WHENEVER_VER, "PlainDateTime.add(hours)", w_localdatetime, eq(LOCALDATETIME_REF))
    measure("zoneddatetime-region", N_ZONE, "zoneinfo", PY_VER, "datetime.fromtimestamp(ts ZoneInfo)", zi_zoned, eq(ZONE_REF))
    measure("datetime-format-iso", N_FMT, "datetime", PY_VER,
            "datetime has microsecond resolution: isoformat(timespec=microseconds)+Z is 27 bytes", dt_format, eq(N_FMT * 27))
    measure("datetime-format-iso", N_FMT, "whenever", WHENEVER_VER, "Instant.format_iso", w_format, eq(N_FMT * 30))
    I = W.Instant.from_timestamp
    inputs = [I(fmt_second(i) * 1_000_000_000 + fmt_nano(i), unit="nanosecond").format_iso() for i in range(N_FMT)]
    measure("datetime-parse-iso", N_FMT, "datetime", PY_VER,
            "datetime.fromisoformat truncates the 9-digit fraction to microseconds; compared at microseconds", dt_parse(inputs), eq(N_FMT))
    measure("datetime-parse-iso", N_FMT, "whenever", WHENEVER_VER, "Instant.parse_iso", w_parse(inputs), eq(N_FMT))


if __name__ == "__main__":
    main()
