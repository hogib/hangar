/* SPDX-License-Identifier: GPL-3.0-or-later */
/* bench/pairs/iceoryx2_pair.c: iceoryx2 as a node pair for bench/pairs (README.md
 * there), through its C binding. The system nearest to hangar in kind: no daemon,
 * the message built in a sample the publisher borrows from shared memory and read
 * by the subscriber where it lies.
 *
 *   iceoryx2_pair --role pub|sub ... [-- --poll] [-- --buffer N]
 *
 * iceoryx2's publish-subscribe does not wake a reader: a subscriber is asked whether
 * a sample has come. So there are two ways to be a subscriber, and two entries in the
 * registry.
 *
 * As is, woken: beside the publish-subscribe service runs an event service, as
 *   iceoryx2's own examples pair them. The publisher notifies after each send; the
 *   subscriber sleeps in its listener's wait and takes the sample when woken. Like
 *   for like with hangar, whose reader sleeps in hg_wait. The notification is part
 *   of the publish, and inside the sample.
 * --poll: no event service on the way. The subscriber asks for a sample in a loop
 *   that never sleeps. iceoryx2's best latency, and a core spent whether or not
 *   anything is published: the cost column shows the price.
 *
 * The subscriber's buffer holds --buffer samples (16, as hangar's ring; iceoryx2's
 * default is 2), with safe overflow, iceoryx2's default: the publisher never waits,
 * and a subscriber that falls that far behind loses the oldest. That is hangar's
 * contract, and the reason no other system's row is as near.
 *
 * Copies between the two clock readings: none. The loan is taken before the
 * harness reads the clock (as hangar-inplace's and ROS 2's are); the send, and when
 * woken the notification, are inside. */
#include "pair.h"

#include "iox2/iceoryx2.h"

#ifndef IOX2_VERSION
#define IOX2_VERSION "unknown"
#endif

#define EV_DATA 1u
#define EV_GONE 2u

static int poll_mode;
static unsigned buffer = 16, size;
static char name[128], ev_name[136];

static iox2_node_h node;
static iox2_port_factory_pub_sub_h svc;
static iox2_port_factory_event_h ev_svc;
static iox2_publisher_h pub;
static iox2_notifier_h notifier;
static iox2_subscriber_h sub;
static iox2_listener_h listener;
static iox2_sample_mut_h lent;
static iox2_sample_h held;
static int gone;

static int fail(const char *what, int rc)
{
	fprintf(stderr, "iceoryx2_pair: %s (error %d)\n", what, rc);
	return -1;
}

static int setup(const struct pair_opts *o)
{
	for (int i = 0; i < o->argc; i++) {
		if (!strcmp(o->argv[i], "--poll")) {
			poll_mode = 1;
		} else if (!strcmp(o->argv[i], "--buffer") && i + 1 < o->argc) {
			buffer = (unsigned)strtoul(o->argv[++i], NULL, 10);
		} else {
			fprintf(stderr, "iceoryx2_pair: unknown option %s\n", o->argv[i]);
			return -1;
		}
	}
	size = o->size;
	snprintf(name, sizeof name, "pair/%s", o->topic);
	snprintf(ev_name, sizeof ev_name, "pair/%s/event", o->topic);
	iox2_set_log_level_from_env_or(iox2_log_level_e_WARN);

	int rc = iox2_node_builder_create(iox2_node_builder_new(NULL), NULL, iox2_service_type_e_IPC, &node);
	if (rc != IOX2_OK)
		return fail("the node could not be created", rc);

	/* The publish-subscribe service: one fixed-size payload of `size` bytes. Both
	 * sides ask for the same service, and whichever comes first creates it. */
	iox2_service_name_h sn = NULL;
	if ((rc = iox2_service_name_new(NULL, name, strlen(name), &sn)) != IOX2_OK)
		return fail("the service name was refused", rc);
	iox2_service_builder_pub_sub_h b =
		iox2_service_builder_pub_sub(iox2_node_service_builder(&node, NULL, iox2_cast_service_name_ptr(sn)));
	char type[48];
	snprintf(type, sizeof type, "pair_u64x%u", size / 8);
	rc = iox2_service_builder_pub_sub_set_payload_type_details(&b, iox2_type_variant_e_FIXED_SIZE, type, strlen(type),
								   size, 8);
	if (rc != IOX2_OK)
		return fail("the payload type was refused", rc);
	iox2_service_builder_pub_sub_set_subscriber_max_buffer_size(&b, buffer);
	iox2_service_builder_pub_sub_set_history_size(&b, 0);
	iox2_service_builder_pub_sub_set_max_subscribers(&b, 16);
	iox2_service_builder_pub_sub_set_max_nodes(&b, 32);
	iox2_service_builder_pub_sub_set_enable_safe_overflow(&b, true);
	rc = iox2_service_builder_pub_sub_open_or_create(b, NULL, &svc);
	iox2_service_name_drop(sn);
	if (rc != IOX2_OK)
		return fail("the publish-subscribe service could not be opened or created", rc);

	/* The event service: what wakes a subscriber, and what tells every subscriber
	 * that the publisher has gone. */
	if ((rc = iox2_service_name_new(NULL, ev_name, strlen(ev_name), &sn)) != IOX2_OK)
		return fail("the event service's name was refused", rc);
	rc = iox2_service_builder_event_open_or_create(
		iox2_service_builder_event(iox2_node_service_builder(&node, NULL, iox2_cast_service_name_ptr(sn))), NULL,
		&ev_svc);
	iox2_service_name_drop(sn);
	if (rc != IOX2_OK)
		return fail("the event service could not be opened or created", rc);
	return 0;
}

