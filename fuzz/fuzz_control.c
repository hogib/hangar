// SPDX-License-Identifier: GPL-3.0-or-later
/* The manager's control socket (hangard/control.c control_handle and every
 * request behind it; hangard/topics.c topic_create for `open`). The socket is
 * a local one, mode 0660 group hangar: its callers are the modules the manager
 * started (each as its own user) and whoever is in the groups hangar and
 * hangar-admin. The manager is root, so a request is input from a process of
 * lower privilege.
 *
 * control.c is included whole: the harness puts one end of a socket pair in
 * its table of connections, sends the request on the other and calls
 * control_handle, which reads the caller's credentials (this process's) from
 * the socket as it does in the manager. The manager around it is a set of five
 * manifests from the tree (safety, mux, depth_hold, mavlink, the vehicle's bridge),
 * checked by mf_set_check, in a root of this process's own; starting, stopping,
 * reloading and re-executing are stubs that only count.
 *
 * Input: one mode byte, then requests, each a 16-bit little-endian length and
 * that many bytes.
 *
 *   bit 0     the caller is hangar-admin (the manager's own user)
 *   bit 1     the caller is in group hangar
 *   bits 2-3  the caller is the main process of: 0 no module, 1 bridge,
 *             2 safety, 3 mux
 *   bit 4     the system is in failsafe
 *   bit 5     depth_hold is running (its parameters are not written then)
 *
 * A file the manager is asked to create is held to 64 MB by RLIMIT_FSIZE:
 * `open` reserves slots x payload bytes, which the request chooses. */
#include "fuzz.h"

#include <dirent.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>

#include "control.c"
#include "wiring.h"

struct hangard G;
static unsigned n_start, n_stop, n_reload, n_reexec;

void hd_log(const char *fmt, ...)
{
	(void)fmt;
}

void hd_die(const char *fmt, ...)
{
	(void)fmt;
	abort();
}

int epoll_add(int fd, uint64_t tag)
{
	(void)fd, (void)tag;
	return 0;
}

void epoll_del(int fd)
{
	(void)fd;
}

struct module *module_find(const char *name)
{
	for (int i = 0; i < G.n_modules; i++)
		if (strcmp(G.modules[i].name, name) == 0)
			return &G.modules[i];
	return NULL;
}

void module_start(struct module *m)
{
	(void)m;
	n_start++;
}

void module_stop(struct module *m, enum state next, int count_failure, const char *why)
{
	(void)m, (void)next, (void)count_failure, (void)why;
	n_stop++;
}

void recompute_sys_state(void)
{
}

void request_publish(void)
{
}

int do_reload(char *err, size_t errlen)
{
	(void)err, (void)errlen;
	n_reload++;
	return 0;
}

int do_reexec(char *err, size_t errlen)
{
	n_reexec++;
	mf_fmt(err, errlen, "not in a harness");
	return -1;
}

static char root[256];
static struct hg_schema admin_schema;

/* Removes what is under dir (two levels: the topics, and var/<module>/params). */
static void empty(const char *dir, int keep_own)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	while (d && (e = readdir(d))) {
		char p[700];
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		if (keep_own && (!strcmp(e->d_name, ".dir") || strstr(e->d_name, "hangar.vehicle.admin.req")))
			continue;
		snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
		if (unlink(p) < 0) {
			empty(p, 0);
			rmdir(p);
		}
	}
	if (d)
		closedir(d);
}

static void cleanup(void)
{
	empty(root, 0);
	rmdir(root);
}

static void add_module(const char *name, const char *file)
{
	char path[600], err[MF_ERR];
	snprintf(path, sizeof path, "%s/%s", FUZZ_REPO, file);
	FILE *f = fopen(path, "r");
	static char text[1 << 16];
	size_t n = f ? fread(text, 1, sizeof text, f) : 0;
	if (!f || n == 0)
		abort();
	fclose(f);
	struct mf_module *m = calloc(1, sizeof *m);
	snprintf(path, sizeof path, "/etc/hangar/modules/%s.module", name);
	if (!m || mf_parse_text(text, n, name, path, m, err, sizeof err) < 0 || mf_set_add(&G.set, m) < 0) {
		fprintf(stderr, "the harness's manifest %s: %s\n", file, err);
		abort();
	}
}

