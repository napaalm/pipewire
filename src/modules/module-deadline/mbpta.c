/*
 * mbpta.c
 *
 * Implementation of the MBPTA estimator declared in mbpta.h.
 *
 * Pure data: no PipeWire runtime symbols, no syscalls; safe to
 * unit-test in isolation. The math follows Cucu-Grosjean et al.
 * 2012 ECRTS §II-§III directly. Where the paper leaves a
 * decision open the code picks the conservative side and
 * comments the choice.
 *
 * Copyright (C) 2026 Antonio Napolitano and Francesco Barcherini
 */

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbpta.h"

/* Working-precision floor for the per-node exceedance target.
 * Cucu-Grosjean 2012 §III-D step 6 evaluates pWCET via the Gumbel
 * inverse CDF
 *
 *     pWCET(eps) = mu - sigma * ln(-ln(1 - eps)),
 *
 * which is numerically degenerate as `eps` approaches the IEEE-754
 * subnormal range: `1 - eps` rounds to 1.0 (cancellation loses every
 * significant bit) and the inner log becomes 0, the outer log
 * diverges, and the extrapolated quantile collapses to mu - sigma *
 * (-inf). The paper picks 10^-16 as its working precision; clamp at
 * the same floor and surface the effective value in diagnostics so a
 * configuration that hit the cap is observable. */
#define MBPTA_EPS_NODE_FLOOR 1.0e-16

static double mbpta_effective_eps_value(double cfg_eps)
{
	if (cfg_eps < MBPTA_EPS_NODE_FLOOR)
		return MBPTA_EPS_NODE_FLOOR;
	return cfg_eps;
}

const char *mbpta_state_name(enum mbpta_state s)
{
	switch (s) {
	case MBPTA_INSUFFICIENT_DATA:   return "insufficient_data";
	case MBPTA_IID_PENDING:         return "iid_pending";
	case MBPTA_NON_GUMBEL:          return "non_gumbel";
	case MBPTA_PENDING_CONVERGENCE: return "pending_convergence";
	case MBPTA_PWCET_VALID:         return "pwcet_valid";
	case MBPTA_DRIFT:               return "drift";
	}
	return "unknown";
}

const char *mbpta_invalidation_reason_name(enum mbpta_invalidation_reason r)
{
	switch (r) {
	case MBPTA_INVALIDATED_NONE:                return "none";
	case MBPTA_INVALIDATED_PERIOD:              return "period";
	case MBPTA_INVALIDATED_FUSION_GROUP:        return "fusion_group";
	case MBPTA_INVALIDATED_TOPOLOGY_GENERATION: return "topology_generation";
	case MBPTA_INVALIDATED_CPU_CLASS:           return "cpu_class";
	case MBPTA_INVALIDATED_OPERATOR_REQUEST:    return "operator_request";
	}
	return "unknown";
}

struct mbpta {
	struct mbpta_config cfg;
	enum mbpta_invalidation_reason last_invalidation;

	/* Sample ring. samples_total counts every sample ever fed;
	 * we drop the first `warmup_discard` ones. window_count is
	 * the live size, capped at sample_window. */
	uint64_t *samples;
	uint32_t  head;             /* next write index */
	uint32_t  window_count;
	uint64_t  samples_total;

	/* Samples accumulated since last evaluation; triggers
	 * re-fit when >= n_delta. */
	uint32_t  samples_since_eval;

	enum mbpta_state state;

	double   ks_stat;
	double   ks_pvalue;
	double   runs_z;
	double   runs_pvalue;
	double   et_pvalue;
	double   gev_shape_k;
	double   crps;
	uint32_t convergence_streak;
	uint32_t iid_reject_streak;

	/* Last Gumbel fit. */
	double   mu;
	double   sigma;
	uint64_t pwcet_ns_cached;

	/* Previous block-maxima 1-CDF for CRPS. The series is
	 * stored sorted ascending, so we can evaluate F_bar at
	 * common abscissae across rounds. NULL until the first fit
	 * lands. */
	double  *prev_bm;
	uint32_t prev_bm_count;
};

