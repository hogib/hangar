/* SPDX-License-Identifier: GPL-3.0-or-later */
/* bench/pairs/iceoryx2_graph.c: a module of a graph, in iceoryx2 (graph.h; README.md,
 * "A vehicle's graph"). Each topic is a publish-subscribe service with an event service
 * beside it, as in iceoryx2_pair.c: a publication notifies after each send, and the
 * module sleeps in one wait set to which the listener of every subscription is
 * attached, with a timeout at its next publication. That is iceoryx2's own way of
 * waiting for several things at once (its event_multiplexing example).
 *
 * Woken only. A polling module would spin a core for the whole run, and a graph of
 * eight modules would want eight cores of a vehicle computer that has four: that row
 * would measure the shortage, not iceoryx2, so it is not run, and the registry says
 * so. Buffers of 16 with safe overflow, as in the pair. */
#include "graph.h"

#include "iox2/iceoryx2.h"

#ifndef IOX2_VERSION
#define IOX2_VERSION "unknown"
#endif

static const struct graph_opts *O;
static iox2_node_h node;
static iox2_waitset_h waitset;
static struct {
	iox2_port_factory_pub_sub_h svc;
	iox2_port_factory_event_h ev;
	iox2_publisher_h pub;
	iox2_notifier_h notifier;
	iox2_sample_mut_h lent;
} P[GRAPH_MAX];
static struct {
	iox2_port_factory_pub_sub_h svc;
	iox2_port_factory_event_h ev;
	iox2_subscriber_h sub;
	iox2_listener_h listener;
	iox2_waitset_guard_h guard;
} S[GRAPH_MAX];

static int fail(const char *what, const char *topic, int rc)
{
	fprintf(stderr, "iceoryx2_graph: %s: %s (error %d)\n", topic, what, rc);
	return -1;
}

static int services(const struct graph_end *e, iox2_port_factory_pub_sub_h *svc, iox2_port_factory_event_h *ev)
{
	char name[160], type[48];
	iox2_service_name_h sn = NULL;
	int rc;
	snprintf(name, sizeof name, "graph/%s", e->topic);
	if ((rc = iox2_service_name_new(NULL, name, strlen(name), &sn)) != IOX2_OK)
		return fail("the service name was refused", e->topic, rc);
	iox2_service_builder_pub_sub_h b =
		iox2_service_builder_pub_sub(iox2_node_service_builder(&node, NULL, iox2_cast_service_name_ptr(sn)));
	snprintf(type, sizeof type, "pair_u64x%u", e->size / 8);
	rc = iox2_service_builder_pub_sub_set_payload_type_details(&b, iox2_type_variant_e_FIXED_SIZE, type, strlen(type),
								   e->size, 8);
	if (rc != IOX2_OK)
		return fail("the payload type was refused", e->topic, rc);
	iox2_service_builder_pub_sub_set_subscriber_max_buffer_size(&b, 16);
	iox2_service_builder_pub_sub_set_history_size(&b, 0);
	iox2_service_builder_pub_sub_set_max_subscribers(&b, 16);
	iox2_service_builder_pub_sub_set_max_nodes(&b, 32);
	iox2_service_builder_pub_sub_set_enable_safe_overflow(&b, true);
	rc = iox2_service_builder_pub_sub_open_or_create(b, NULL, svc);
	iox2_service_name_drop(sn);
	if (rc != IOX2_OK)
		return fail("the publish-subscribe service could not be opened or created", e->topic, rc);
	snprintf(name, sizeof name, "graph/%s/event", e->topic);
	if ((rc = iox2_service_name_new(NULL, name, strlen(name), &sn)) != IOX2_OK)
		return fail("the event service's name was refused", e->topic, rc);
	rc = iox2_service_builder_event_open_or_create(
		iox2_service_builder_event(iox2_node_service_builder(&node, NULL, iox2_cast_service_name_ptr(sn))), NULL, ev);
	iox2_service_name_drop(sn);
	if (rc != IOX2_OK)
		return fail("the event service could not be opened or created", e->topic, rc);
	return 0;
}

