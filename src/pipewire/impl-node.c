/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <malloc.h>
#include <limits.h>
#include <sys/syscall.h>
#if defined(__FreeBSD__) || defined(__MidnightBSD__)
#include <sys/thr.h>
#endif
#if defined(__GNU__)
#include <hurd.h>
#endif
#if defined(__linux__)
#endif

#include <spa/support/system.h>
#include <spa/pod/parser.h>
#include <spa/pod/filter.h>
#include <spa/pod/dynamic.h>
#include <spa/node/utils.h>
#include <spa/debug/types.h>
#include <spa/utils/string.h>
#include <spa/utils/json-pod.h>

#define PW_API_NODE_IMPL	SPA_EXPORT
#include "pipewire/cycle-counter.h"
#include "pipewire/impl-node.h"
#include "pipewire/private.h"

PW_LOG_TOPIC_EXTERN(log_node);
#define PW_LOG_TOPIC_DEFAULT log_node

#define DEFAULT_SYNC_TIMEOUT  ((uint64_t)(5 * SPA_NSEC_PER_SEC))

/** \cond */
struct impl {
	struct pw_impl_node this;

	enum pw_node_state pending_state;
	uint32_t pending_id;

	struct pw_work_queue *work;

	int last_error;

	struct spa_list param_list;
	struct spa_list pending_list;

	unsigned int cache_params:1;
	unsigned int pending_play:1;

	struct spa_command *pending_request_process;

	char *group;
	char *link_group;
	char *sync_group;
};

static const char * const global_keys[] = {
	PW_KEY_OBJECT_PATH,
	PW_KEY_MODULE_ID,
	PW_KEY_FACTORY_ID,
	PW_KEY_CLIENT_ID,
	PW_KEY_CLIENT_API,
	PW_KEY_DEVICE_ID,
	PW_KEY_PRIORITY_SESSION,
	PW_KEY_PRIORITY_DRIVER,
	PW_KEY_APP_NAME,
	PW_KEY_NODE_DESCRIPTION,
	PW_KEY_NODE_NAME,
	PW_KEY_NODE_NICK,
	PW_KEY_NODE_REMOTE,
	PW_KEY_NODE_SESSION,
	PW_KEY_NODE_LOOP_TID,
	PW_KEY_NODE_LOOP_DYNAMIC,
	PW_KEY_MEDIA_CLASS,
	PW_KEY_MEDIA_TYPE,
	PW_KEY_MEDIA_CATEGORY,
	PW_KEY_MEDIA_ROLE,
	NULL
};

#define pw_node_resource(r,m,v,...)	pw_resource_call(r,struct pw_node_events,m,v,__VA_ARGS__)
#define pw_node_resource_info(r,...)	pw_node_resource(r,info,0,__VA_ARGS__)
#define pw_node_resource_param(r,...)	pw_node_resource(r,param,0,__VA_ARGS__)

struct resource_data {
	struct pw_impl_node *node;

	struct pw_resource *resource;
	struct spa_hook resource_listener;
	struct spa_hook object_listener;

	uint32_t subscribe_ids[MAX_PARAMS];
	uint32_t n_subscribe_ids;

	/* for async replies */
	int seq;
	int end;
	struct spa_hook listener;
};

static pid_t _gettid(void)
{
#if defined(HAVE_GETTID)
	return (pid_t) gettid();
#elif defined(__linux__)
	return syscall(SYS_gettid);
#elif defined(__FreeBSD__) || defined(__MidnightBSD__)
	long tid;
	thr_self(&tid);
	return (pid_t)tid;
#elif defined(__GNU__)
       mach_port_t thread = hurd_thread_self();
       return (pid_t)thread;
#else
#error "No gettid impl"
#endif
}

SPA_EXPORT
struct pw_node_peer *pw_node_peer_ref(struct pw_impl_node *onode, struct pw_impl_node *inode)
{
	struct pw_node_peer *peer;

	spa_list_for_each(peer, &onode->peer_list, link) {
		if (peer->target.id == inode->info.id) {
			pw_log_debug("exiting peer %p from %p to %p", peer, onode, inode);
			peer->ref++;
			return peer;
		}
	}
	peer = calloc(1, sizeof(*peer));
	if (peer == NULL)
		return NULL;

	peer->ref = 1;
	peer->output = onode;
	copy_target(&peer->target, &inode->rt.target);
	/* Arm the same-loop fast path in trigger_target_v1 when the
	 * operator left context.inline-dispatch at its default of true.
	 * We stamp the producer's data-loop system onto the peer's
	 * target so the trigger function can do a single pointer
	 * compare against peer->target.system (= the consumer's
	 * system) to decide between the eventfd path and an inline
	 * process_node call.
	 *
	 * The remote/exported/driver opt-out below covers the cases
	 * where calling process_node on the consumer here would not
	 * actually run the consumer's audio code:
	 *
	 *   - remote: the daemon-side proxy of a node whose real
	 *     process callback runs in another pw_context. The local
	 *     spa_node bound to it is a protocol stub that just
	 *     re-writes the eventfd; inlining it saves nothing.
	 *   - exported: locally-owned but its process callback is
	 *     served through the client-node protocol. Same proxy-
	 *     vs-real-node hazard.
	 *   - driver: drivers are never fused with their followers
	 *     and process_node on a driver finalises the cycle via
	 *     the else branch that emits rt_complete; inlining would
	 *     skip wake-back plumbing downstream relies on.
	 *
	 * Leaving src_system at NULL is always safe -- it keeps
	 * trigger_target_v1 on the eventfd-write path. */
	if (pw_properties_get_bool(onode->context->properties,
				"context.inline-dispatch", true) &&
	    onode->data_loop != NULL &&
	    !onode->remote && !onode->exported && !onode->driver &&
	    !inode->remote && !inode->exported && !inode->driver) {
		peer->target.src_system = onode->data_loop->system;
		pw_log_debug("peer %p (%u->%u): inline-dispatch armed "
				"(src_system=%p loop=%s)",
				peer, onode->info.id, inode->info.id,
				peer->target.src_system,
				onode->data_loop->name);
	} else {
		pw_log_debug("peer %p (%u->%u): inline-dispatch disarmed "
				"(remote o:%d i:%d exported o:%d i:%d "
				"driver o:%d i:%d)",
				peer, onode->info.id, inode->info.id,
				onode->remote, inode->remote,
				onode->exported, inode->exported,
				onode->driver, inode->driver);
	}

	spa_list_append(&onode->peer_list, &peer->link);
	pw_log_debug("new peer %p from %p to %p", peer, onode, inode);
	pw_impl_node_add_target(onode, &peer->target);

	return peer;
}

SPA_EXPORT
void pw_node_peer_unref(struct pw_node_peer *peer)
{
	if (peer == NULL || --peer->ref > 0)
		return;
	spa_list_remove(&peer->link);
	pw_log_debug("remove peer %p from %p to %p", peer, peer->output, peer->target.node);
	pw_impl_node_remove_target(peer->output, &peer->target);
	free(peer);
}

static inline void activate_target(struct pw_impl_node *node, struct pw_node_target *t)
{
	struct pw_node_activation_state *state = &t->activation->state[0];
	if (!t->active) {
		if (!node->async) {
			if (!node->exported) {
				SPA_ATOMIC_INC(state->required);
				SPA_ATOMIC_INC(state->pending);
			}
		}
		t->active = true;
		pw_log_debug("%p: target state:%p id:%d pending:%d/%d %d:%d:%d",
				node, state, t->id, state->pending, state->required,
				node->async, node->driving, node->exported);
	}
}

static inline void deactivate_target(struct pw_impl_node *node, struct pw_node_target *t, uint64_t trigger)
{
	if (t->active) {
		struct pw_node_activation_state *state = &t->activation->state[0];
		if (!node->async) {
			/* the driver copies the required to the pending state
			 * so first try to resume the node and then decrement the
			 * required state. This way we either resume with the old value
			 * or we don't when the driver has not yet copied */
			if (trigger != 0)
				t->trigger(t, trigger);
			if (!node->exported)
				SPA_ATOMIC_DEC(state->required);
		}
		t->active = false;
		pw_log_debug("%p: target state:%p id:%d pending:%d/%d %d:%d:%d trigger:%"PRIu64,
				node, state, t->id, state->pending, state->required,
				node->async, node->driving, node->exported, trigger);
	}
}

/* called from data-loop when all the targets of a node need to be triggered */
static inline void trigger_targets(struct pw_impl_node *node, int status, uint64_t nsec)
{
	struct pw_node_target *ta;

	pw_log_trace_fp("%p: (%s-%u) trigger targets %"PRIu64,
			node, node->name, node->info.id, nsec);

	spa_list_for_each(ta, &node->rt.target_list, link)
		ta->trigger(ta, nsec);
}

/** \endcond */

/* Called from the node data loop when a node needs to be scheduled by
 * the given driver.
 *
 * - the node adds the source to the data loop
 * - from all the targets of the node, we can now be scheduled ourselves
 *   so activate our target.
 * - When we get scheduled, we will activate our peer targets
 */
static int
do_node_prepare(struct spa_loop *loop, bool async, uint32_t seq,
		const void *data, size_t size, void *user_data)
{
	struct pw_impl_node *this = user_data;
	struct pw_node_target *t;
	uint64_t dummy;
	int res;

	pw_log_trace("%p: prepare %d remote:%d exported:%d", this, this->rt.prepared,
			this->remote, this->exported);

	if (this->rt.prepared)
		return 0;

	if (!this->remote) {
		/* clear the eventfd in case it was written to while the node was stopped */
		res = spa_system_eventfd_read(this->rt.target.system, this->source.fd, &dummy);
		if (SPA_UNLIKELY(res != -EAGAIN && res != 0))
			pw_log_warn("%p: read failed %m", this);

		spa_loop_add_source(loop, &this->source);
	}
	if (!this->remote || this->rt.target.activation->client_version < 1)
		SPA_ATOMIC_STORE(this->rt.target.activation->status, PW_NODE_ACTIVATION_FINISHED);

	spa_list_for_each(t, &this->rt.target_list, link)
		activate_target(this, t);

	this->rt.prepared = true;

	return 0;
}

static void add_node_to_graph(struct pw_impl_node *node)
{
	pw_loop_locked(node->data_loop, do_node_prepare, 1, NULL, 0, node);
}

/* called from the node data loop and undoes the changes done in do_node_prepare.  */
static int
do_node_unprepare(struct spa_loop *loop, bool async, uint32_t seq,
		const void *data, size_t size, void *user_data)
{
	struct pw_impl_node *this = user_data;
	struct pw_node_target *t;
	int old_state;
	uint64_t trigger = 0;

	pw_log_trace("%p: unprepare %d remote:%d exported:%d", this, this->rt.prepared,
			this->remote, this->exported);

	if (!this->rt.prepared)
		return 0;

	/* The remote client will INACTIVE itself and remove itself from the loop to avoid
	 * being scheduled.
	 * The server will mark remote nodes as FINISHED and trigger the peers. This will
	 * make sure the remote node will not trigger the peers anymore when it will
	 * stop (it only triggers peers when it has PENDING_TRIGGER (<= AWAKE)). We have
	 * to trigger the peers on the server because the client might simply be dead and
	 * not able to trigger anything.
	 */
	old_state = SPA_ATOMIC_XCHG(this->rt.target.activation->status, PW_NODE_ACTIVATION_INACTIVE);
	if (PW_NODE_ACTIVATION_PENDING_TRIGGER(old_state))
		trigger = get_time_ns(this->rt.target.system);

	if (!this->remote)
		spa_loop_remove_source(loop, &this->source);

	spa_list_for_each(t, &this->rt.target_list, link)
		deactivate_target(this, t, trigger);

	this->rt.prepared = false;
	return 0;
}

static void remove_node_from_graph(struct pw_impl_node *node)
{
	pw_loop_locked(node->data_loop, do_node_unprepare, 1, NULL, 0, node);
}

static void node_deactivate(struct pw_impl_node *this)
{
	struct pw_impl_port *port;
	struct pw_impl_link *link;

	pw_log_debug("%p: deactivate", this);

	/* make sure the node doesn't get woken up while not active */
	remove_node_from_graph(this);

	spa_list_for_each(port, &this->input_ports, link) {
		spa_list_for_each(link, &port->links, input_link)
			pw_impl_link_deactivate(link);
	}
	spa_list_for_each(port, &this->output_ports, link) {
		spa_list_for_each(link, &port->links, output_link)
			pw_impl_link_deactivate(link);
	}
}

static int idle_node(struct pw_impl_node *this)
{
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);
	int res = 0;

	pw_log_debug("%p: idle node state:%s pending:%s pause-on-idle:%d", this,
			pw_node_state_as_string(this->info.state),
			pw_node_state_as_string(impl->pending_state),
			this->pause_on_idle);

	if (impl->pending_state <= PW_NODE_STATE_IDLE)
		return 0;

	if (!this->pause_on_idle)
		return 0;

	node_deactivate(this);

	res = spa_node_send_command(this->node,
				    &SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Pause));
	if (res < 0)
		pw_log_debug("%p: pause node error %s", this, spa_strerror(res));

	return res;
}

static void node_activate(struct pw_impl_node *this)
{
	struct pw_impl_port *port;

	pw_log_debug("%p: activate", this);
	spa_list_for_each(port, &this->output_ports, link) {
		struct pw_impl_link *link;
		spa_list_for_each(link, &port->links, output_link)
			pw_impl_link_activate(link);
	}
	spa_list_for_each(port, &this->input_ports, link) {
		struct pw_impl_link *link;
		spa_list_for_each(link, &port->links, input_link)
			pw_impl_link_activate(link);
	}
}

static int start_node(struct pw_impl_node *this)
{
	struct impl *impl = SPA_CONTAINER_OF(this, struct impl, this);
	int res = 0;

	node_activate(this);

	if (impl->pending_state >= PW_NODE_STATE_RUNNING)
		return 0;

	pw_log_debug("%p: start node driving:%d driver:%d prepared:%d", this,
			this->driving, this->driver, this->rt.prepared);

	this->lazy = this->rt.position && SPA_FLAG_IS_SET(this->rt.position->clock.flags,
			SPA_IO_CLOCK_FLAG_LAZY);

	if (!(this->driving && this->driver)) {
		impl->pending_play = true;
		res = spa_node_send_command(this->node,
			&SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Start));
	} else {
		/* driver nodes will wait until all other nodes are started before
		 * they are started */
		spa_clear_ptr(impl->pending_request_process, free);
		res = EBUSY;
	}

	if (res < 0)
		pw_log_error("(%s-%u) start node error %d: %s", this->name, this->info.id,
				res, spa_strerror(res));

	return res;
}

static void emit_info_changed(struct pw_impl_node *node, bool flags_changed)
{
	if (node->info.change_mask == 0 && !flags_changed)
		return;

	pw_impl_node_emit_info_changed(node, &node->info);

	if (node->global && node->info.change_mask != 0) {
		struct pw_resource *resource;
		if (node->info.change_mask & PW_NODE_CHANGE_MASK_PROPS)
			pw_global_update_keys(node->global, node->info.props, global_keys);
		spa_list_for_each(resource, &node->global->resource_list, link)
			pw_node_resource_info(resource, &node->info);
	}

	node->info.change_mask = 0;
}

static int resource_is_subscribed(struct pw_resource *resource, uint32_t id)
{
	struct resource_data *data = pw_resource_get_user_data(resource);
	uint32_t i;

	for (i = 0; i < data->n_subscribe_ids; i++) {
		if (data->subscribe_ids[i] == id)
			return 1;
	}
	return 0;
}