static int sort_u64_asc(const void *a, const void *b)
{
	uint64_t ua = *(const uint64_t *)a;
	uint64_t ub = *(const uint64_t *)b;
	return (ua > ub) - (ua < ub);
}

static int sort_double_asc(const void *a, const void *b)
{
	double da = *(const double *)a;
	double db = *(const double *)b;
	return (da > db) - (da < db);
}

mbpta_t *mbpta_create(const struct mbpta_config *cfg)
{
	mbpta_t *e;

	if (cfg == NULL)
		return NULL;
	if (cfg->sample_window < 2 || cfg->block_size < 2 ||
			cfg->min_blocks < 2)
		return NULL;
	if (cfg->alpha_iid <= 0.0 || cfg->alpha_iid >= 1.0)
		return NULL;
	if (cfg->crps_threshold <= 0.0)
		return NULL;
	if (cfg->eps_node <= 0.0 || cfg->eps_node >= 1.0)
		return NULL;
	if (cfg->gumbel_r2_threshold < 0.0 ||
	    cfg->gumbel_r2_threshold > 1.0)
		return NULL;
	if (cfg->alpha_et < 0.0 || cfg->alpha_et >= 1.0)
		return NULL;

	e = calloc(1, sizeof(*e));
	if (e == NULL)
		return NULL;
	e->cfg = *cfg;
	e->samples = calloc(cfg->sample_window, sizeof(*e->samples));
	if (e->samples == NULL) {
		free(e);
		return NULL;
	}
	e->state = MBPTA_INSUFFICIENT_DATA;
	e->ks_pvalue = 1.0;
	e->runs_pvalue = 1.0;
	e->et_pvalue = 1.0;
	return e;
}

void mbpta_destroy(mbpta_t *e)
{
	if (e == NULL)
		return;
	free(e->samples);
	free(e->prev_bm);
	free(e);
}

void mbpta_invalidate_with_reason(mbpta_t *e,
		enum mbpta_invalidation_reason reason)
{
	if (e == NULL)
		return;
	memset(e->samples, 0, e->cfg.sample_window * sizeof(*e->samples));
	e->head = 0;
	e->window_count = 0;
	e->samples_total = 0;
	e->samples_since_eval = 0;
	e->state = MBPTA_INSUFFICIENT_DATA;
	e->ks_stat = 0.0;
	e->ks_pvalue = 1.0;
	e->runs_z = 0.0;
	e->runs_pvalue = 1.0;
	e->et_pvalue = 1.0;
	e->gev_shape_k = 0.0;
	e->crps = 0.0;
	e->convergence_streak = 0;
	e->iid_reject_streak = 0;
	e->mu = 0.0;
	e->sigma = 0.0;
	e->pwcet_ns_cached = 0;
	free(e->prev_bm);
	e->prev_bm = NULL;
	e->prev_bm_count = 0;
	e->last_invalidation = reason;
}

void mbpta_invalidate(mbpta_t *e)
{
	mbpta_invalidate_with_reason(e, MBPTA_INVALIDATED_OPERATOR_REQUEST);
}

enum mbpta_invalidation_reason mbpta_last_invalidation_reason(const mbpta_t *e)
{
	return e ? e->last_invalidation : MBPTA_INVALIDATED_NONE;
}

/* Two-sample KS statistic on the most recent window split in
 * halves. Returns D = sup|F1 - F2| over the merged sorted values.
 * If the window is below the minimum size, returns 0. */