static void setup(void)
{
	const char *tmp = getenv("TMPDIR");
	snprintf(root, sizeof root, "%s/hg-fuzz-control-XXXXXX", tmp && *tmp ? tmp : "/tmp");
	if (!mkdtemp(root))
		abort();
	atexit(cleanup);
	struct rlimit lim = { 64u << 20, 64u << 20 };
	setrlimit(RLIMIT_FSIZE, &lim);
	signal(SIGXFSZ, SIG_IGN);

	memset(&G, 0, sizeof G);
	mf_fmt(G.paths.root, sizeof G.paths.root, "%s", root);
	mf_fmt(G.paths.var, sizeof G.paths.var, "%s/var", root);
	mf_fmt(G.paths.run, sizeof G.paths.run, "%s/run", root);
	mf_fmt(G.paths.registry, sizeof G.paths.registry, "%s/run/registry", root);
	mf_fmt(G.paths.registry_tmp, sizeof G.paths.registry_tmp, "%s/run/.registry.tmp", root);
	mf_fmt(G.conf.role, sizeof G.conf.role, "vehicle");
	mf_fmt(G.conf.topic_dir, sizeof G.conf.topic_dir, "%s/topics", root);
	mf_fmt(G.conf.module_dir, sizeof G.conf.module_dir, "/etc/hangar/modules");
	mf_fmt(G.conf.msg_dir, sizeof G.conf.msg_dir, "%s/msg", FUZZ_REPO);
	mf_fmt(G.conf.log_dir, sizeof G.conf.log_dir, "%s/run/log", root);
	mkdir(G.paths.var, 0700);
	mkdir(G.paths.run, 0700);
	mkdir(G.conf.topic_dir, 0700);
	G.epfd = G.sigfd = G.timfd = G.ctlfd = G.notify_fd = G.reexec_state_fd = -1;
	G.uid = getuid();
	G.next_call_id = 1;

	mf_set_init(&G.set, "vehicle", "", G.conf.msg_dir);
	mf_fmt(G.set.bridge_conf_path, sizeof G.set.bridge_conf_path, "%s/profiles/rov-vehicle/bridge.conf", FUZZ_REPO);
	add_module("bridge", "profiles/rov-vehicle/bridge.module.off");
	add_module("safety", "modules/safety/safety.module");
	add_module("mux", "modules/mux/mux.module");
	add_module("depth_hold", "modules/depth_hold/depth_hold.module");
	add_module("mavlink", "profiles/rov-vehicle/mavlink.module");
	mf_set_check(&G.set);
	static char wtext[HD_WIRING_MAX];
	for (int i = 0; i < G.set.n_modules; i++) {
		struct module *m = &G.modules[G.n_modules++];
		struct mf_module *mf = G.set.modules[i];
		if (mf->refused) {
			fprintf(stderr, "the harness's module %s is refused: %s\n", mf->name, mf->reason);
			abort();
		}
		mf_fmt(m->name, sizeof m->name, "%s", mf->name);
		m->mf = mf;
		m->st = ST_READY;
		m->pidfd = m->wiring_fd = m->page_fd = -1;
		int n = wiring_text(&G.set, mf, wtext, sizeof wtext);
		if (n < 0)
			abort();
		m->wiring = strdup(wtext);
		m->wiring_len = (size_t)n;
		m->page = calloc(1, HD_PAGE_BYTES);
		if (!m->wiring || !m->page)
			abort();
		snprintf((char *)m->page + 64, 128, "ready: \x01 a status line");
	}

	/* The manager's own directory word and admin topic, as hangard.c opens them. */
	setenv("HANGAR_TOPIC_DIR", G.conf.topic_dir, 1);
	unsetenv("HANGAR_CONTROL");
	unsetenv("HANGAR_WIRING");
	unsetenv("HANGAR_WIRING_FILE");
	unsetenv("HANGAR_PEER_TOPIC_DIR");
	if (topic_dir_prepare() < 0)
		abort();
	const struct mf_type *adm = mf_type_find(&G.set, "req:hangar");
	if (!adm || hg_schema_parse(adm->schema, adm->schema_bytes, &admin_schema, NULL, 0) < 0)
		abort();
	admin_schema.type.name = "req:hangar";
	G.pub_admin = hg_pub_open("hangar.vehicle.admin.req", &admin_schema.type, &(struct hg_pub_opts){ .slots = 16 });
	if (!G.pub_admin)
		abort();
}

