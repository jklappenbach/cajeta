// === Cajeta runtime fragment — TEXTUALLY #included into cajeta_runtime.c ===
// Clock correlation: maps each device tick domain onto the host timeline by
// recovering BOTH offset and rate, and declines to answer until it can.

// The sandwich width above which a sample is discarded.
#define CAJ_CLOCK_DEFAULT_DISPERSION_NS 50000

typedef struct {
    double  period;          // nominal host ns per tick, as the driver claims
    int32_t periodSet;

    // Accumulating about an origin, not zero: tick counts reach 1e11 and their
    // squares leave the f64 mantissa's exact range, flattening the slope.
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

    // Copied, not aliased: a backend need not keep its vendor string alive.
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

/// Refits from the accumulators; one sample's slope stays at the nominal rate.
static void caj_clock_refit(CajClockDomain* d) {
    if (d->n < 1.0) { d->valid = 0; return; }
    if (d->n < 2.0) {
        d->slope = d->period;
    } else {
        double denom = d->n * d->sxx - d->sx * d->sx;
        // Every sample on one tick value leaves the rate unknowable.
        if (denom > 0.0) {
            d->slope = (d->n * d->sxy - d->sx * d->sy) / denom;
        } else {
            d->slope = d->period;
        }
    }
    double meanX = d->sx / d->n;
    double meanY = d->sy / d->n;
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
    // NaN fails every comparison, so the range check below cannot catch it.
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

/// Folds one sandwich (host before, device ticks, host after) into the fit;
/// returns CAJETA_CLOCK_OK or the CAJETA_CLOCK_REJECT_* reason it was refused.
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

    // The read happened inside the sandwich; its midpoint is the least wrong.
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

/// Device ticks to host ns; an uncorrelated domain returns 0, never raw ticks.
int64_t __cajeta_prof_clock_to_host(int32_t domain, int64_t devTicks) {
    CajClockDomain* d = caj_clock_at(domain);
    if (!d || !d->valid) return 0;
    return (int64_t) llround(d->intercept + d->slope * (double) devTicks);
}

/// The measured rate error against the period the driver claimed, in ppm.
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

/// 0 means no trustworthy correlation, and consumers must render no timeline.
/// Above that it falls with the sandwich width and with how few samples there are.
int32_t __cajeta_prof_clock_confidence(int32_t domain) {
    CajClockDomain* d = caj_clock_at(domain);
    if (!d || !d->valid || d->accepted <= 0) return 0;
    int32_t c = 100;
    int64_t disp = d->bestDispersion;
    if (disp > 0) {
        int64_t penalty = (disp * 5) / 1000;
        if (penalty > 90) penalty = 90;
        c -= (int32_t) penalty;
    }
    if (d->accepted < 2)      { if (c > 40) c = 40; }
    else if (d->accepted < 8) { if (c > 75) c = 75; }
    if (c < 10) c = 10;
    return c;
}

/// The CAJETA_SPAN_* flags for one span: flagged, never dropped nor rendered.
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
    // Advancing only on a sane span keeps one bad timestamp from poisoning it.
    if (!(flags & (CAJETA_SPAN_NEGATIVE | CAJETA_SPAN_IMPLAUSIBLE
                   | CAJETA_SPAN_NONMONOTONIC))) {
        d->lastSpanStart = startNs;
        d->haveLastSpan = 1;
    }
    pthread_mutex_unlock(&caj_clock_mutex);
    return flags;
}

/// Samples through `read` until `wantSamples` are accepted or `maxAttempts`
/// spent, returning the accepted count. A failed read burns an attempt too.
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
                // Anchored on the FIRST accepted pair, not the refined last.
                caj_clock_record_snapshot(domain, before + (after - before) / 2,
                                          ticks);
            }
            accepted++;
        }
    }
    return accepted;
}

// ── §7.5: clock snapshots ─────────────────────────────────────────────────
// A calibration records the pair it anchored on, so the conversion is checkable
// from the trace itself. The ring drops the OLDEST on overflow.
static CajetaClockSnapshot caj_clock_snaps[CAJETA_CLOCK_MAX_SNAPSHOTS];
static int32_t caj_clock_snap_n = 0;
static int32_t caj_clock_generation[CAJETA_CLOCK_MAX_DOMAINS];

/// Appends one anchor pair to the ring under a fresh per-domain generation.
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