static int notify_param(void *data, int seq, uint32_t id,
		uint32_t index, uint32_t next, struct spa_pod *param)
{
	struct pw_impl_node *node = data;
	struct pw_resource *resource;

	spa_list_for_each(resource, &node->global->resource_list, link) {
		if (!resource_is_subscribed(resource, id))
			continue;

		pw_log_debug("%p: resource %p notify param %d", node, resource, id);
		pw_node_resource_param(resource, seq, id, index, next, param);
	}
	return 0;
}

static void emit_params(struct pw_impl_node *node, uint32_t *changed_ids, uint32_t n_changed_ids)
{
	uint32_t i;
	int res;

	if (node->global == NULL)
		return;

	pw_log_debug("%p: emit %d params", node, n_changed_ids);

	for (i = 0; i < n_changed_ids; i++) {
		struct pw_resource *resource;
		int subscribed = 0;

		/* first check if anyone is subscribed */
		spa_list_for_each(resource, &node->global->resource_list, link) {
			if ((subscribed = resource_is_subscribed(resource, changed_ids[i])))
				break;
		}
		if (!subscribed)
			continue;

		if ((res = pw_impl_node_for_each_param(node, 1, changed_ids[i], 0, UINT32_MAX,
					NULL, notify_param, node)) < 0) {
			pw_log_error("%p: error %d (%s)", node, res, spa_strerror(res));
		}
	}
}

static void node_update_state(struct pw_impl_node *node, enum pw_node_state state, int res, char *error)
{
	struct impl *impl = SPA_CONTAINER_OF(node, struct impl, this);
	enum pw_node_state old = node->info.state;
	bool emit_pending_request_process = false;

	switch (state) {
	case PW_NODE_STATE_RUNNING:
		pw_log_debug("%p: start node driving:%d driver:%d prepared:%d", node,
				node->driving, node->driver, node->rt.prepared);

		if (res >= 0) {
			add_node_to_graph(node);
		}
		if (node->driving && node->driver) {
			res = spa_node_send_command(node->node,
				&SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Start));
			if (res < 0) {
				state = PW_NODE_STATE_ERROR;
				error = spa_aprintf("Start error: %s", spa_strerror(res));
				remove_node_from_graph(node);
			} else if (impl->pending_request_process != NULL) {
				emit_pending_request_process = true;
			}
		}
		break;
	case PW_NODE_STATE_IDLE:
	case PW_NODE_STATE_SUSPENDED:
	case PW_NODE_STATE_ERROR:
		if (state != PW_NODE_STATE_IDLE || node->pause_on_idle)
			if (old != PW_NODE_STATE_CREATING)
				remove_node_from_graph(node);
		break;
	default:
		break;
	}

	free((char*)node->info.error);
	node->info.error = error;
	node->info.state = state;
	impl->pending_state = state;

	pw_log_debug("%p: (%s) %s -> %s (%s)", node, node->name,
		     pw_node_state_as_string(old), pw_node_state_as_string(state), error);

	if (old == state)
		return;

	if (state == PW_NODE_STATE_ERROR) {
		pw_log_error("(%s-%u) %s -> error (%s)", node->name, node->info.id,
		     pw_node_state_as_string(old), error);
	} else {
		pw_log_info("(%s-%u) %s -> %s", node->name, node->info.id,
		     pw_node_state_as_string(old), pw_node_state_as_string(state));
	}
	pw_impl_node_emit_state_changed(node, old, state, error);

	if (emit_pending_request_process) {
		pw_log_debug("%p: request process:%p", node, impl->pending_request_process);
		spa_node_send_command(node->node, impl->pending_request_process);
		spa_clear_ptr(impl->pending_request_process, free);
	}

	node->info.change_mask |= PW_NODE_CHANGE_MASK_STATE;
	emit_info_changed(node, false);

	if (state == PW_NODE_STATE_ERROR && node->global) {
		struct pw_resource *resource;
		spa_list_for_each(resource, &node->global->resource_list, link)
			pw_resource_error(resource, res, error);
	}
	if (old == PW_NODE_STATE_RUNNING &&
	    state == PW_NODE_STATE_IDLE &&
	    node->suspend_on_idle) {
		pw_impl_node_set_state(node, PW_NODE_STATE_SUSPENDED);
	}
}

static int suspend_node(struct pw_impl_node *this)
{
	int res = 0;
	struct pw_impl_port *p;

	pw_log_debug("%p: suspend node state:%s", this,
			pw_node_state_as_string(this->info.state));

	if (this->info.state > 0 && this->info.state <= PW_NODE_STATE_SUSPENDED)
		return 0;

	node_deactivate(this);

	pw_log_debug("%p: suspend node driving:%d driver:%d prepared:%d", this,
			this->driving, this->driver, this->rt.prepared);

	res = spa_node_send_command(this->node,
				    &SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Suspend));
	if (res == -ENOTSUP)
		res = spa_node_send_command(this->node,
				    &SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Pause));
	if (res < 0 && res != -EIO)
		pw_log_warn("%p: suspend node error %s", this, spa_strerror(res));

	spa_list_for_each(p, &this->input_ports, link) {
		if ((res = pw_impl_port_set_param(p, SPA_PARAM_Format, 0, NULL)) < 0)
			pw_log_warn("%p: error unset format input: %s",
					this, spa_strerror(res));
		/* force CONFIGURE in case of async */
		p->state = PW_IMPL_PORT_STATE_CONFIGURE;
	}

	spa_list_for_each(p, &this->output_ports, link) {
		if ((res = pw_impl_port_set_param(p, SPA_PARAM_Format, 0, NULL)) < 0)
			pw_log_warn("%p: error unset format output: %s",
					this, spa_strerror(res));
		/* force CONFIGURE in case of async */
		p->state = PW_IMPL_PORT_STATE_CONFIGURE;
	}

	node_update_state(this, PW_NODE_STATE_SUSPENDED, 0, NULL);

	return res;
}

static void
clear_info(struct pw_impl_node *this)
{
	pw_free_strv(this->groups);
	pw_free_strv(this->link_groups);
	pw_free_strv(this->sync_groups);
	free(this->name);
	free((char*)this->info.error);
}

static int reply_param(void *data, int seq, uint32_t id,
		uint32_t index, uint32_t next, struct spa_pod *param)
{
	struct resource_data *d = data;
	pw_log_debug("%p: resource %p reply param %d", d->node, d->resource, seq);
	pw_node_resource_param(d->resource, seq, id, index, next, param);
	return 0;
}

static int node_enum_params(void *object, int seq, uint32_t id,
		uint32_t index, uint32_t num, const struct spa_pod *filter)
{
	struct resource_data *data = object;
	struct pw_resource *resource = data->resource;
	struct pw_impl_node *node = data->node;
	int res;

	pw_log_debug("%p: resource %p enum params seq:%d id:%d (%s) index:%u num:%u",
			node, resource, seq, id,
			spa_debug_type_find_name(spa_type_param, id), index, num);

	if ((res = pw_impl_node_for_each_param(node, seq, id, index, num,
				filter, reply_param, data)) < 0) {
		pw_resource_errorf(resource, res,
				"enum params id:%d (%s) failed", id,
				spa_debug_type_find_name(spa_type_param, id));
	}
	return 0;
}

static int node_subscribe_params(void *object, uint32_t *ids, uint32_t n_ids)
{
	struct resource_data *data = object;
	struct pw_resource *resource = data->resource;
	uint32_t i;

	n_ids = SPA_MIN(n_ids, SPA_N_ELEMENTS(data->subscribe_ids));
	data->n_subscribe_ids = n_ids;

	for (i = 0; i < n_ids; i++) {
		data->subscribe_ids[i] = ids[i];
		pw_log_debug("%p: resource %p subscribe param id:%d (%s)",
				data->node, resource, ids[i],
				spa_debug_type_find_name(spa_type_param, ids[i]));
		node_enum_params(data, 1, ids[i], 0, UINT32_MAX, NULL);
	}
	return 0;
}

static void remove_busy_resource(struct resource_data *d)
{
	if (d->end != -1) {
		spa_hook_remove(&d->listener);
		d->end = -1;
		pw_impl_client_set_busy(d->resource->client, false);
	}
}

static void result_node_sync(void *data, int seq, int res, uint32_t type, const void *result)
{
	struct resource_data *d = data;

	pw_log_debug("%p: sync result %d %d (%d/%d)", d->node, res, seq, d->seq, d->end);
	if (seq == d->end)
		remove_busy_resource(d);
}

static int node_set_param(void *object, uint32_t id, uint32_t flags,
		const struct spa_pod *param)
{
	struct resource_data *data = object;
	struct pw_resource *resource = data->resource;
	struct pw_impl_node *node = data->node;
	struct pw_impl_client *client = resource->client;
	int res;
	static const struct spa_node_events node_events = {
		SPA_VERSION_NODE_EVENTS,
		.result = result_node_sync,
	};

	pw_log_debug("%p: resource %p set param id:%d (%s) %08x", node, resource,
			id, spa_debug_type_find_name(spa_type_param, id), flags);

	res = spa_node_set_param(node->node, id, flags, param);

	if (res < 0) {
		pw_resource_errorf(resource, res,
				"set param id:%d (%s) flags:%08x failed", id,
				spa_debug_type_find_name(spa_type_param, id), flags);

	} else if (SPA_RESULT_IS_ASYNC(res)) {
		pw_impl_client_set_busy(client, true);
		if (data->end == -1)
			spa_node_add_listener(node->node, &data->listener,
				&node_events, data);
		data->seq = res;
		data->end = spa_node_sync(node->node, res);
	}
	return 0;
}

static int node_send_command(void *object, const struct spa_command *command)
{
	struct resource_data *data = object;
	struct pw_impl_node *node = data->node;
	uint32_t id = SPA_NODE_COMMAND_ID(command);
	int res;

	pw_log_debug("%p: got command %d (%s)", node, id,
		    spa_debug_type_find_name(spa_type_node_command_id, id));

	switch (id) {
	case SPA_NODE_COMMAND_Suspend:
		res = suspend_node(node);
		break;
	default:
		res = spa_node_send_command(node->node, command);
		break;
	}
	return res;
}

static const struct pw_node_methods node_methods = {
	PW_VERSION_NODE_METHODS,
	.subscribe_params = node_subscribe_params,
	.enum_params = node_enum_params,
	.set_param = node_set_param,
	.send_command = node_send_command
};

static void resource_destroy(void *data)
{
	struct resource_data *d = data;
	remove_busy_resource(d);
	spa_hook_remove(&d->resource_listener);
	spa_hook_remove(&d->object_listener);
}

static void resource_pong(void *data, int seq)
{
	struct resource_data *d = data;
	struct pw_resource *resource = d->resource;
	pw_log_debug("%p: resource %p: got pong %d", d->node,
			resource, seq);
}

static const struct pw_resource_events resource_events = {
	PW_VERSION_RESOURCE_EVENTS,
	.destroy = resource_destroy,
	.pong = resource_pong,
};

static int
global_bind(void *object, struct pw_impl_client *client, uint32_t permissions,
	    uint32_t version, uint32_t id)
{
	struct pw_impl_node *this = object;
	struct pw_global *global = this->global;
	struct pw_resource *resource;
	struct resource_data *data;

	resource = pw_resource_new(client, id, permissions, global->type, version, sizeof(*data));
	if (resource == NULL)
		goto error_resource;

	data = pw_resource_get_user_data(resource);
	data->node = this;
	data->resource = resource;
	data->end = -1;

	pw_resource_add_listener(resource,
			&data->resource_listener,
			&resource_events, data);
	pw_resource_add_object_listener(resource,
			&data->object_listener,
			&node_methods, data);

	pw_log_debug("%p: bound to %d", this, resource->id);
	pw_global_add_resource(global, resource);

	this->info.change_mask = PW_NODE_CHANGE_MASK_ALL;
	pw_node_resource_info(resource, &this->info);
	this->info.change_mask = 0;

	return 0;

error_resource:
	pw_log_error("%p: can't create node resource: %m", this);
	return -errno;
}

static void global_free(void *data)
{
	struct pw_impl_node *this = data;
	spa_hook_remove(&this->global_listener);
	this->global = NULL;
	pw_impl_node_destroy(this);
}

static const struct pw_global_events global_events = {
	PW_VERSION_GLOBAL_EVENTS,
	.free = global_free,
};

static inline void insert_driver(struct pw_context *context, struct pw_impl_node *node)
{
	struct pw_impl_node *n, *t;

	spa_list_for_each_safe(n, t, &context->driver_list, driver_link) {
		if (n->priority_driver < node->priority_driver)
			break;
		if (n->priority_driver == 0 && node->priority_driver == 0) {
			/* no priority is set, we prefer the driver that does
			 * lazy scheduling. */
			if (n->supports_request > 0 && node->supports_lazy > 0) {
				if (n->supports_request <= node->supports_lazy)
					break;
			}
		}
	}
	spa_list_append(&n->driver_link, &node->driver_link);
	pw_context_emit_driver_added(context, node);
}

static inline void remove_driver(struct pw_context *context, struct pw_impl_node *node)
{
	spa_list_remove(&node->driver_link);
	pw_context_emit_driver_removed(context, node);
}

static int
do_update_position(struct spa_loop *loop,
		bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct pw_impl_node *node = user_data;
	void *position = *(void**)data;
	pw_log_trace("%p: set position %p", node, position);
	node->rt.position = position;
	if (position) {
		node->target_rate = node->rt.position->clock.target_rate;
		node->target_quantum = node->rt.position->clock.target_duration;
	}
	return 0;
}

SPA_EXPORT
int pw_impl_node_set_io(struct pw_impl_node *this, uint32_t id, void *data, size_t size)
{
	int res;
	struct pw_impl_port *port;

	switch (id) {
	case SPA_IO_Position:
		if (data != NULL && size < sizeof(struct spa_io_position))
			return -EINVAL;
		pw_log_debug("%p: set position %p", this, data);
		pw_loop_locked(this->data_loop,
				do_update_position, SPA_ID_INVALID, &data, sizeof(void*), this);
		break;
	case SPA_IO_Clock:
		if (data != NULL && size < sizeof(struct spa_io_clock))
			return -EINVAL;
		pw_log_debug("%p: set clock %p", this, data);
		this->rt.clock = data;
		if (this->rt.clock) {
			this->info.id = this->rt.clock->id;
			this->rt.target.id = this->info.id;
		}
		break;
	}
	this->driving = this->driver && this->rt.clock && this->rt.position &&
		this->rt.position->clock.id == this->rt.clock->id;

	pw_log_debug("%p: driving:%d clock-id:%d driver-id:%d", this, this->driving,
			this->rt.clock ? this->rt.clock->id : SPA_ID_INVALID,
			this->rt.position ? this->rt.position->clock.id : SPA_ID_INVALID);

	spa_list_for_each(port, &this->input_ports, link)
		spa_node_set_io(port->mix, id, data, size);
	spa_list_for_each(port, &this->output_ports, link)
		spa_node_set_io(port->mix, id, data, size);

	res = spa_node_set_io(this->node, id, data, size);

	if (this->rt.position &&
	    ((res >= 0 && !SPA_RESULT_IS_ASYNC(res)) ||
	    this->rt.target.activation->client_version < 1))
		this->rt.target.activation->active_driver_id = this->rt.position->clock.id;

	pw_log_debug("%p: set io: %s", this, spa_strerror(res));

	return res;
}

static int
do_add_target(struct spa_loop *loop,
                bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct pw_node_target *t = user_data;
	struct pw_impl_node *node = *(struct pw_impl_node**)data;

	pw_log_debug("%p: target:%p id:%d added:%d prepared:%d", node, t, t->id, t->added, node->rt.prepared);

	if (!t->added) {
		spa_list_append(&node->rt.target_list, &t->link);
		t->added = true;
		if (node->rt.prepared)
			activate_target(node, t);
	}
	return 0;
}

