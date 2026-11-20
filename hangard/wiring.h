// SPDX-License-Identifier: GPL-3.0-or-later
/* The wiring text of docs/03 "Wiring", written from a checked manifest set
 * for one module, into a buffer or a sealed memfd the module inherits. */
#ifndef HANGARD_WIRING_H
#define HANGARD_WIRING_H

#include <stddef.h>

#include "manifest.h"

/* Writes the module's wiring into out; returns its length, or -1 (ENOSPC)
 * when it does not fit. */
int wiring_text(const struct mf_set *s, const struct mf_module *m, char *out, size_t len);

/* A sealed memfd (F_SEAL_SHRINK | GROW | WRITE | SEAL) holding text.
 * Close-on-exec: the module gets a duplicate, made in the child after its
 * sweep of the descriptors, and the manager's own does not cross a re-exec,
 * whose image makes its own. -1 with errno. */
int wiring_memfd(const char *text, size_t len);

#endif
