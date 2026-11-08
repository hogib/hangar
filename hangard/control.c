// SPDX-License-Identifier: GPL-3.0-or-later
/* The control socket (docs/03 "Control socket"; the wire format is in
 * lib/hg/hg_control.h): SOCK_SEQPACKET at <run>/hangar/control, mode 0660,
 * callers identified by SO_PEERCRED and SO_PEERGROUPS. One request per
 * packet, one reply per request, the connection closed after the reply. */
#include "hangard.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "hg_call.h"
#include "hg_control.h"
#include "hg_params.h"

int control_open(void)
{
	unlink(G.paths.control);
	int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (s < 0)
		return -1;
	struct sockaddr_un sa;
	memset(&sa, 0, sizeof sa);
	sa.sun_family = AF_UNIX;
	if (strlen(G.paths.control) >= sizeof sa.sun_path) {
		close(s);
		errno = ENAMETOOLONG;
		return -1;
	}
	strcpy(sa.sun_path, G.paths.control);
	mode_t old = umask(0117);
	int rc = bind(s, (struct sockaddr *)&sa, sizeof sa);
	umask(old);
	if (rc < 0 || listen(s, 32) < 0) {
		int e = errno;
		close(s);
		errno = e;
		return -1;
	}
	if (G.is_root && G.have_gid_hangar)
		(void)chown(G.paths.control, 0, G.gid_hangar);
	(void)chmod(G.paths.control, 0660);
	return s;
}

void control_accept(void)
{
	for (;;) {
		int c = accept4(G.ctlfd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
		if (c < 0)
			return;
		if (epoll_add(c, TAG_CONN | (uint64_t)c) < 0)
			close(c);
	}
}

struct peer {
	pid_t pid;
	uid_t uid;
	gid_t gid;
	int n_groups;
	gid_t groups[64];
};

static int peer_of(int fd, struct peer *p)
{
	struct ucred uc;
	socklen_t len = sizeof uc;
	memset(p, 0, sizeof *p);
	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &uc, &len) < 0)
		return -1;
	p->pid = uc.pid;
	p->uid = uc.uid;
	p->gid = uc.gid;
	len = sizeof p->groups;
	if (getsockopt(fd, SOL_SOCKET, SO_PEERGROUPS, p->groups, &len) == 0)
		p->n_groups = (int)(len / sizeof(gid_t));
	return 0;
}

static int in_group(const struct peer *p, gid_t g)
{
	if (p->gid == g)
		return 1;
	for (int i = 0; i < p->n_groups; i++)
		if (p->groups[i] == g)
			return 1;
	return 0;
}

/* hangar-admin: the group, uid 0, or the manager's own user (so that the
 * suites, which run the manager as an ordinary user, can drive it). */
static int is_admin(const struct peer *p)
{
	return p->uid == 0 || p->uid == G.uid || (G.have_gid_admin && in_group(p, G.gid_admin));
}

static int is_reader(const struct peer *p)
{
	return is_admin(p) || (G.have_gid_hangar && in_group(p, G.gid_hangar));
}

static struct module *module_by_pid(pid_t pid)
{
	if (pid <= 0)
		return NULL;
	for (int i = 0; i < G.n_modules; i++)
		if (G.modules[i].pid == pid)
			return &G.modules[i];
	return NULL;
}

static void reply(int fd, const char *text, size_t len, int pass_fd)
{
	union {
		struct cmsghdr h;
		unsigned char buf[CMSG_SPACE(sizeof(int))];
	} cm;
	struct iovec iov = { (void *)text, len };
	struct msghdr mh;
	memset(&mh, 0, sizeof mh);
	mh.msg_iov = &iov;
	mh.msg_iovlen = 1;
	if (pass_fd >= 0) {
		memset(&cm, 0, sizeof cm);
		mh.msg_control = &cm;
		mh.msg_controllen = sizeof cm;
		struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
		c->cmsg_level = SOL_SOCKET;
		c->cmsg_type = SCM_RIGHTS;
		c->cmsg_len = CMSG_LEN(sizeof(int));
		memcpy(CMSG_DATA(c), &pass_fd, sizeof(int));
	}
	ssize_t w;
	while ((w = sendmsg(fd, &mh, MSG_NOSIGNAL)) < 0 && errno == EINTR)
		;
}