SPA_EXPORT
int pw_impl_node_add_target(struct pw_impl_node *node, struct pw_node_target *t)
{
	pw_loop_locked(node->data_loop,
			do_add_target, SPA_ID_INVALID, &node, sizeof(void *), t);
	if (t->node)
		pw_impl_node_emit_peer_added(node, t->node);

	return 0;
}

static int
do_remove_target(struct spa_loop *loop,
                bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct pw_node_target *t = user_data;
	struct pw_impl_node *node = *(struct pw_impl_node**)data;

	pw_log_debug("%p: target:%p id:%d added:%d prepared:%d", node, t, t->id, t->added, node->rt.prepared);

	if (t->added) {
		spa_list_remove(&t->link);
		t->added = false;
		if (node->rt.prepared) {
			int old_state = SPA_ATOMIC_LOAD(node->rt.target.activation->status);
			uint64_t trigger = 0;
			if (PW_NODE_ACTIVATION_PENDING_TRIGGER(old_state))
				trigger = get_time_ns(node->rt.target.system);
			deactivate_target(node, t, trigger);
		}
	}
	return 0;
}

SPA_EXPORT
int pw_impl_node_remove_target(struct pw_impl_node *node, struct pw_node_target *t)
{
	/* we also update the target list for remote nodes so that the profiler
	 * can inspect the nodes as well */
	pw_loop_locked(node->data_loop,
			do_remove_target, SPA_ID_INVALID, &node, sizeof(void *), t);
	if (t->node)
		pw_impl_node_emit_peer_removed(node, t->node);

	return 0;
}

static void update_io(struct pw_impl_node *node)
{
	struct pw_node_target *t = &node->rt.target;

	pw_log_debug("%p: id:%d", node, node->info.id);

	pw_impl_node_set_io(node, SPA_IO_Clock, &t->activation->position.clock,
                            sizeof(struct spa_io_clock));
	pw_impl_node_set_io(node, SPA_IO_Position, &t->activation->position,
                            sizeof(struct spa_io_position));
}

SPA_EXPORT
int pw_impl_node_register(struct pw_impl_node *this,
		     struct pw_properties *properties)
{
	struct pw_context *context = this->context;
	struct pw_impl_port *port;

	pw_log_debug("%p: register remote:%d exported:%d", this, this->remote, this->exported);

	if (this->registered)
		goto error_existed;

	this->global = pw_global_new(context,
				     PW_TYPE_INTERFACE_Node,
				     PW_VERSION_NODE,
				     PW_NODE_PERM_MASK,
				     properties,
				     global_bind,
				     this);
	if (this->global == NULL)
		return -errno;

	spa_list_append(&context->node_list, &this->link);
	if (this->driver)
		insert_driver(context, this);
	this->registered = true;

	this->info.id = this->global->id;
	this->rt.target.id = this->info.id;
	this->rt.target.activation->position.clock.id = this->global->id;

	this->from_driver_peer = pw_node_peer_ref(this, this);
	this->to_driver_peer = pw_node_peer_ref(this, this);

	pw_properties_setf(this->properties, PW_KEY_OBJECT_ID, "%d", this->global->id);
	pw_properties_setf(this->properties, PW_KEY_OBJECT_SERIAL, "%"PRIu64,
			pw_global_get_serial(this->global));

	pw_global_update_keys(this->global, &this->properties->dict, global_keys);

	pw_impl_node_initialized(this);

	pw_global_add_listener(this->global, &this->global_listener, &global_events, this);
	pw_global_register(this->global);

	if (this->node)
		update_io(this);

	spa_list_for_each(port, &this->input_ports, link)
		pw_impl_port_register(port, NULL);
	spa_list_for_each(port, &this->output_ports, link)
		pw_impl_port_register(port, NULL);

	if (this->active)
		pw_context_recalc_graph(context, "register active node");

	return 0;

error_existed:
	pw_properties_free(properties);
	return -EEXIST;
}

SPA_EXPORT
int pw_impl_node_initialized(struct pw_impl_node *this)
{
	pw_log_debug("%p initialized", this);
	pw_impl_node_emit_initialized(this);
	node_update_state(this, PW_NODE_STATE_SUSPENDED, 0, NULL);
	return 0;
}

static void remove_segment_owner(struct pw_impl_node *driver, uint32_t node_id)
{
	struct pw_node_activation *a = driver->rt.target.activation;
	SPA_ATOMIC_CAS(a->segment_owner[0], node_id, 0);
	SPA_ATOMIC_CAS(a->segment_owner[1], node_id, 0);
}

SPA_EXPORT
int pw_impl_node_set_driver(struct pw_impl_node *node, struct pw_impl_node *driver)
{
	struct impl *impl = SPA_CONTAINER_OF(node, struct impl, this);
	struct pw_impl_node *old = node->driver_node;
	bool was_driving, no_driver = (driver == NULL);

	if (no_driver)
		driver = node;

	spa_list_remove(&node->follower_link);
	spa_list_append(&driver->follower_list, &node->follower_link);

	if (old == driver)
		return 0;

	remove_segment_owner(old, node->info.id);

	pw_log_debug("%p: driver %p driving:%u", node, driver, node->driving);
	pw_log_info("(%s-%u) -> change driver (%s-%d -> %s-%d)",
			node->name, node->info.id,
			old->name, old->info.id, driver->name, driver->info.id);

	/* make sure the old driver doesn't trigger the node anymore */
	pw_node_peer_unref(spa_steal_ptr(node->from_driver_peer));
	/* make sure the node doesn't trigger the old driver anymore */
	pw_node_peer_unref(spa_steal_ptr(node->to_driver_peer));

	node->driver_node = driver;
	node->moved = true;

	/* first send new driver target to node, the node is not yet being
	 * scheduled so it won't trigger yet */
	node->to_driver_peer = pw_node_peer_ref(node, driver);

	was_driving = node->driving;

	/* then set the new driver node activation */
	pw_impl_node_set_io(node, SPA_IO_Position,
			&driver->rt.target.activation->position,
			sizeof(struct spa_io_position));

	/* When a node was driver (and is waiting for all nodes to complete
	 * the Start command) cancel the pending state and let the new driver
	 * calculate a new state so that the Start command is sent to the
	 * node */
	if (was_driving && !node->driving)
		impl->pending_state = node->info.state;

	/* and then make the driver trigger the node */
	node->from_driver_peer = pw_node_peer_ref(driver, node);

	pw_impl_node_emit_driver_changed(node, old, driver);

	if (no_driver) {
		/* We don't have a driver, so remove the property */
		pw_properties_set(node->properties, PW_KEY_NODE_DRIVER_ID, NULL);
	} else if (node->driver_node->global) {
		/* Expose the driver ID if it is available as a global */
		pw_properties_setf(node->properties, PW_KEY_NODE_DRIVER_ID, "%u",
				pw_global_get_id(node->driver_node->global));
	}

	node->info.change_mask |= PW_NODE_CHANGE_MASK_PROPS;
	pw_impl_node_emit_info_changed(driver, &node->info);

	return 0;
}

struct match {
	struct pw_impl_node *node;
	int count;
};
#define MATCH_INIT(n) ((struct match){ .node = (n) })

static int execute_match(void *data, const char *location, const char *action,
		const char *val, size_t len)
{
	struct match *match = data;
	struct pw_impl_node *this = match->node;
	if (spa_streq(action, "update-props")) {
		match->count += pw_properties_update_string(this->properties, val, len);
	}
	return 1;
}

static void check_properties(struct pw_impl_node *node)
{
	struct impl *impl = SPA_CONTAINER_OF(node, struct impl, this);
	struct pw_context *context = node->context;
	const char *str, *recalc_reason = NULL;
	struct spa_fraction frac;
	uint32_t value;
	bool driver, trigger, sync, async;
	struct match match;

	match = MATCH_INIT(node);
	pw_context_conf_section_match_rules(context, "node.rules",
			&node->properties->dict, execute_match, &match);

	value = pw_properties_get_uint32(node->properties, PW_KEY_PRIORITY_DRIVER, 0);
	if (value != node->priority_driver) {
		pw_log_debug("%p: priority driver %d -> %d", node, node->priority_driver, value);
		node->priority_driver = value;
		if (node->registered && node->driver) {
			remove_driver(context, node);
			insert_driver(context, node);
			recalc_reason = "driver priority changed";
		}
	}
	node->supports_lazy = pw_properties_get_uint32(node->properties, PW_KEY_NODE_SUPPORTS_LAZY, 0);
	node->supports_request = pw_properties_get_uint32(node->properties, PW_KEY_NODE_SUPPORTS_REQUEST, 0);

	if ((str = pw_properties_get(node->properties, PW_KEY_NODE_NAME)) &&
	    (node->name == NULL || !spa_streq(node->name, str))) {
		free(node->name);
		node->name = strdup(str);
		snprintf(node->rt.target.name, sizeof(node->rt.target.name), "%s", node->name);
		pw_log_debug("%p: name '%s'", node, node->name);
	}

	node->pause_on_idle = pw_properties_get_bool(node->properties, PW_KEY_NODE_PAUSE_ON_IDLE, true);
	node->suspend_on_idle = pw_properties_get_bool(node->properties, PW_KEY_NODE_SUSPEND_ON_IDLE, false);
	node->transport_sync = pw_properties_get_bool(node->properties, PW_KEY_NODE_TRANSPORT_SYNC, false);
	impl->cache_params =  pw_properties_get_bool(node->properties, PW_KEY_NODE_CACHE_PARAMS, true);
	driver = pw_properties_get_bool(node->properties, PW_KEY_NODE_DRIVER, false);
	node->exclusive = pw_properties_get_bool(node->properties, PW_KEY_NODE_EXCLUSIVE, false);
	node->reliable = pw_properties_get_bool(node->properties, PW_KEY_NODE_RELIABLE, false);

	if (node->driver != driver) {
		pw_log_debug("%p: driver %d -> %d", node, node->driver, driver);
		node->driver = driver;
		if (node->registered) {
			if (driver)
				insert_driver(context, node);
			else
				remove_driver(context, node);
		}
		if (driver && node->driver_node == node)
			node->driving = true;
		recalc_reason = "driver changed";
	}

	/* not scheduled automatically so we add an additional required trigger */
	trigger = pw_properties_get_bool(node->properties, PW_KEY_NODE_TRIGGER, false);
	if (trigger != node->trigger) {
		node->trigger = trigger;
		if (trigger)
			SPA_ATOMIC_INC(node->rt.target.activation->state[0].required);
		else
			SPA_ATOMIC_DEC(node->rt.target.activation->state[0].required);
	}

	/* group defines what nodes are scheduled together */
	str = pw_properties_get(node->properties, PW_KEY_NODE_GROUP);
	if (!spa_streq(str, impl->group)) {
		pw_log_info("%p: group '%s'->'%s'", node, impl->group, str);
		free(impl->group);
		impl->group = str ? strdup(str) : NULL;
		pw_free_strv(node->groups);
		node->groups = impl->group ?
			pw_strv_parse(impl->group, strlen(impl->group), INT_MAX, NULL) : NULL;
		node->freewheel = pw_strv_find(node->groups, "pipewire.freewheel") >= 0;
		recalc_reason = "group changed";
	}

	/* link group defines what nodes are logically linked together */
	str = pw_properties_get(node->properties, PW_KEY_NODE_LINK_GROUP);
	if (!spa_streq(str, impl->link_group)) {
		pw_log_info("%p: link group '%s'->'%s'", node, impl->link_group, str);
		free(impl->link_group);
		impl->link_group = str ? strdup(str) : NULL;
		pw_free_strv(node->link_groups);
		node->link_groups = impl->link_group ?
			pw_strv_parse(impl->link_group, strlen(impl->link_group), INT_MAX, NULL) : NULL;
		recalc_reason = "link group changed";
	}

	/* sync group defines what nodes are part of the same sync */
	str = pw_properties_get(node->properties, PW_KEY_NODE_SYNC_GROUP);
	if (str == NULL)
		str = "group.sync.0";
	if (!spa_streq(str, impl->sync_group)) {
		pw_log_info("%p: sync group '%s'->'%s'", node, impl->sync_group, str);
		free(impl->sync_group);
		impl->sync_group = str ? strdup(str) : NULL;
		pw_free_strv(node->sync_groups);
		node->sync_groups = impl->sync_group ?
			pw_strv_parse(impl->sync_group, strlen(impl->sync_group), INT_MAX, NULL) : NULL;
		recalc_reason = "sync group changed";
	}
	sync = pw_properties_get_bool(node->properties, PW_KEY_NODE_SYNC, false);
	if (sync != node->sync) {
		pw_log_info("%p: sync %d -> %d", node, node->sync, sync);
		node->sync = sync;
		recalc_reason = "sync changed";
	}

	str = pw_properties_get(node->properties, PW_KEY_NODE_TRANSPORT);
	if (str != NULL) {
		node->transport = spa_atob(str) ?
			PW_NODE_ACTIVATION_COMMAND_START :
			PW_NODE_ACTIVATION_COMMAND_STOP;
		pw_log_info("%p: transport %d", node, node->transport);
		pw_properties_set(node->properties, PW_KEY_NODE_TRANSPORT, NULL);
		recalc_reason = "transport changed";
	}
	async = pw_properties_get_bool(node->properties, PW_KEY_NODE_ASYNC, false);
	async &= !node->driver;
	if (async != node->async) {
		pw_log_info("%p: async %d -> %d", node, node->async, async);
		node->async = async;
		SPA_FLAG_UPDATE(node->rt.target.activation->flags, PW_NODE_ACTIVATION_FLAG_ASYNC, async);
	}

	if ((str = pw_properties_get(node->properties, PW_KEY_MEDIA_CLASS)) != NULL &&
	    (strstr(str, "/Sink") != NULL || strstr(str, "/Source") != NULL)) {
		node->can_suspend = true;
	} else {
		node->can_suspend = false;
	}
	if ((str = pw_properties_get(node->properties, PW_KEY_NODE_PASSIVE)) == NULL)
		str = "false";
	if (spa_streq(str, "out"))
		node->out_passive = true;
	else if (spa_streq(str, "in"))
		node->in_passive = true;
	else
		node->in_passive = node->out_passive = spa_atob(str);

	node->want_driver = pw_properties_get_bool(node->properties, PW_KEY_NODE_WANT_DRIVER, false);
	node->always_process = pw_properties_get_bool(node->properties, PW_KEY_NODE_ALWAYS_PROCESS, false);

	if (node->always_process)
		node->want_driver = true;

	if ((str = pw_properties_get(node->properties, PW_KEY_NODE_LATENCY))) {
                if (sscanf(str, "%u/%u", &frac.num, &frac.denom) == 2 && frac.denom != 0) {
			if (node->latency.num != frac.num || node->latency.denom != frac.denom) {
				pw_log_info("(%s-%u) latency:%u/%u -> %u/%u", node->name,
						node->info.id, node->latency.num,
						node->latency.denom, frac.num, frac.denom);
				node->latency = frac;
				recalc_reason = "quantum changed";
			}
		}
	}
	if ((str = pw_properties_get(node->properties, PW_KEY_NODE_MAX_LATENCY))) {
                if (sscanf(str, "%u/%u", &frac.num, &frac.denom) == 2 && frac.denom != 0) {
			if (node->max_latency.num != frac.num || node->max_latency.denom != frac.denom) {
				pw_log_info("(%s-%u) max-latency:%u/%u -> %u/%u", node->name,
						node->info.id, node->max_latency.num,
						node->max_latency.denom, frac.num, frac.denom);
				node->max_latency = frac;
				recalc_reason = "max quantum changed";
			}
		}
	}
	node->lock_quantum = pw_properties_get_bool(node->properties, PW_KEY_NODE_LOCK_QUANTUM, false);

	value = pw_properties_get_uint32(node->properties, PW_KEY_NODE_FORCE_QUANTUM, 0);
	if (node->force_quantum != value) {
	        node->force_quantum = value;
		node->stamp = ++context->stamp;
		recalc_reason = "force quantum changed";
	}

	if ((str = pw_properties_get(node->properties, PW_KEY_NODE_RATE))) {
                if (sscanf(str, "%u/%u", &frac.num, &frac.denom) == 2 && frac.denom != 0) {
			if (node->rate.num != frac.num || node->rate.denom != frac.denom) {
				pw_log_info("(%s-%u) rate:%u/%u -> %u/%u", node->name,
						node->info.id, node->rate.num,
						node->rate.denom, frac.num, frac.denom);
				node->rate = frac;
				recalc_reason = "node rate changed";
			}
		}
	}
	node->lock_rate = pw_properties_get_bool(node->properties, PW_KEY_NODE_LOCK_RATE, false);
	/* the leaf node is one that only produces/consumes the data. We can deduce this from the
	 * absence of a link-group and the fact that it has no output/input ports. */
	node->leaf = node->link_groups == NULL &&
			(node->info.max_input_ports == 0 || node->info.max_output_ports == 0);

	value = pw_properties_get_uint32(node->properties, PW_KEY_NODE_FORCE_RATE, SPA_ID_INVALID);
	if (value == 0)
		value = node->rate.denom;
	if (value == SPA_ID_INVALID)
		value = 0;
	if (node->force_rate != value) {
		pw_log_info("(%s-%u) force-rate:%u -> %u", node->name,
					node->info.id, node->force_rate, value);
		node->force_rate = value;
		node->stamp = ++context->stamp;
		recalc_reason = "force rate changed";
	}

	pw_log_debug("%p: driver:%d recalc:%s active:%d", node, node->driver,
			recalc_reason, node->active);

	if (recalc_reason != NULL && node->active)
		pw_context_recalc_graph(context, recalc_reason);
}

