// SPDX-License-Identifier: GPL-3.0-or-later
/* The vision module's configuration and presets: presets.h. */
#include "presets.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hg_ini.h"

static void say(char *err, size_t errlen, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void say(char *err, size_t errlen, const char *fmt, ...)
{
	if (!err || !errlen)
		return;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(err, errlen, fmt, ap);
	va_end(ap);
}

static const char *const NAMES[] = { "none", "grayscale", "invert", "threshold", "tint", "sobel",
				     "blur", "contrast",  "light",  "log",       "canny" };

const char *vision_effect_name(EffectType e)
{
	return (unsigned)e < sizeof NAMES / sizeof NAMES[0] ? NAMES[e] : "?";
}

/* A stage as eeye's config seeds it before its settings: every field 0 but
 * the neutral light level and Canny's hysteresis (effects.h). */
static EffectStage stage_seed(EffectType e)
{
	EffectStage s;
	memset(&s, 0, sizeof s);
	s.effect = e;
	s.light_level = 128;
	s.canny_low = 40;
	s.canny_high = 90;
	return s;
}

static int parse_u8(const char *v, uint8_t *out)
{
	char *end;
	long n = strtol(v, &end, 10);
	if (end == v || *end || n < 0 || n > 255)
		return -1;
	*out = (uint8_t)n;
	return 0;
}

/* One `key=value` of a stage: the keys each operator takes, and no other. */
static int stage_key(EffectStage *s, const char *key, const char *val, char *err, size_t errlen)
{
	uint8_t v;
	if (parse_u8(val, &v) < 0) {
		say(err, errlen, "%s: %s=%s is not 0 to 255", vision_effect_name(s->effect), key, val);
		return -1;
	}
	uint8_t *f = NULL;
	switch (s->effect) {
	case EFFECT_THRESHOLD:
		f = strcmp(key, "value") == 0 ? &s->threshold_value : NULL;
		break;
	case EFFECT_TINT:
		f = strcmp(key, "u") == 0 ? &s->tint_u :
		    strcmp(key, "v") == 0 ? &s->tint_v :
		    strcmp(key, "strength") == 0 ? &s->tint_strength : NULL;
		break;
	case EFFECT_SOBEL:
		f = strcmp(key, "threshold") == 0 ? &s->sobel_threshold : NULL;
		break;
	case EFFECT_BLUR:
		f = strcmp(key, "strength") == 0 ? &s->blur_strength : NULL;
		break;
	case EFFECT_LIGHT:
		f = strcmp(key, "level") == 0 ? &s->light_level : NULL;
		break;
	case EFFECT_LOG:
		f = strcmp(key, "strength") == 0 ? &s->log_strength :
		    strcmp(key, "threshold") == 0 ? &s->log_threshold : NULL;
		break;
	case EFFECT_CANNY:
		f = strcmp(key, "strength") == 0 ? &s->canny_strength :
		    strcmp(key, "low") == 0 ? &s->canny_low :
		    strcmp(key, "high") == 0 ? &s->canny_high : NULL;
		break;
	default:
		break;
	}
	if (!f) {
		say(err, errlen, "%s takes no setting %s", vision_effect_name(s->effect), key);
		return -1;
	}
	*f = v;
	return 0;
}

int vision_chain_parse(const char *text, EffectChainConfig *out, char *err, size_t errlen)
{
	memset(out, 0, sizeof *out);
	char buf[512];
	if (strlen(text) >= sizeof buf) {
		say(err, errlen, "the chain is longer than %zu characters", sizeof buf - 1);
		return -1;
	}
	snprintf(buf, sizeof buf, "%s", text);
	char *save = NULL;
	for (char *tok = strtok_r(buf, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save)) {
		if (out->stage_count == max_chain_stages) {
			say(err, errlen, "more than %d stages", max_chain_stages);
			return -1;
		}
		char *settings = strchr(tok, ':');
		if (settings)
			*settings++ = 0;
		EffectType e = EFFECT_NONE;
		for (unsigned i = 1; i < sizeof NAMES / sizeof NAMES[0]; i++)
			if (strcmp(tok, NAMES[i]) == 0)
				e = (EffectType)i;
		if (e == EFFECT_NONE) {
			say(err, errlen, "%s is not an operator (grayscale invert threshold tint sobel blur contrast "
					 "light log canny)", tok);
			return -1;
		}
		EffectStage s = stage_seed(e);
		char *save2 = NULL;
		for (char *kv = settings ? strtok_r(settings, ",", &save2) : NULL; kv; kv = strtok_r(NULL, ",", &save2)) {
			char *eq = strchr(kv, '=');
			if (!eq) {
				say(err, errlen, "%s: %s is not key=value", tok, kv);
				return -1;
			}
			*eq = 0;
			if (stage_key(&s, kv, eq + 1, err, errlen) < 0)
				return -1;
		}
		out->stages[out->stage_count++] = s;
	}
	return 0;
}

/* The five every vision module has; vision.conf may redefine them. */
static const struct {
	const char *name;
	int autovis;
	const char *chain;
} BUILTIN[] = {
	{ "normal", 0, "" },                                    /* the camera's picture as it comes */
	{ "auto", 1, "" },                                      /* auto visibility: cast, range, haze */
	{ "edges", 0, "blur:strength=2 sobel:threshold=16" },   /* outlines, for turbid water */
	{ "sharpen", 0, "contrast" },                           /* the range stretched: no sharpening kernel */
	{ "lowlight", 1, "light:level=176" },                   /* auto visibility, then brighter */
};

static struct vision_preset *find(struct vision_conf *c, const char *name)
{
	for (int i = 0; i < c->n_presets; i++)
		if (strcmp(c->presets[i].name, name) == 0)
			return &c->presets[i];
	return NULL;
}

void vision_conf_defaults(struct vision_conf *c)
{
	memset(c, 0, sizeof *c);
	snprintf(c->input, sizeof c->input, "cam.decoded");
	snprintf(c->output, sizeof c->output, "cam.pilot");
	for (unsigned i = 0; i < sizeof BUILTIN / sizeof BUILTIN[0]; i++) {
		struct vision_preset *p = &c->presets[c->n_presets++];
		snprintf(p->name, sizeof p->name, "%s", BUILTIN[i].name);
		(void)vision_chain_parse(BUILTIN[i].chain, &p->chain, NULL, 0);
		p->chain.auto_visibility = BUILTIN[i].autovis;
	}
}

struct pstate {
	struct vision_conf *c;
	int section;                 /* 0 none, 1 [Vision], 2 [Preset] */
	struct vision_preset *preset;
};

static int valid_name(const char *s)
{
	size_t n = strlen(s);
	if (!n || n >= VISION_NAME || strcmp(s, "custom") == 0)
		return 0;
	for (; *s; s++)
		if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_' || *s == '-'))
			return 0;
	return 1;
}

static int on_section(void *ctx, const char *section, const char *arg, int line, char *err, size_t errlen)
{
	(void)line;
	struct pstate *st = ctx;
	if (strcmp(section, "Vision") == 0 && !arg[0]) {
		st->section = 1;
		return 0;
	}
	if (strcmp(section, "Preset") == 0) {
		if (!valid_name(arg)) {
			say(err, errlen, "[Preset %s]: a name of 1 to 15 of a-z 0-9 _ -, not custom", arg);
			return -1;
		}
		struct vision_preset *p = find(st->c, arg);
		if (!p) {
			if (st->c->n_presets == VISION_PRESETS) {
				say(err, errlen, "more than %d presets", VISION_PRESETS);
				return -1;
			}
			p = &st->c->presets[st->c->n_presets++];
			snprintf(p->name, sizeof p->name, "%s", arg);
		}
		memset(&p->chain, 0, sizeof p->chain); /* a redefined preset starts empty */
		st->preset = p;
		st->section = 2;
		return 0;
	}
	say(err, errlen, "[%s%s%s]: not a section of vision.conf ([Vision], [Preset <name>])", section,
	    arg[0] ? " " : "", arg);
	return -1;
}

static int on_key(void *ctx, const char *key, const char *value, int line, char *err, size_t errlen)
{
	(void)line;
	struct pstate *st = ctx;
	if (st->section == 1) {
		char *dst = strcmp(key, "Input") == 0 ? st->c->input : strcmp(key, "Output") == 0 ? st->c->output : NULL;
		if (!dst) {
			say(err, errlen, "[Vision] %s: not a key (Input, Output)", key);
			return -1;
		}
		if (!value[0] || strlen(value) >= VISION_TOPIC) {
			say(err, errlen, "%s: a topic name", key);
			return -1;
		}
		snprintf(dst, VISION_TOPIC, "%s", value);
		return 0;
	}
	if (st->section == 2) {
		if (strcmp(key, "Auto") == 0) {
			int b = hg_ini_parse_bool(value);
			if (b < 0) {
				say(err, errlen, "Auto: yes or no");
				return -1;
			}
			st->preset->chain.auto_visibility = b;
			return 0;
		}
		if (strcmp(key, "Chain") == 0) {
			int autovis = st->preset->chain.auto_visibility;
			if (vision_chain_parse(value, &st->preset->chain, err, errlen) < 0)
				return -1;
			st->preset->chain.auto_visibility = autovis;
			return 0;
		}
		say(err, errlen, "[Preset] %s: not a key (Auto, Chain)", key);
		return -1;
	}
	say(err, errlen, "%s before any section", key);
	return -1;
}

int vision_conf_parse(struct vision_conf *c, const char *text, size_t len, char *err, size_t errlen)
{
	struct pstate st = { .c = c };
	return hg_ini_parse(text, len, on_section, on_key, &st, err, errlen);
}

int vision_effective_chain(const struct vision_conf *c, const vision_params *p, EffectChainConfig *out, char *err,
			   size_t errlen)
{
	char name[VISION_NAME];
	memcpy(name, p->preset, sizeof name - 1);
	name[sizeof name - 1] = 0;
	if (strcmp(name, "custom") != 0) {
		for (int i = 0; i < c->n_presets; i++)
			if (strcmp(c->presets[i].name, name) == 0) {
				*out = c->presets[i].chain;
				return 0;
			}
		size_t n = 0;
		char known[VISION_PRESETS * VISION_NAME + 16] = "";
		for (int i = 0; i < c->n_presets; i++)
			n += (size_t)snprintf(known + n, sizeof known - n, "%s%s", i ? " " : "", c->presets[i].name);
		say(err, errlen, "no preset %s (%s custom)", name, known);
		return -1;
	}
	memset(out, 0, sizeof *out);
	out->auto_visibility = p->auto_visibility != 0;
	out->stage_count = p->stage_count > max_chain_stages ? max_chain_stages : p->stage_count;
	for (size_t i = 0; i < out->stage_count; i++) {
		if (p->effect[i] > EFFECT_CANNY) {
			say(err, errlen, "stage %zu: effect %u is not an operator", i, p->effect[i]);
			return -1;
		}
		EffectStage *s = &out->stages[i];
		s->effect = (EffectType)p->effect[i];
		s->threshold_value = p->threshold_value[i];
		s->tint_u = p->tint_u[i];
		s->tint_v = p->tint_v[i];
		s->tint_strength = p->tint_strength[i];
		s->sobel_threshold = p->sobel_threshold[i];
		s->blur_strength = p->blur_strength[i];
		s->light_level = p->light_level[i];
		s->log_strength = p->log_strength[i];
		s->log_threshold = p->log_threshold[i];
		s->canny_strength = p->canny_strength[i];
		s->canny_low = p->canny_low[i];
		s->canny_high = p->canny_high[i];
	}
	return 0;
}