static double ks_statistic_two_sample(const mbpta_t *e)
{
	const uint32_t n = e->window_count;
	uint32_t m1, m2;
	uint64_t *h1, *h2;
	double d_max = 0.0;
	uint32_t i, j;

	if (n < 4)
		return 0.0;

	m1 = n / 2;
	m2 = n - m1;
	h1 = calloc(m1, sizeof(*h1));
	h2 = calloc(m2, sizeof(*h2));
	if (h1 == NULL || h2 == NULL) {
		free(h1); free(h2);
		return 0.0;
	}
	/* Walk the ring oldest-first. The oldest sample sits at
	 * (head - window_count + sample_window) mod sample_window;
	 * subsequent samples follow. We split into the first half
	 * (h1) and the second half (h2) for the two-sample test. */
	uint32_t start = (e->head + e->cfg.sample_window - e->window_count)
		% e->cfg.sample_window;
	for (i = 0; i < m1; i++)
		h1[i] = e->samples[(start + i) % e->cfg.sample_window];
	for (j = 0; j < m2; j++)
		h2[j] = e->samples[(start + m1 + j) % e->cfg.sample_window];

	qsort(h1, m1, sizeof(*h1), sort_u64_asc);
	qsort(h2, m2, sizeof(*h2), sort_u64_asc);

	/* Two-pointer sweep over the merge to evaluate the CDFs at
	 * every distinct value. */
	i = 0; j = 0;
	while (i < m1 || j < m2) {
		double f1 = (double)i / (double)m1;
		double f2 = (double)j / (double)m2;
		double d = f1 > f2 ? f1 - f2 : f2 - f1;
		if (d > d_max)
			d_max = d;
		if (i < m1 && (j >= m2 || h1[i] <= h2[j]))
			i++;
		else
			j++;
	}

	free(h1);
	free(h2);
	return d_max;
}

/* KS critical value at significance alpha for two samples of
 * size m and n. The Smirnov / Massey approximation:
 *
 *   D_crit = c(alpha) * sqrt((m + n) / (m * n))
 *
 * with c(0.05) ~ 1.36. */
static double ks_critical(double alpha, uint32_t m, uint32_t n)
{
	double c = 1.36; /* alpha = 0.05 default */
	if (alpha <= 0.01)
		c = 1.63;
	else if (alpha <= 0.025)
		c = 1.48;
	else if (alpha <= 0.05)
		c = 1.36;
	else if (alpha <= 0.10)
		c = 1.22;
	else if (alpha <= 0.20)
		c = 1.07;
	return c * sqrt((double)(m + n) / ((double)m * (double)n));
}

/* Asymptotic two-sample Kolmogorov-Smirnov p-value (Smirnov 1948,
 * via the Stephens 1970 small-sample correction):
 *
 *     Q(lambda) = 2 * sum_{k=1..inf} (-1)^(k-1) * exp(-2 k^2 lambda^2)
 *     lambda   = (sqrt(n_eff) + 0.12 + 0.11 / sqrt(n_eff)) * D
 *     n_eff    = m * n / (m + n)
 *
 * Returns p in [0, 1]; degenerate inputs (D == 0, m or n < 2)
 * return 1.0 because there is no evidence against H_0. The series
 * converges very fast for typical D values (~5 terms is plenty);
 * the loop bails at 100 iterations to stay bounded under
 * pathological inputs. */
static double ks_pvalue(double d_stat, uint32_t m, uint32_t n)
{
	double n_eff, sqrt_neff, lambda, term, sum;
	int sign, k;

	if (d_stat <= 0.0 || m < 2 || n < 2)
		return 1.0;

	n_eff = ((double)m * (double)n) / ((double)m + (double)n);
	sqrt_neff = sqrt(n_eff);
	lambda = (sqrt_neff + 0.12 + 0.11 / sqrt_neff) * d_stat;

	sum = 0.0;
	sign = 1;
	for (k = 1; k <= 100; k++) {
		term = exp(-2.0 * (double)k * (double)k * lambda * lambda);
		sum += (double)sign * term;
		if (term < 1.0e-12)
			break;
		sign = -sign;
	}
	sum *= 2.0;
	if (sum < 0.0) sum = 0.0;
	if (sum > 1.0) sum = 1.0;
	return sum;
}

/* Two-sided p-value for the Wald-Wolfowitz runs-test Z under the
 * asymptotic normal approximation:
 *
 *     p = 2 * Q(|z|) = erfc(|z| / sqrt(2))
 *
 * Returns 1.0 for z == 0 (no evidence against independence) and
 * collapses smoothly to 0 for large |z|. */
static double runs_pvalue(double z)
{
	double az = z < 0.0 ? -z : z;
	if (az == 0.0)
		return 1.0;
	return erfc(az / sqrt(2.0));
}