static const char *str_status(uint32_t status)
{
	switch (status) {
	case PW_NODE_ACTIVATION_NOT_TRIGGERED:
		return "not-triggered";
	case PW_NODE_ACTIVATION_TRIGGERED:
		return "triggered";
	case PW_NODE_ACTIVATION_AWAKE:
		return "awake";
	case PW_NODE_ACTIVATION_FINISHED:
		return "finished";
	case PW_NODE_ACTIVATION_INACTIVE:
		return "inactive";
	}
	return "unknown";
}

static inline void update_xrun_stats(struct pw_node_activation *a, uint32_t count, uint64_t trigger, uint64_t delay)
{
	a->xrun_count += count;
	a->xrun_time = trigger;
	a->xrun_delay = delay;
	a->max_delay = SPA_MAX(a->max_delay, delay);
}

static inline void debug_xrun_target(struct pw_impl_node *driver,
		struct pw_node_target *t, int status, uint64_t nsec)
{
	struct pw_node_activation *a = t->activation;
	struct pw_node_activation_state *state = &a->state[0];
	int suppressed;
	enum spa_log_level level = SPA_LOG_LEVEL_DEBUG;

	if ((suppressed = spa_ratelimit_test(&driver->rt.rate_limit, nsec)) >= 0)
		level = SPA_LOG_LEVEL_INFO;

	pw_log(level, "(%s-%u) xrun state:%p pending:%d/%d s:%"PRIu64" a:%"PRIu64" f:%"PRIu64
		" waiting:%"PRIu64" process:%"PRIu64" runtime:%"PRIu64" status:%s (%d suppressed)",
		t->name, t->id, state,
		state->pending, state->required,
		a->signal_time,
		a->awake_time,
		a->finish_time,
		a->awake_time - a->signal_time,
		a->finish_time - a->awake_time,
		a->finish_cputime - a->awake_cputime,
		str_status(status), suppressed);
}

static inline void debug_xrun_graph(struct pw_impl_node *driver, uint64_t nsec, uint32_t old_status)
{
	int suppressed;
	enum spa_log_level level = SPA_LOG_LEVEL_DEBUG;
	struct pw_node_target *t;

	if ((suppressed = spa_ratelimit_test(&driver->rt.rate_limit, nsec)) >= 0)
		level = SPA_LOG_LEVEL_INFO;

	pw_log(level, "(%s-%u) graph xrun %s (%d suppressed)",
			driver->name, driver->info.id, str_status(old_status), suppressed);

	spa_list_for_each(t, &driver->rt.target_list, link) {
		struct pw_node_activation *a = t->activation;
		struct pw_node_activation_state *state = &a->state[0];
		uint32_t status = SPA_ATOMIC_LOAD(a->status);

		if (status == PW_NODE_ACTIVATION_TRIGGERED ||
		    status == PW_NODE_ACTIVATION_AWAKE) {
			pw_log(level, "(%s-%u) xrun state:%p pending:%d/%d s:%"PRIu64" a:%"PRIu64" f:%"PRIu64
					" waiting:%"PRIu64" process:%"PRIu64" runtime:%"PRIu64" status:%s",
					t->name, t->id, state,
					state->pending, state->required,
					a->signal_time,
					a->awake_time,
					a->finish_time,
					a->awake_time - a->signal_time,
					a->finish_time - a->awake_time,
					a->finish_cputime - a->awake_cputime,
					str_status(status));

		}
	}
}

static void debug_sync_timeout(struct pw_impl_node *driver, uint64_t nsec)
{
	struct pw_node_target *t;
	enum spa_log_level level = SPA_LOG_LEVEL_DEBUG;
	int suppressed;

	if ((suppressed = spa_ratelimit_test(&driver->rt.rate_limit, nsec)) >= 0)
		level = SPA_LOG_LEVEL_INFO;

	pw_log(level, "(%s-%u) sync timeout, going to RUNNING (%d suppressed)",
				driver->name, driver->info.id, suppressed);

	spa_list_for_each(t, &driver->rt.target_list, link) {
		struct pw_node_activation *a = t->activation;
		struct pw_node_activation_state *state = &a->state[0];
		uint32_t status = SPA_ATOMIC_LOAD(a->status);

		if (!a->pending_sync)
			continue;

		pw_log(level, "(%s-%u) sync state:%p pending:%d/%d s:%"PRIu64" a:%"PRIu64" f:%"PRIu64
				" waiting:%"PRIu64" process:%"PRIu64" runtime:%"PRIu64" status:%s",
				t->name, t->id, state,
				state->pending, state->required,
				a->signal_time,
				a->awake_time,
				a->finish_time,
				a->awake_time - a->signal_time,
				a->finish_time - a->awake_time,
				a->finish_cputime - a->awake_cputime,
				str_status(status));
	}
}

static inline void calculate_stats(struct pw_impl_node *this,  struct pw_node_activation *a)
{
	uint64_t signal_time = a->signal_time;
	uint64_t prev_signal_time = a->prev_signal_time;
	uint64_t process_time = a->finish_cputime - a->awake_cputime;
	uint64_t period_time = signal_time - prev_signal_time;

	if (SPA_LIKELY(signal_time > prev_signal_time)) {
		float load = (float) process_time / (float) period_time;
		a->cpu_load[0] = (a->cpu_load[0] + load) / 2.0f;
		a->cpu_load[1] = (a->cpu_load[1] * 7.0f + load) / 8.0f;
		a->cpu_load[2] = (a->cpu_load[2] * 31.0f + load) / 32.0f;
	}
	pw_log_trace_fp("%p: graph completed wait:%"PRIu64" run:%"PRIu64
			" busy:%"PRIu64" period:%"PRIu64" cpu:%f:%f:%f", this,
			a->awake_time - signal_time, process_time,
			a->finish_time - a->signal_time, period_time,
			a->cpu_load[0], a->cpu_load[1], a->cpu_load[2]);
}

/* The main processing entry point of a node. This is called from the data-loop and usually
 * as a result of signaling the eventfd of the node.
 *
 * This code runs on the client and the server, depending on where the node is.
 *
 * The per-cycle CPU cycle count comes from the perf_event helpers in
 * cycle-counter.h. They are gated at runtime on
 * /proc/sys/kernel/perf_event_paranoid; on hosts that refuse
 * perf_event_open() the fd stays -2 ("do not retry"), the awake/finish
 * cycle stamps are zero, and consumers must fall back on the existing
 * wall-clock prev_run_time. The read() syscall in process_node adds
 * ~hundred ns per cycle in the cycles-enabled path -- a small price
 * for the precision it brings, and an optimisation candidate (mmap +
 * RDPMC on x86) for a follow-up commit.
 */

static inline int process_node(void *data, uint64_t awake_nsec, uint64_t awake_cpu_nsec)
{
	struct pw_impl_node *this = data;
	struct pw_impl_port *p;
	struct pw_node_activation *a = this->rt.target.activation;
	struct spa_system *data_system = this->rt.target.system;
	int status;
	uint64_t nsec, cpu_nsec;
	uint64_t awake_cycles_local = 0, finish_cycles_local = 0;
	bool was_awake;

	if (!SPA_ATOMIC_CAS(a->status,
				PW_NODE_ACTIVATION_TRIGGERED,
				PW_NODE_ACTIVATION_AWAKE))
		return 0;

	/* Per-thread perf cycle counter. perf_event_open(pid=0) binds
	 * to the calling thread, so the counter must be (re-)opened
	 * whenever the executing thread changes (dynamic data-loop
	 * migration). cycle_tid tracks which thread owns the current
	 * fd; a mismatch triggers a reopen. -2 is the "do not retry"
	 * sentinel after a failed open on a given thread.
	 *
	 * Skip for remote (server-side proxy) nodes: their
	 * process_node merely relays the trigger to the real client
	 * process, so the cycles measured here are relay overhead, not
	 * audio work. The client's own process_node writes the
	 * authoritative cycle counts into the shared activation. */
	if (!this->remote) {
		int cur_tid = (int)gettid();
		if (SPA_UNLIKELY(this->cycle_fd < 0 ||
				 this->cycle_tid != cur_tid)) {
			if (this->cycle_fd >= 0)
				close(this->cycle_fd);
			this->cycle_fd = pw_cycle_counter_open();
			this->cycle_tid = cur_tid;
			if (this->cycle_fd < 0)
				this->cycle_fd = -2;
		}
		awake_cycles_local = pw_cycle_counter_read(this->cycle_fd);
	}

	pw_log_trace_fp("%p: %s-%d process remote:%u exported:%u %"PRIu64" %"PRIu64,
			this, this->name, this->info.id, this->remote, this->exported,
			a->signal_time, nsec);

	/* when transport sync is not supported, just clear the flag */
	if (SPA_UNLIKELY(!this->transport_sync))
		a->pending_sync = false;

	if (SPA_LIKELY(this->rt.prepared)) {
		/* process input mixers */
		spa_list_for_each(p, &this->rt.input_mix, rt.node_link)
			spa_node_process_fast(p->mix);

		/* process the actual node */
		status = spa_node_process_fast(this->node);

		/* process output tee */
		if (status & SPA_STATUS_HAVE_DATA) {
			spa_list_for_each(p, &this->rt.output_mix, rt.node_link)
				spa_node_process_fast(p->mix);
		}
	} else {
		/* This can happen when we deactivated the node but some links are
		 * still not shut down. We simply don't schedule the node and make
		 * sure we trigger the peers in trigger_targets below. */
		pw_log_debug("%p: scheduling non-active node %s", this, this->name);
		status = SPA_STATUS_HAVE_DATA;
	}
	a->state[0].status = status;

	nsec = get_time_ns(data_system);
	cpu_nsec = get_cputime_ns(data_system);
	if (!this->remote)
		finish_cycles_local = pw_cycle_counter_read(this->cycle_fd);
	was_awake = SPA_ATOMIC_CAS(a->status,
				PW_NODE_ACTIVATION_AWAKE,
				PW_NODE_ACTIVATION_FINISHED);
	if (this->driving && a->driver_start_cputime != 0) {
		a->awake_time    = a->driver_start_time;
		a->awake_cputime = a->driver_start_cputime;
	} else {
		a->awake_time    = awake_nsec;
		a->awake_cputime = awake_cpu_nsec;
	}
	a->finish_time    = nsec;
	a->finish_cputime = cpu_nsec;

	if (!this->remote) {
		if (this->driving && a->driver_start_cycles != 0)
			a->awake_cycles = a->driver_start_cycles;
		else
			a->awake_cycles = awake_cycles_local;
		a->finish_cycles = finish_cycles_local;

		/* Compute prev_run_time / prev_run_cycles at the point
		 * where awake and finish stamps are authoritative (same
		 * thread, no race with the driver).  Remote nodes have
		 * their client compute this (signal_sync / pw-jack).
		 *
		 * was_awake == true  ⇒ normal completion.
		 * was_awake == false ⇒ xrun: the driver already consumed
		 *   stale prev_run fields, so deposit the real runtime
		 *   in xrun_run_time/cycles for deferred pickup. */
		uint64_t run_time = 0, run_cycles = 0;
		if (a->finish_cputime >= a->awake_cputime &&
				a->awake_cputime != 0 && a->finish_cputime != 0)
			run_time = a->finish_cputime - a->awake_cputime;
		if (a->finish_cycles != 0 && a->awake_cycles != 0 &&
				a->finish_cycles >= a->awake_cycles)
			run_cycles = a->finish_cycles - a->awake_cycles;

		if (was_awake) {
			SPA_ATOMIC_STORE(a->prev_run_time, run_time);
			SPA_ATOMIC_STORE(a->prev_run_cycles, run_cycles);
		} else {
			SPA_ATOMIC_STORE(a->xrun_run_time, run_time);
			SPA_ATOMIC_STORE(a->xrun_run_cycles, run_cycles);
		}
	}

	pw_log_trace_fp("%p: finished status:%d %"PRIu64" was_awake:%d",
			this, status, nsec, was_awake);

	/* we don't need to trigger targets when the node was driving the
	 * graph because that means we finished the graph. */
	if (SPA_LIKELY(!this->driving)) {
		if ((!this->async || a->server_version < 1) && was_awake)
			trigger_targets(this, status, nsec);
	} else {
		/* calculate CPU time when finished */
		a->signal_time = this->driver_start;
		calculate_stats(this, a);
		pw_impl_node_rt_emit_complete(this);
		a->finish_cputime = get_cputime_ns(data_system);
	}

	if (SPA_UNLIKELY(status & SPA_STATUS_DRAINED))
		pw_impl_node_rt_emit_drained(this);

	return status;
}

int pw_impl_node_trigger(struct pw_impl_node *node)
{
	uint64_t nsec = get_time_ns(node->rt.target.system);
	struct pw_node_target *t = &node->rt.target;
	return t->trigger(t, nsec);
}

/* Out-of-line companion to trigger_target_v1's same-loop fast path
 * (declared in private.h). Called when the producer that owns `t`
 * runs on the same data-loop thread as the consumer t points at, so
 * we can skip the eventfd write + epoll wake and call process_node
 * directly. State-machine invariants the cross-thread path enforces:
 *
 *   - trigger_target_v1 already set a->signal_time and CAS'd
 *     a->status from NOT_TRIGGERED to TRIGGERED, so process_node's
 *     own TRIGGERED -> AWAKE CAS succeeds exactly as on the eventfd
 *     path; if the consumer was concurrently driven INACTIVE between
 *     the two CAS calls, the inner CAS fails and process_node bails
 *     -- identical to the cross-thread case.
 *   - awake_time stamps: the eventfd path reads wall + cputime in
 *     node_on_fd_events AFTER the wake. Inline, we are running on
 *     the consumer's data-loop thread already (same thread as the
 *     producer's), so the awake clocks are the producer's finish
 *     clocks plus a few ns of function-call overhead. We pass the
 *     producer's nsec (= signal_time of the consumer) and resample
 *     the per-thread CPU clock here so awake_cputime is accurate
 *     under the same thread for both the producer's finish and the
 *     consumer's awake.
 *   - recursion: if the consumer has same-loop downstream targets,
 *     process_node -> trigger_targets -> trigger_target_v1 will
 *     nest further. The depth is bounded by the longest same-loop
 *     chain in the fused subgraph, which audio graphs keep small
 *     (tens of nodes); stack pressure is negligible. */
