/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <string.h>
#include <stdio.h>
#include <dlfcn.h>

#include <spa/node/node.h>
#include <spa/node/utils.h>
#include <spa/utils/cleanup.h>
#include <spa/utils/result.h>
#include <spa/param/props.h>
#include <spa/pod/iter.h>
#include <spa/debug/types.h>

#include "spa-node.h"

struct impl {
	struct pw_impl_node *this;

	enum pw_spa_node_flags flags;

	struct spa_handle *handle;
	struct spa_node *node;          /**< handle to SPA node */
	struct pw_loop *spa_data_loop;

	struct spa_hook node_listener;
	int init_pending;

	void *user_data;

	unsigned int async_init:1;
};

static void spa_node_free(void *data)
{
	struct impl *impl = data;
	struct pw_impl_node *node = impl->this;

	pw_log_debug("spa-node %p: free", node);

	spa_hook_remove(&impl->node_listener);
	if (impl->handle)
		pw_unload_spa_handle(impl->handle);
	if (impl->spa_data_loop) {
		pw_context_release_node_loop(
				pw_impl_node_get_context(node),
				impl->spa_data_loop);
		impl->spa_data_loop = NULL;
	}
}

static void complete_init(struct impl *impl)
{
        struct pw_impl_node *this = impl->this;

	impl->init_pending = SPA_ID_INVALID;

	if (SPA_FLAG_IS_SET(impl->flags, PW_SPA_NODE_FLAG_ACTIVATE))
		pw_impl_node_set_active(this, true);

	if (!SPA_FLAG_IS_SET(impl->flags, PW_SPA_NODE_FLAG_NO_REGISTER))
		pw_impl_node_register(this, NULL);
	else
		pw_impl_node_initialized(this);
}

static void spa_node_result(void *data, int seq, int res, uint32_t type, const void *result)
{
	struct impl *impl = data;
	struct pw_impl_node *node = impl->this;

	if (seq == impl->init_pending) {
		pw_log_debug("spa-node %p: init complete event %d %d", node, seq, res);
		complete_init(impl);
	}
}

static const struct pw_impl_node_events node_events = {
	PW_VERSION_IMPL_NODE_EVENTS,
	.free = spa_node_free,
	.result = spa_node_result,
};

struct pw_impl_node *
pw_spa_node_new(struct pw_context *context,
		enum pw_spa_node_flags flags,
		struct spa_node *node,
		struct spa_handle *handle,
		struct pw_properties *properties,
		size_t user_data_size)
{
	struct pw_impl_node *this;
	struct impl *impl;
	int res;

	this = pw_context_create_node(context, properties, sizeof(struct impl) + user_data_size);
	if (this == NULL) {
		res = -errno;
		goto error_exit;
	}

	impl = pw_impl_node_get_user_data(this);
	impl->this = this;
	impl->node = node;
	impl->handle = handle;
	impl->flags = flags;

	if (user_data_size > 0)
                impl->user_data = SPA_PTROFF(impl, sizeof(struct impl), void);

	pw_impl_node_add_listener(this, &impl->node_listener, &node_events, impl);
	if ((res = pw_impl_node_set_implementation(this, impl->node)) < 0)
		goto error_exit_clean_node;

	if (flags & PW_SPA_NODE_FLAG_ASYNC) {
		impl->init_pending = spa_node_sync(impl->node, res);
	} else {
		complete_init(impl);
	}
	return this;

error_exit_clean_node:
	pw_impl_node_destroy(this);
	handle = NULL;
error_exit:
	if (handle)
		pw_unload_spa_handle(handle);
	errno = -res;
	return NULL;

}

void *pw_spa_node_get_user_data(struct pw_impl_node *node)
{
	struct impl *impl = pw_impl_node_get_user_data(node);
	return impl->user_data;
}

struct match {
	struct pw_properties *props;
	int count;
};
#define MATCH_INIT(p) ((struct match){ .props = (p) })

static int execute_match(void *data, const char *location, const char *action,
		const char *val, size_t len)
{
	struct match *match = data;
	if (spa_streq(action, "update-props")) {
		match->count += pw_properties_update_string(match->props, val, len);
	}
	return 1;
}


