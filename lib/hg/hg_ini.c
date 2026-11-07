// SPDX-License-Identifier: LGPL-3.0-or-later
/* The strict INI walker of hg_ini.h. */
#define _GNU_SOURCE
#include "hg_ini.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "hg_wiring.h"

static void trim(const char **s, size_t *n)
{
	while (*n && (**s == ' ' || **s == '\t' || **s == '\r'))
		(*s)++, (*n)--;
	while (*n && ((*s)[*n - 1] == ' ' || (*s)[*n - 1] == '\t' || (*s)[*n - 1] == '\r'))
		(*n)--;
}

static int copy(char *dst, size_t cap, const char *s, size_t n)
{
	if (n >= cap)
		return -1;
	memcpy(dst, s, n);
	dst[n] = 0;
	return 0;
}

int hg_ini_parse(const char *text, size_t len, hg_ini_section_fn on_section, hg_ini_key_fn on_key, void *ctx,
		 char *err, size_t errlen)
{
	int line = 0, in_section = 0;
	char why[256];
	const char *p = text, *end = text + len;
	while (p < end) {
		const char *nl = memchr(p, '\n', (size_t)(end - p));
		const char *l = p;
		size_t n = nl ? (size_t)(nl - p) : (size_t)(end - p);
		p = nl ? nl + 1 : end;
		line++;
		trim(&l, &n);
		if (n == 0 || l[0] == '#' || l[0] == ';')
			continue;
		why[0] = 0;
		if (l[0] == '[') {
			if (l[n - 1] != ']') {
				snprintf(why, sizeof why, "a section line must end in ']'");
				goto refuse;
			}
			const char *s = l + 1;
			size_t sn = n - 2;
			trim(&s, &sn);
			const char *sp = memchr(s, ' ', sn);
			char section[HG_INI_SECTION], arg[HG_INI_ARG];
			arg[0] = 0;
			if (sp) {
				if (copy(section, sizeof section, s, (size_t)(sp - s)) < 0) {
					snprintf(why, sizeof why, "section name too long");
					goto refuse;
				}
				const char *a = sp + 1;
				size_t an = sn - (size_t)(sp - s) - 1;
				trim(&a, &an);
				if (copy(arg, sizeof arg, a, an) < 0) {
					snprintf(why, sizeof why, "section argument too long");
					goto refuse;
				}
			} else if (copy(section, sizeof section, s, sn) < 0) {
				snprintf(why, sizeof why, "section name too long");
				goto refuse;
			}
			if (section[0] == 0) {
				snprintf(why, sizeof why, "empty section name");
				goto refuse;
			}
			in_section = 1;
			if (on_section && on_section(ctx, section, arg, line, why, sizeof why) < 0)
				goto refuse;
			continue;
		}
		const char *eq = memchr(l, '=', n);
		if (!eq) {
			snprintf(why, sizeof why, "expected [Section] or Key=Value");
			goto refuse;
		}
		if (!in_section) {
			snprintf(why, sizeof why, "a key before any section");
			goto refuse;
		}
		const char *k = l;
		size_t kn = (size_t)(eq - l);
		const char *v = eq + 1;
		size_t vn = n - kn - 1;
		trim(&k, &kn);
		trim(&v, &vn);
		char key[HG_INI_KEY], value[HG_INI_VALUE];
		if (kn == 0) {
			snprintf(why, sizeof why, "empty key");
			goto refuse;
		}
		if (copy(key, sizeof key, k, kn) < 0) {
			snprintf(why, sizeof why, "key too long");
			goto refuse;
		}
		if (copy(value, sizeof value, v, vn) < 0) {
			snprintf(why, sizeof why, "value too long");
			goto refuse;
		}
		if (on_key && on_key(ctx, key, value, line, why, sizeof why) < 0)
			goto refuse;
		continue;
refuse:
		if (err && errlen)
			snprintf(err, errlen, "line %d: %s", line, why[0] ? why : "refused");
		errno = EINVAL;
		return -1;
	}
	return 0;
}

int hg_ini_read_file(const char *path, char *buf, size_t cap, size_t *len)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	size_t n = 0;
	for (;;) {
		if (n == cap) {
			close(fd);
			errno = EFBIG;
			return -1;
		}
		ssize_t k = read(fd, buf + n, cap - n);
		if (k < 0) {
			if (errno == EINTR)
				continue;
			int e = errno;
			close(fd);
			errno = e;
			return -1;
		}
		if (k == 0)
			break;
		n += (size_t)k;
	}
	close(fd);
	*len = n;
	return 0;
}

int64_t hg_ini_parse_size(const char *s)
{
	if (!s || !*s)
		return -1;
	uint64_t v = 0;
	const char *p = s;
	if (*p < '0' || *p > '9')
		return -1;
	while (*p >= '0' && *p <= '9') {
		if (v > (uint64_t)1 << 50)
			return -1;
		v = v * 10 + (uint64_t)(*p - '0');
		p++;
	}
	uint64_t unit;
	if (*p == 0)
		unit = v == 0 ? 1 : 0; /* a bare number is only 0 */
	else if (strcmp(p, "KB") == 0)
		unit = 1024u;
	else if (strcmp(p, "MB") == 0)
		unit = 1024u * 1024u;
	else if (strcmp(p, "GB") == 0)
		unit = 1024u * 1024u * 1024u;
	else
		return -1;
	if (!unit)
		return -1;
	if (v > ((uint64_t)1 << 62) / unit)
		return -1;
	return (int64_t)(v * unit);
}

int64_t hg_ini_parse_duration(const char *s)
{
	size_t n = strlen(s);
	if (n > 3 && strcmp(s + n - 3, "min") == 0) {
		char buf[64];
		if (n - 3 >= sizeof buf - 1)
			return -1;
		memcpy(buf, s, n - 3);
		buf[n - 3] = 's';
		buf[n - 2] = 0;
		int64_t v = hg_parse_duration(buf);
		if (v < 0 || v > INT64_MAX / 60)
			return -1;
		return v * 60;
	}
	return hg_parse_duration(s);
}

int hg_ini_parse_bool(const char *s)
{
	if (strcmp(s, "yes") == 0)
		return 1;
	if (strcmp(s, "no") == 0)
		return 0;
	return -1;
}
