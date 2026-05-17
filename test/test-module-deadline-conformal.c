/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 Antonio Napolitano and Francesco Barcherini */
/* SPDX-License-Identifier: MIT */

/*
 * Unit tests for module-deadline/conformal.c. This file pins the
 * configuration helpers and stable token tables first; the estimator
 * state machine, score-ring quantile and adaptive update are pinned
 * by additional PWTEST cases in subsequent commits as those code
 * paths land.
 */

#include "config.h"

#include <errno.h>
#include <stdint.h>

#include "pwtest.h"

#include "../src/modules/module-deadline/conformal.h"

PWTEST(conformal_state_name_stable)
{
	pwtest_str_eq(rt_conformal_state_name(RT_CONF_INSUFFICIENT_DATA),
			"insufficient_data");
	pwtest_str_eq(rt_conformal_state_name(RT_CONF_BOOTSTRAP),
			"bootstrap");
	pwtest_str_eq(rt_conformal_state_name(RT_CONF_VALID), "valid");
	pwtest_str_eq(rt_conformal_state_name(RT_CONF_SHIFT), "shift");
	pwtest_str_eq(rt_conformal_state_name(RT_CONF_DISABLED),
			"disabled");
	pwtest_str_eq(rt_conformal_state_name((enum rt_conformal_state)999),
			"unknown");
	return PWTEST_PASS;
}

PWTEST(conformal_invalidation_reason_name_stable)
{
	pwtest_str_eq(rt_conformal_invalidation_reason_name(
			RT_CONF_INVALIDATED_NONE), "none");
	pwtest_str_eq(rt_conformal_invalidation_reason_name(
			RT_CONF_INVALIDATED_PERIOD), "period");
	pwtest_str_eq(rt_conformal_invalidation_reason_name(
			RT_CONF_INVALIDATED_FUSION_GROUP), "fusion_group");
	pwtest_str_eq(rt_conformal_invalidation_reason_name(
			RT_CONF_INVALIDATED_TOPOLOGY_GENERATION),
			"topology_generation");
	pwtest_str_eq(rt_conformal_invalidation_reason_name(
			RT_CONF_INVALIDATED_CPU_CLASS), "cpu_class");
	pwtest_str_eq(rt_conformal_invalidation_reason_name(
			RT_CONF_INVALIDATED_OPERATOR_REQUEST),
			"operator_request");
	pwtest_str_eq(rt_conformal_invalidation_reason_name(
			RT_CONF_INVALIDATED_PLUGIN_MODE), "plugin_mode");
	pwtest_str_eq(rt_conformal_invalidation_reason_name(
			(enum rt_conformal_invalidation_reason)999), "unknown");
	return PWTEST_PASS;
}

PWTEST(conformal_risk_allocation_name_stable)
{
	pwtest_str_eq(rt_conformal_risk_allocation_name(
			RT_CONF_RISK_ALLOC_UNIFORM), "uniform");
	pwtest_str_eq(rt_conformal_risk_allocation_name(
			RT_CONF_RISK_ALLOC_DENSITY_WEIGHTED),
			"density_weighted");
	pwtest_str_eq(rt_conformal_risk_allocation_name(
			RT_CONF_RISK_ALLOC_SLOPE_WEIGHTED),
			"slope_weighted");
	pwtest_str_eq(rt_conformal_risk_allocation_name(
			(enum rt_conformal_risk_allocation)999), "unknown");
	return PWTEST_PASS;
}

PWTEST(conformal_config_defaults_validate)
{
	struct rt_conformal_config cfg;
	rt_conformal_config_defaults(&cfg);
	pwtest_int_eq(rt_conformal_config_validate(&cfg), 0);
	pwtest_bool_true(cfg.alpha_target > 0.0 && cfg.alpha_target < 1.0);
	pwtest_bool_true(cfg.alpha_min  > 0.0 && cfg.alpha_min  <= cfg.alpha_target);
	pwtest_bool_true(cfg.alpha_max  >= cfg.alpha_target && cfg.alpha_max < 1.0);
	pwtest_bool_true(cfg.window >= 2 && cfg.window <= RT_CONFORMAL_MAX_WINDOW);
	pwtest_bool_true(cfg.recalc_period >= 1);
	pwtest_int_eq((int)cfg.risk_allocation, (int)RT_CONF_RISK_ALLOC_UNIFORM);
	return PWTEST_PASS;
}