static void err_reply(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void err_reply(int fd, const char *fmt, ...)
{
	char buf[512];
	int n = mf_fmt(buf, sizeof buf, "err ");
	va_list ap;
	va_start(ap, fmt);
	n += vsnprintf(buf + n, sizeof buf - (size_t)n, fmt, ap);
	va_end(ap);
	if (n > (int)sizeof buf - 2)
		n = (int)sizeof buf - 2;
	hd_log("control: refused: %s", buf + 4);
	buf[n++] = '\n';
	reply(fd, buf, (size_t)n, -1);
}

static void ok_reply(int fd)
{
	reply(fd, "ok\n", 3, -1);
}

/* ---- Requests ---------------------------------------------------------- */

static void req_page(int fd, const struct peer *p)
{
	struct module *m = module_by_pid(p->pid);
	if (!m || !m->mf) {
		err_reply(fd, "pid %d is not a module's main process", (int)p->pid);
		return;
	}
	if (m->mf->legacy) {
		err_reply(fd, "%s is a legacy module and has no page", m->name);
		return;
	}
	int tracer = proc_tracer_pid(p->pid);
	if (tracer != 0) {
		err_reply(fd, "%s is being traced (TracerPid %d)", m->name, tracer);
		return;
	}
	if (m->page_fd < 0) {
		err_reply(fd, "%s has no page", m->name);
		return;
	}
	reply(fd, "ok\n", 3, m->page_fd);
}

/* "open <topic> hash=<hex> payload=<n> schema=<len>[ boot=<hex> first=<n>]\n<schema>" */
static void req_open(int fd, const struct peer *p, char *req, size_t len)
{
	struct module *m = module_by_pid(p->pid);
	if (!m || !m->mf) {
		err_reply(fd, "pid %d is not a module's main process", (int)p->pid);
		return;
	}
	char *nl = memchr(req, '\n', len);
	if (!nl) {
		err_reply(fd, "open: malformed request");
		return;
	}
	*nl = 0;
	const char *schema = nl + 1;
	size_t schema_len = len - (size_t)(schema - req);
	char topic[MF_TOPIC] = "", boot_hex[40] = "";
	unsigned long long hash = 0, first = 0;
	unsigned payload = 0, slen = 0;
	int have_boot = 0;
	char *save, *tok = strtok_r(req + 5, " ", &save);
	if (!tok || strlen(tok) >= sizeof topic) {
		err_reply(fd, "open: malformed topic");
		return;
	}
	mf_fmt(topic, sizeof topic, "%s", tok);
	while ((tok = strtok_r(NULL, " ", &save))) {
		if (sscanf(tok, "hash=%llx", &hash) == 1 || sscanf(tok, "payload=%u", &payload) == 1 ||
		    sscanf(tok, "schema=%u", &slen) == 1 || sscanf(tok, "first=%llu", &first) == 1)
			continue;
		if (sscanf(tok, "boot=%32s", boot_hex) == 1) {
			have_boot = 1;
			continue;
		}
		err_reply(fd, "open: unknown key %s", tok);
		return;
	}
	if (slen != schema_len || slen > HG_SCHEMA_MAX) {
		err_reply(fd, "open: schema length %u does not match the %zu bytes sent", slen, schema_len);
		return;
	}
	if (!hg_valid_name(topic)) {
		err_reply(fd, "open: %s is not a topic name", topic);
		return;
	}
	const struct mf_pub *pub = NULL;
	for (int i = 0; i < m->mf->n_pubs; i++)
		if (strcmp(m->mf->pubs[i].topic, topic) == 0)
			pub = &m->mf->pubs[i];
	if (!pub) {
		err_reply(fd, "%s is not a declared or implicit output of %s", topic, m->name);
		return;
	}
	if (have_boot) {
		err_reply(fd, "%s is not the bridge: a mirrored topic (boot=) is refused", m->name);
		return;
	}
	const struct mf_type *t = mf_type_at(&G.set, pub->type_idx);
	if (!t) {
		err_reply(fd, "internal: %s has no resolved type", topic);
		return;
	}
	struct hg_schema *parsed = malloc(sizeof *parsed);
	if (!parsed) {
		err_reply(fd, "out of memory");
		return;
	}
	char perr[160];
	if (hg_schema_parse(schema, schema_len, parsed, perr, sizeof perr) < 0) {
		free(parsed);
		err_reply(fd, "the schema does not parse: %s", perr);
		return;
	}
	if (parsed->type.hash != hash || parsed->type.payload_bytes != payload) {
		free(parsed);
		err_reply(fd, "the schema's hash or payload size differs from the request's");
		return;
	}
	if (t->frame) {
		if (parsed->type.n_fields != 6 || strcmp(parsed->fields[0].name, "width") != 0 ||
		    strcmp(parsed->fields[5].name, "data") != 0) {
			free(parsed);
			err_reply(fd, "%s is declared type=frame; the schema is not a frame type", topic);
			return;
		}
	} else if (t->hash != hash) {
		free(parsed);
		err_reply(fd, "type mismatch: %s is declared %s (hash %016llx), the module opened it with hash %016llx",
			  topic, t->name, (unsigned long long)t->hash, hash);
		return;
	}
	free(parsed);
	char who[96];
	if (topic_other_writer(topic, p->pid, who, sizeof who)) {
		err_reply(fd, "%s already has a writer: %s", topic, who);
		return;
	}
	struct topic_params tp = { .name = topic,
				   .writer = m->name,
				   .writer_pid = p->pid,
				   .type_hash = hash,
				   .payload_bytes = payload,
				   .schema = schema,
				   .schema_bytes = (uint32_t)schema_len,
				   .slots = pub->slots,
				   .rate_mhz = pub->rate_mhz,
				   .stale_after_ns = pub->rate_mhz ? pub->stale_after_ns : 0 };
	char err[256];
	int tfd = topic_create(&tp, err, sizeof err);
	if (tfd < 0) {
		err_reply(fd, "cannot create %s: %s", topic, err);
		return;
	}
	reply(fd, "ok\n", 3, tfd);
	close(tfd);
	hd_log("%s: opened %s (%s, %u slots)", m->name, topic, t->name, pub->slots);
}

static void page_status(const struct module *m, char *out, size_t len)
{
	out[0] = 0;
	if (!m->page)
		return;
	size_t n = 0;
	for (; n < 127 && n < len - 1; n++) {
		char c = (char)m->page[64 + n];
		if (!c)
			break;
		out[n] = (c >= 32 && c < 127) ? c : ' ';
	}
	out[n] = 0;
}

static void req_status(int fd)
{
	static char buf[HG_CONTROL_MAX];
	int n = mf_fmt(buf, sizeof buf, "ok\nsystem %s %llu %s\n", sys_state_names[G.sys],
			 (unsigned long long)G.sys_since_ns, G.sys_reason);
	for (int i = 0; i < G.n_modules && n < (int)sizeof buf - 1; i++) {
		const struct module *m = &G.modules[i];
		char ex[32], st[136];
		if (m->exit_known && m->exit_status >= 0)
			mf_fmt(ex, sizeof ex, "exit:%d", m->exit_status);
		else if (m->exit_known)
			mf_fmt(ex, sizeof ex, "signal:%d", -m->exit_status);
		else if (m->adopted || m->failures)
			mf_fmt(ex, sizeof ex, "unknown");
		else
			mf_fmt(ex, sizeof ex, "-");
		page_status(m, st, sizeof st);
		n += mf_fmt(buf + n, sizeof buf - (size_t)n, "module %s %s %d %u %s %llu %s\n", m->name,
			      state_names[m->st], (int)m->pid, m->failures, ex, (unsigned long long)m->since_ns,
			      m->st == ST_REFUSED ? m->why : st);
	}
	if (n > (int)sizeof buf - 1)
		n = (int)sizeof buf - 1;
	reply(fd, buf, (size_t)n, -1);
}

static void req_wiring(int fd, const char *name)
{
	struct module *m = module_find(name);
	if (!m) {
		err_reply(fd, "no module %s", name);
		return;
	}
	if (!m->wiring) {
		err_reply(fd, "%s has no wiring (%s)", name, m->st == ST_REFUSED ? m->why : "legacy or refused");
		return;
	}
	static char buf[HG_CONTROL_MAX];
	int n = mf_fmt(buf, sizeof buf, "ok\n%s", m->wiring);
	if (n > (int)sizeof buf - 1)
		n = (int)sizeof buf - 1;
	reply(fd, buf, (size_t)n, -1);
}

static void req_start(int fd, const char *name, int restart)
{
	struct module *m = module_find(name);
	if (!m || !m->mf) {
		err_reply(fd, "no module %s", name);
		return;
	}
	if (m->mf->refused) {
		err_reply(fd, "%s is refused: %s", name, m->mf->reason);
		return;
	}
	if (G.shutting_down) {
		err_reply(fd, "shutting down");
		return;
	}
	if (m->pid && !restart) {
		err_reply(fd, "%s is running (pid %d)", name, (int)m->pid);
		return;
	}
	/* A start by hand clears the restart history (a failed module is
	 * started again this way). */
	m->failures = 0;
	m->delay_ns = 0;
	m->n_fail_times = 0;
	m->gave_up = 0;
	if (m->pid)
		module_stop(m, ST_STARTING, 0, "restart requested");
	else
		module_start(m);
	recompute_sys_state();
	ok_reply(fd);
}

static void req_stop(int fd, const char *name)
{
	struct module *m = module_find(name);
	if (!m) {
		err_reply(fd, "no module %s", name);
		return;
	}
	if (!m->pid && m->st != ST_BACKOFF) {
		err_reply(fd, "%s is not running (%s)", name, state_names[m->st]);
		return;
	}
	module_stop(m, ST_STOPPED, 0, "stopped on request");
	ok_reply(fd);
}

static void req_clear_failsafe(int fd)
{
	if (G.sys != SYS_FAILSAFE) {
		err_reply(fd, "the system is %s, not failsafe", sys_state_names[G.sys]);
		return;
	}
	for (int i = 0; i < G.n_modules; i++) {
		const struct module *m = &G.modules[i];
		if (m->st == ST_FAILED && m->gave_up && m->mf && m->mf->giveup == MF_GIVEUP_FAILSAFE) {
			err_reply(fd, "%s is failed: restart it first", m->name);
			return;
		}
	}
	G.sys = SYS_NOMINAL;
	G.sys_since_ns = hg_now_ns();
	mf_fmt(G.sys_reason, sizeof G.sys_reason, "failsafe cleared");
	hd_log("system nominal: failsafe cleared");
	registry_write();
	request_publish();
	recompute_sys_state();
	ok_reply(fd);
}

/* ---- Calls and parameters (docs/04 "Admin calls", docs/05) ------------- */

/* The method <module>.<method> of an enabled module, set_params included. */
static const struct mf_serve *find_method(const char *target, struct module **mod, char *err, size_t errlen)
{
	const char *dot = strchr(target, '.');
	if (!dot || dot == target || !dot[1]) {
		mf_fmt(err, errlen, "'%s' is not <module>.<method>", target);
		return NULL;
	}
	char name[MF_NAME];
	mf_fmt(name, sizeof name, "%.*s", (int)(dot - target), target);
	struct module *m = module_find(name);
	if (!m || !m->mf || m->mf->refused) {
		mf_fmt(err, errlen, "no module serves %s: %s is %s", target, name,
			 !m || !m->mf ? "not a module here" : "refused");
		return NULL;
	}
	for (int i = 0; i < m->mf->n_serves; i++)
		if (strcmp(m->mf->serves[i].method, dot + 1) == 0) {
			*mod = m;
			return &m->mf->serves[i];
		}
	mf_fmt(err, errlen, "no module serves %s: %s has no method %s", target, name, dot + 1);
	return NULL;
}

/* "call <module>.<method> deadline=<ms> type=<hex> body=<len>\n<body>": the
 * manager, the one writer of hangar.<role>.admin.req, publishes the request
 * with admin:<user> in the envelope, and answers with the call id and the
 * topic id the reply will name as caller_id. */
static void req_call(int fd, const struct peer *p, char *req, size_t len)
{
	char *nl = memchr(req, '\n', len);
	if (!nl) {
		err_reply(fd, "call: malformed request");
		return;
	}
	*nl = 0;
	const unsigned char *body = (const unsigned char *)nl + 1;
	size_t body_len = len - (size_t)((const char *)body - req);
	char *save, *target = strtok_r(req + 5, " ", &save), *tok;
	unsigned long long deadline_ms = 1000, type_hash = 0, blen = 0;
	int have_type = 0;
	if (!target) {
		err_reply(fd, "call: no method");
		return;
	}
	while ((tok = strtok_r(NULL, " ", &save))) {
		if (sscanf(tok, "deadline=%llu", &deadline_ms) == 1 || sscanf(tok, "body=%llu", &blen) == 1)
			continue;
		if (sscanf(tok, "type=%llx", &type_hash) == 1) {
			have_type = 1;
			continue;
		}
		err_reply(fd, "call: unknown key %s", tok);
		return;
	}
	char err[MF_ERR];
	struct module *m = NULL;
	const struct mf_serve *sv = find_method(target, &m, err, sizeof err);
	if (!sv) {
		err_reply(fd, "%s", err);
		return;
	}
	const struct mf_type *rq = mf_type_at(&G.set, sv->req_idx);
	if (blen != body_len || !rq || body_len > rq->payload_bytes || body_len > HG_CALL_MAX_PAYLOAD) {
		err_reply(fd, "call: the body is %zu bytes; %s takes %u", body_len, target, rq ? rq->payload_bytes : 0);
		return;
	}
	if (!deadline_ms || deadline_ms > 3600000ull) {
		err_reply(fd, "call: the deadline is in ms, 1 to 3600000");
		return;
	}
	static unsigned char payload[HG_CALL_REQ_ENVELOPE + HG_CALL_MAX_PAYLOAD];
	struct hg_call_request_envelope env;
	memset(&env, 0, sizeof env);
	uint64_t now = hg_now_ns();
	env.call_id = G.next_call_id++;
	env.deadline_ns = now + deadline_ms * 1000000ull;
	env.request_type = have_type ? type_hash : rq->hash;
	char rep_topic[MF_TOPIC];
	mf_fmt(rep_topic, sizeof rep_topic, "%s.rep", m->name);
	env.target_id = hg_topic_id(rep_topic);
	mf_fmt(env.method, sizeof env.method, "%s", sv->method);
	char user[64];
	lookup_user_name(p->uid, user, sizeof user);
	mf_fmt(env.caller, sizeof env.caller, "admin:%s", user);
	memset(payload, 0, sizeof payload);
	memcpy(payload, &env, sizeof env);
	memcpy(payload + HG_CALL_REQ_ENVELOPE, body, body_len);
	if (hg_publish_with(G.pub_admin, payload, sizeof payload, now, HG_NO_REFS, NULL, 0) < 0) {
		err_reply(fd, "cannot publish on the admin topic: %s", strerror(errno));
		return;
	}
	char ok[160];
	int n = mf_fmt(ok, sizeof ok, "ok %llu %u %s\n", (unsigned long long)env.call_id,
			 hg_pub_topic_id(G.pub_admin), rep_topic);
	reply(fd, ok, (size_t)n, -1);
	hd_log("call %s by admin:%s: call %llu, deadline %llu ms", target, user, (unsigned long long)env.call_id,
	       deadline_ms);
}

/* "method <module>.<method>": its types, with their schemas, for a command
 * building a request by field name. */
static void req_method(int fd, const char *target)
{
	char err[MF_ERR];
	struct module *m = NULL;
	const struct mf_serve *sv = find_method(target, &m, err, sizeof err);
	if (!sv) {
		err_reply(fd, "%s", err);
		return;
	}
	const struct mf_type *rq = mf_type_at(&G.set, sv->req_idx), *rp = mf_type_at(&G.set, sv->rep_idx);
	if (!rq || !rp) {
		err_reply(fd, "internal: %s has unresolved types", target);
		return;
	}
	static char buf[HG_CONTROL_MAX];
	int n = mf_fmt(buf, sizeof buf,
			 "ok\nreq=%s hash=%016llx size=%u rep=%s hash=%016llx size=%u deadline=%llu running=%d reqschema=%u repschema=%u\n",
			 rq->name, (unsigned long long)rq->hash, rq->payload_bytes, rp->name, (unsigned long long)rp->hash,
			 rp->payload_bytes, (unsigned long long)(sv->deadline_ns / 1000000u), m->pid ? 1 : 0,
			 rq->schema_bytes, rp->schema_bytes);
	if ((size_t)n + rq->schema_bytes + rp->schema_bytes >= sizeof buf) {
		err_reply(fd, "the schemas do not fit a reply");
		return;
	}
	memcpy(buf + n, rq->schema, rq->schema_bytes);
	n += (int)rq->schema_bytes;
	memcpy(buf + n, rp->schema, rp->schema_bytes);
	n += (int)rp->schema_bytes;
	reply(fd, buf, (size_t)n, -1);
}

static struct module *params_module(int fd, const char *name)
{
	struct module *m = module_find(name);
	if (!m || !m->mf || m->mf->refused) {
		err_reply(fd, "no module %s", name);
		return NULL;
	}
	if (!m->mf->has_params) {
		err_reply(fd, "%s has no [Params]", name);
		return NULL;
	}
	return m;
}

static int read_whole(const char *path, char *buf, size_t len)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	ssize_t n = read(fd, buf, len - 1);
	close(fd);
	if (n < 0)
		return -1;
	buf[n] = 0;
	return (int)n;
}

