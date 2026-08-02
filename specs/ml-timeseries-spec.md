# ml-timeseries — stationarity, correlograms, and the ARIMA family

## 1. Definition

### 1.1 Purpose

Cajeta has no time-series capability at any layer. `cajeta.math.stats` has
distribution functions and `cajeta.math.fft` has transforms, but there is no
series abstraction, no stationarity test, no correlogram, and no forecasting
model. This spec adds the statsmodels-role surface: series handling, seasonal
decomposition, stationarity testing and differencing, ACF/PACF, and the
AR/MA/ARMA/ARIMA family with forecasting.

### 1.2 Scope basis

The classical time-series surface — series components and decomposition,
stationarity and differencing, white-noise and random-walk diagnostics,
correlograms, and the AR/MA/ARMA/ARIMA family — scoped against statsmodels'
`tsa` module.

### 1.3 The parity target is statsmodels, not scikit-learn

Every other estimator in `dev.cajeta.ml` pins against scikit-learn. Time series
does not — `statsmodels.tsa` is the reference implementation for this surface,
and that is what cajeta must agree with. The oracle-not-a-port discipline is unchanged; only the oracle
differs. Pin the statsmodels version explicitly in the plan.

### 1.4 Scope

Series representation and lag operators; trend/seasonal/residual decomposition;
the ADF stationarity test and differencing; white-noise and random-walk
diagnostics; ACF and PACF with confidence bands; simple and weighted moving
averages; AR, MA, ARMA, ARIMA estimation and forecasting.

### 1.5 Non-goals

- **1.5.1** SARIMA / seasonal orders beyond accepting the parameter shape, VAR,
  state-space models, exponential smoothing / Holt-Winters, and Prophet-style
  decomposition. None have a consumer yet.
- **1.5.2** Automatic order selection (`auto_arima`). §9.5 exposes the
  information criteria that would drive it; the search itself is deferred.
- **1.5.3** Plotting. Cajeta produces the correlogram *values* and confidence
  bands; rendering belongs to `cajeta-chart`.
- **1.5.4** Irregularly-sampled or multivariate series. A series here is
  measurements made at regular intervals.

### 1.6 Systems

`cajeta.math.Tensor`, `cajeta.math.linalg.LinAlg` (`lstsq`, `solve`,
`cholesky`), `cajeta.math.stats.Stats` (`normalCdf`, `tCdf`),
`cajeta.math.fft.Fft` (FFT-based autocovariance), `cajeta.nucleo.frame`
(time-indexed columns), `dev.cajeta.unit`.

---

## 2. Feature: series representation

- **2.1** When a series from a numeric tensor and a regular time index is
  constructed, the result is a value type carrying both, and construction
  rejects a mismatched index length.
- **2.2** When `lag(k)` of a series is taken, the result is the series shifted
  by `k` with the first `k` positions marked missing rather than zero-filled.
- **2.3** When a lag matrix over lags `1..p` is requested, the result is the
  design matrix the autoregressions in §8 need, with incomplete leading rows
  dropped and the alignment documented.
- **2.4** When a series carries missing values, every operation states its
  policy explicitly — propagate, drop, or raise — and no operation silently
  imputes.
- **2.5** When a series for backtesting is split, the split is chronological,
  never shuffled — the ordinary `trainTestSplit` is wrong here and must not be
  reachable by accident.

---

## 3. Feature: components and decomposition

A series decomposes into four components: trend, seasonality, cyclical, and
irregular (residual).

- **3.1** When a series with a stated period is decomposed, the result is
  trend, seasonal, and residual components that recombine to the original.
- **3.2** When the additive model is chosen, `y = trend + seasonal + residual`;
  multiplicative gives `y = trend × seasonal × residual`.
- **3.3** When a multiplicative decomposition on a series containing zero or
  negative values is requested, it is rejected with the reason named, not
  silently producing infinities.
- **3.4** When the series is shorter than two full periods, decomposition fails
  loudly — statsmodels requires two complete cycles.
- **3.5** When not supply a period is done, it is inferred from the time index
  if one is available, and otherwise the call is rejected.
- **3.6** When a series is decomposed, the trend is extracted by a centered moving
  average
  whose window follows from the period, matching `seasonal_decompose`'s
  convention including its endpoint handling.

---

## 4. Feature: stationarity

