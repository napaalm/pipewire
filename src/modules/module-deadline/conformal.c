/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 Antonio Napolitano and Francesco Barcherini */
/* SPDX-License-Identifier: MIT */

/*
 * conformal.c -- skeleton for the adaptive-conformal runtime budget
 * estimator. This translation unit currently carries only the
 * configuration helpers (defaults + validator) and the stable token
 * tables. The estimator state machine, score ring, EWMA base
 * predictor and budget-emission paths land in subsequent commits.
 *
 * Copyright (C) 2026 Antonio Napolitano and Francesco Barcherini
 */

#include "conformal.h"

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

void rt_conformal_config_defaults(struct rt_conformal_config *cfg)
{
	if (cfg == NULL)
		return;
	memset(cfg, 0, sizeof(*cfg));
	cfg->alpha_target          = 1e-3;
	cfg->alpha_min             = 1e-5;
	cfg->alpha_max             = 5e-2;
	cfg->eta                   = 0.005;
	cfg->ewma_location_lambda  = 0.05;
	cfg->ewma_scale_lambda     = 0.05;
	cfg->window                = 1024;
	cfg->bootstrap_min_samples = 64;
	cfg->bootstrap_runtime_ns  = 0;
	cfg->guard_ns              = 1500;
	cfg->guard_percent         = 0.05;
	cfg->sigma_floor_ns        = 1;
	cfg->runtime_floor_ns      = 1000;
	cfg->recalc_period         = 1;
	cfg->compatible_history    = true;
	cfg->risk_allocation       = RT_CONF_RISK_ALLOC_UNIFORM;
	cfg->max_update_cost_ns    = 5000;
	cfg->trace_export          = false;
}

int rt_conformal_config_validate(const struct rt_conformal_config *cfg)
{
	if (cfg == NULL)
		return -EINVAL;
	if (!(cfg->alpha_target > 0.0 && cfg->alpha_target < 1.0))
		return -EINVAL;
	if (!(cfg->alpha_min > 0.0 && cfg->alpha_min <= cfg->alpha_target))
		return -EINVAL;
	if (!(cfg->alpha_max >= cfg->alpha_target && cfg->alpha_max < 1.0))
		return -EINVAL;
	if (!(cfg->eta > 0.0 && cfg->eta < 1.0))
		return -EINVAL;
	if (!(cfg->ewma_location_lambda > 0.0 && cfg->ewma_location_lambda < 1.0))
		return -EINVAL;
	if (!(cfg->ewma_scale_lambda > 0.0 && cfg->ewma_scale_lambda < 1.0))
		return -EINVAL;
	if (cfg->window < 2 || cfg->window > RT_CONFORMAL_MAX_WINDOW)
		return -EINVAL;
	if (cfg->bootstrap_min_samples < 2)
		return -EINVAL;
	if (!(cfg->guard_percent >= 0.0 && cfg->guard_percent < 1.0))
		return -EINVAL;
	if (cfg->recalc_period < 1)
		return -EINVAL;
	if (cfg->risk_allocation != RT_CONF_RISK_ALLOC_UNIFORM &&
	    cfg->risk_allocation != RT_CONF_RISK_ALLOC_DENSITY_WEIGHTED &&
	    cfg->risk_allocation != RT_CONF_RISK_ALLOC_SLOPE_WEIGHTED)
		return -EINVAL;
	if (!isfinite(cfg->alpha_target) || !isfinite(cfg->alpha_min) ||
	    !isfinite(cfg->alpha_max)    || !isfinite(cfg->eta) ||
	    !isfinite(cfg->ewma_location_lambda) ||
	    !isfinite(cfg->ewma_scale_lambda) ||
	    !isfinite(cfg->guard_percent))
		return -EINVAL;
	return 0;
}

const char *rt_conformal_state_name(enum rt_conformal_state s)
{
	switch (s) {
	case RT_CONF_INSUFFICIENT_DATA: return "insufficient_data";
	case RT_CONF_BOOTSTRAP:         return "bootstrap";
	case RT_CONF_VALID:             return "valid";
	case RT_CONF_SHIFT:             return "shift";
	case RT_CONF_DISABLED:          return "disabled";
	}
	return "unknown";
}

const char *rt_conformal_invalidation_reason_name(
		enum rt_conformal_invalidation_reason r)
{
	switch (r) {
	case RT_CONF_INVALIDATED_NONE:                return "none";
	case RT_CONF_INVALIDATED_PERIOD:              return "period";
	case RT_CONF_INVALIDATED_FUSION_GROUP:        return "fusion_group";
	case RT_CONF_INVALIDATED_TOPOLOGY_GENERATION: return "topology_generation";
	case RT_CONF_INVALIDATED_CPU_CLASS:           return "cpu_class";
	case RT_CONF_INVALIDATED_OPERATOR_REQUEST:    return "operator_request";
	case RT_CONF_INVALIDATED_PLUGIN_MODE:         return "plugin_mode";
	}
	return "unknown";
}

const char *rt_conformal_risk_allocation_name(
		enum rt_conformal_risk_allocation a)
{
	switch (a) {
	case RT_CONF_RISK_ALLOC_UNIFORM:          return "uniform";
	case RT_CONF_RISK_ALLOC_DENSITY_WEIGHTED: return "density_weighted";
	case RT_CONF_RISK_ALLOC_SLOPE_WEIGHTED:   return "slope_weighted";
	}
	return "unknown";
}
