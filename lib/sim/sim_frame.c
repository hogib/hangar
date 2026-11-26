// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Frame files: see sim_frame.h for the format. A small parser of its own,
 * since lib/sim depends on nothing but libm.
 */
#include "sim_frame.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(char *err, size_t errlen, int line, const char *what) {
  if (err && errlen)
    snprintf(err, errlen, "line %d: %s", line, what);
  return -1;
}

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

// Three finite numbers and nothing after them.
static int three(const char *s, double v[3]) {
  char *end;
  for (int i = 0; i < 3; i++) {
    v[i] = strtod(s, &end);
    if (end == s || !isfinite(v[i]))
      return -1;
    s = end;
  }
  while (is_space(*s))
    s++;
  return *s ? -1 : 0;
}

int sim_frame_parse(const char *text, size_t len, struct sim_frame *out, char *err,
                    size_t errlen) {
  memset(out, 0, sizeof *out);
  for (int i = 0; i < SIM_NTHRUSTERS; i++)
    out->geometry.polarity[i] = 1.0;
  int have_pos[SIM_NTHRUSTERS] = {0}, have_dir[SIM_NTHRUSTERS] = {0};
  int section = 0; /* 0 none, -1 [Frame], 1..8 a thruster */
  int line = 0;
  size_t at = 0;
  while (at < len) {
    char buf[160];
    size_t n = 0;
    int too_long = 0;
    while (at < len && text[at] != '\n') {
      if (n + 1 < sizeof buf)
        buf[n++] = text[at];
      else
        too_long = 1;
      at++;
    }
    at++; /* the newline */
    line++;
    buf[n] = '\0';
    if (too_long)
      return fail(err, errlen, line, "line too long");
    char *hash = strchr(buf, '#');
    if (hash)
      *hash = '\0';
    char *s = buf;
    while (is_space(*s))
      s++;
    char *e = s + strlen(s);
    while (e > s && is_space(e[-1]))
      *--e = '\0';
    if (!*s)
      continue;
    if (*s == '[') {
      if (strcmp(s, "[Frame]") == 0) {
        section = -1;
        continue;
      }
      int k = 0;
      char close = 0, extra = 0;
      if (sscanf(s, "[Thruster %d%c%c", &k, &close, &extra) != 2 || close != ']')
        return fail(err, errlen, line, "a section is [Frame] or [Thruster N]");
      if (k < 1 || k > SIM_NTHRUSTERS)
        return fail(err, errlen, line, "thruster number out of range");
      if (k != out->n + 1)
        return fail(err, errlen, line, "thrusters are numbered from 1 in order, each once");
      out->n = k;
      section = k;
      continue;
    }
    char *eq = strchr(s, '=');
    if (!eq)
      return fail(err, errlen, line, "expected Key=value");
    *eq = '\0';
    char *key = s, *val = eq + 1;
    e = eq;
    while (e > key && is_space(e[-1]))
      *--e = '\0';
    while (is_space(*val))
      val++;
    if (section == 0)
      return fail(err, errlen, line, "a key before any section");
    if (section == -1) {
      if (strcmp(key, "Name") != 0)
        return fail(err, errlen, line, "unknown key in [Frame]");
      if (!*val || strlen(val) >= sizeof out->name)
        return fail(err, errlen, line, "Name is empty or too long");
      strcpy(out->name, val);
      continue;
    }
    int i = section - 1;
    if (strcmp(key, "Position") == 0) {
      if (three(val, out->geometry.pos[i]) < 0)
        return fail(err, errlen, line, "Position takes three numbers (m)");
      have_pos[i] = 1;
    } else if (strcmp(key, "Direction") == 0) {
      double d[3];
      if (three(val, d) < 0)
        return fail(err, errlen, line, "Direction takes three numbers");
      double norm = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
      if (norm < 1e-9)
        return fail(err, errlen, line, "Direction has no length");
      for (int j = 0; j < 3; j++)
        out->geometry.dir[i][j] = d[j] / norm;
      have_dir[i] = 1;
    } else if (strcmp(key, "Reversed") == 0) {
      if (strcmp(val, "yes") == 0)
        out->geometry.polarity[i] = -1.0;
      else if (strcmp(val, "no") == 0)
        out->geometry.polarity[i] = 1.0;
      else
        return fail(err, errlen, line, "Reversed is yes or no");
    } else {
      return fail(err, errlen, line, "unknown key in [Thruster N]");
    }
  }
  if (out->n == 0)
    return fail(err, errlen, line, "no thruster described");
  for (int i = 0; i < out->n; i++)
    if (!have_pos[i] || !have_dir[i])
      return fail(err, errlen, line, "a thruster lacks its Position or its Direction");
  if (!out->name[0])
    strcpy(out->name, "unnamed");
  return 0;
}

void sim_frame_bluerov2_heavy(struct sim_frame *out) {
  memset(out, 0, sizeof *out);
  strcpy(out->name, "bluerov2-heavy");
  out->n = SIM_NTHRUSTERS;
  out->geometry = *sim_geometry_bluerov2_heavy();
}