SPA_EXPORT
int pw_impl_node_dispatch_inline(struct pw_node_target *t, uint64_t nsec)
{
	uint64_t cpu_nsec = get_cputime_ns(t->system);
	return process_node(t->node, nsec, cpu_nsec);
}

static void node_on_fd_events(struct spa_source *source)
{
	struct pw_impl_node *this = source->data;

	if (SPA_UNLIKELY(source->rmask & (SPA_IO_ERR | SPA_IO_HUP))) {
		pw_log_warn("%p: got socket error %08x", this, source->rmask);
		return;
	}
	if (SPA_LIKELY(source->rmask & SPA_IO_IN)) {
		uint64_t cmd, nsec, cpu_nsec;
		struct spa_system *data_system = this->rt.target.system;

		nsec = get_time_ns(data_system);
		cpu_nsec = get_cputime_ns(data_system);

		if (SPA_UNLIKELY(spa_system_eventfd_read(data_system, this->source.fd, &cmd) < 0))
			pw_log_warn("%p: read failed %m", this);
		else if (SPA_UNLIKELY(cmd > 1)) {
			pw_log_info("(%s-%u) client missed %"PRIu64" wakeups",
				this->name, this->info.id, cmd - 1);
			update_xrun_stats(this->rt.target.activation, cmd - 1,
					nsec / 1000, 0);
		}

		pw_log_trace_fp("%p: remote:%u exported:%u %s-%d got process %"PRIu64,
				this, this->remote, this->exported, this->name, this->info.id,
				nsec);

		process_node(this, nsec, cpu_nsec);
	}
}

static void reset_segment(struct spa_io_segment *seg)
{
	spa_zero(*seg);
	seg->rate = 1.0;
}

static void reset_position(struct pw_impl_node *this, struct spa_io_position *pos)
{
	uint32_t i;
	struct settings *s = &this->context->settings;
	uint32_t quantum = s->clock_force_quantum == 0 ? s->clock_quantum : s->clock_force_quantum;
	uint32_t rate = s->clock_force_rate == 0 ? s->clock_rate : s->clock_force_rate;

	this->target_rate = SPA_FRACTION(1, rate);
	this->target_quantum = quantum;
	this->elapsed = 0;

	pos->clock.rate = pos->clock.target_rate = this->target_rate;
	pos->clock.duration = pos->clock.target_duration = this->target_quantum;
	pos->video.flags = SPA_IO_VIDEO_SIZE_VALID;
	pos->video.size = s->video_size;
	pos->video.stride = pos->video.size.width * 16;
	pos->video.framerate = s->video_rate;
	pos->offset = INT64_MIN;

	pos->n_segments = 1;
	for (i = 0; i < SPA_IO_POSITION_MAX_SEGMENTS; i++)
		reset_segment(&pos->segments[i]);
}

static int do_gettid(struct spa_loop *loop,
                bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct pw_properties *props = user_data;

	pid_t tid = _gettid();
	pw_properties_setf(props, PW_KEY_NODE_LOOP_TID, "%d", tid);

	return 0;
}

SPA_EXPORT
struct pw_impl_node *pw_context_create_node(struct pw_context *context,
			    struct pw_properties *properties,
			    size_t user_data_size)
{
	struct impl *impl;
	struct pw_impl_node *this;
	size_t size;
	int res;

	impl = calloc(1, sizeof(struct impl) + user_data_size);
	if (impl == NULL) {
		res = -errno;
		goto error_exit;
	}

	spa_list_init(&impl->param_list);
	spa_list_init(&impl->pending_list);

	this = &impl->this;
	this->context = context;
	this->name = strdup("node");
	this->source.fd = -1;
	this->cycle_fd = -1;        /* lazy-init in process_node */
	this->cycle_tid = 0;

	if (properties == NULL)
		properties = pw_properties_new(NULL, NULL);
	if (properties == NULL) {
		res = -errno;
		goto error_clean;
	}

	this->remote = pw_properties_get_bool(properties, PW_KEY_NODE_REMOTE, false);

	this->data_loop = pw_context_acquire_node_loop(context, properties, this->remote);
	if (this->data_loop == NULL) {
		pw_log_error("can't find data-loop");
		res = -ENOENT;
		goto error_clean;
	}

	if (this->data_loop != context->main_loop && !this->remote) {
		/* Run do_gettid synchronously on the data loop so the
		 * resulting PW_KEY_NODE_LOOP_TID is the TID of the thread
		 * that will actually run the node's process callback. For
		 * remote nodes (the daemon's view of a client-node proxy)
		 * the callback runs in the client process, not here, so
		 * publishing the daemon-side data-loop TID would mislead
		 * downstream consumers; the client publishes its own TID
		 * via the same property and the value propagates back
		 * through the client-node info update path. */
		pw_loop_invoke(this->data_loop,
				do_gettid, SPA_ID_INVALID, NULL, 0, true, properties);
	} else {
		pw_properties_set(properties, PW_KEY_NODE_LOOP_TID, NULL);
	}

	if (user_data_size > 0)
                this->user_data = SPA_PTROFF(impl, sizeof(struct impl), void);

	this->properties = properties;

	/* the eventfd used to signal the node */
	if ((res = spa_system_eventfd_create(this->data_loop->system,
					SPA_FD_CLOEXEC | SPA_FD_NONBLOCK)) < 0)
		goto error_clean;

	pw_log_debug("%p: new fd:%d loop:%s", this, res, this->data_loop->name);

	this->source.fd = res;
	this->source.func = node_on_fd_events;
	this->source.data = this;
	this->source.mask = SPA_IO_IN | SPA_IO_ERR | SPA_IO_HUP;
	this->source.rmask = 0;

	size = sizeof(struct pw_node_activation);

	this->activation = pw_mempool_alloc(this->context->pool,
			PW_MEMBLOCK_FLAG_READWRITE |
			PW_MEMBLOCK_FLAG_SEAL |
			PW_MEMBLOCK_FLAG_MAP,
			SPA_DATA_MemFd, size);
	if (this->activation == NULL) {
		res = -errno;
                goto error_clean;
	}

	impl->work = pw_context_get_work_queue(this->context);
	impl->pending_id = SPA_ID_INVALID;

	spa_list_init(&this->follower_list);
	spa_list_init(&this->peer_list);

	spa_hook_list_init(&this->listener_list);
	spa_hook_list_init(&this->rt_listener_list);

	this->info.state = PW_NODE_STATE_CREATING;
	this->info.props = &this->properties->dict;
	this->info.params = this->params;

	spa_list_init(&this->input_ports);
	pw_map_init(&this->input_port_map, 64, 64);
	spa_list_init(&this->output_ports);
	pw_map_init(&this->output_port_map, 64, 64);

	spa_list_init(&this->rt.input_mix);
	spa_list_init(&this->rt.output_mix);
	spa_list_init(&this->rt.target_list);

	this->rt.target.activation = this->activation->map->ptr;
	this->rt.target.node = this;
	this->rt.target.system = this->data_loop->system;
	this->rt.target.fd = this->source.fd;
	this->rt.target.trigger = trigger_target_v1;
	/* The self-target (rt.target) is only invoked from
	 * pw_impl_node_trigger, the manual external-wake entry point.
	 * That caller is generally NOT on this node's data-loop thread
	 * (e.g. the main thread waking a driver, or a control thread
	 * kicking a stalled node) -- the eventfd is precisely the
	 * cross-thread handoff there. Leaving src_system at NULL keeps
	 * pw_impl_node_trigger on the eventfd path. The same-loop
	 * fast path is only activated for peer-owned target copies, by
	 * pw_node_peer_ref (and refreshed by pw_impl_node_set_data_loop
	 * when the producer's loop changes). */
	this->rt.target.src_system = NULL;

	reset_position(this, &this->rt.target.activation->position);
	this->rt.target.activation->sync_timeout = DEFAULT_SYNC_TIMEOUT;
	this->rt.target.activation->sync_left = 0;
	this->rt.target.activation->status = PW_NODE_ACTIVATION_INACTIVE;
	this->rt.target.activation->server_version = PW_VERSION_NODE_ACTIVATION;
	this->rt.target.activation->client_version = PW_VERSION_NODE_ACTIVATION;

	this->rt.rate_limit.interval = 2 * SPA_NSEC_PER_SEC;
	this->rt.rate_limit.burst = 1;

	this->driver_node = this;
	spa_list_append(&this->follower_list, &this->follower_link);

	check_properties(this);

	return this;

error_clean:
	if (this->activation)
		pw_memblock_unref(this->activation);
	if (this->source.fd != -1)
		spa_system_close(this->data_loop->system, this->source.fd);
	if (this->data_loop)
		pw_context_release_node_loop(context, this->data_loop);
	free(this->name);
	free(impl);
error_exit:
	pw_properties_free(properties);
	errno = -res;
	return NULL;
}

/* ---------------------------------------------------------------------
 * Runtime relocation of a node onto a different data loop. See
 * pw_impl_node_set_data_loop() below for the API contract and the
 * sequencing reasoning. The helpers here are private to this file
 * because they are tightly coupled to do_node_prepare /
 * do_node_unprepare and to the layout of struct pw_node_peer.
 * ---------------------------------------------------------------------
 *
 * Brief sequencing argument:
 *
 *   1. (non-remote only) unprepare on old_loop (synchronous via
 *      pw_loop_invoke) -> after this, no more dispatches on
 *      old_loop for this node. Remote proxies skip this step --
 *      see "remote-node activation preservation" below for why.
 *   2. for each peer P that has cached our old system, pw_loop_invoke
 *      on P's owning loop to update P->target.system -- runs on P's
 *      loop so no trigger is in flight while the pointer flips
 *   3. update node->rt.target.system / node->data_loop atomically from
 *      the main loop
 *   4. refresh src_system on every outbound peer so the
 *      same-loop fast path in trigger_target_v1 reflects the
 *      producer's new loop assignment
 *   5. (non-remote only) prepare on new_loop (synchronous) ->
 *      for non-remote nodes the source is re-added to the new
 *      loop's epoll set. do_node_prepare drains the eventfd once
 *      before adding, so any peer write that landed during the
 *      migration window is consumed; the affected cycle is
 *      reported as a missed wake by the driver's per-target reset
 *      on the next cycle and the graph recovers from there.
 *   6. emit data_loop_changed so listeners can rebind their own
 *      loop-tied resources (notably client-node.c's wake-back
 *      source on impl->data_source).
 *   7. refresh PW_KEY_NODE_LOOP_TID by running do_gettid on new_loop
 *
 * fd lifetime across migration
 * ----------------------------
 *
 * The eventfd backing node->source.fd is kept open across migration.
 * The earlier design closed the old fd and allocated a new one on
 * new_loop->system; that was unnecessary -- the fd is a counter, not
 * a per-loop resource, and the spa_loop_*_source pair already handles
 * the epoll-set membership move via do_node_unprepare /
 * do_node_prepare. Keeping the fd has two payoffs:
 *
 *   - Remote-proxy migration becomes free of protocol coordination.
 *     client_node_resource_transport published node->source.fd to
 *     the client via SCM_RIGHTS; the client's dup keeps working as
 *     long as the underlying eventfd exists. The previous
 *     close-and-recreate forced a protocol round-trip to publish the
 *     new fd and was the load-bearing reason fusion_node_eligible
 *     refused remote nodes. With the fd preserved, that refusal can
 *     be lifted (see context.c::fusion_node_eligible).
 *
 * *   - The peer fd-update invoke shrinks to a system-pointer write.
 *     Inbound peers no longer have to flip an int fd value -- only
 *     the spa_system handle, which is what trigger_target_v1 uses
 *     to dispatch eventfd_write through the new loop's system
 *     implementation. The peer's target.fd is the same kernel fd as
 *     before, so the write hits the same eventfd object.
 *
 * Remote-node activation preservation
 * -----------------------------------
 *
 * For non-remote nodes the do_node_unprepare / do_node_prepare pair
 * around the migration is what removes node->source from the old
 * loop's epoll set and re-adds it on the new loop. As a side effect
 * it also resets the activation status: do_node_unprepare XCHGs the
 * status to INACTIVE, do_node_prepare sets it back to FINISHED. The
 * window between the two is racy: any producer trying to trigger
 * the node during that window sees status=INACTIVE, its
 * NOT_TRIGGERED->TRIGGERED CAS in trigger_target_v1 fails, and the
 * cycle is silently dropped. For locally-owned nodes this is
 * acceptable -- the next driver cycle resets all targets to
 * NOT_TRIGGERED and the graph recovers, costing one missed cycle
 * per migration.
 *
 * For remote-proxy nodes the same trick would land a perceptible
 * xrun on the *other* side of the protocol: the client process is
 * waiting for an eventfd wake that we suppressed by failing the
 * CAS, and it cannot tell the difference between "trigger lost in
 * migration" and "graph genuinely paused this cycle". Skipping
 * do_node_unprepare / do_node_prepare for remote nodes preserves
 * the activation status across migration -- the proxy's source
 * was never on a daemon-side loop anyway (do_node_prepare's
 * `if (!this->remote)` guard), so the only thing those calls did
 * was the status flip and the symmetric target_list deactivate /
 * activate. Skipping both keeps producers triggering the proxy
 * uninterrupted; the wake-back source on the daemon side is
 * relocated separately by the data_loop_changed listener in
 * client-node.c, which is the one resource on the daemon that
 * actually moves loops for a remote proxy.
 */

/* Payload of the inbound-target system-update invoke. Carries the
 * migrating node's id (the discriminator for the targets we want to
 * touch on the producer's loop) and the migrating node's new
 * spa_system handle. The migrating node's fd does not move across
 * migration -- see the fd-lifetime block in the file header above
 * -- so the fd field on each matched target stays valid. */
struct peer_target_update {
	uint32_t            target_id;
	struct spa_system  *system;
};

/* Runs on the producer's data-loop thread. Scans the producer's
 * rt.target_list for every entry that points at the migrating node
 * (matched by target.id) and rewrites target.system to the migrating
 * node's new spa_system. Walking rt.target_list -- rather than the
 * higher-level peer_list -- catches both kinds of producer-side
 * outbound targets:
 *
 *   - daemon-side peers created by pw_node_peer_ref (the regular
 *     graph link and the driver-follower glue);
 *   - client-side links created by client_node_set_activation in
 *     module-client-node/remote-node.c, which add to the producer's
 *     rt.target_list directly without ever passing through
 *     pw_node_peer_ref.
 *
 * Both kinds are touched in one pass, which is the load-bearing
 * property for fusion-driven cross-context migrations: when a
 * client's local pw_impl_node moves to a different data loop, its
 * old loop is released and its spa_system goes away. Anyone still
 * holding the old spa_system in a cached target would segfault on
 * the next eventfd_write. */
static int do_update_target_system(struct spa_loop *loop, bool async,
		uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct pw_impl_node *producer = user_data;
	const struct peer_target_update *u = data;
	struct pw_node_target *t;

	spa_list_for_each(t, &producer->rt.target_list, link) {
		if (t->id != u->target_id)
			continue;
		pw_log_debug("producer %p target.id=%u: system %p -> %p "
				"(fd %d unchanged)",
				producer, t->id, t->system, u->system,
				t->fd);
		t->system = u->system;
	}
	return 0;
}