/* Runs test on the up/down sequence sign(x_{i+1} - x_i) of a
 * continuous-valued series (Cucu-Grosjean 2012 §V-C). The
 * classical Wald-Wolfowitz two-sample formula E[R] = 2pm/N + 1
 * applies to a binary sequence with *fixed* margin counts p and m
 * (e.g. a string of pre-decided heads and tails). When the binary
 * sequence is derived from differences of continuous i.i.d.
 * samples the marginal sign-change rate is not 1/2 but 2/3 -- out
 * of the six equally-likely orderings of three i.i.d. continuous
 * values, four make sign(x_{i+1}-x_i) flip and only two keep it.
 * The correct moments for this case are due to Bartels (1982):
 *
 *     E[R] = (2N - 1) / 3
 *     Var[R] = (16N - 29) / 90
 *
 * with N = number of samples. Under H_0 (independence) Z is
 * asymptotically standard normal. Using the binary formula here
 * pulls E[R] roughly halfway toward the alternative; the
 * resulting Z grows linearly in N and the test rejects every
 * truly random stream of any nontrivial size. */
static double runs_z(const mbpta_t *e)
{
	const uint32_t n = e->window_count;
	uint32_t runs = 0;
	int prev_sign = 0;
	uint32_t i;
	double er, vr;

	if (n < 4)
		return 0.0;

	uint32_t start = (e->head + e->cfg.sample_window - e->window_count)
		% e->cfg.sample_window;
	uint64_t prev_x = e->samples[start];
	for (i = 1; i < n; i++) {
		uint64_t x = e->samples[(start + i) % e->cfg.sample_window];
		int s = (x > prev_x) ? 1 : (x < prev_x ? -1 : 0);
		if (s != 0) {
			if (s != prev_sign) {
				runs++;
				prev_sign = s;
			}
		}
		prev_x = x;
	}

	if (runs == 0)
		return 0.0;

	er = (2.0 * (double)n - 1.0) / 3.0;
	vr = (16.0 * (double)n - 29.0) / 90.0;
	if (vr <= 0.0)
		return 0.0;
	return ((double)runs - er) / sqrt(vr);
}

/* Build the block-maxima series from the current window. Block
 * size is e->cfg.block_size; the result has floor(window / block)
 * elements in `out`. Returns the number of blocks written. */
static uint32_t build_block_maxima(const mbpta_t *e, double *out, uint32_t cap)
{
	uint32_t n_blocks, b, k;
	uint32_t start = (e->head + e->cfg.sample_window - e->window_count)
		% e->cfg.sample_window;

	n_blocks = e->window_count / e->cfg.block_size;
	if (n_blocks > cap)
		n_blocks = cap;

	for (b = 0; b < n_blocks; b++) {
		uint64_t mx = 0;
		for (k = 0; k < e->cfg.block_size; k++) {
			uint32_t idx = (start + b * e->cfg.block_size + k)
				% e->cfg.sample_window;
			if (e->samples[idx] > mx)
				mx = e->samples[idx];
		}
		out[b] = (double)mx;
	}
	return n_blocks;
}

/* Gumbel parameter estimate via QQ-plot linear regression. The
 * input array `bm` is mutated (sorted ascending). For each
 * sorted bm[i], the empirical CDF is p_i = (i + 1) / (n + 1);
 * the standard-Gumbel quantile is q_i = -ln(-ln(p_i)). A least-
 * squares fit y = mu + sigma * q on (q_i, bm[i]) gives mu =
 * intercept, sigma = slope. */
static void gumbel_fit(double *bm, uint32_t n, double *out_mu,
		double *out_sigma, double *out_r2)
{
	double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
	double sy_mean, ss_res = 0.0, ss_tot = 0.0;
	uint32_t i;

	*out_mu = 0.0;
	*out_sigma = 0.0;
	*out_r2 = 0.0;
	if (n < 2)
		return;

	qsort(bm, n, sizeof(*bm), sort_double_asc);
	for (i = 0; i < n; i++) {
		double p = (double)(i + 1) / (double)(n + 1);
		double q = -log(-log(p));
		sx += q;
		sy += bm[i];
		sxx += q * q;
		sxy += q * bm[i];
	}
	double denom = (double)n * sxx - sx * sx;
	if (denom <= 0.0)
		return;
	*out_sigma = ((double)n * sxy - sx * sy) / denom;
	*out_mu = (sy - *out_sigma * sx) / (double)n;

	/* Coefficient of determination R^2 = 1 - SS_res / SS_tot.
	 * Gumbel data fits a straight line on the QQ plot; a low
	 * R^2 is evidence the distribution is not Gumbel and the
	 * caller routes to NON_GUMBEL. */
	sy_mean = sy / (double)n;
	for (i = 0; i < n; i++) {
		double p = (double)(i + 1) / (double)(n + 1);
		double q = -log(-log(p));
		double y_pred = *out_mu + *out_sigma * q;
		double res = bm[i] - y_pred;
		double tot = bm[i] - sy_mean;
		ss_res += res * res;
		ss_tot += tot * tot;
	}
	*out_r2 = (ss_tot > 0.0) ? 1.0 - (ss_res / ss_tot) : 0.0;
}