/* "param-read <module>": the parameter type's schema (attributes included)
 * and the saved file, for hangar param on a module that is not running. */
static void req_param_read(int fd, const char *name)
{
	struct module *m = params_module(fd, name);
	if (!m)
		return;
	const struct mf_type *pt = mf_type_at(&G.set, m->mf->params_idx);
	static char file[HG_SCHEMA_MAX * 4];
	char path[HD_PATH + 48];
	mf_fmt(path, sizeof path, "%s/%s/params", G.paths.var, name);
	int flen = read_whole(path, file, sizeof file);
	if (flen < 0)
		flen = 0;
	static char buf[HG_CONTROL_MAX];
	int n = mf_fmt(buf, sizeof buf, "ok\ntype=%s hash=%016llx running=%d schema=%u file=%d\n", pt->name,
			 (unsigned long long)pt->hash, m->pid ? 1 : 0, pt->schema_bytes, flen);
	if ((size_t)n + pt->schema_bytes + (size_t)flen >= sizeof buf) {
		err_reply(fd, "the file does not fit a reply");
		return;
	}
	memcpy(buf + n, pt->schema, pt->schema_bytes);
	n += (int)pt->schema_bytes;
	memcpy(buf + n, file, (size_t)flen);
	n += flen;
	reply(fd, buf, (size_t)n, -1);
}