Forecasting methods assume stationarity, and lag-based predictors must be
near-independent for regression to work.

- **4.1** When the Augmented Dickey-Fuller test is run, the result is the test
  statistic, the p-value, the lag used, the number of observations, and the
  critical values at 1%, 5%, and 10%.
- **4.2** When not specify `maxLag` is done, the default is `⌊12·(n/100)^¼⌋`,
  the statsmodels default.
- **4.3** When automatic lag selection is selected, AIC, BIC, and t-statistic
  strategies are all available.
- **4.4** When the regression form is chosen, constant, constant+trend, no-
  constant, and constant+linear+quadratic-trend are selectable, matching
  `adfuller`'s `regression` parameter.
- **4.5** When the result is read, the null hypothesis is stated in the API
  docs — ADF's null is a **unit root**, so a small p-value means stationary.
  This is the inverse of KPSS and is the single most common misreading; the
  type must make it hard to get backwards.
- **4.6** When a series is differenced, `diff(t) = y(t) − y(t−1)`, the result
  is one element shorter, and the leading value is dropped rather than filled —
  the worked example `[2,3,4,10,20] → [1,1,6,10] → [0,5,4]` is a test.
- **4.7** When differenceing to order `d`, differencing is applied `d` times
  and the series shortens by `d`.
- **4.8** When a forecast's input has been differenced, there is a documented
  inverse that integrates predictions back to the original scale — a forecast
  left in differenced space is useless.

---

## 5. Feature: white-noise and random-walk diagnostics

These series are unpredictable, so detect them before spending effort
modelling.

- **5.1** When a series for white noise is tested, all three conditions are
  checked — mean approximately zero, constant variance over
  time, and no significant autocorrelation at any lag — and the result reports
  which condition failed.
- **5.2** When model residuals for white noise is tested, a pass means the
  model has extracted the available signal, and this is the documented way to
  judge a fit.
- **5.3** When a portmanteau test on residuals is run, the result is a
  statistic and p-value over a chosen number of lags.
- **5.4** When a random walk from a seed is generated, `y(t) = y(t−1) + ε(t)`
  with `ε` white noise, and it is reproducible — needed to test that the
  diagnostics actually fire.
- **5.5** When a series is a random walk, the ADF test fails to reject and the
  diagnostic says so in those terms.

---

## 6. Feature: ACF and PACF

- **6.1** When the ACF to `nlags` is computed, the result is the
  autocorrelation at each lag, with lag 0 equal to 1.
- **6.2** When confidence intervals at level `alpha` (default 0.05) is
  requested, the result is the band significance is read from.
- **6.3** When Bartlett's formula for the band are chosen, it widens with lag,
  matching `plot_acf`'s `bartlett_confint` default.
- **6.4** When the adjusted (unbiased) estimator is selected, denominators use
  `n−k` rather than `n`.
- **6.5** When the series is long, autocovariance may be computed by FFT
  through `cajeta.math.fft` without changing the result beyond tolerance.
- **6.6** When the PACF is computed, the result is the partial autocorrelations
  — the correlation at lag `k` net of lags `1..k−1`.
- **6.7** When a PACF method is chosen, Yule-Walker (`ywm`, the default), OLS,
  and Levinson-Durbin are available.
- **6.8** When the correlograms is read, the reading rule is documented: the
  last lag outside the band suggests `q` from the ACF and `p` from the PACF, and
  lag 0 is ignored in the PACF.

---

## 7. Feature: moving averages

- **7.1** When a simple moving average of window `w` is computed, each value is
  the mean of the previous `w` observations.
- **7.2** When a weighted moving average is computed, weights are supplied, and
  weights that do not sum to 1 are rejected.
- **7.3** When the window exceeds the series length, the call is rejected
  rather than returning an empty series.
- **7.4** When a moving average forecasts, it is available as a baseline
  `Predictor` — the naïve model everything else must beat.

---

## 8. Feature: AR, MA, ARMA, ARIMA

- **8.1** When `AutoReg` with `p` lags is fitted, coefficients are estimated by
  least squares on the lag matrix and are readable with their
  standard errors.
- **8.2** When an explicit lag list rather than an order is passed, only those
  lags enter the model, e.g. `[1, 4]`.
- **8.3** When the trend term is chosen, constant, no-constant, and
  constant+trend are selectable.