static int pub_open(const struct pair_opts *o)
{
	if (setup(o) < 0)
		return -1;
	int rc = iox2_port_factory_publisher_builder_create(iox2_port_factory_pub_sub_publisher_builder(&svc, NULL), NULL,
							    &pub);
	if (rc != IOX2_OK)
		return fail("the publisher could not be created", rc);
	rc = iox2_port_factory_notifier_builder_create(iox2_port_factory_event_notifier_builder(&ev_svc, NULL), NULL,
						       &notifier);
	if (rc != IOX2_OK)
		return fail("the notifier could not be created", rc);
	return 0;
}

static void *pub_buffer(void)
{
	void *p = NULL;
	lent = NULL;
	if (iox2_publisher_loan_slice_uninit(&pub, NULL, &lent, 1) != IOX2_OK) {
		lent = NULL;
		return NULL; /* pub_send then fails, and the failure is counted */
	}
	iox2_sample_mut_payload_mut(&lent, &p, NULL);
	return p;
}

static int pub_send(void *msg, size_t n)
{
	(void)msg;
	(void)n;
	if (!lent)
		return -1;
	int rc = iox2_sample_mut_send(lent, NULL);
	lent = NULL;
	if (rc != IOX2_OK)
		return -1;
	if (!poll_mode) {
		iox2_event_id_t id = { .value = EV_DATA };
		if (iox2_notifier_notify_with_custom_event_id(&notifier, &id, NULL) != IOX2_OK)
			return -1;
	}
	return 0;
}

static void pub_close(void)
{
	iox2_event_id_t id = { .value = EV_GONE };
	iox2_notifier_notify_with_custom_event_id(&notifier, &id, NULL);
	iox2_notifier_drop(notifier);
	iox2_publisher_drop(pub);
	iox2_port_factory_event_drop(ev_svc);
	iox2_port_factory_pub_sub_drop(svc);
	iox2_node_drop(node);
}

static int sub_open(const struct pair_opts *o)
{
	if (setup(o) < 0)
		return -1;
	iox2_port_factory_subscriber_builder_h sb = iox2_port_factory_pub_sub_subscriber_builder(&svc, NULL);
	iox2_port_factory_subscriber_builder_set_buffer_size(&sb, buffer);
	int rc = iox2_port_factory_subscriber_builder_create(sb, NULL, &sub);
	if (rc != IOX2_OK)
		return fail("the subscriber could not be created", rc);
	rc = iox2_port_factory_listener_builder_create(iox2_port_factory_event_listener_builder(&ev_svc, NULL), NULL,
						       &listener);
	if (rc != IOX2_OK)
		return fail("the listener could not be created", rc);
	return 0;
}

static void on_event(const struct iox2_event_id_t *id, uint64_t count, iox2_callback_context ctx)
{
	(void)count;
	(void)ctx;
	if (id->value == EV_GONE)
		gone = 1;
}

static long take(const void **msg)
{
	held = NULL;
	if (iox2_subscriber_receive(&sub, NULL, &held) != IOX2_OK)
		return -1;
	if (!held)
		return 0;
	iox2_sample_payload(&held, msg, NULL);
	return (long)size;
}

static long sub_recv(const void **msg, int timeout_ms)
{
	uint64_t n = 0;
	long len = take(msg);
	if (len != 0 || timeout_ms <= 0)
		return len;
	if (gone)
		return -1; /* told so, and nothing was left to take */
	if (!poll_mode) {
		if (iox2_listener_timed_wait(&listener, &n, on_event, NULL, (uint64_t)timeout_ms / 1000,
					     (uint32_t)(timeout_ms % 1000) * 1000000u) != IOX2_OK)
			return -1;
		return take(msg);
	}
	/* Polling: ask again and again, never sleeping, for as long as the harness
	 * allows; the clock is read every 256 times round. */
	uint64_t end = pair_now_ns() + (uint64_t)timeout_ms * 1000000ull;
	for (unsigned k = 1;; k++) {
		if ((len = take(msg)) != 0)
			return len;
		if (!(k & 255u) && pair_now_ns() >= end)
			break;
	}
	iox2_listener_try_wait(&listener, &n, on_event, NULL); /* only for "gone" */
	return 0;
}

static int sub_done(void)
{
	if (held) {
		iox2_sample_drop(held);
		held = NULL;
	}
	return 1;
}

static void sub_close(void)
{
	sub_done();
	iox2_listener_drop(listener);
	iox2_subscriber_drop(sub);
	iox2_port_factory_event_drop(ev_svc);
	iox2_port_factory_pub_sub_drop(svc);
	iox2_node_drop(node);
}

static const char *version(void)
{
	return "iceoryx2 " IOX2_VERSION " (C binding)";
}

int main(int argc, char **argv)
{
	static const struct pair_system sys = {
		.name = "iceoryx2", .version = version,
		.pub_open = pub_open, .pub_buffer = pub_buffer, .pub_send = pub_send, .pub_close = pub_close,
		.sub_open = sub_open, .sub_recv = sub_recv, .sub_done = sub_done, .sub_close = sub_close,
	};
	return pair_main(argc, argv, &sys);
}