/* Exponential-tail (ET) test on the block-maxima series.
 * Cucu-Grosjean 2012 §II-A gates the Gumbel fit on an ET test that
 * decides whether the GEV shape parameter k is consistent with 0
 * (Gumbel sub-family) or significantly non-zero (Frechet for k > 0,
 * reversed Weibull for k < 0). This implementation uses the
 * Hosking & Wallis (1985) probability-weighted-moment (PWM)
 * estimator for k, which is the standard reference in regional-
 * frequency analysis and the form Gomes & Pestana cite:
 *
 *     b_r = (1/n) * sum_{i=1..n} ( prod_{j=1..r} (i - j) /
 *                                  prod_{j=1..r} (n - j) ) * M_(i)
 *
 *   tau   = (b_2 - b_1) / (b_1 - b_0)            (L-skewness)
 *   c     = 2 / (3 + tau) - ln(2) / ln(3)
 *   k_hat = 7.8590 * c + 2.9554 * c^2            (Hosking 1985 eq. 8)
 *
 * Under H_0: k = 0, k_hat is asymptotically N(0, 0.5633 / n)
 * (Hosking & Wallis 1985, Table 2), so the two-sided p-value is
 *   z   = k_hat * sqrt(n / 0.5633)
 *   p   = erfc(|z| / sqrt(2))
 *
 * Returns the p-value and writes the shape estimate via the
 * out parameter. Degenerate input (n < 8, denominators that
 * vanish) returns p=1.0 and k_hat=0.0 -- no evidence against H_0.
 *
 * Caller must pass a sorted ascending block-maxima series; the
 * Gumbel fit path already sorts it. */
static double et_test_pwm(const double *bm_sorted, uint32_t n,
		double *out_k_hat)
{
	double b0 = 0.0, b1 = 0.0, b2 = 0.0;
	double tau, c, k_hat, z;
	uint32_t i;

	*out_k_hat = 0.0;
	if (n < 8 || bm_sorted == NULL)
		return 1.0;

	for (i = 0; i < n; i++) {
		double x = bm_sorted[i];
		double w0 = 1.0 / (double)n;
		double w1, w2;
		if (n < 2)
			return 1.0;
		w1 = ((double)i) / ((double)n * (double)(n - 1));
		if (n < 3) {
			b0 += w0 * x;
			b1 += w1 * x;
			continue;
		}
		w2 = ((double)i * (double)(i - 1)) /
			((double)n * (double)(n - 1) * (double)(n - 2));
		b0 += w0 * x;
		b1 += w1 * x;
		b2 += w2 * x;
	}

	if ((b1 - b0) == 0.0)
		return 1.0;
	tau = (b2 - b1) / (b1 - b0);

	c = 2.0 / (3.0 + tau) - log(2.0) / log(3.0);
	k_hat = 7.8590 * c + 2.9554 * c * c;
	*out_k_hat = k_hat;

	z = k_hat * sqrt((double)n / 0.5633);
	if (z < 0.0) z = -z;
	return erfc(z / sqrt(2.0));
}

/* CRPS comparison between two sorted block-maxima series. We use
 * the discrete approximation
 *
 *   CRPS = sum_i (F_prev_bar(x_i) - F_curr_bar(x_i))^2
 *
 * evaluated at the union of abscissae from both series, with
 * F_bar(x) = 1 - F(x). Smaller values indicate the fit has
 * stabilised. */