/* The reply, and with it any descriptor the manager passed: closed here, as a
 * caller that does not want it would. */
static void take_reply(int sock)
{
	static char buf[HG_CONTROL_MAX + 64];
	union {
		struct cmsghdr h;
		unsigned char b[CMSG_SPACE(4 * sizeof(int))];
	} cm;
	for (;;) {
		struct iovec iov = { buf, sizeof buf };
		struct msghdr mh;
		memset(&mh, 0, sizeof mh);
		mh.msg_iov = &iov;
		mh.msg_iovlen = 1;
		mh.msg_control = &cm;
		mh.msg_controllen = sizeof cm;
		ssize_t n = recvmsg(sock, &mh, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
		if (n <= 0)
			break;
		FZ_CHECK(n >= 3 && (memcmp(buf, "ok", 2) == 0 || memcmp(buf, "err ", 4) == 0));
		FZ_NOTE("reply: %.*s", (int)(n > 100 ? 100 : n), buf);
		for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
			if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS)
				continue;
			size_t k = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
			for (size_t i = 0; i < k; i++) {
				int fd;
				memcpy(&fd, CMSG_DATA(c) + i * sizeof(int), sizeof fd);
				close(fd);
			}
		}
	}
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (!root[0])
		setup();
	if (size < 1)
		return 0;
	unsigned mode = data[0];
	data++;
	size--;
	gid_t nobody = (gid_t)-2;
	G.uid = (mode & 1) ? getuid() : (uid_t)-2;
	G.have_gid_admin = 0;
	G.have_gid_hangar = 1;
	G.gid_hangar = (mode & 2) ? getgid() : nobody;
	G.sys = (mode & 16) ? SYS_FAILSAFE : SYS_NOMINAL;
	mf_fmt(G.sys_reason, sizeof G.sys_reason, "%s", (mode & 16) ? "mux: gave up" : "");
	G.shutting_down = 0;
	static const char *const callers[] = { "", "bridge", "safety", "mux" };
	for (int i = 0; i < G.n_modules; i++) {
		struct module *m = &G.modules[i];
		m->pid = strcmp(m->name, callers[(mode >> 2) & 3]) == 0 ? getpid() : 0;
		if ((mode & 32) && strcmp(m->name, "depth_hold") == 0)
			m->pid = getppid();
		m->stopping = 0;
		m->failures = 0;
		m->st = ST_READY;
	}
	while (size >= 2) {
		size_t len = data[0] | (size_t)data[1] << 8;
		data += 2;
		size -= 2;
		if (len > size)
			len = size;
		if (len > HG_CONTROL_MAX)
			len = HG_CONTROL_MAX;
		int sp[2];
		if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, sp) < 0)
			abort();
		if (len && send(sp[1], data, len, MSG_NOSIGNAL) != (ssize_t)len)
			abort();
		if (!len)
			shutdown(sp[1], SHUT_WR); /* a caller that connects and goes away */
		FZ_CHECK(n_conns < HD_MAX_CONNS);
		conns[n_conns].fd = sp[0];
		conns[n_conns].at = hg_now_ns();
		n_conns++;
		control_handle(sp[0]);
		/* One request, one answer, and the connection is closed. */
		FZ_CHECK(n_conns == 0);
		take_reply(sp[1]);
		close(sp[1]);
		data += len;
		size -= len;
	}
	control_tick(hg_now_ns() + 3 * CONN_IDLE_NS);
	empty(G.conf.topic_dir, 1);
	empty(G.paths.var, 0);
	return 0;
}