PWTEST(conformal_config_defaults_null_safe)
{
	rt_conformal_config_defaults(NULL);
	pwtest_int_eq(rt_conformal_config_validate(NULL), -EINVAL);
	return PWTEST_PASS;
}

PWTEST(conformal_config_validator_rejects_out_of_range)
{
	struct rt_conformal_config cfg;

	rt_conformal_config_defaults(&cfg);
	cfg.alpha_target = 0.0;
	pwtest_int_eq(rt_conformal_config_validate(&cfg), -EINVAL);

	rt_conformal_config_defaults(&cfg);
	cfg.alpha_target = 1.0;
	pwtest_int_eq(rt_conformal_config_validate(&cfg), -EINVAL);

	rt_conformal_config_defaults(&cfg);
	cfg.alpha_min = cfg.alpha_target * 2.0;
	pwtest_int_eq(rt_conformal_config_validate(&cfg), -EINVAL);

	rt_conformal_config_defaults(&cfg);
	cfg.alpha_max = cfg.alpha_target / 2.0;
	pwtest_int_eq(rt_conformal_config_validate(&cfg), -EINVAL);

	rt_conformal_config_defaults(&cfg);
	cfg.eta = 0.0;
	pwtest_int_eq(rt_conformal_config_validate(&cfg), -EINVAL);

	rt_conformal_config_defaults(&cfg);
	cfg.eta = 1.5;
	pwtest_int_eq(rt_conformal_config_validate(&cfg), -EINVAL);

	rt_conformal_config_defaults(&cfg);
	cfg.ewma_location_lambda = 0.0;
	pwtest_int_eq(rt_conformal_config_validate(&cfg), -EINVAL);

	rt_conformal_config_defaults(&cfg);
	cfg.ewma_scale_lambda = 1.0;
	pwtest_int_eq(rt_conformal_config_validate(&cfg), -EINVAL);

	rt_conformal_config_defaults(&cfg);
	cfg.window = 1;
	pwtest_int_eq(rt_conformal_config_validate(&cfg), -EINVAL);

	rt_conformal_config_defaults(&cfg);
	cfg.window = RT_CONFORMAL_MAX_WINDOW + 1;
	pwtest_int_eq(rt_conformal_config_validate(&cfg), -EINVAL);

	rt_conformal_config_defaults(&cfg);
	cfg.bootstrap_min_samples = 1;
	pwtest_int_eq(rt_conformal_config_validate(&cfg), -EINVAL);

	rt_conformal_config_defaults(&cfg);
	cfg.guard_percent = 1.0;
	pwtest_int_eq(rt_conformal_config_validate(&cfg), -EINVAL);

	rt_conformal_config_defaults(&cfg);
	cfg.guard_percent = -0.01;
	pwtest_int_eq(rt_conformal_config_validate(&cfg), -EINVAL);

	rt_conformal_config_defaults(&cfg);
	cfg.recalc_period = 0;
	pwtest_int_eq(rt_conformal_config_validate(&cfg), -EINVAL);

	rt_conformal_config_defaults(&cfg);
	cfg.risk_allocation = (enum rt_conformal_risk_allocation)42;
	pwtest_int_eq(rt_conformal_config_validate(&cfg), -EINVAL);

	return PWTEST_PASS;
}

PWTEST_SUITE(module_deadline_conformal)
{
	pwtest_add(conformal_state_name_stable, PWTEST_NOARG);
	pwtest_add(conformal_invalidation_reason_name_stable, PWTEST_NOARG);
	pwtest_add(conformal_risk_allocation_name_stable, PWTEST_NOARG);
	pwtest_add(conformal_config_defaults_validate, PWTEST_NOARG);
	pwtest_add(conformal_config_defaults_null_safe, PWTEST_NOARG);
	pwtest_add(conformal_config_validator_rejects_out_of_range,
			PWTEST_NOARG);

	return PWTEST_PASS;
}