static int open_all(const struct graph_opts *o)
{
	O = o;
	if (o->argc) {
		fprintf(stderr, "iceoryx2_graph: unknown option %s\n", o->argv[0]);
		return -1;
	}
	iox2_set_log_level_from_env_or(iox2_log_level_e_ERROR);
	int rc = iox2_node_builder_create(iox2_node_builder_new(NULL), NULL, iox2_service_type_e_IPC, &node);
	if (rc != IOX2_OK)
		return fail("the node could not be created", o->module, rc);
	for (int i = 0; i < o->npub; i++) {
		if (services(&o->pub[i], &P[i].svc, &P[i].ev) < 0)
			return -1;
		rc = iox2_port_factory_publisher_builder_create(iox2_port_factory_pub_sub_publisher_builder(&P[i].svc, NULL), NULL,
								&P[i].pub);
		if (rc != IOX2_OK)
			return fail("the publisher could not be created", o->pub[i].topic, rc);
		rc = iox2_port_factory_notifier_builder_create(iox2_port_factory_event_notifier_builder(&P[i].ev, NULL), NULL,
							       &P[i].notifier);
		if (rc != IOX2_OK)
			return fail("the notifier could not be created", o->pub[i].topic, rc);
	}
	if (o->nsub) {
		iox2_waitset_builder_h wb = NULL;
		iox2_waitset_builder_new(NULL, &wb);
		if ((rc = iox2_waitset_builder_create(wb, iox2_service_type_e_IPC, NULL, &waitset)) != IOX2_OK)
			return fail("the wait set could not be created", o->module, rc);
	}
	for (int i = 0; i < o->nsub; i++) {
		if (services(&o->sub[i], &S[i].svc, &S[i].ev) < 0)
			return -1;
		iox2_port_factory_subscriber_builder_h sb = iox2_port_factory_pub_sub_subscriber_builder(&S[i].svc, NULL);
		iox2_port_factory_subscriber_builder_set_buffer_size(&sb, 16);
		if ((rc = iox2_port_factory_subscriber_builder_create(sb, NULL, &S[i].sub)) != IOX2_OK)
			return fail("the subscriber could not be created", o->sub[i].topic, rc);
		rc = iox2_port_factory_listener_builder_create(iox2_port_factory_event_listener_builder(&S[i].ev, NULL), NULL,
							       &S[i].listener);
		if (rc != IOX2_OK)
			return fail("the listener could not be created", o->sub[i].topic, rc);
		rc = iox2_waitset_attach_notification(&waitset, iox2_listener_get_file_descriptor(&S[i].listener), NULL,
						      &S[i].guard);
		if (rc != IOX2_OK)
			return fail("the listener could not be attached to the wait set", o->sub[i].topic, rc);
	}
	return 0;
}

static void *pub_buffer(int i)
{
	void *p = NULL;
	P[i].lent = NULL;
	if (iox2_publisher_loan_slice_uninit(&P[i].pub, NULL, &P[i].lent, 1) != IOX2_OK) {
		P[i].lent = NULL;
		return NULL;
	}
	iox2_sample_mut_payload_mut(&P[i].lent, &p, NULL);
	return p;
}

static int pub_send(int i, void *msg, size_t n)
{
	(void)msg;
	(void)n;
	if (!P[i].lent)
		return -1;
	int rc = iox2_sample_mut_send(P[i].lent, NULL);
	P[i].lent = NULL;
	if (rc != IOX2_OK)
		return -1;
	return iox2_notifier_notify(&P[i].notifier, NULL) == IOX2_OK ? 0 : -1;
}

static void no_event(const struct iox2_event_id_t *id, uint64_t count, iox2_callback_context ctx)
{
	(void)id;
	(void)count;
	(void)ctx;
}

static void drain(int i)
{
	uint64_t n = 0;
	/* The notifications taken in, or the wait set wakes for them again and again. */
	iox2_listener_try_wait(&S[i].listener, &n, no_event, NULL);
	for (;;) {
		iox2_sample_h sample = NULL;
		const void *bytes = NULL;
		if (iox2_subscriber_receive(&S[i].sub, NULL, &sample) != IOX2_OK || !sample)
			break;
		iox2_sample_payload(&sample, &bytes, NULL);
		graph_deliver(i, bytes, (long)O->sub[i].size);
		iox2_sample_drop(sample);
	}
}

static iox2_callback_progression_e on_ready(iox2_waitset_attachment_id_h id, void *ctx)
{
	(void)ctx;
	for (int i = 0; i < O->nsub; i++)
		if (iox2_waitset_attachment_id_has_event_from(&id, &S[i].guard)) {
			drain(i);
			break;
		}
	iox2_waitset_attachment_id_drop(id);
	return iox2_callback_progression_e_CONTINUE;
}

static int wait_any(int64_t timeout_ns)
{
	if (!O->nsub) {
		graph_sleep(timeout_ns);
		return 0;
	}
	iox2_waitset_run_result_e result = iox2_waitset_run_result_e_ALL_EVENTS_HANDLED;
	int rc = iox2_waitset_wait_and_process_once_with_timeout(&waitset, on_ready, NULL, (uint64_t)timeout_ns / 1000000000ull,
								 (uint32_t)(timeout_ns % 1000000000), &result);
	if (rc != IOX2_OK)
		return -1;
	return result == iox2_waitset_run_result_e_TERMINATION_REQUEST ? -1 : 0;
}

static void close_all(void)
{
	for (int i = 0; i < O->nsub; i++) {
		iox2_waitset_guard_drop(S[i].guard);
		iox2_listener_drop(S[i].listener);
		iox2_subscriber_drop(S[i].sub);
		iox2_port_factory_event_drop(S[i].ev);
		iox2_port_factory_pub_sub_drop(S[i].svc);
	}
	if (O->nsub)
		iox2_waitset_drop(waitset);
	for (int i = 0; i < O->npub; i++) {
		iox2_notifier_drop(P[i].notifier);
		iox2_publisher_drop(P[i].pub);
		iox2_port_factory_event_drop(P[i].ev);
		iox2_port_factory_pub_sub_drop(P[i].svc);
	}
	iox2_node_drop(node);
}

static const char *version(void)
{
	return "iceoryx2 " IOX2_VERSION " (C binding)";
}

int main(int argc, char **argv)
{
	static const struct graph_system sys = {
		.name = "iceoryx2", .version = version, .open = open_all, .pub_buffer = pub_buffer, .pub_send = pub_send,
		.wait = wait_any, .close = close_all,
	};
	return graph_main(argc, argv, &sys);
}