/* Walk every node in the context and tell each producer's data
 * loop to rewrite its target.system pointers that point at the
 * migrating node. Includes a self-walk so a node that is both
 * producer and consumer for some peer (the driver-follower glue
 * uses this) gets its bookkeeping coherent.
 *
 * We do not pre-filter to nodes that have at least one matching
 * target -- the inner walk is cheap and a precise filter would
 * require accessing the producer's rt.target_list outside its
 * data-loop thread, which is the very contract this invoke is
 * trying to honour. */
static int update_inbound_peers(struct pw_impl_node *node,
		struct spa_system *new_system)
{
	struct pw_context *context = node->context;
	struct pw_impl_node *other;
	struct peer_target_update upd = {
		.target_id = node->info.id,
		.system = new_system,
	};

	spa_list_for_each(other, &context->node_list, link) {
		if (other->data_loop == NULL)
			continue;
		pw_loop_invoke(other->data_loop, do_update_target_system,
				SPA_ID_INVALID, &upd, sizeof(upd),
				true, other);
	}
	return 0;
}

SPA_EXPORT
int pw_impl_node_set_data_loop(struct pw_impl_node *node, struct pw_loop *new_loop)
{
	struct pw_loop *old_loop;
	struct spa_system *new_system;

	if (node == NULL || new_loop == NULL)
		return -EINVAL;

	/* Refuse to relocate a remote-proxy node from the daemon side.
	 * The proxy is the daemon's stand-in for a node whose real
	 * process callback runs in another pw_context (in-process for
	 * daemon-loaded modules, cross-process for external clients).
	 * Moving the proxy's daemon-side data loop without moving the
	 * owning process's actual data-loop thread would advertise
	 * fusion bookkeeping that does not reflect any real
	 * consolidation of work, and the owning side's thread is the
	 * only thing module-deadline (which reads node.loop.tid) can
	 * use to group SCHED_DEADLINE budgets.
	 *
	 * The remote-aware path lives in the fusion pass and in the
	 * client-node protocol: when the daemon decides a proxy
	 * should join a co-location group, it stamps the desired
	 * group on the proxy's properties and emits the
	 * pw_client_node set_loop_group event; the owner reacquires
	 * its own data loop with that group and publishes its new
	 * thread back through the standard info-update path, so the
	 * proxy's PW_KEY_NODE_LOOP_TID then reflects the actual
	 * thread. The daemon-side proxy never moves. */
	if (node->remote)
		return -EINVAL;

	old_loop = node->data_loop;
	if (old_loop == new_loop)
		return 0;

	/* Refusing to move a node attached to the main loop keeps the
	 * relocation surface limited to the dynamic-data-loop case.
	 * Main-loop-bound nodes have no separate data-loop thread to
	 * receive sched parameters anyway. */
	if (old_loop == node->context->main_loop ||
	    new_loop == node->context->main_loop)
		return -EINVAL;

	new_system = new_loop->system;

	pw_log_info("%p: relocating from loop:'%s' to loop:'%s' "
			"(fd=%d kept across migration)",
			node, old_loop->name, new_loop->name,
			node->source.fd);

	/* 1. quiesce the source on the old loop: remove node->source
	 *    from old_loop's epoll set and do a full state-machine
	 *    reset (status -> INACTIVE, targets deactivated). */
	pw_loop_invoke(old_loop, do_node_unprepare, 1, NULL, 0, true, node);

	/* 2. publish the new spa_system to inbound peers. The fd does
	 *    not move (see the fd-lifetime block in this file's header),
	 *    so trigger_target_v1's spa_system_eventfd_write will hit
	 *    the same kernel eventfd; only the routing of the write
	 *    through the new loop's system implementation flips. */
	update_inbound_peers(node, new_system);

	/* 3. flip the node's own bookkeeping. node->source.fd and
	 *    node->rt.target.fd are NOT touched -- they point at the
	 *    same eventfd as before; only the system handle is swapped
	 *    so subsequent eventfd reads (the drain in do_node_prepare,
	 *    notably) go through new_loop's spa_system. */
	node->rt.target.system = new_system;
	node->data_loop = new_loop;

	/* 4. refresh src_system on every outbound peer.
	 *
	 *    src_system encodes "the producer dispatches from this
	 *    spa_system" for the same-loop fast path in
	 *    trigger_target_v1 (private.h). Migrating the producer
	 *    (= this node) means its outbound peers' src_system, which
	 *    used to point at old_system, is now stale and must follow
	 *    node->data_loop to new_system, or else the fast path
	 *    would either false-positive (dispatch inline on a thread
	 *    the consumer is no longer registered with) or
	 *    false-negative (miss a now-coalescible same-loop hop
	 *    after the producer landed on the consumer's loop).
	 *
	 *    Peers in node->peer_list are read by the producer's data
	 *    loop during trigger_targets. The producer's loop is
	 *    quiesced through this whole window (steps 1 and 5), so
	 *    the main-thread write here is unobserved by any data-
	 *    loop iteration.
	 *
	 *    Peers whose src_system was already NULL (peer creation
	 *    happened with context.inline-dispatch=false, or either
	 *    endpoint was remote/exported/driver, or onode->data_loop
	 *    was unset at the time) are left at NULL so the opt-out
	 *    remains sticky across migrations. */
	{
		struct pw_node_peer *peer;
		spa_list_for_each(peer, &node->peer_list, link) {
			if (peer->target.src_system != NULL)
				peer->target.src_system = new_system;
		}
	}

	/* 5. re-arm the source on the new loop. do_node_prepare
	 *    drains the eventfd once before adding the source, so any
	 *    peer write that landed during the migration window is
	 *    consumed; the affected cycle is reported as a missed wake
	 *    by the driver's per-target reset on the next cycle and
	 *    the graph recovers from there. */
	pw_loop_invoke(new_loop, do_node_prepare, 1, NULL, 0, true, node);

	/* 6. notify listeners that the data_loop has been swapped.
	 *    Fires BEFORE info_changed: listeners use this hook to
	 *    rebind loop-tied resources they registered themselves.
	 *    Doing it here -- after do_node_prepare on the new loop
	 *    has returned -- ensures the listener can pw_loop_invoke
	 *    on both old and new loops without re-entering the
	 *    migration that is still in flight. */
	pw_impl_node_emit_data_loop_changed(node, old_loop, new_loop);

	/* 7. refresh the published TID and propagate it. do_gettid
	 *    writes PW_KEY_NODE_LOOP_TID via pw_properties_setf, which
	 *    bypasses pw_impl_node_update_properties and therefore does
	 *    not bump node->info.change_mask. Without the PROPS bit set,
	 *    the remote-node info-changed listener that forwards property
	 *    deltas to the server (pw_client_node_update with
	 *    SPA_NODE_CHANGE_MASK_PROPS) leaves the server-side proxy
	 *    pinned to the dead TID of the old pthread, and any
	 *    consumer that drives kernel scheduling off the published TID
	 *    (sched_setattr, sched_setaffinity) hits ESRCH every cycle. */
	pw_loop_invoke(new_loop, do_gettid, SPA_ID_INVALID, NULL, 0, true,
			node->properties);
	node->info.change_mask |= PW_NODE_CHANGE_MASK_PROPS;
	emit_info_changed(node, false);

	pw_log_info("%p: relocation complete, new loop:'%s' new tid:%s",
			node, new_loop->name,
			pw_properties_get(node->properties, PW_KEY_NODE_LOOP_TID));

	return 0;
}

SPA_EXPORT
const struct pw_node_info *pw_impl_node_get_info(struct pw_impl_node *node)
{
	return &node->info;
}

SPA_EXPORT
void * pw_impl_node_get_user_data(struct pw_impl_node *node)
{
	return node->user_data;
}

SPA_EXPORT
struct pw_context * pw_impl_node_get_context(struct pw_impl_node *node)
{
	return node->context;
}

SPA_EXPORT
struct pw_global *pw_impl_node_get_global(struct pw_impl_node *node)
{
	return node->global;
}

SPA_EXPORT
const struct pw_properties *pw_impl_node_get_properties(struct pw_impl_node *node)
{
	return node->properties;
}

static int update_properties(struct pw_impl_node *node, const struct spa_dict *dict, bool filter)
{
	static const char * const ignored[] = {
		PW_KEY_OBJECT_ID,
		PW_KEY_MODULE_ID,
		PW_KEY_FACTORY_ID,
		PW_KEY_CLIENT_ID,
		PW_KEY_DEVICE_ID,
		NULL
	};

	int changed;

	changed = pw_properties_update_ignore(node->properties, dict, filter ? ignored : NULL);

	pw_log_debug("%p: updated %d properties", node, changed);

	if (changed) {
		check_properties(node);
		node->info.change_mask |= PW_NODE_CHANGE_MASK_PROPS;
	}
	return changed;
}

SPA_EXPORT
int pw_impl_node_update_properties(struct pw_impl_node *node, const struct spa_dict *dict)
{
	int changed = update_properties(node, dict, false);
	emit_info_changed(node, false);
	return changed;
}

static void node_info(void *data, const struct spa_node_info *info)
{
	struct pw_impl_node *node = data;
	uint32_t changed_ids[MAX_PARAMS], n_changed_ids = 0;
	bool flags_changed = false;

	node->info.max_input_ports = info->max_input_ports;
	node->info.max_output_ports = info->max_output_ports;

	pw_log_debug("%p: flags:%08"PRIx64" change_mask:%08"PRIx64" max_in:%u max_out:%u",
			node, info->flags, info->change_mask, info->max_input_ports,
			info->max_output_ports);

	if (info->change_mask & SPA_NODE_CHANGE_MASK_FLAGS) {
		if (node->spa_flags != info->flags) {
			flags_changed = node->spa_flags != 0;
			pw_log_debug("%p: flags %"PRIu64"->%"PRIu64, node, node->spa_flags, info->flags);
			node->spa_flags = info->flags;
		}
	}
	if (info->change_mask & SPA_NODE_CHANGE_MASK_PROPS) {
		update_properties(node, info->props, true);
	}
	if (info->change_mask & SPA_NODE_CHANGE_MASK_PARAMS) {
		uint32_t i;

		node->info.change_mask |= PW_NODE_CHANGE_MASK_PARAMS;
		node->info.n_params = SPA_MIN(info->n_params, SPA_N_ELEMENTS(node->params));

		for (i = 0; i < node->info.n_params; i++) {
			uint32_t id = info->params[i].id;

			pw_log_debug("%p: param %d id:%d (%s) %08x:%08x", node, i,
					id, spa_debug_type_find_name(spa_type_param, id),
					node->info.params[i].flags, info->params[i].flags);

			node->info.params[i].id = info->params[i].id;
			if (node->info.params[i].flags == info->params[i].flags)
				continue;

			pw_log_debug("%p: update param %d", node, id);
			node->info.params[i] = info->params[i];
			node->info.params[i].user = 0;

			if (info->params[i].flags & SPA_PARAM_INFO_READ)
				changed_ids[n_changed_ids++] = id;
		}
	}
	emit_info_changed(node, flags_changed);

	if (n_changed_ids > 0)
		emit_params(node, changed_ids, n_changed_ids);

	if (flags_changed)
		pw_context_recalc_graph(node->context, "node flags changed");
}

static void node_port_info(void *data, enum spa_direction direction, uint32_t port_id,
		const struct spa_port_info *info)
{
	struct pw_impl_node *node = data;
	struct pw_impl_port *port = pw_impl_node_find_port(node, direction, port_id);

	if (info == NULL) {
		if (port) {
			pw_log_debug("%p: %s port %d removed", node,
					pw_direction_as_string(direction), port_id);
			pw_impl_port_destroy(port);
		} else {
			pw_log_warn("%p: %s port %d unknown", node,
					pw_direction_as_string(direction), port_id);
		}
	} else if (port) {
		pw_log_debug("%p: %s port %d changed", node,
				pw_direction_as_string(direction), port_id);
		pw_impl_port_update_info(port, info);
	} else {
		int res;

		pw_log_debug("%p: %s port %d added", node,
				pw_direction_as_string(direction), port_id);

		if ((port = pw_context_create_port(node->context, direction, port_id, info,
					node->port_user_data_size))) {
			if ((res = pw_impl_port_add(port, node)) < 0) {
				pw_log_error("%p: can't add port %p: %d, %s",
						node, port, res, spa_strerror(res));
				pw_impl_port_destroy(port);
			}
		}
	}
}

static void node_result(void *data, int seq, int res, uint32_t type, const void *result)
{
	struct pw_impl_node *node = data;
	struct impl *impl = SPA_CONTAINER_OF(node, struct impl, this);

	pw_log_trace("%p: result seq:%d res:%d type:%u", node, seq, res, type);
	if (res < 0)
		impl->last_error = res;

	if (SPA_RESULT_IS_ASYNC(seq))
	        pw_work_queue_complete(impl->work, &impl->this, SPA_RESULT_ASYNC_SEQ(seq), res);

	pw_impl_node_emit_result(node, seq, res, type, result);
}

static void handle_request_process_command(struct pw_impl_node *node, const struct spa_command *command)
{
	struct impl *impl = SPA_CONTAINER_OF(node, struct impl, this);
	if (node->driving) {
		pw_log_debug("request process %d %d", node->info.state, impl->pending_state);
		if (node->info.state == PW_NODE_STATE_RUNNING) {
			spa_node_send_command(node->driver_node->node, command);
		} else if (impl->pending_state == PW_NODE_STATE_RUNNING) {
			spa_clear_ptr(impl->pending_request_process, free);
			impl->pending_request_process = (struct spa_command*)spa_pod_copy(&command->pod);
		}
	}
}

static void node_event(void *data, const struct spa_event *event)
{
	struct pw_impl_node *node = data;
	struct impl *impl = SPA_CONTAINER_OF(node, struct impl, this);
	uint32_t id = SPA_NODE_EVENT_ID(event);

	pw_log_debug("%p: event %d (%s)", node, id,
		    spa_debug_type_find_name(spa_type_node_event_id, id));

	switch (id) {
	case SPA_NODE_EVENT_Error:
		impl->last_error = -EFAULT;
		node_update_state(node, PW_NODE_STATE_ERROR,
				-EFAULT, strdup("Received error event"));
		break;
	case SPA_NODE_EVENT_RequestProcess:
		if (!node->driving && !node->exported) {
			struct spa_command *command;
			size_t size = SPA_POD_SIZE(&event->pod);

			/* turn the event and all the arguments into a command */
			command = alloca(size);
			memcpy(command, event, size);
			command->body.body.type = SPA_TYPE_COMMAND_Node;
			command->body.body.id = SPA_NODE_COMMAND_RequestProcess;

			/* send the request process to the driver but only on the
			 * server size */
			handle_request_process_command(node->driver_node, command);
		}
		break;
	default:
		pw_log_debug("unhandled event %d", SPA_NODE_EVENT_ID(event));
		break;
	}
	pw_impl_node_emit_event(node, event);
}

static const struct spa_node_events node_events = {
	SPA_VERSION_NODE_EVENTS,
	.info = node_info,
	.port_info = node_port_info,
	.result = node_result,
	.event = node_event,
};

#define SYNC_CHECK	0
#define SYNC_START	1
#define SYNC_STOP	2