struct pw_impl_node *pw_spa_node_load(struct pw_context *context,
				 const char *factory_name,
				 enum pw_spa_node_flags flags,
				 struct pw_properties *properties,
				 size_t user_data_size)
{
	struct pw_impl_node *this;
	struct spa_node *spa_node;
	int res;
	struct spa_handle *handle;
	void *iface;
	const struct pw_properties *p;
	struct pw_loop *loop;
	struct match match;

	if (properties == NULL) {
		properties = pw_properties_new(NULL, NULL);
		if (properties == NULL)
			return NULL;
	}
	p = pw_context_get_properties(context);
	pw_properties_set(properties, "clock.quantum-limit",
			pw_properties_get(p, "default.clock.quantum-limit"));

	match = MATCH_INIT(properties);
	pw_context_conf_section_match_rules(context, "node.rules",
			&properties->dict, execute_match, &match);

	/* Acquire the loop the node will actually run on.  This must be
	 * the same loop we pass to the SPA handle as DataLoop support,
	 * because the SPA plugin (ALSA, AVB, Bluetooth, audiotestsrc, ...)
	 * registers its own timer/hardware fd sources on that loop.  If
	 * the node's eventfd ends up on a different loop, process_node
	 * becomes reachable from two threads -- a data race that deadlocks
	 * the daemon.
	 *
	 * pw_context_acquire_node_loop returns a dynamic-pool loop when
	 * context.dynamic-data-loops is active, or a static-pool loop
	 * otherwise.  We stamp a private co-location group so that the
	 * second acquire inside pw_context_create_node finds the SAME loop
	 * instance (ref++) rather than creating another one. */
	if (pw_properties_get(properties, PW_KEY_NODE_LOOP_GROUP) == NULL) {
		char group_buf[48];
		snprintf(group_buf, sizeof(group_buf), "_spa_node.%p",
				(void *)properties);
		pw_properties_set(properties, PW_KEY_NODE_LOOP_GROUP,
				group_buf);
	}

	loop = pw_context_acquire_node_loop(context, properties, false);
	if (loop == NULL) {
		res = -errno;
		goto error_exit;
	}

	pw_properties_set(properties, PW_KEY_NODE_LOOP_NAME, loop->name);

	{
		const char *lib;
		const struct spa_support *base_support;
		struct spa_support support[18];
		uint32_t n_support;

		lib = pw_context_find_spa_lib(context, factory_name);
		if (lib == NULL)
			lib = spa_dict_lookup(&properties->dict,
					SPA_KEY_LIBRARY_NAME);
		if (lib == NULL) {
			pw_context_release_node_loop(context, loop);
			res = -ENOENT;
			pw_log_warn("no library for %s: %m", factory_name);
			goto error_exit;
		}

		base_support = pw_context_get_support(context, &n_support);
		if (n_support > 18)
			n_support = 18;
		memcpy(support, base_support, n_support * sizeof(*support));

		/* pw_context_get_support appends DataSystem + DataLoop
		 * from the static pool as the last two entries.  Replace
		 * them with the node's actual loop so the SPA plugin's
		 * sources land on the same thread as the node's eventfd. */
		if (n_support >= 2) {
			support[n_support - 2] = SPA_SUPPORT_INIT(
					SPA_TYPE_INTERFACE_DataSystem,
					loop->system);
			support[n_support - 1] = SPA_SUPPORT_INIT(
					SPA_TYPE_INTERFACE_DataLoop,
					loop->loop);
		}

		handle = pw_load_spa_handle(lib, factory_name,
				&properties->dict, n_support, support);
	}
	if (handle == NULL) {
		pw_context_release_node_loop(context, loop);
		res = -errno;
		goto error_exit;
	}

	if ((res = spa_handle_get_interface(handle, SPA_TYPE_INTERFACE_Node, &iface)) < 0) {
		pw_log_error("can't get node interface %d", res);
		pw_unload_spa_handle(handle);
		pw_context_release_node_loop(context, loop);
		goto error_exit;
	}
	if (SPA_RESULT_IS_ASYNC(res))
		flags |= PW_SPA_NODE_FLAG_ASYNC;

	spa_node = iface;

	this = pw_spa_node_new(context, flags,
			       spa_node, handle, spa_steal_ptr(properties), user_data_size);
	if (this == NULL) {
		res = -errno;
		pw_unload_spa_handle(handle);
		pw_context_release_node_loop(context, loop);
		goto error_exit;
	}

	{
		struct impl *impl = pw_impl_node_get_user_data(this);
		impl->spa_data_loop = loop;
	}
	return this;

error_exit:
	pw_properties_free(properties);
	errno = -res;
	return NULL;

}
