// SPDX-License-Identifier: LGPL-3.0-or-later
/* The control socket's client side and its wire format (docs/03-manager.md
 * "Control socket"). The socket is $HANGAR_CONTROL, SOCK_SEQPACKET: one
 * request per packet, one reply per request, text. The manager identifies
 * the sender by SO_PEERCRED and SO_PEERGROUPS; nothing in a request names
 * who sends it.
 *
 * Requests (one packet each, at most HG_CONTROL_MAX bytes):
 *
 *   page
 *       From a module's main process at hg_init. Reply "ok\n" with the
 *       module page descriptor (SCM_RIGHTS), or "err <reason>\n".
 *   open <topic> hash=<16 hex> payload=<bytes> schema=<len>\n<schema bytes>
 *       From a module's main process in hg_pub_open: the topic name, the
 *       type hash, the payload size and the schema text (its length in
 *       schema=, the bytes after the newline). The bridge adds
 *       boot=<32 hex> first=<seq> for a mirrored topic. Reply "ok\n" with
 *       the writable topic descriptor (SCM_RIGHTS), or "err <reason>\n".
 *   restart-self
 *       From a module's main process: stop and start it without counting a
 *       failure (a @restart parameter changed). Reply "ok\n" or "err <reason>\n".
 *   call <module>.<method> deadline=<ms> type=<16 hex> body=<len>\n<body bytes>
 *       hangar-admin (hangar call, hangar param set). The manager publishes
 *       the request on hangar.<role>.admin.req with admin:<user> in the
 *       envelope's caller field, which servers trust on that topic alone,
 *       and replies "ok <call_id> <caller_id> <server>.rep\n": the command
 *       reads the reply on the server's .rep topic, keeping the one whose
 *       call_id and caller_id are these. Refused when no module serves the
 *       method. type= is the request type's hash (the server checks it).
 *   method <module>.<method>
 *       Anyone in group hangar: the method's types, "ok\nreq=<type>
 *       hash=<hex> size=<n> rep=<type> hash=<hex> size=<n> deadline=<ms>
 *       running=<0|1> reqschema=<len> repschema=<len>\n" then the two
 *       schema texts, so a command can build a request by field name.
 *   param-read <module>
 *       hangar-admin: "ok\ntype=<name> hash=<hex> running=<0|1>
 *       schema=<len> file=<len>\n" then the parameter type's schema text
 *       (attributes included) and the saved file, empty if none.
 *   param-write <module>\n<name = value lines>
 *       hangar-admin, for a module that is not running: the saved values
 *       with these lines applied, ranges checked, written and fsync'ed as
 *       the module's user with the version one above the saved one, set by
 *       admin:<user>. Reply "ok version=<n>\n".
 *   status
 *       Anyone in group hangar. Reply "ok\n" then one line per item:
 *         system <nominal|degraded|failsafe> <since_ns> <reason...>
 *         module <name> <state> <pid> <failures> <last_exit> <since_ns> <status line...>
 *       last_exit is "exit:<n>", "signal:<n>", "unknown" or "-".
 *   wiring <module>
 *       Anyone in group hangar: the wiring text as handed to the module.
 *       Reply "ok\n" then the text.
 *   start <module> | stop <module> | restart <module>
 *   reload | reexec | clear-failsafe
 *       hangar-admin (or uid 0). Reply "ok\n" or "err <reason>\n".
 *
 * Replies always start with "ok" or "err". The manager answers each
 * request with exactly one packet. */
#ifndef HG_CONTROL_H
#define HG_CONTROL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define HG_CONTROL_MAX 17408  /* a 16 KB request body and its line fit one packet */

/* Connects to the socket at path (NULL: $HANGAR_CONTROL). -1 with errno;
 * ENOENT or ECONNREFUSED while the manager is not listening. */
int hg_control_connect(const char *path);

/* Sends one request and waits for its reply. The reply is NUL-terminated in
 * reply[]; a descriptor passed with it goes to *fd_out (-1 if none; fd_out
 * may be NULL, then it is closed). Returns the reply's length, or -1 with
 * errno (EPROTO: the manager closed without answering). */
ssize_t hg_control_request(int sock, const void *req, size_t len, char *reply, size_t replylen,
			   int *fd_out);

/* 1 if the reply starts with "ok". */
int hg_control_ok(const char *reply);

/* Connects, sends, receives and closes, retrying the connection while the
 * manager is not listening (it is restarting: docs/03 says the module waits
 * until it answers) when wait is 1. */
ssize_t hg_control_call(const char *path, const void *req, size_t len, char *reply,
			size_t replylen, int *fd_out, int wait);

#endif
