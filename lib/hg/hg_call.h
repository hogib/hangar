// SPDX-License-Identifier: LGPL-3.0-or-later
/* Calls: request and reply between modules, as two messages on two topics
 * (docs/04-calls.md). The request goes on the caller's `<caller>.req`
 * topic, the reply on the server's `<server>.rep`; both are ordinary
 * topics the manager created from the manifests, with the generated types
 * `req:<module>` and `rep:<module>` (docs/03 "Wiring"). Nothing blocks and
 * nothing has a thread: hg_call_start publishes and returns, hg_call_poll
 * reads the reply topic, hg_serve reads the callers' request topics. The
 * caller is the writer of the request topic, never anything in the
 * request. Every setting comes from the wiring: the module's code names
 * methods and types, and the library refuses a call or a method the wiring
 * does not list. */
#ifndef HG_CALL_H
#define HG_CALL_H

#include <stddef.h>
#include <stdint.h>

#include "hg_topic.h"

/* Status codes (docs/04 "Status codes"). HG_OK and the errors travel in
 * the reply envelope's i32 `status`; HG_PENDING is hg_call_poll's "not
 * yet", and never published. */
enum hg_status {
	HG_OK = 0,            /* done; the reply holds the result */
	HG_PENDING = 1,       /* caller side: no reply yet, and the timeout not passed */
	HG_E_REFUSED = -1,    /* the handler said no; reason says why */
	HG_E_DENIED = -2,     /* the caller is not in Allow */
	HG_E_NOMETHOD = -3,   /* no such method */
	HG_E_BADTYPE = -4,    /* the request type differs from the server's; or the reply type from the caller's */
	HG_E_EXPIRED = -5,    /* the request waited past its deadline */
	HG_E_BUSY = -6,       /* caller side: 15 calls are already in flight */
	HG_E_NOROUTE = -7,    /* caller side: no module serves the method on either side */
	HG_E_TIMEOUT = -8,    /* caller side: no reply within the timeout; the outcome is unknown */
};
const char *hg_status_name(int status);

#define HG_CALL_MAX_PAYLOAD 16384  /* a request or a reply is at most 16 KB */
#define HG_CALL_IN_FLIGHT 15       /* Slots - 1: a request in flight is never overwritten */
#define HG_SERVE_PER_PASS 8        /* requests answered per hg_serve() */
#define HG_CALL_REASON 64          /* the reply's reason, one line, with its NUL */
#define HG_CALL_CALLER 48          /* a caller token, with its NUL */

/* The envelopes, to the byte (docs/04 "The envelopes"; msg/call_request_envelope.msg
 * and msg/call_reply_envelope.msg are the same layouts). A `.req` payload is
 * the request envelope then the request; a `.rep` payload the reply envelope
 * then the reply, absent (zero) unless status is HG_OK. */
struct hg_call_request_envelope {
	uint64_t call_id;       /* per caller, counting from 1 */
	uint64_t deadline_ns;   /* CLOCK_MONOTONIC: when the caller stops waiting */
	uint64_t request_type;  /* type hash */
	uint32_t target_id;     /* topic id of the server's <server>.rep */
	uint32_t reserved;      /* 0 */
	char method[32];
	char caller[48];        /* empty; set by the manager on hangar.<role>.admin.req only */
};
struct hg_call_reply_envelope {
	uint64_t call_id;
	uint32_t caller_id;     /* topic id of the request topic the request was read from */
	int32_t status;         /* enum hg_status */
	uint64_t reply_type;    /* type hash */
	uint64_t received_ns;   /* CLOCK_MONOTONIC: when the server read the request */
	char reason[64];        /* one line, when not HG_OK */
};
#define HG_CALL_REQ_ENVELOPE 112u
#define HG_CALL_REP_ENVELOPE 96u

/* ---- Calling ----------------------------------------------------------- */

typedef struct hg_pending hg_pending;

typedef struct hg_call_result {
	int status;             /* HG_PENDING until the reply or the timeout, then HG_OK or an error */
	uint64_t call_id;
	uint64_t received_ns;   /* when the server read the request; 0 without a reply */
	char reason[HG_CALL_REASON]; /* one line when refused, denied, or otherwise not HG_OK */
} hg_call_result;

/* Publishes the request for `<module>.<method>` on this module's `.req`
 * topic, with no references, and returns at once. HG_OK and *out set; or
 * HG_E_BUSY (15 calls in flight), HG_E_NOROUTE (the wiring says nothing
 * serves it on either side), HG_E_BADTYPE (req_type or rep_type differ
 * from the wiring's call line), HG_E_NOMETHOD with errno EACCES (the
 * module did not declare the call: the library refuses it, docs/04). A
 * timeout of 0 or less means the default 100 ms. req may be NULL for an
 * empty request. */
