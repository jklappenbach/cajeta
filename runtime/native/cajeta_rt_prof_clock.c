// === Cajeta runtime fragment — TEXTUALLY #included into cajeta_runtime.c ===
//
// cajeta-profiler Unit 9 — clock correlation (spec §6, §11.3, §11.4, §11.6).
//
// Every device lane has to land on the host timeline, and the natural default
// on each backend is wrong in a way that produces confident, plausible, badly
// incorrect numbers: the reference device in §6.6 drifts −15 ppm (about 54 ms
// per hour) with raw ticks sitting 104.6 s from the host clock, and §6.5
// measured two backends' preferred domains 5.68 s apart with nothing reporting
// an error. So this module recovers BOTH terms — offset and rate — and refuses
// to answer at all when it has not earned the right to (§11.6). A profiler that
// guesses here is worse than one that declines, because a guess is indistin-
// guishable from a measurement once it is in the trace.
//
// The fit is ordinary least squares over accepted samples, kept incrementally:
// no sample buffer, no allocation, and a recalibration is just more samples.
// Sums are held about a per-domain origin (the first accepted sample) rather
// than about zero — device tick counts reach 1e11 and their squares overflow
// the f64 mantissa's exact range, which silently flattens the slope to the
// nominal period and produces exactly the offset-only fit §6.6 says is
// insufficient.

// Host ns per device tick, and the sandwich width above which a sample is
// discarded. 50 us is generous for a warm device and still rejects the multi-
// millisecond reads a device in a low power state returns (§6.7).
#define CAJ_CLOCK_DEFAULT_DISPERSION_NS 50000

typedef struct {
    double  period;          // nominal host ns per tick, as the driver claims
    int32_t periodSet;

    // Origin for the accumulators; see the overflow note above.
    double  originDev;
    double  originHost;

    double  n, sx, sy, sxx, sxy;   // OLS accumulators, about the origin

    double  slope;           // host ns per device tick, MEASURED
    double  intercept;       // host ns at device tick 0
    int32_t valid;

    int32_t accepted;
    int32_t rejected;
    int64_t bestDispersion;  // ns; the tightest sandwich accepted so far

    int64_t lastSpanStart;   // §11.3 monotonicity watchdog
    int32_t haveLastSpan;

    // §7.8. Copied, not aliased — a backend handing over a vendor library's
    // string has no obligation to keep it alive until drain, and the trace is
    // written long after initialization.
    char driver[CAJETA_DRIVER_ID_MAX];
    char layers[CAJETA_DRIVER_ID_MAX];
} CajClockDomain;

static CajClockDomain caj_clock[CAJETA_CLOCK_MAX_DOMAINS];
// Defined with the snapshot ring at the foot of this file; used by calibrate.
static void caj_clock_record_snapshot(int32_t domain, int64_t hostNs,
                                      int64_t devTicks);
static int64_t caj_clock_dispersion_cap = CAJ_CLOCK_DEFAULT_DISPERSION_NS;
static pthread_mutex_t caj_clock_mutex = PTHREAD_MUTEX_INITIALIZER;

static CajClockDomain* caj_clock_at(int32_t domain) {
    if (domain < 0 || domain >= CAJETA_CLOCK_MAX_DOMAINS) return NULL;
    return &caj_clock[domain];
}

// Recompute slope/intercept from the accumulators. One accepted sample fixes
// the offset but says nothing about the rate, so the slope stays at the
// driver's nominal period until a second sample disagrees with it — which is
// the honest reading of one point, and keeps a single-sample domain usable.
static void caj_clock_refit(CajClockDomain* d) {
    if (d->n < 1.0) { d->valid = 0; return; }
    if (d->n < 2.0) {
        d->slope = d->period;
    } else {
        double denom = d->n * d->sxx - d->sx * d->sx;
        // Every sample landing on one tick value leaves the rate unknowable.
        // Keeping the nominal period is right; dividing is not.
        if (denom > 0.0) {
            d->slope = (d->n * d->sxy - d->sx * d->sy) / denom;
        } else {
            d->slope = d->period;
        }
    }
    double meanX = d->sx / d->n;
    double meanY = d->sy / d->n;
    // Intercept is carried back to absolute tick zero from the fit origin.
    d->intercept = (d->originHost + meanY) - d->slope * (d->originDev + meanX);
    d->valid = 1;
}