static double crps_between(const double *prev, uint32_t np,
		const double *curr, uint32_t nc)
{
	uint32_t i, j;
	double sum = 0.0;

	if (np == 0 || nc == 0)
		return 0.0;

	for (i = 0; i < np; i++) {
		/* F_prev(prev[i]) = (i + 1) / (np + 1) */
		double fp = (double)(i + 1) / (double)(np + 1);
		double fc;
		/* Empirical F_curr at prev[i]: number of curr values
		 * <= prev[i], divided by nc. */
		uint32_t cnt = 0;
		for (j = 0; j < nc; j++)
			if (curr[j] <= prev[i])
				cnt++;
		fc = (double)cnt / (double)nc;
		double d = (1.0 - fp) - (1.0 - fc);
		sum += d * d;
	}
	/* Normalise by np so threshold values are comparable across
	 * runs with different block counts. */
	return sum / (double)np;
}

/* Re-evaluate the pipeline. Updates e->state, e->ks_stat,
 * e->runs_z, e->mu, e->sigma, e->crps, e->convergence_streak,
 * e->iid_reject_streak, and the pWCET cache. */
static void mbpta_step(mbpta_t *e)
{
	double *bm = NULL;
	uint32_t n_blocks;
	double mu = 0.0, sigma = 0.0;
	bool iid_ok, gumbel_ok = true;

	if (e->window_count < e->cfg.block_size * e->cfg.min_blocks) {
		e->state = MBPTA_INSUFFICIENT_DATA;
		return;
	}

	e->ks_stat = ks_statistic_two_sample(e);
	e->runs_z = runs_z(e);

	uint32_t m_half = e->window_count / 2;
	uint32_t n_half = e->window_count - m_half;
	e->ks_pvalue = ks_pvalue(e->ks_stat, m_half, n_half);
	e->runs_pvalue = runs_pvalue(e->runs_z);

	double ks_crit = ks_critical(e->cfg.alpha_iid, m_half, n_half);
	bool ks_ok = e->ks_stat <= ks_crit;
	bool runs_ok = (e->runs_z > -1.96 && e->runs_z < 1.96);
	iid_ok = ks_ok && runs_ok;

	if (!iid_ok) {
		e->iid_reject_streak++;
		if (e->state == MBPTA_PWCET_VALID) {
			if (e->iid_reject_streak >= e->cfg.n_iid_reject)
				e->state = MBPTA_DRIFT;
		} else if (e->state != MBPTA_DRIFT) {
			/* Once in DRIFT, stay there until i.i.d.
			 * recovers; transitioning back to IID_PENDING
			 * would erase the historical-diagnostics
			 * signal the plan specifically requires. */
			e->state = MBPTA_IID_PENDING;
		}
		e->convergence_streak = 0;
		return;
	}
	e->iid_reject_streak = 0;

	bm = calloc(e->cfg.sample_window / e->cfg.block_size,
			sizeof(*bm));
	if (bm == NULL)
		return;

	n_blocks = build_block_maxima(e, bm,
			e->cfg.sample_window / e->cfg.block_size);
	if (n_blocks < e->cfg.min_blocks) {
		e->state = MBPTA_INSUFFICIENT_DATA;
		free(bm);
		return;
	}

	double r2 = 0.0;
	gumbel_fit(bm, n_blocks, &mu, &sigma, &r2);
	if (sigma <= 0.0)
		gumbel_ok = false;
	if (r2 < e->cfg.gumbel_r2_threshold)
		gumbel_ok = false;

	/* gumbel_fit sorts bm in place; et_test_pwm needs a sorted
	 * ascending series, so the call can read the same buffer. */
	double k_hat = 0.0;
	e->et_pvalue = et_test_pwm(bm, n_blocks, &k_hat);
	e->gev_shape_k = k_hat;
	if (e->cfg.alpha_et > 0.0 && e->et_pvalue < e->cfg.alpha_et)
		gumbel_ok = false;

	if (!gumbel_ok) {
		e->state = MBPTA_NON_GUMBEL;
		e->convergence_streak = 0;
		free(bm);
		return;
	}

	e->mu = mu;
	e->sigma = sigma;

	/* CRPS against the previous fit's block-maxima 1-CDF. The
	 * very first valid fit has no previous; that round just
	 * stores the series and stays in PENDING_CONVERGENCE. */
	if (e->prev_bm != NULL && e->prev_bm_count > 0) {
		e->crps = crps_between(e->prev_bm, e->prev_bm_count,
				bm, n_blocks);
		if (e->crps <= e->cfg.crps_threshold) {
			e->convergence_streak++;
			if (e->convergence_streak >= e->cfg.n_conv) {
				double eps_eff = mbpta_effective_eps_value(
						e->cfg.eps_node);
				e->state = MBPTA_PWCET_VALID;
				e->pwcet_ns_cached = (uint64_t)(mu - sigma *
					log(-log(1.0 - eps_eff)));
			} else {
				e->state = MBPTA_PENDING_CONVERGENCE;
			}
		} else {
			e->convergence_streak = 0;
			e->state = MBPTA_PENDING_CONVERGENCE;
		}
	} else {
		e->state = MBPTA_PENDING_CONVERGENCE;
		e->convergence_streak = 0;
	}

	/* Cache the current block-maxima series for the next CRPS
	 * comparison. */
	free(e->prev_bm);
	e->prev_bm = bm;
	e->prev_bm_count = n_blocks;
}