/* Writes text to path as the module's user when root, in a short-lived
 * child, with fsync: no control loop waits on it (docs/05 "Correcting the
 * parameters of a module that is not running"). */
static int write_as_module(const struct module *m, const char *path, const char *text, size_t len,
			   char *err, size_t errlen)
{
	uid_t uid = 0;
	gid_t gid = 0;
	int switch_user = G.is_root && lookup_user(m->mf->user, &uid, &gid) == 0;
	char dir[HD_PATH + 48];
	mf_fmt(dir, sizeof dir, "%s/%s", G.paths.var, m->name);
	if (mkdir(dir, 0750) == 0 && switch_user)
		(void)chown(dir, uid, gid);
	int errp[2];
	if (pipe2(errp, O_CLOEXEC) < 0) {
		mf_fmt(err, errlen, "pipe: %s", strerror(errno));
		return -1;
	}
	pid_t pid = fork();
	if (pid < 0) {
		mf_fmt(err, errlen, "fork: %s", strerror(errno));
		close(errp[0]);
		close(errp[1]);
		return -1;
	}
	if (pid == 0) {
		int e = 0;
		char tmp[HD_PATH + 80];
		mf_fmt(tmp, sizeof tmp, "%s.tmp.%ld", path, (long)getpid());
		if (switch_user && (setgid(gid) < 0 || setuid(uid) < 0))
			e = errno;
		int fd = e ? -1 : open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0640);
		if (!e && fd < 0)
			e = errno;
		if (!e && write(fd, text, len) != (ssize_t)len)
			e = errno ? errno : EIO;
		if (!e && fsync(fd) < 0)
			e = errno;
		if (fd >= 0)
			close(fd);
		if (!e && rename(tmp, path) < 0)
			e = errno;
		if (e)
			unlink(tmp);
		ssize_t w = write(errp[1], &e, sizeof e);
		(void)w;
		_exit(e ? 1 : 0);
	}
	close(errp[1]);
	int e = EIO;
	ssize_t r;
	while ((r = read(errp[0], &e, sizeof e)) < 0 && errno == EINTR)
		;
	close(errp[0]);
	int st;
	while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
		;
	if (r != (ssize_t)sizeof e || e) {
		mf_fmt(err, errlen, "cannot write %s: %s", path, strerror(r == (ssize_t)sizeof e ? e : EIO));
		return -1;
	}
	return 0;
}

