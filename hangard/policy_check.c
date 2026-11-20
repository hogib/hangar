// SPDX-License-Identifier: GPL-3.0-or-later
/* The manager's check of a policy module (docs/09-learning.md "Policy
 * modules"): a module whose Exec runs hangar-policy is policy_<Name>, Name
 * from the spec its Exec names, and publishes policy.<Name>.wrench (wrench),
 * .active (policy_active) and .load (policy_load). A file of its own, called
 * once from check_module, so that manifest.c is otherwise untouched. */
#include "manifest.h"

#include <stdio.h>
#include <string.h>

/* The spec's Name= in its [Policy] section, by a plain line scan: the
 * policy itself parses the spec strictly at start. */
static int spec_name(const char *path, char *out, size_t len)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;
	char line[512];
	int in_policy = 0, found = 0;
	while (fgets(line, sizeof line, f)) {
		char *s = line;
		while (*s == ' ' || *s == '\t')
			s++;
		s[strcspn(s, "\r\n")] = 0;
		if (*s == '[') {
			in_policy = strncmp(s, "[Policy]", 8) == 0;
			continue;
		}
		if (!in_policy || strncmp(s, "Name", 4) != 0)
			continue;
		char *v = s + 4;
		while (*v == ' ' || *v == '\t')
			v++;
		if (*v != '=')
			continue;
		v++;
		while (*v == ' ' || *v == '\t')
			v++;
		size_t n = strlen(v);
		while (n && (v[n - 1] == ' ' || v[n - 1] == '\t'))
			v[--n] = 0;
		snprintf(out, len, "%s", v);
		found = 1;
		break;
	}
	fclose(f);
	return found ? 0 : -1;
}

int mf_policy_check(const struct mf_module *m, char *why, size_t len)
{
	if (m->exec.argc < 1)
		return 0;
	const char *bin = strrchr(m->exec.argv[0], '/');
	bin = bin ? bin + 1 : m->exec.argv[0];
	if (strcmp(bin, "hangar-policy") != 0)
		return 0;
	if (m->exec.argc != 2) {
		snprintf(why, len, "hangar-policy takes one argument, its policy spec");
		return -1;
	}
	char name[64];
	if (spec_name(m->exec.argv[1], name, sizeof name) < 0) {
		snprintf(why, len, "hangar-policy: %s: no readable [Policy] Name=", m->exec.argv[1]);
		return -1;
	}
	char want[96];
	snprintf(want, sizeof want, "policy_%s", name);
	if (strcmp(m->name, want) != 0) {
		snprintf(why, len, "hangar-policy runs policy %s, so the module must be %s", name, want);
		return -1;
	}
	static const char *suffix[] = { "wrench", "active", "load" };
	static const char *type[] = { "wrench", "policy_active", "policy_load" };
	for (int k = 0; k < 3; k++) {
		char topic[128];
		snprintf(topic, sizeof topic, "policy.%s.%s", name, suffix[k]);
		int ok = 0;
		for (int i = 0; i < m->n_pubs; i++)
			if (strcmp(m->pubs[i].topic, topic) == 0 && strcmp(m->pubs[i].type, type[k]) == 0)
				ok = 1;
		if (!ok) {
			snprintf(why, len, "hangar-policy: needs [Publish %s] Type=%s", topic, type[k]);
			return -1;
		}
	}
	return 0;
}