static inline int check_updates(struct pw_impl_node *node, uint32_t *reposition_owner)
{
	int res = SYNC_CHECK;
	struct pw_node_activation *a = node->rt.target.activation;
	uint32_t command;

	if (SPA_UNLIKELY(a->position.offset == INT64_MIN))
		a->position.offset = a->position.clock.position;

	command = SPA_ATOMIC_XCHG(a->command, PW_NODE_ACTIVATION_COMMAND_NONE);
	*reposition_owner = SPA_ATOMIC_XCHG(a->reposition_owner, 0);

	if (SPA_UNLIKELY(command != PW_NODE_ACTIVATION_COMMAND_NONE)) {
		pw_log_debug("%p: update command:%u", node, command);
		switch (command) {
		case PW_NODE_ACTIVATION_COMMAND_STOP:
			a->position.state = SPA_IO_POSITION_STATE_STOPPED;
			res = SYNC_STOP;
			break;
		case PW_NODE_ACTIVATION_COMMAND_START:
			a->position.state = SPA_IO_POSITION_STATE_STARTING;
			a->sync_left = a->sync_timeout /
				((a->position.clock.duration * SPA_NSEC_PER_SEC) /
				 a->position.clock.rate.denom);
			res = SYNC_START;
			break;
		}
	}
	return res;
}

static void do_reposition(struct pw_impl_node *driver, struct pw_node_target *target)
{
	struct pw_node_activation *a = driver->rt.target.activation;
	struct spa_io_segment *dst, *src;

	src = &target->activation->reposition;
	dst = &a->position.segments[0];

	pw_log_info("%p: %u update position:%"PRIu64, driver, target->id, src->position);

	dst->version = src->version;
	dst->flags = src->flags;
	dst->start = src->start;
	dst->duration = src->duration;
	dst->rate = src->rate;
	dst->position = src->position;
	if (src->bar.flags & SPA_IO_SEGMENT_BAR_FLAG_VALID)
		dst->bar = src->bar;
	if (src->video.flags & SPA_IO_SEGMENT_VIDEO_FLAG_VALID)
		dst->video = src->video;

	if (dst->start == 0)
		dst->start = a->position.clock.position - a->position.offset;

	switch (a->position.state) {
	case SPA_IO_POSITION_STATE_RUNNING:
		a->position.state = SPA_IO_POSITION_STATE_STARTING;
		a->sync_left = a->sync_timeout /
			((a->position.clock.duration * SPA_NSEC_PER_SEC) /
			 a->position.clock.rate.denom);
		break;
	}
}

static inline void update_position(struct pw_impl_node *node, int all_ready, uint64_t nsec)
{
	struct pw_node_activation *a = node->rt.target.activation;

	if (SPA_UNLIKELY(a->position.state == SPA_IO_POSITION_STATE_STARTING)) {
		if (!all_ready && --a->sync_left == 0) {
			pw_impl_node_rt_emit_timeout(node);
			debug_sync_timeout(node, nsec);
			all_ready = true;
		}
		if (all_ready)
			a->position.state = SPA_IO_POSITION_STATE_RUNNING;
	}
	if (SPA_LIKELY(a->position.state == SPA_IO_POSITION_STATE_RUNNING))
		node->elapsed += a->position.clock.duration;

	a->position.offset = a->position.clock.position - node->elapsed;
}

/* Called from the data-loop and it is the starting point for driver nodes.
 * Most of the logic here is to check for reposition updates and transport changes.
 */
static int node_ready(void *data, int status)
{
	struct pw_impl_node *node = data;
	struct pw_impl_node *driver = node->driver_node;
	struct pw_node_activation *a = node->rt.target.activation;
	struct pw_node_activation_state *state = &a->state[0];
	struct spa_system *data_system = node->rt.target.system;
	struct pw_node_target *t, *reposition_target = NULL;;
	struct pw_impl_port *p;
	struct spa_io_clock *cl = &node->rt.position->clock;
	int sync_type, all_ready, update_sync, target_sync, old_status;
	uint32_t owner[2], reposition_owner, pending;
	uint64_t min_timeout = UINT64_MAX, nsec, cpu_nsec;

	pw_log_trace_fp("%p: ready driver:%d exported:%d %p status:%d prepared:%d", node,
			node->driver, node->exported, driver, status, node->rt.prepared);

	if (SPA_UNLIKELY(!node->rt.prepared)) {
		/* This can happen when we are stopping a node and removed it from the
		 * graph but we still have not completed the Pause/Suspend command on
		 * the node. In that case, the node might still emit ready events,
		 * which we should simply ignore here. */
		pw_log_info("%p: ready non-active node %s in state %d", node, node->name, node->info.state);
		return -EIO;
	}
	if (SPA_UNLIKELY(node != driver)) {
		pw_log_warn("%p: ready non-driver node %s", node, node->name);
		return -EIO;
	}

	nsec = get_time_ns(data_system);
	cpu_nsec = get_cputime_ns(data_system);

	a->driver_start_time    = nsec;
	a->driver_start_cputime = cpu_nsec;

	if (!node->remote) {
		int cur_tid = (int)gettid();
		if (SPA_UNLIKELY(node->cycle_fd < 0 ||
				 node->cycle_tid != cur_tid)) {
			if (node->cycle_fd >= 0)
				close(node->cycle_fd);
			node->cycle_fd = pw_cycle_counter_open();
			node->cycle_tid = cur_tid;
			if (node->cycle_fd < 0)
				node->cycle_fd = -2;
		}
		a->driver_start_cycles = pw_cycle_counter_read(node->cycle_fd);
	} else {
		a->driver_start_cycles = 0;
	}

	while (true) {
		old_status = SPA_ATOMIC_LOAD(a->status);
		if (SPA_LIKELY(old_status == PW_NODE_ACTIVATION_FINISHED))
			/* all good, graph completed */
			break;
		if (SPA_ATOMIC_CAS(a->status, old_status, PW_NODE_ACTIVATION_TRIGGERED)) {
			/* if we got triggered but did not run the processing yet we don't
			 * really have an error so we can skip the error reporting. We need
			 * to run recovery anyway because the ready callback is already
			 * emitted */
			if (old_status != PW_NODE_ACTIVATION_TRIGGERED) {
				/* otherwise, something was wrong and we debug */
				debug_xrun_graph(node, nsec, old_status);
				pw_impl_node_rt_emit_incomplete(driver);
			}
			SPA_FLAG_SET(cl->flags, SPA_IO_CLOCK_FLAG_XRUN_RECOVER);
			process_node(node, nsec, cpu_nsec);
			SPA_FLAG_CLEAR(cl->flags, SPA_IO_CLOCK_FLAG_XRUN_RECOVER);
			break;
		}
	}

	sync_type = check_updates(node, &reposition_owner);
	owner[0] = SPA_ATOMIC_LOAD(a->segment_owner[0]);
	owner[1] = SPA_ATOMIC_LOAD(a->segment_owner[1]);
again:
	all_ready = sync_type == SYNC_CHECK;
	update_sync = !all_ready;
	target_sync = sync_type == SYNC_START ? true : false;
	pending = 0;

	spa_list_for_each(t, &driver->rt.target_list, link) {
		struct pw_node_activation *ta = t->activation;
		uint32_t id = t->id;

		ta->driver_id = driver->info.id;
retry_status:
		pw_node_activation_state_reset(&ta->state[0]);

		if (ta->active_driver_id != ta->driver_id) {
			pw_log_trace_fp("%p: (%s-%u) %d waiting for driver %d<>%d", t->node,
					t->name, t->id, ta->status,
					ta->active_driver_id, ta->driver_id);
			continue;
		}

		/* we don't change the state of inactive nodes and don't use them
		 * for reposition. The pending will be at least 1 and they might
		 * get decremented to 0 but since the status is inactive, we don't
		 * do the atomic CAS from NOT_TRIGGERED to TRIGGERED and we don't
		 * write the eventfd. */
		old_status = SPA_ATOMIC_LOAD(ta->status);
		if (SPA_UNLIKELY(old_status == PW_NODE_ACTIVATION_INACTIVE))
			continue;

		/* if this fails, the node might just have stopped and we need to retry */
		if (SPA_UNLIKELY(!SPA_ATOMIC_CAS(ta->status, old_status, PW_NODE_ACTIVATION_NOT_TRIGGERED)))
			goto retry_status;

		if (!SPA_FLAG_IS_SET(ta->flags, PW_NODE_ACTIVATION_FLAG_ASYNC))
			pending++;

		if (old_status == PW_NODE_ACTIVATION_TRIGGERED ||
		    old_status == PW_NODE_ACTIVATION_AWAKE) {
			update_xrun_stats(ta, 1, nsec / 1000, 0);
			debug_xrun_target(node, t, old_status, nsec);
		}

		/* this is the node with reposition info */
		if (SPA_UNLIKELY(id == reposition_owner))
			reposition_target = t;

		/* update extra segment info if it is the owner */
		if (SPA_UNLIKELY(id == owner[0]))
			a->position.segments[0].bar = ta->segment.bar;
		if (SPA_UNLIKELY(id == owner[1]))
			a->position.segments[0].video = ta->segment.video;

		min_timeout = SPA_MIN(min_timeout, ta->sync_timeout);

		if (SPA_UNLIKELY(update_sync)) {
			ta->pending_sync = target_sync;
			ta->pending_new_pos = target_sync;
		} else {
			all_ready &= ta->pending_sync == false;
		}
		ta->prev_signal_time = ta->signal_time;
		ta->prev_awake_time = ta->awake_time;
		ta->prev_finish_time = ta->finish_time;
		/* prev_run_time and prev_run_cycles are now computed
		 * at the completion point by the node itself (see
		 * process_node for local nodes, signal_sync for JACK
		 * clients).  Propagate cycles to the node's own
		 * activation so the profiler can always find them
		 * via tn->rt.target.activation regardless of which
		 * target entry it iterates. */
		if (ta->prev_run_cycles != 0 && t->node != NULL &&
		    t->node->rt.target.activation != ta)
			t->node->rt.target.activation->prev_run_cycles =
				ta->prev_run_cycles;
	}

	node->driver_start = nsec;

	a->sync_timeout = SPA_MIN(min_timeout, DEFAULT_SYNC_TIMEOUT);

	if (SPA_UNLIKELY(reposition_target != NULL)) {
		do_reposition(node, reposition_target);
		sync_type = SYNC_START;
		reposition_owner = 0;
		reposition_target = NULL;
		goto again;
	}
	state->pending = pending;

	update_position(node, all_ready, nsec);

	/* move output with previous cycle, this makes the async nodes
	 * pick up the new data immediately */
	if (status & SPA_STATUS_HAVE_DATA) {
		spa_list_for_each(p, &node->rt.output_mix, rt.node_link)
			spa_node_process_fast(p->mix);
	}

	a->position.clock.cycle++;
	pw_impl_node_rt_emit_start(node);

	/* now signal all the nodes we drive */
	trigger_targets(node, status, nsec);
	return 0;
}

static int node_reuse_buffer(void *data, uint32_t port_id, uint32_t buffer_id)
{
	struct pw_impl_node *node = data;
	struct pw_impl_port *p;

	spa_list_for_each(p, &node->rt.input_mix, rt.node_link) {
		if (p->port_id != port_id)
			continue;
		spa_node_port_reuse_buffer(p->mix, 0, buffer_id);
		break;
	}
	return 0;
}

static int node_xrun(void *data, uint64_t trigger, uint64_t delay, struct spa_pod *info)
{
	struct pw_impl_node *this = data;
	struct pw_node_activation *a = this->rt.target.activation;
	struct spa_system *data_system = this->rt.target.system;
	uint64_t nsec = get_time_ns(data_system);
	int suppressed;

	update_xrun_stats(a, 1, trigger, delay);

	if ((suppressed = spa_ratelimit_test(&this->rt.rate_limit, nsec)) >= 0) {
		struct spa_fraction rate;
		if (a) {
			struct spa_io_clock *cl = &a->position.clock;
			rate.num = cl->rate.num * cl->duration;
			rate.denom = cl->rate.denom;
		} else {
			rate = SPA_FRACTION(0,0);
		}
		pw_log_info("(%s-%d) XRun! rate:%u/%u count:%u time:%"PRIu64
				" delay:%"PRIu64" max:%"PRIu64" (%d suppressed)",
				this->name, this->info.id,
				rate.num, rate.denom, a->xrun_count,
				trigger, delay, a->max_delay,
				suppressed);
	}

	pw_impl_node_rt_emit_xrun(this);

	return 0;
}

static const struct spa_node_callbacks node_callbacks = {
	SPA_VERSION_NODE_CALLBACKS,
	.ready = node_ready,
	.reuse_buffer = node_reuse_buffer,
	.xrun = node_xrun,
};

static int handle_node_param(struct pw_impl_node *node, const char *key, const char *value)
{
	const struct spa_type_info *ti;
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	struct spa_pod *pod;
	int res;

	ti = spa_debug_type_find_short(spa_type_param, key);
	if (ti == NULL)
		return -ENOENT;

	if ((res = spa_json_to_pod(&b, 0, ti, value, strlen(value))) < 0)
		return res;

	if ((pod = spa_pod_builder_deref(&b, 0)) == NULL)
		return -ENOSPC;

	if ((res = pw_impl_node_set_param(node, ti->type, 0, pod)) < 0)
		return res;

	return 0;
}

SPA_EXPORT
int pw_impl_node_set_implementation(struct pw_impl_node *node,
			struct spa_node *spa_node)
{
	int res;
	const struct spa_dict_item *it;

	pw_log_debug("%p: implementation %p", node, spa_node);

	if (node->node) {
		pw_log_error("%p: implementation existed %p", node, node->node);
		return -EEXIST;
	}

	node->node = spa_node;
	spa_node_set_callbacks(node->node, &node_callbacks, node);
	res = spa_node_add_listener(node->node, &node->listener, &node_events, node);

again:
	spa_dict_for_each(it, &node->properties->dict) {
		if (spa_strstartswith(it->key, "node.param.")) {
			if ((res = handle_node_param(node, &it->key[11], it->value)) < 0)
				pw_log_warn("can't set node param: %s", spa_strerror(res));
			pw_properties_set(node->properties, it->key, NULL);
			goto again;
		}
	}

	if (node->registered)
		update_io(node);

	return res;
}

SPA_EXPORT
struct spa_node *pw_impl_node_get_implementation(struct pw_impl_node *node)
{
	return node->node;
}

SPA_EXPORT
void pw_impl_node_add_listener(struct pw_impl_node *node,
			   struct spa_hook *listener,
			   const struct pw_impl_node_events *events,
			   void *data)
{
	spa_hook_list_append(&node->listener_list, listener, events, data);
}

struct listener_data {
	struct spa_hook *listener;
	const struct pw_impl_node_rt_events *events;
	void *data;
};

static int
do_add_rt_listener(struct spa_loop *loop,
		bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct pw_impl_node *node = user_data;
	const struct listener_data *d = data;
	spa_hook_list_append(&node->rt_listener_list,
			d->listener, d->events, d->data);
	return 0;
}

SPA_EXPORT
void pw_impl_node_add_rt_listener(struct pw_impl_node *node,
			   struct spa_hook *listener,
			   const struct pw_impl_node_rt_events *events,
			   void *data)
{
	struct listener_data d = { .listener = listener, .events = events, .data = data };
	pw_loop_locked(node->data_loop,
                       do_add_rt_listener, SPA_ID_INVALID, &d, sizeof(d), node);
}

static int do_remove_listener(struct spa_loop *loop,
		bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct spa_hook *listener = user_data;
	spa_hook_remove(listener);
	return 0;
}

SPA_EXPORT
void pw_impl_node_remove_rt_listener(struct pw_impl_node *node,
			  struct spa_hook *listener)
{
	pw_loop_locked(node->data_loop,
                       do_remove_listener, SPA_ID_INVALID, NULL, 0, listener);
}