/* "param-write <module>\n<name = value lines>": the saved values with the
 * lines applied, ranges checked, the version one above the saved one, set
 * by admin:<user>. For a module that is not running; a bad saved file is
 * replaced (that is what correcting it means), its version kept. */
static void req_param_write(int fd, const struct peer *p, char *req, size_t len)
{
	char *nl = memchr(req, '\n', len);
	const char *lines = nl ? nl + 1 : req + len;
	size_t lines_len = nl ? len - (size_t)(lines - req) : 0;
	if (nl)
		*nl = 0;
	const char *name = req + 12;
	while (*name == ' ')
		name++;
	struct module *m = params_module(fd, name);
	if (!m)
		return;
	if (m->pid) {
		err_reply(fd, "%s is running (pid %d): its parameters change through set_params", name, (int)m->pid);
		return;
	}
	const struct mf_type *pt = mf_type_at(&G.set, m->mf->params_idx);
	struct hg_schema *schema = malloc(sizeof *schema);
	struct hg_param_attr *attrs = calloc(HG_SCHEMA_MAX_FIELDS, sizeof *attrs);
	unsigned char *values = calloc(1, pt->payload_bytes + 8);
	unsigned char present[HG_SCHEMA_MAX_FIELDS];
	static char text[HG_SCHEMA_MAX * 4];
	char err[MF_ERR], path[HD_PATH + 48];
	if (!schema || !attrs || !values) {
		err_reply(fd, "out of memory");
		goto out;
	}
	if (hg_schema_parse(pt->schema, pt->schema_bytes, schema, err, sizeof err) < 0 ||
	    (schema->type.name = pt->name, hg_params_attrs(&schema->type, attrs, err, sizeof err)) < 0 ||
	    hg_params_defaults(&schema->type, attrs, values, err, sizeof err) < 0) {
		err_reply(fd, "type %s: %s", pt->name, err);
		goto out;
	}
	struct hg_params_header hdr;
	memset(&hdr, 0, sizeof hdr);
	mf_fmt(path, sizeof path, "%s/%s/params", G.paths.var, name);
	int flen = read_whole(path, text, sizeof text);
	if (flen >= 0) {
		if (hg_params_file_parse(&schema->type, text, (size_t)flen, values, &hdr, present, 1, err, sizeof err) < 0) {
			hd_log("param-write %s: the saved file is refused (%s): corrected from the defaults", name, err);
			hg_params_defaults(&schema->type, attrs, values, err, sizeof err);
		}
	}
	struct hg_params_header given;
	if (hg_params_file_parse(&schema->type, lines, lines_len, values, &given, present, 0, err, sizeof err) < 0) {
		err_reply(fd, "param-write %s: %s", name, err);
		goto out;
	}
	char reason[HG_PARAMS_REASON];
	for (uint32_t i = 0; i < schema->type.n_fields; i++)
		if (hg_param_check_range(&schema->type.fields[i], &attrs[i], values, reason, sizeof reason) < 0) {
			err_reply(fd, "param-write %s: %s", name, reason);
			goto out;
		}
	struct hg_params_header out_hdr = { .version = hdr.version + 1 };
	char user[64];
	lookup_user_name(p->uid, user, sizeof user);
	mf_fmt(out_hdr.changed_by, sizeof out_hdr.changed_by, "admin:%s", user);
	int n = hg_params_file_format(&schema->type, values, &out_hdr, text, sizeof text);
	if (n < 0 || n >= (int)sizeof text) {
		err_reply(fd, "param-write %s: the file does not fit", name);
		goto out;
	}
	if (write_as_module(m, path, text, (size_t)n, err, sizeof err) < 0) {
		err_reply(fd, "param-write %s: %s", name, err);
		goto out;
	}
	char ok[64];
	n = mf_fmt(ok, sizeof ok, "ok version=%llu\n", (unsigned long long)out_hdr.version);
	reply(fd, ok, (size_t)n, -1);
	hd_log("param-write %s by admin:%s: version %llu written", name, user, (unsigned long long)out_hdr.version);
out:
	free(schema);
	free(attrs);
	free(values);
}

