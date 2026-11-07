// SPDX-License-Identifier: LGPL-3.0-or-later
/* Parameters (docs/05-parameters.md): a module's settings as one typed,
 * fixed-size struct, declared as a .msg type with a default, a range and
 * an optional @restart mark per field. They change only through the
 * `set_params` call the library serves inside hg_serve(); every change is
 * checked, applied whole, saved, published on `<module>.params` and so
 * recorded. Saved values are loaded at the next start, strictly: a file
 * that fails stops the module (exit status 78) rather than running a
 * vehicle on gains nobody chose.
 *
 * The first half is the module's API. The second is the parameter grammar
 * (values, ranges, the saved file), which the manager's `param-write` and
 * `hangar param` use as well. */
#ifndef HG_PARAMS_H
#define HG_PARAMS_H

#include <stddef.h>
#include <stdint.h>

#include "hg_type.h"

/* ---- In the module ------------------------------------------------------ */

/* Returns HG_OK, or HG_E_REFUSED with one line in reason (64 bytes with
 * the NUL): the module's own check beyond the ranges, given the candidate
 * and the values in force. At start both are the saved values. */
typedef int (*hg_params_validate)(const void *candidate, const void *current, char reason[64]);

/* After hg_init: loads /var/lib/hangar/<module>/params (under $HANGAR_ROOT;
 * $HANGAR_STATE_DIR names the directory outright), checks it, fills *P
 * (the module's struct of `type`), publishes `<module>.params` and
 * registers the `set_params` method. No file is the first start: the
 * defaults are taken and written as the file before the first publish. A
 * file without its header line, with an unknown field, a value that does
 * not parse or is out of range, or refused by the validator stops the
 * start: the reason goes to the status line and the process exits with
 * status 78 (EX_CONFIG), as docs/05 says; the manager does not restart
 * it. A missing field takes its default, and the log says so. Returns 0,
 * or -1 with errno for what is the module's fault: EACCES (the wiring has
 * no `<module>.params`), EPROTOTYPE (the manifest's Type is not this
 * one), EINVAL (the type's attributes do not parse). */
int hg_params_open(const struct hg_type *type, void *P, hg_params_validate validate, void *ctx);
/* 1 once after each change applied to *P inside hg_serve(), then 0. */
int hg_params_changed(void);
/* The version in force, "saved" or the caller of the last change, whether
 * a restart is pending, and the ctx given to hg_params_open. */
uint64_t hg_params_version(void);
const char *hg_params_changed_by(void);
int hg_params_restart_pending(void);
void *hg_params_ctx(void);

/* ---- The grammar -------------------------------------------------------- */

#define HG_PARAMS_CHANGED_BY 48
#define HG_PARAMS_REASON 64

/* A field's attributes, pointing into the type's schema text. */
struct hg_param_attr {
	const char *dflt;  /* "" if none; a list for arrays, quoted text for char */
	size_t dflt_len;
	const char *min;   /* "" if no range */
	size_t min_len;
	const char *max;
	size_t max_len;
	int restart;
};

/* Reads the attributes of every field from the type's schema text, one
 * entry per field in field order (out holds at least n_fields). 0, or -1
 * with EINVAL and one line in err. */
int hg_params_attrs(const struct hg_type *t, struct hg_param_attr *out, char *err, size_t errlen);

/* One field's value as text: a number; for an array a comma-separated
 * list, or one value for every element; for char, quoted text. Parses it
 * into the field's bytes at dst (the whole struct's base). 0, or -1 with
 * EINVAL and the fault in err. */
int hg_param_parse(const struct hg_field *f, const char *text, size_t len, unsigned char *dst, char *err,
		   size_t errlen);
/* Formats the field's value at src as the saved file writes it. Returns
 * the length it needs, like snprintf. */
int hg_param_format(const struct hg_field *f, const unsigned char *src, char *out, size_t outlen);
/* Checks the field's value at src against its range, each element of an
 * array. 0, or -1 with the reason ("kp 12 above 10") in reason. */
int hg_param_check_range(const struct hg_field *f, const struct hg_param_attr *a, const unsigned char *src,
			 char *reason, size_t rlen);
/* Fills dst (the whole struct, payload_bytes) with the defaults. */
int hg_params_defaults(const struct hg_type *t, const struct hg_param_attr *attrs, unsigned char *dst,
		       char *err, size_t errlen);

/* The saved file's second line. */
struct hg_params_header {
	uint64_t version;
	char changed_by[HG_PARAMS_CHANGED_BY];
	int restart_pending;   /* saved for a @restart change the running module did not apply */
	int have_version;      /* the line was there */
};

/* Parses a saved file (docs/05 "Saved values") into dst, which holds the
 * defaults already: a missing field keeps them, and present[i] (if given,
 * n_fields bytes) says which fields the file set. Strict: with
 * require_header the first line must be `# hangar parameters: <t->name>`;
 * an unknown field, a repeated one, or a value that does not parse is
 * refused (-1, EINVAL, the fault in err). Ranges are not checked here. */
int hg_params_file_parse(const struct hg_type *t, const char *text, size_t len, unsigned char *dst,
			 struct hg_params_header *hdr, unsigned char *present, int require_header,
			 char *err, size_t errlen);
/* Writes the file's text for the values at src. Returns the length it
 * needs, like snprintf. */
int hg_params_file_format(const struct hg_type *t, const unsigned char *src, const struct hg_params_header *hdr,
			  char *out, size_t outlen);

#endif