int32_t __cajeta_prof_clock_reset(int32_t domain) {
    CajClockDomain* d = caj_clock_at(domain);
    if (!d) return 0;
    pthread_mutex_lock(&caj_clock_mutex);
    memset(d, 0, sizeof(*d));
    pthread_mutex_unlock(&caj_clock_mutex);
    return 1;
}

int32_t __cajeta_prof_clock_set_period(int32_t domain, double nsPerTick) {
    CajClockDomain* d = caj_clock_at(domain);
    if (!d) return 0;
    // §11.4. NaN fails every comparison, so test it explicitly rather than
    // relying on the range check to catch it.
    if (!(nsPerTick == nsPerTick)) return 0;
    if (nsPerTick < CAJETA_CLOCK_PERIOD_MIN || nsPerTick > CAJETA_CLOCK_PERIOD_MAX) {
        return 0;
    }
    pthread_mutex_lock(&caj_clock_mutex);
    d->period = nsPerTick;
    d->periodSet = 1;
    pthread_mutex_unlock(&caj_clock_mutex);
    return 1;
}

double __cajeta_prof_clock_period(int32_t domain) {
    CajClockDomain* d = caj_clock_at(domain);
    return d ? d->period : 0.0;
}

int32_t __cajeta_prof_clock_set_dispersion_cap(int64_t ns) {
    if (ns <= 0) return 0;
    __atomic_store_n(&caj_clock_dispersion_cap, ns, __ATOMIC_RELEASE);
    return 1;
}

int64_t __cajeta_prof_clock_dispersion_cap(void) {
    return __atomic_load_n(&caj_clock_dispersion_cap, __ATOMIC_ACQUIRE);
}

// One calibration sandwich. Returns CAJETA_CLOCK_OK or the reason it was
// refused — the caller retries a REJECT, and the retry is bounded by the
// caller's own loop count, so a device stuck in a bad power state costs a fixed
// number of samples rather than hanging the profiler (§6.7).
int32_t __cajeta_prof_clock_sample(int32_t domain, int64_t hostBeforeNs,
                                   int64_t devTicks, int64_t hostAfterNs) {
    CajClockDomain* d = caj_clock_at(domain);
    if (!d) return CAJETA_CLOCK_REJECT_DOMAIN;
    if (!d->periodSet) {
        pthread_mutex_lock(&caj_clock_mutex);
        d->rejected++;
        pthread_mutex_unlock(&caj_clock_mutex);
        return CAJETA_CLOCK_REJECT_PERIOD;
    }
    int64_t dispersion = hostAfterNs - hostBeforeNs;
    if (dispersion < 0) {
        pthread_mutex_lock(&caj_clock_mutex);
        d->rejected++;
        pthread_mutex_unlock(&caj_clock_mutex);
        return CAJETA_CLOCK_REJECT_BACKWARD;
    }
    if (dispersion > __cajeta_prof_clock_dispersion_cap()) {
        pthread_mutex_lock(&caj_clock_mutex);
        d->rejected++;
        pthread_mutex_unlock(&caj_clock_mutex);
        return CAJETA_CLOCK_REJECT_DISPERSION;
    }

    // The device read happened somewhere inside the sandwich; its midpoint is
    // the least-wrong host instant to pair it with, and the sandwich width is
    // the bound on how wrong that is.
    int64_t hostMid = hostBeforeNs + dispersion / 2;

    pthread_mutex_lock(&caj_clock_mutex);
    if (d->n == 0.0) {
        d->originDev = (double) devTicks;
        d->originHost = (double) hostMid;
        d->bestDispersion = dispersion;
    } else if (dispersion < d->bestDispersion) {
        d->bestDispersion = dispersion;
    }
    double x = (double) devTicks - d->originDev;
    double y = (double) hostMid - d->originHost;
    d->n   += 1.0;
    d->sx  += x;
    d->sy  += y;
    d->sxx += x * x;
    d->sxy += x * y;
    d->accepted++;
    caj_clock_refit(d);
    pthread_mutex_unlock(&caj_clock_mutex);
    return CAJETA_CLOCK_OK;
}

int32_t __cajeta_prof_clock_samples(int32_t domain) {
    CajClockDomain* d = caj_clock_at(domain);
    return d ? d->accepted : 0;
}

int32_t __cajeta_prof_clock_rejected(int32_t domain) {
    CajClockDomain* d = caj_clock_at(domain);
    return d ? d->rejected : 0;
}

int32_t __cajeta_prof_clock_valid(int32_t domain) {
    CajClockDomain* d = caj_clock_at(domain);
    return d ? d->valid : 0;
}