bool mbpta_add_sample(mbpta_t *e, uint64_t sample_ns)
{
	if (e == NULL)
		return false;

	e->samples_total++;
	if (e->samples_total <= e->cfg.warmup_discard)
		return false;

	e->samples[e->head] = sample_ns;
	e->head = (e->head + 1) % e->cfg.sample_window;
	if (e->window_count < e->cfg.sample_window)
		e->window_count++;

	e->samples_since_eval++;
	if (e->samples_since_eval < e->cfg.n_delta)
		return false;

	e->samples_since_eval = 0;
	mbpta_step(e);
	return true;
}

enum mbpta_state mbpta_state(const mbpta_t *e)
{
	return e ? e->state : MBPTA_INSUFFICIENT_DATA;
}

uint32_t mbpta_sample_count(const mbpta_t *e)
{
	return e ? e->window_count : 0;
}

uint32_t mbpta_block_count(const mbpta_t *e)
{
	return e ? (e->window_count / e->cfg.block_size) : 0;
}

double mbpta_mu(const mbpta_t *e)        { return e ? e->mu : 0.0; }
double mbpta_sigma(const mbpta_t *e)     { return e ? e->sigma : 0.0; }
double mbpta_ks_stat(const mbpta_t *e)   { return e ? e->ks_stat : 0.0; }
double mbpta_ks_pvalue(const mbpta_t *e) { return e ? e->ks_pvalue : 1.0; }
double mbpta_runs_z(const mbpta_t *e)    { return e ? e->runs_z : 0.0; }
double mbpta_runs_pvalue(const mbpta_t *e) { return e ? e->runs_pvalue : 1.0; }
double mbpta_et_pvalue(const mbpta_t *e) { return e ? e->et_pvalue : 1.0; }
double mbpta_gev_shape_k(const mbpta_t *e) { return e ? e->gev_shape_k : 0.0; }
double mbpta_crps(const mbpta_t *e)      { return e ? e->crps : 0.0; }
uint32_t mbpta_convergence_streak(const mbpta_t *e)
{
	return e ? e->convergence_streak : 0;
}
uint32_t mbpta_iid_reject_streak(const mbpta_t *e)
{
	return e ? e->iid_reject_streak : 0;
}

uint64_t mbpta_pwcet_ns(const mbpta_t *e)
{
	if (e == NULL || e->state != MBPTA_PWCET_VALID)
		return 0;
	return e->pwcet_ns_cached;
}

double mbpta_effective_eps_node(const mbpta_t *e)
{
	if (e == NULL)
		return 0.0;
	return mbpta_effective_eps_value(e->cfg.eps_node);
}

bool mbpta_eps_node_capped(const mbpta_t *e)
{
	if (e == NULL)
		return false;
	return e->cfg.eps_node < MBPTA_EPS_NODE_FLOOR;
}
