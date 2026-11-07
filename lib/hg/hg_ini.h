// SPDX-License-Identifier: LGPL-3.0-or-later
/* The manifest syntax (docs/02-manifest.md "Syntax") as a strict walker,
 * for the files that share it: dataset specs, recorder.conf, pairer.conf,
 * and the manifests a spec is checked against. Full-line comments only
 * ('#' or ';' first), `[Section]` or `[Section argument]`, `Key=Value`
 * with the blanks around '=' and at the ends of the value dropped. A line
 * that is none of these, or a key before any section, is refused with its
 * line number. Repeated keys are the caller's to refuse: the walker
 * reports every line in order. */
#ifndef HG_INI_H
#define HG_INI_H

#include <stddef.h>
#include <stdint.h>

#define HG_INI_SECTION 64
#define HG_INI_ARG 128
#define HG_INI_KEY 64
#define HG_INI_VALUE 1024

/* Both return 0 to go on, -1 to stop; a refusal puts its reason in err
 * (the walker prefixes the line number). */
typedef int (*hg_ini_section_fn)(void *ctx, const char *section, const char *arg, int line, char *err,
				 size_t errlen);
typedef int (*hg_ini_key_fn)(void *ctx, const char *key, const char *value, int line, char *err, size_t errlen);

/* 0, or -1 with errno EINVAL and "line N: <reason>" in err. */
int hg_ini_parse(const char *text, size_t len, hg_ini_section_fn on_section, hg_ini_key_fn on_key, void *ctx,
		 char *err, size_t errlen);

/* Reads a whole file into buf (at most cap bytes; longer is EFBIG). 0, or
 * -1 with errno. */
int hg_ini_read_file(const char *path, char *buf, size_t cap, size_t *len);

/* "32MB", "1GB", "512KB", "0": bytes in powers of 1024; -1 if malformed. */
int64_t hg_ini_parse_size(const char *s);
/* "60s", "200ms", "1min", "0": nanoseconds; -1 if malformed (the manifest's
 * duration grammar: hg_parse_duration plus `min`). */
int64_t hg_ini_parse_duration(const char *s);
/* yes/no; -1 if neither. */
int hg_ini_parse_bool(const char *s);

#endif