/** Destroy a node
 * \param node a node to destroy
 *
 * Remove \a node. This will stop the transfer on the node and
 * free the resources allocated by \a node.
 */
SPA_EXPORT
void pw_impl_node_destroy(struct pw_impl_node *node)
{
	struct impl *impl = SPA_CONTAINER_OF(node, struct impl, this);
	struct pw_impl_port *port;
	struct pw_impl_node *follower;
	struct pw_context *context = node->context;
	bool active, had_driver;

	active = node->active;
	node->active = false;
	node->runnable = false;

	pw_log_debug("%p: destroy", impl);
	pw_log_info("(%s-%u) destroy", node->name, node->info.id);

	node_deactivate(node);

	suspend_node(node);

	pw_impl_node_emit_destroy(node);

	pw_log_debug("%p: driver node %p", impl, node->driver_node);
	had_driver = node != node->driver_node;

	/* remove ourself as a follower from the driver node */
	spa_list_remove(&node->follower_link);
	pw_node_peer_unref(spa_steal_ptr(node->from_driver_peer));
	pw_node_peer_unref(spa_steal_ptr(node->to_driver_peer));
	remove_segment_owner(node->driver_node, node->info.id);

	spa_list_consume(follower, &node->follower_list, follower_link) {
		pw_log_debug("%p: reassign follower %p", impl, follower);
		pw_impl_node_set_driver(follower, NULL);
	}

	if (node->registered) {
		spa_list_remove(&node->link);
		if (node->driver)
			remove_driver(context, node);
	}

	if (node->node) {
		spa_hook_remove(&node->listener);
		spa_node_set_callbacks(node->node, NULL, NULL);
	}

	pw_log_debug("%p: destroy ports", node);
	spa_list_consume(port, &node->input_ports, link)
		pw_impl_port_destroy(port);
	spa_list_consume(port, &node->output_ports, link)
		pw_impl_port_destroy(port);

	if (node->global) {
		spa_hook_remove(&node->global_listener);
		pw_global_destroy(node->global);
	}

	if (active || had_driver)
		pw_context_recalc_graph(context,
				"active node destroy");

	pw_log_debug("%p: free", node);
	pw_impl_node_emit_free(node);

	spa_hook_list_clean(&node->listener_list);

	pw_memblock_unref(node->activation);

	pw_param_clear(&impl->param_list, SPA_ID_INVALID);
	pw_param_clear(&impl->pending_list, SPA_ID_INVALID);

	pw_map_clear(&node->input_port_map);
	pw_map_clear(&node->output_port_map);

	pw_work_queue_cancel(impl->work, node, SPA_ID_INVALID);

	pw_properties_free(node->properties);
	spa_clear_ptr(impl->pending_request_process, free);

	clear_info(node);

	spa_system_close(node->rt.target.system, node->source.fd);

	if (node->cycle_fd >= 0) {
		close(node->cycle_fd);
		node->cycle_fd = -2;
	}

	/* Free the per-node subgraph-fusion sliding-window backing
	 * buffer. Allocated lazily by context.c::fusion_ensure_window_size
	 * on first WCET sample; release here on node teardown so we do
	 * not leak across destroys. */
	free(node->fusion_window.samples);
	node->fusion_window.samples = NULL;

	if (node->data_loop)
		pw_context_release_node_loop(context, node->data_loop);

	free(impl->group);
	free(impl->link_group);
	free(impl->sync_group);
	free(impl);

#ifdef HAVE_MALLOC_TRIM
	int res = malloc_trim(0);
	pw_log_debug("malloc_trim(): %d", res);
#endif
}

SPA_EXPORT
int pw_impl_node_for_each_port(struct pw_impl_node *node,
			  enum pw_direction direction,
			  int (*callback) (void *data, struct pw_impl_port *port),
			  void *data)
{
	struct spa_list *ports;
	struct pw_impl_port *p, *t;
	int res;

	if (direction == PW_DIRECTION_INPUT)
		ports = &node->input_ports;
	else
		ports = &node->output_ports;

	spa_list_for_each_safe(p, t, ports, link)
		if ((res = callback(data, p)) != 0)
			return res;
	return 0;
}

struct result_node_params_data {
	struct impl *impl;
	void *data;
	int (*callback) (void *data, int seq,
			uint32_t id, uint32_t index, uint32_t next,
			struct spa_pod *param);
	int seq;
	unsigned int cache:1;
};

static void result_node_params(void *data, int seq, int res, uint32_t type, const void *result)
{
	struct result_node_params_data *d = data;
	struct impl *impl = d->impl;
	switch (type) {
	case SPA_RESULT_TYPE_NODE_PARAMS:
	{
		const struct spa_result_node_params *r = result;
		if (d->seq == seq) {
			d->callback(d->data, seq, r->id, r->index, r->next, r->param);
			if (d->cache)
				pw_param_add(&impl->pending_list, seq, r->id, r->param);
		}
		break;
	}
	default:
		break;
	}
}

SPA_EXPORT
int pw_impl_node_for_each_param(struct pw_impl_node *node,
			   int seq, uint32_t param_id,
			   uint32_t index, uint32_t max,
			   const struct spa_pod *filter,
			   int (*callback) (void *data, int seq,
					    uint32_t id, uint32_t index, uint32_t next,
					    struct spa_pod *param),
			   void *data)
{
	int res;
	struct impl *impl = SPA_CONTAINER_OF(node, struct impl, this);
	struct result_node_params_data user_data = { impl, data, callback, seq, false };
	struct spa_hook listener;
	struct spa_param_info *pi;
	static const struct spa_node_events node_events = {
		SPA_VERSION_NODE_EVENTS,
		.result = result_node_params,
	};

	pi = pw_param_info_find(node->info.params, node->info.n_params, param_id);
	if (pi == NULL)
		return -ENOENT;

	if (max == 0)
		max = UINT32_MAX;

	pw_log_debug("%p: params id:%d (%s) index:%u max:%u cached:%d", node, param_id,
			spa_debug_type_find_name(spa_type_param, param_id),
			index, max, pi->user);

	if (pi->user == 1) {
		struct pw_param *p;
		uint8_t buffer[4096];
		struct spa_pod_dynamic_builder b;
	        struct spa_result_node_params result;
		uint32_t count = 0;

		result.id = param_id;
		result.next = 0;

		spa_list_for_each(p, &impl->param_list, link) {
			if (p->id != param_id)
				continue;

			result.index = result.next++;
			if (result.index < index)
				continue;

			spa_pod_dynamic_builder_init(&b, buffer, sizeof(buffer), 4096);

			if (spa_pod_filter(&b.b, &result.param, p->param, filter) == 0)  {
				pw_log_debug("%p: %d param %u", node, seq, result.index);
				result_node_params(&user_data, seq, 0, SPA_RESULT_TYPE_NODE_PARAMS, &result);
				count++;
			}
			spa_pod_dynamic_builder_clean(&b);

			if (count == max)
				break;
		}
		res = 0;
	} else {
		user_data.cache = impl->cache_params &&
			(filter == NULL && index == 0 && max == UINT32_MAX);

		if (user_data.cache)
			pw_param_add(&impl->pending_list, seq, param_id, NULL);

		spa_zero(listener);
		spa_node_add_listener(node->node, &listener, &node_events, &user_data);
		res = spa_node_enum_params(node->node, seq,
						param_id, index, max,
						filter);
		spa_hook_remove(&listener);

		if (user_data.cache) {
			pw_param_update(&impl->param_list, &impl->pending_list, 0, NULL);
			pi->user = 1;
		}
	}
	return res;
}

SPA_EXPORT
int pw_impl_node_set_param(struct pw_impl_node *node,
		uint32_t id, uint32_t flags, const struct spa_pod *param)
{
	pw_log_debug("%p: set_param id:%d (%s) flags:%08x param:%p", node, id,
			spa_debug_type_find_name(spa_type_param, id), flags, param);
	return spa_node_set_param(node->node, id, flags, param);
}

SPA_EXPORT
struct pw_impl_port *
pw_impl_node_find_port(struct pw_impl_node *node, enum pw_direction direction, uint32_t port_id)
{
	struct pw_impl_port *port, *p;
	struct pw_map *portmap;
	struct spa_list *ports;

	if (direction == PW_DIRECTION_INPUT) {
		portmap = &node->input_port_map;
		ports = &node->input_ports;
	} else {
		portmap = &node->output_port_map;
		ports = &node->output_ports;
	}

	if (port_id != PW_ID_ANY)
		port = pw_map_lookup(portmap, port_id);
	else {
		port = NULL;
		/* try to find an unlinked port */
		spa_list_for_each(p, ports, link) {
			if (spa_list_is_empty(&p->links)) {
				port = p;
				break;
			}
			/* We can use this port if it can multiplex */
			if (SPA_FLAG_IS_SET(p->mix_flags, PW_IMPL_PORT_MIX_FLAG_MULTI))
				port = p;
		}
	}
	pw_log_debug("%p: return %s port %d: %p", node,
			pw_direction_as_string(direction), port_id, port);
	return port;
}

SPA_EXPORT
uint32_t pw_impl_node_get_free_port_id(struct pw_impl_node *node, enum pw_direction direction)
{
	uint32_t n_ports, max_ports;
	struct pw_map *portmap;
	uint32_t port_id;
	bool dynamic;
	int res;

	if (direction == PW_DIRECTION_INPUT) {
		max_ports = node->info.max_input_ports;
		n_ports = node->info.n_input_ports;
		portmap = &node->input_port_map;
		dynamic = SPA_FLAG_IS_SET(node->spa_flags, SPA_NODE_FLAG_IN_DYNAMIC_PORTS);
	} else {
		max_ports = node->info.max_output_ports;
		n_ports = node->info.n_output_ports;
		portmap = &node->output_port_map;
		dynamic = SPA_FLAG_IS_SET(node->spa_flags, SPA_NODE_FLAG_OUT_DYNAMIC_PORTS);
	}
	pw_log_debug("%p: direction %s n_ports:%u max_ports:%u",
			node, pw_direction_as_string(direction), n_ports, max_ports);

	if (!dynamic || n_ports >= max_ports) {
		res = -ENOSPC;
		goto error;
	}

	port_id = pw_map_insert_new(portmap, NULL);
	if (port_id == SPA_ID_INVALID) {
		res = -errno;
		goto error;
	}

	pw_log_debug("%p: free port %d", node, port_id);

	return port_id;

error:
	pw_log_warn("%p: no more port available: %s", node, spa_strerror(res));
	errno = -res;
	return SPA_ID_INVALID;
}

SPA_EXPORT
struct pw_impl_port *pw_impl_node_get_free_port(struct pw_impl_node *node, enum pw_direction direction)
{
	uint32_t port_id = pw_impl_node_get_free_port_id(node, direction);
	if (port_id == SPA_ID_INVALID)
		return NULL;

	spa_node_add_port(node->node, direction, port_id, NULL);

	return pw_impl_node_find_port(node, direction, port_id);
}


static void on_state_complete(void *obj, void *data, int res, uint32_t seq)
{
	struct pw_impl_node *node = obj;
	struct impl *impl = SPA_CONTAINER_OF(node, struct impl, this);
	enum pw_node_state state = SPA_PTR_TO_INT(data);
	char *error = NULL;

	/* driver nodes added -EBUSY. This is then not an error */
	if (res == -EBUSY)
		res = 0;

	impl->pending_id = SPA_ID_INVALID;
	impl->pending_play = false;

	pw_log_debug("%p: state complete res:%d seq:%d", node, res, seq);
	if (impl->last_error < 0) {
		res = impl->last_error;
		impl->last_error = 0;
	}
	if (SPA_RESULT_IS_ERROR(res)) {
		if (node->info.state == PW_NODE_STATE_SUSPENDED) {
			state = PW_NODE_STATE_SUSPENDED;
			res = 0;
		} else {
			error = spa_aprintf("error changing node state: %s", spa_strerror(res));
			state = PW_NODE_STATE_ERROR;
		}
	}
	node_update_state(node, state, res, error);
}

/** Set the node state
 * \param node a \ref pw_impl_node
 * \param state a \ref pw_node_state
 * \return 0 on success < 0 on error
 *
 * Set the state of \a node to \a state.
 */
SPA_EXPORT
int pw_impl_node_set_state(struct pw_impl_node *node, enum pw_node_state state)
{
	int res = 0;
	struct impl *impl = SPA_CONTAINER_OF(node, struct impl, this);
	enum pw_node_state old = impl->pending_state;

	pw_log_debug("%p: set state (%s) %s -> %s, active %d pause_on_idle:%d", node,
			pw_node_state_as_string(node->info.state),
			pw_node_state_as_string(old),
			pw_node_state_as_string(state),
			node->active,
			node->pause_on_idle);

	if (old != state)
		pw_impl_node_emit_state_request(node, state);

	switch (state) {
	case PW_NODE_STATE_CREATING:
		return -EIO;

	case PW_NODE_STATE_SUSPENDED:
		res = suspend_node(node);
		break;

	case PW_NODE_STATE_IDLE:
		res = idle_node(node);
		break;

	case PW_NODE_STATE_RUNNING:
		if (node->active)
			res = start_node(node);
		break;

	case PW_NODE_STATE_ERROR:
		break;
	}
	if (SPA_RESULT_IS_ERROR(res))
		return res;

	if (SPA_RESULT_IS_ASYNC(res)) {
		res = spa_node_sync(node->node, res);
	}

	if (old != state) {
		if (impl->pending_id != SPA_ID_INVALID) {
			pw_log_debug("cancel state from %s to %s to %s",
				pw_node_state_as_string(node->info.state),
				pw_node_state_as_string(impl->pending_state),
				pw_node_state_as_string(state));

			if (impl->pending_state == PW_NODE_STATE_RUNNING &&
			    state < PW_NODE_STATE_RUNNING &&
			    impl->pending_play) {
				impl->pending_play = false;
				idle_node(node);
			}
			pw_work_queue_cancel(impl->work, node, impl->pending_id);
			node->info.state = impl->pending_state;
		}
		/* driver nodes return EBUSY to add a -EBUSY to the work queue. This
		 * will wait until all previous items in the work queue are
		 * completed */
		impl->pending_state = state;
		if (node->exported) {
			/* exported nodes must complete immediately. This is important
			 * because the server sends ping to check completion. The server
			 * will only send Start to driver nodes when all clients are
			 * ready for processing. */
			on_state_complete(node, SPA_INT_TO_PTR(state), -EBUSY, 0);
		} else {
			impl->pending_id = pw_work_queue_add(impl->work,
					node, res == EBUSY ? -EBUSY : res,
					on_state_complete, SPA_INT_TO_PTR(state));
		}
	}
	return res;
}

SPA_EXPORT
int pw_impl_node_set_active(struct pw_impl_node *node, bool active)
{
	bool old = node->active;

	if (old != active) {
		pw_log_debug("%p: %s registered:%d exported:%d", node,
				active ? "activate" : "deactivate",
				node->registered, node->exported);

		node->active = active;
		pw_impl_node_emit_active_changed(node, active);

		if (node->registered)
			pw_context_recalc_graph(node->context,
					active ? "node activate" : "node deactivate");
		else if (!active && node->exported)
			remove_node_from_graph(node);
	}
	return 0;
}

SPA_EXPORT
bool pw_impl_node_is_active(struct pw_impl_node *node)
{
	return node->active;
}

SPA_EXPORT
int pw_impl_node_send_command(struct pw_impl_node *node, const struct spa_command *command)
{
	uint32_t id = SPA_NODE_COMMAND_ID(command);
	int res = 0;

	switch (id) {
	case SPA_NODE_COMMAND_RequestProcess:
		handle_request_process_command(node, command);
		break;
	default:
		res = spa_node_send_command(node->node, command);
	}
	return res;
}
