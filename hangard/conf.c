// SPDX-License-Identifier: GPL-3.0-or-later
/* hangard.conf (docs/03 "Running it") and the fixed paths, relocated under
 * $HANGAR_ROOT so that the suites run as an ordinary user. */
#include "hangard.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void join(char *out, size_t len, const char *root, const char *path)
{
	mf_fmt(out, len, "%s%s", root, path);
}

void paths_init(struct paths *p, const char *conf_file)
{
	memset(p, 0, sizeof *p);
	const char *root = getenv("HANGAR_ROOT");
	if (root && *root) {
		mf_fmt(p->root, sizeof p->root, "%s", root);
		size_t n = strlen(p->root);
		while (n > 1 && p->root[n - 1] == '/')
			p->root[--n] = 0;
	}
	join(p->etc, sizeof p->etc, p->root, "/etc/hangar");
	join(p->run, sizeof p->run, p->root, "/run/hangar");
	join(p->var, sizeof p->var, p->root, "/var/lib/hangar");
	join(p->share_msg, sizeof p->share_msg, p->root, "/usr/share/hangar/msg");
	join(p->shm, sizeof p->shm, p->root, "/dev/shm/hangar");
	join(p->journal, sizeof p->journal, p->root, "/run/systemd/journal/stdout");
	mf_fmt(p->control, sizeof p->control, "%s/control", p->run);
	mf_fmt(p->registry, sizeof p->registry, "%s/registry", p->run);
	mf_fmt(p->registry_tmp, sizeof p->registry_tmp, "%s/.registry.tmp", p->run);
	mf_fmt(p->pages, sizeof p->pages, "%s/modules", p->run);
	mf_fmt(p->neutral, sizeof p->neutral, "%s/neutral", p->run);
	if (conf_file)
		mf_fmt(p->conf, sizeof p->conf, "%s", conf_file);
	else
		mf_fmt(p->conf, sizeof p->conf, "%s/hangard.conf", p->etc);
}

static char *trim(char *s)
{
	while (*s == ' ' || *s == '\t')
		s++;
	size_t n = strlen(s);
	while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n'))
		s[--n] = 0;
	return s;
}

static int set_path(char *dst, size_t len, const char *v)
{
	if (v[0] != '/' || strlen(v) >= len)
		return -1;
	mf_fmt(dst, len, "%s", v);
	return 0;
}

/* "256 KB", "1 MB", "4096": bytes. */
static int parse_size(const char *v, uint64_t *out)
{
	char *e;
	unsigned long long n = strtoull(v, &e, 10);
	if (e == v)
		return -1;
	while (*e == ' ')
		e++;
	if (!*e || strcmp(e, "B") == 0)
		*out = n;
	else if (strcmp(e, "KB") == 0 || strcmp(e, "K") == 0)
		*out = n << 10;
	else if (strcmp(e, "MB") == 0 || strcmp(e, "M") == 0)
		*out = n << 20;
	else
		return -1;
	return 0;
}

int conf_load(const char *path, const struct paths *pp, struct conf *c, char *err, size_t errlen)
{
	memset(c, 0, sizeof *c);
	mf_fmt(c->module_dir, sizeof c->module_dir, "%s/modules", pp->etc);
	mf_fmt(c->msg_dir, sizeof c->msg_dir, "%s", pp->share_msg);
	mf_fmt(c->topic_dir, sizeof c->topic_dir, "%s", pp->shm);
	mf_fmt(c->bridge_conf, sizeof c->bridge_conf, "%s/bridge.conf", pp->etc);
	mf_fmt(c->dataset_dir, sizeof c->dataset_dir, "%s/datasets", pp->etc);
	mf_fmt(c->recording_dir, sizeof c->recording_dir, "%s/recordings", pp->var);
	mf_fmt(c->log_dir, sizeof c->log_dir, "%s/log", pp->run);
	c->health_tick_ns = 100000000;
	c->max_modules = 64;
	c->log_cap = 256 << 10;

	FILE *f = fopen(path, "re");
	if (!f) {
		mf_fmt(err, errlen, "%s:0: %s", path, strerror(errno));
		return -1;
	}
	char line[1024];
	int ln = 0;
	unsigned seen = 0;
	int rc = 0;
	while (fgets(line, sizeof line, f)) {
		ln++;
		char *t = trim(line);
		if (!*t || *t == '#' || *t == ';')
			continue;
		char *eq = strchr(t, '=');
		if (!eq) {
			mf_fmt(err, errlen, "%s:%d: not a Key=Value line", path, ln);
			rc = -1;
			break;
		}
		*eq = 0;
		const char *k = trim(t), *v = trim(eq + 1);
		static const char *const keys[] = { "Role",       "PrivilegedModule", "ModuleDir",
						    "MsgDir",     "TopicDir",         "BridgeConf",
						    "DatasetDir", "RecordingDir",     "HealthTick",
						    "MaxModules", "LogFallbackDir",   "LogFallbackCap" };
		int ki = -1;
		for (int i = 0; i < (int)(sizeof keys / sizeof keys[0]); i++)
			if (strcmp(keys[i], k) == 0)
				ki = i;
		if (ki < 0) {
			mf_fmt(err, errlen, "%s:%d: unknown key %s", path, ln, k);
			rc = -1;
			break;
		}
		if (seen & (1u << ki)) {
			mf_fmt(err, errlen, "%s:%d: %s given twice", path, ln, k);
			rc = -1;
			break;
		}
		seen |= 1u << ki;
		int bad = 0;
		switch (ki) {
		case 0:
			if (strcmp(v, "vehicle") != 0 && strcmp(v, "topside") != 0)
				bad = 1;
			else
				mf_fmt(c->role, sizeof c->role, "%s", v);
			break;
		case 1:
			if (!mf_valid_module_name(v))
				bad = 1;
			else
				mf_fmt(c->privileged_module, sizeof c->privileged_module, "%s", v);
			break;
		case 2:
			bad = set_path(c->module_dir, sizeof c->module_dir, v);
			break;
		case 3:
			bad = set_path(c->msg_dir, sizeof c->msg_dir, v);
			break;
		case 4:
			bad = set_path(c->topic_dir, sizeof c->topic_dir, v);
			break;
		case 5:
			bad = set_path(c->bridge_conf, sizeof c->bridge_conf, v);
			break;
		case 6:
			bad = set_path(c->dataset_dir, sizeof c->dataset_dir, v);
			break;
		case 7:
			bad = set_path(c->recording_dir, sizeof c->recording_dir, v);
			break;
		case 8: {
			int64_t d = mf_parse_duration(v);
			if (d < 1000000)
				bad = 1; /* under a millisecond would spin */
			else
				c->health_tick_ns = (uint64_t)d;
			break;
		}
		case 9: {
			char *e;
			long n = strtol(v, &e, 10);
			if (*e || n < 1 || n > HD_MAX_MODULES)
				bad = 1;
			else
				c->max_modules = (int)n;
			break;
		}
		case 10:
			bad = set_path(c->log_dir, sizeof c->log_dir, v);
			break;
		case 11:
			bad = parse_size(v, &c->log_cap);
			break;
		}
		if (bad) {
			mf_fmt(err, errlen, "%s:%d: %s: '%s' does not parse", path, ln, k, v);
			rc = -1;
			break;
		}
	}
	fclose(f);
	if (rc == 0 && !c->role[0]) {
		mf_fmt(err, errlen, "%s:%d: Role= is required (vehicle or topside)", path, ln);
		rc = -1;
	}
	if (rc < 0)
		errno = EINVAL;
	return rc;
}