int hg_call_start(const char *target, const struct hg_type *req_type, const void *req,
		  const struct hg_type *rep_type, hg_pending **out, int timeout_ms);
/* Reads the server's `.rep` topic in order, keeping the replies addressed
 * to this module's calls in flight, and reports pc's: HG_PENDING, or the
 * outcome, after which pc is free. rep (the caller's reply type) is filled
 * on HG_OK; it may be NULL. A reply that arrives after the timeout is
 * discarded by its call id. */
hg_call_result hg_call_poll(hg_pending *pc, void *rep);
/* hg_call_start, then hg_wait on the server's `.rep` topic until the reply
 * or the timeout: stops the module's loop for up to the timeout. Refused
 * (HG_E_REFUSED, with the reason) in a module with an actuator output. */
hg_call_result hg_call(const char *target, const struct hg_type *req_type, const void *req,
		       const struct hg_type *rep_type, void *rep, int timeout_ms);

/* ---- Serving ----------------------------------------------------------- */

/* What a handler is given. caller is the token docs/04 "Callers" derives
 * from the request topic: `<module>`, `<role>:<module>`, `admin:<user>`,
 * `<role>:admin:<user>`. req is the request message, `{topic_id, seq}`,
 * for hg_publish_with on an output that acts on the call. ctx is the
 * pointer hg_serve_method was given. */
typedef struct hg_call_ctx {
	const char *caller;
	const char *method;
	uint64_t call_id;
	uint64_t deadline_ns;
	uint64_t received_ns;
	struct hg_input req;
	void *ctx;
} hg_call_ctx;

/* Returns HG_OK and fills rep (the method's reply type, zeroed first), or
 * HG_E_REFUSED with one line in reason (HG_CALL_REASON bytes, NUL
 * included). */
typedef int (*hg_handler)(const hg_call_ctx *c, const void *req, void *rep, char *reason);

/* Registers the handler of a method the wiring lists under serve. 0, or -1
 * with EACCES (not served by this module), EEXIST, ENOSPC. */
int hg_serve_method(const char *name, hg_handler fn, void *ctx);
/* The library's own: a function run after the reply to `name` has been
 * published (set_params asks the manager for a restart there). */
int hg_serve_after(const char *name, void (*after)(void *ctx));
/* Reads each request topic the wiring lists (the named `.req` subs and the
 * matches of `subglob *.req`) in order from the oldest message still in
 * its ring, skips what is not addressed to this module's `.rep` topic, and
 * for each request addressed to it: HG_E_EXPIRED without the handler when
 * the deadline has passed; HG_E_DENIED when the caller is not in Allow;
 * HG_E_NOMETHOD, HG_E_BADTYPE; else the handler's reply. Each reply
 * references the request it answers. At most HG_SERVE_PER_PASS requests
 * per call, the topics taken in turn; the rest stay in their rings where
 * hg_wait finds them. Returns the number answered, or -1 with errno. */
int hg_serve(void);

/* Every subscription the call library holds (the request topics it serves,
 * the reply topics it polls), for the module's hg_wait set: a request or
 * a reply then wakes the module like any message. The count, or -1 with
 * E2BIG if max is too small. */
int hg_call_subs(hg_sub **out, int max);

/* This module's generated types as the library builds them from the
 * wiring: `req:<module>` and `rep:<module>`. NULL (errno) when the module
 * calls nothing, serves nothing, or is unwired. */
const struct hg_type *hg_call_req_type(void);
const struct hg_type *hg_call_rep_type(void);

/* The caller token of a request read from `sub` with envelope `env`
 * (docs/04 "Callers"), into out[HG_CALL_CALLER]; the `caller` field is
 * read on `hangar.<role>.admin.req` alone. For tools that read request
 * topics. */
void hg_call_caller_token(const hg_sub *sub, const struct hg_call_request_envelope *env,
			  const char *my_role, char *out);
/* 1 if token passes an Allow list of n tokens (docs/04 "Allow tokens"). */
int hg_call_allowed(const char *token, const char (*allow)[HG_WIRING_NAME], int n);

/* The schema texts of the generated types, as the manager writes them:
 * `req:<module>` with the largest request `largest`, `rep:<module>` with
 * the largest reply. Returns the length written (without the NUL). */
int hg_call_req_schema(const char *module, uint32_t largest, char *out, size_t outlen);
int hg_call_rep_schema(const char *module, uint32_t largest, char *out, size_t outlen);

#endif