// §11.6 — an uncorrelated domain returns 0, not the raw ticks. Handing back
// ticks dressed as nanoseconds is precisely the "plausible timeline" the spec
// forbids: it renders, it looks like a measurement, and nothing says otherwise.
int64_t __cajeta_prof_clock_to_host(int32_t domain, int64_t devTicks) {
    CajClockDomain* d = caj_clock_at(domain);
    if (!d || !d->valid) return 0;
    return (int64_t) llround(d->intercept + d->slope * (double) devTicks);
}

// Rate error against the period the driver claimed. This is the number §6.6
// says a single calibration cannot produce and `timestampPeriod` alone cannot
// substitute for.
double __cajeta_prof_clock_drift_ppm(int32_t domain) {
    CajClockDomain* d = caj_clock_at(domain);
    if (!d || !d->valid || d->period <= 0.0) return 0.0;
    return (d->slope / d->period - 1.0) * 1e6;
}

int64_t __cajeta_prof_clock_offset_ns(int32_t domain) {
    CajClockDomain* d = caj_clock_at(domain);
    if (!d || !d->valid) return 0;
    return (int64_t) llround(d->intercept);
}

// 0 = no trustworthy correlation, and every consumer must treat it as "do not
// render a timeline" (§11.6). Above that it is driven by the tightest sandwich
// accepted and by how many samples the fit rests on: one sample cannot see
// rate error at all, so it is capped well below a converged fit no matter how
// tight it was.
int32_t __cajeta_prof_clock_confidence(int32_t domain) {
    CajClockDomain* d = caj_clock_at(domain);
    if (!d || !d->valid || d->accepted <= 0) return 0;
    int32_t c = 100;
    int64_t disp = d->bestDispersion;
    if (disp > 0) {
        // Every 1 us of best-case sandwich costs 5 points, floored at 10.
        int64_t penalty = (disp * 5) / 1000;
        if (penalty > 90) penalty = 90;
        c -= (int32_t) penalty;
    }
    if (d->accepted < 2)      { if (c > 40) c = 40; }
    else if (d->accepted < 8) { if (c > 75) c = 75; }
    if (c < 10) c = 10;
    return c;
}

// §11.3 — flag, never drop and never silently render. A dropped span reads as
// idle time and a rendered one reads as a measurement; only a flagged one reads
// as what it is.
int32_t __cajeta_prof_clock_check_span(int32_t domain, int64_t startNs,
                                       int64_t endNs) {
    CajClockDomain* d = caj_clock_at(domain);
    int32_t flags = CAJETA_SPAN_OK;
    if (!d) return CAJETA_SPAN_UNCORRELATED;
    if (!d->valid) flags |= CAJETA_SPAN_UNCORRELATED;
    if (endNs < startNs) {
        flags |= CAJETA_SPAN_NEGATIVE;
    } else if (endNs - startNs > CAJETA_SPAN_MAX_NS) {
        flags |= CAJETA_SPAN_IMPLAUSIBLE;
    }
    pthread_mutex_lock(&caj_clock_mutex);
    if (d->haveLastSpan && startNs < d->lastSpanStart) {
        flags |= CAJETA_SPAN_NONMONOTONIC;
    }
    // Advance the watchdog only on a span that is itself sane, so one bad
    // timestamp cannot poison the ordering check for every span after it.
    if (!(flags & (CAJETA_SPAN_NEGATIVE | CAJETA_SPAN_IMPLAUSIBLE
                   | CAJETA_SPAN_NONMONOTONIC))) {
        d->lastSpanStart = startNs;
        d->haveLastSpan = 1;
    }
    pthread_mutex_unlock(&caj_clock_mutex);
    return flags;
}

// §6.7 — bounded calibration. Stops as soon as `wantSamples` are accepted, so
// a healthy device pays for exactly what it needs, and stops unconditionally at
// `maxAttempts`, so an unhealthy one costs a fixed number of reads instead of
// the run. A read the backend itself reports as failed burns an attempt like
// any other: a device that cannot be read is not a device worth waiting for.
int32_t __cajeta_prof_clock_calibrate(int32_t domain, CajetaClockReadFn read,
                                      void* user, int32_t wantSamples,
                                      int32_t maxAttempts) {
    if (!read || wantSamples <= 0 || maxAttempts <= 0) return 0;
    CajClockDomain* d = caj_clock_at(domain);
    if (!d) return 0;

    int32_t accepted = 0;
    for (int32_t attempt = 0; attempt < maxAttempts && accepted < wantSamples;
         ++attempt) {
        int64_t before = 0, ticks = 0, after = 0;
        if (!read(&before, &ticks, &after, user)) continue;
        if (__cajeta_prof_clock_sample(domain, before, ticks, after)
                == CAJETA_CLOCK_OK) {
            if (accepted == 0) {
                // §7.5 — anchor the snapshot on the FIRST accepted pair of this
                // round. Anchoring on the last would describe a mapping the
                // round's own later samples had already refined.
                caj_clock_record_snapshot(domain, before + (after - before) / 2,
                                          ticks);
            }
            accepted++;
        }
    }
    return accepted;
}