- **8.4** When an MA(q) model is fitted, the model is expressed in past error
  terms and estimated by maximum likelihood, since the errors are unobserved
  and least squares does not apply.
- **8.5** When ARMA(p, q) is fitted, both past values and past errors enter,
  and ARMA is the `d = 0` case of §8.6 rather than a separate implementation.
- **8.6** When `ARIMA(p, d, q)` is fitted, the series is differenced `d` times,
  an ARMA(p, q) is fitted, and forecasts are integrated back to the original
  scale automatically.
- **8.7** When stationarity or invertibility is enforced, the parameter search
  is constrained accordingly, matching statsmodels' defaults of enabled.
- **8.8** When estimation fails to converge, the failure is loud and the last
  iterate is reported as such — never returned as if converged.
- **8.9** When any of these is fitted, they conform to the `Predictor` protocol
  so far as forecasting allows, and where they cannot (no `predict(x)` on
  unseen rows), the difference is explicit in the type rather than a method
  that throws.

---

## 9. Feature: forecasting and evaluation

- **9.1** When `h` steps ahead is forecast, the result is `h` predictions in
  the original scale of the series.
- **9.2** When prediction intervals is requested, the result is them at a
  stated confidence level, widening with the horizon.
- **9.3** When a model is backtested with a rolling origin, it refits as the
  origin advances and never sees future data — the time-series analogue of
  cross-validation, and the only honest way to score a forecast.
- **9.4** When a forecast is scored, MSE, RMSE, MAE, and MAPE are available;
  the first three reuse `Metrics`, and MAPE is added with its divide-by-zero
  behaviour documented.
- **9.5** When a fitted model's summary is read, the result is coefficients,
  standard errors, log-likelihood, AIC, and BIC — the statistics order
  selection depends on.

---

## 10. Open questions (resolve at plan time)

- **10.1** *(resolved — see roadmap §4.)* **`dev.cajeta.timeseries`**, a
  separate library depending on `cajeta.math` and reusing `Metrics`. The parity
  oracle differs (statsmodels, not sklearn), the `Predictor` protocol fits
  awkwardly (§8.9), and the domain is self-contained.
- **10.2** *(resolved 2026-08-01 — stdlib gains one.)* **`cajeta.math` gains
  L-BFGS and Nelder-Mead**, specified in `stdlib-completion` §8.6. This spec
  consumes them and does not bring its own.

  It was the largest unknown here, and the placement matters beyond this spec:
  an optimizer is domain-neutral by the roadmap's §1.3.1 test, `dev.cajeta.ml`'s
  solvers and any future MLE both want one, and building it inside
  `dev.cajeta.timeseries` would repeat exactly the mistake the distance kernels
  nearly made. **Sequencing consequence: `stdlib-completion` §8.6 must land
  before §8's MA/ARMA units can open.**
- **10.3** *(resolved 2026-08-01 — a thin dedicated type.)* The series is its
  own value type, constructible from a `nucleo.frame` column but not requiring
  one, so the frame does not become a dependency of the numerics.
- **10.4** **Still open — an action for the plan's first unit.** Pin an
  explicit statsmodels version and record it. `cajeta-ml`'s README cites
  statsmodels-computed fixtures with no version at all, which makes every
  existing fixture unreproducible; fixing that is part of this work, not
  separate from it.
- **10.5** *(resolved 2026-08-01 — add it.)* KPSS ships beside ADF. It tests
  the complementary null and materially reduces the §4.5 misreading, which is
  the single most common error in reading a stationarity test.

---

## 11. Acceptance criteria (spec-level)

- **11.1** Every numeric claim is pinned against a fixture computed by the
  pinned statsmodels version; deviations are recorded in a
  `DifferencesFromStatsmodels` note mirroring the sklearn one.
- **11.2** The differencing example (§4.6) is a test.
- **11.3** A generated random walk is correctly identified as non-stationary,
  and generated white noise as stationary with no significant autocorrelation.
- **11.4** An ARIMA fitted on a differenced series returns forecasts in the
  original scale — verified by round-tripping a series with known trend.
- **11.5** No forecast API permits accidental lookahead: chronological
  splitting and rolling-origin backtesting are the only paths offered.
- **11.6** ADF's null hypothesis is unmistakable at the call site.