void control_handle(int fd)
{
	static char req[HG_CONTROL_MAX + 1];
	ssize_t n;
	while ((n = recv(fd, req, HG_CONTROL_MAX, 0)) < 0 && errno == EINTR)
		;
	if (n <= 0) {
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return;
		epoll_del(fd);
		close(fd);
		return;
	}
	req[n] = 0;
	struct peer p;
	if (peer_of(fd, &p) < 0) {
		err_reply(fd, "cannot identify the caller");
		goto done;
	}
	/* The first word is the request. */
	char word[32];
	size_t wl = 0;
	while (wl < (size_t)n && wl < sizeof word - 1 && req[wl] != ' ' && req[wl] != '\n')
		word[wl] = req[wl], wl++;
	word[wl] = 0;
	char *arg = req + wl;
	while (*arg == ' ')
		arg++;
	char *nl = strchr(arg, '\n');
	if (nl && strcmp(word, "open") != 0 && strcmp(word, "call") != 0 && strcmp(word, "param-write") != 0)
		*nl = 0;

	if (strcmp(word, "page") == 0) {
		req_page(fd, &p);
	} else if (strcmp(word, "open") == 0) {
		req_open(fd, &p, req, (size_t)n);
	} else if (strcmp(word, "restart-self") == 0) {
		struct module *m = module_by_pid(p.pid);
		if (!m)
			err_reply(fd, "pid %d is not a module's main process", (int)p.pid);
		else {
			module_stop(m, ST_STARTING, 0, "restart requested by the module");
			ok_reply(fd);
		}
	} else if (strcmp(word, "status") == 0) {
		if (!is_reader(&p))
			err_reply(fd, "permission denied: group hangar required");
		else
			req_status(fd);
	} else if (strcmp(word, "paths") == 0) {
		/* Where this manager keeps things, for a tool that was not spawned by
		 * it and so has no HANGAR_TOPIC_DIR in its environment: the
		 * configuration may put the topics on a tmpfs outside $HANGAR_ROOT. */
		if (!is_reader(&p)) {
			err_reply(fd, "permission denied: group hangar required");
		} else {
			static char buf[2048];
			int n = mf_fmt(buf, sizeof buf, "ok\nroot %s\ntopics %s\nmodules %s\nmsg %s\nlog %s\n",
					 G.paths.root[0] ? G.paths.root : "/", G.conf.topic_dir, G.conf.module_dir,
					 G.conf.msg_dir, G.conf.log_dir);
			reply(fd, buf, (size_t)n, -1);
		}
	} else if (strcmp(word, "wiring") == 0) {
		if (!is_reader(&p))
			err_reply(fd, "permission denied: group hangar required");
		else
			req_wiring(fd, arg);
	} else if (strcmp(word, "method") == 0) {
		if (!is_reader(&p))
			err_reply(fd, "permission denied: group hangar required");
		else
			req_method(fd, arg);
	} else if (strcmp(word, "clear-failsafe") == 0 &&
		   (is_admin(&p) || (module_by_pid(p.pid) && strcmp(module_by_pid(p.pid)->name, "safety") == 0))) {
		req_clear_failsafe(fd);
	} else if (!is_admin(&p)) {
		err_reply(fd, "permission denied: hangar-admin required for %s", word);
	} else if (strcmp(word, "start") == 0) {
		req_start(fd, arg, 0);
	} else if (strcmp(word, "restart") == 0) {
		req_start(fd, arg, 1);
	} else if (strcmp(word, "stop") == 0) {
		req_stop(fd, arg);
	} else if (strcmp(word, "call") == 0) {
		req_call(fd, &p, req, (size_t)n);
	} else if (strcmp(word, "param-read") == 0) {
		req_param_read(fd, arg);
	} else if (strcmp(word, "param-write") == 0) {
		req_param_write(fd, &p, req, (size_t)n);
	} else if (strcmp(word, "reload") == 0) {
		char err[MF_ERR];
		if (do_reload(err, sizeof err) < 0)
			err_reply(fd, "%s", err);
		else
			ok_reply(fd);
	} else if (strcmp(word, "reexec") == 0) {
		char err[MF_ERR];
		ok_reply(fd);
		epoll_del(fd);
		close(fd);
		if (do_reexec(err, sizeof err) < 0)
			hd_log("reexec failed: %s", err);
		return;
	} else if (strcmp(word, "clear-failsafe") == 0) {
		req_clear_failsafe(fd);
	} else {
		err_reply(fd, "unknown request %s", word);
	}
done:
	epoll_del(fd);
	close(fd);
}