// ── §7.5: clock snapshots ─────────────────────────────────────────────────
//
// A completed calibration records the host/device pair it anchored on. The
// point is reproducibility from the TRACE: converted timestamps alone leave a
// reader unable to check the conversion, which is the same position §11.6
// refuses to put a consumer in.
//
// The ring is small and drops the OLDEST on overflow. A long run recalibrating
// every few seconds would otherwise either grow without bound or — worse —
// keep only the first, which is precisely the single-calibration answer §6.6
// says is insufficient.
static CajetaClockSnapshot caj_clock_snaps[CAJETA_CLOCK_MAX_SNAPSHOTS];
static int32_t caj_clock_snap_n = 0;
static int32_t caj_clock_generation[CAJETA_CLOCK_MAX_DOMAINS];

static void caj_clock_record_snapshot(int32_t domain, int64_t hostNs,
                                      int64_t devTicks) {
    pthread_mutex_lock(&caj_clock_mutex);
    int32_t gen = ++caj_clock_generation[domain];
    if (caj_clock_snap_n == CAJETA_CLOCK_MAX_SNAPSHOTS) {
        for (int32_t i = 1; i < CAJETA_CLOCK_MAX_SNAPSHOTS; ++i) {
            caj_clock_snaps[i - 1] = caj_clock_snaps[i];
        }
        caj_clock_snap_n--;
    }
    CajetaClockSnapshot* s = &caj_clock_snaps[caj_clock_snap_n++];
    s->domain = domain;
    s->generation = gen;
    s->hostNs = hostNs;
    s->devTicks = devTicks;
    pthread_mutex_unlock(&caj_clock_mutex);
}

static void caj_clock_copy_id(char* dst, const char* src) {
    if (!src) { dst[0] = 0; return; }
    int32_t i = 0;
    while (src[i] && i < CAJETA_DRIVER_ID_MAX - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

int32_t __cajeta_prof_set_driver_identity(int32_t domain, const char* driver,
                                          const char* layers) {
    CajClockDomain* d = caj_clock_at(domain);
    if (!d) return 0;
    pthread_mutex_lock(&caj_clock_mutex);
    caj_clock_copy_id(d->driver, driver);
    caj_clock_copy_id(d->layers, layers);
    pthread_mutex_unlock(&caj_clock_mutex);
    return 1;
}

const char* __cajeta_prof_driver_identity(int32_t domain) {
    CajClockDomain* d = caj_clock_at(domain);
    return (d && d->driver[0]) ? d->driver : NULL;
}

const char* __cajeta_prof_active_layers(int32_t domain) {
    CajClockDomain* d = caj_clock_at(domain);
    return (d && d->layers[0]) ? d->layers : NULL;
}

int32_t __cajeta_prof_clock_generation(int32_t domain) {
    if (domain < 0 || domain >= CAJETA_CLOCK_MAX_DOMAINS) return 0;
    return caj_clock_generation[domain];
}

int32_t __cajeta_prof_clock_snapshot_count(void) { return caj_clock_snap_n; }

int32_t __cajeta_prof_clock_snapshot_get(int32_t index,
                                         CajetaClockSnapshot* out) {
    if (!out || index < 0 || index >= caj_clock_snap_n) return 0;
    pthread_mutex_lock(&caj_clock_mutex);
    *out = caj_clock_snaps[index];
    pthread_mutex_unlock(&caj_clock_mutex);
    return 1;
}

int32_t __cajeta_prof_clock_snapshot_clear(void) {
    pthread_mutex_lock(&caj_clock_mutex);
    int32_t had = caj_clock_snap_n;
    caj_clock_snap_n = 0;
    memset(caj_clock_generation, 0, sizeof(caj_clock_generation));
    pthread_mutex_unlock(&caj_clock_mutex);
    return had;
}
