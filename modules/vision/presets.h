// SPDX-License-Identifier: GPL-3.0-or-later
/* The vision module's configuration and presets (modules/vision/README.md):
 * which frame topic it reads and which it writes, and the named chains the
 * `preset` parameter selects. Pure functions over strings and structs, so
 * the suite checks them without a manager. */
#ifndef VISION_PRESETS_H
#define VISION_PRESETS_H

#include <stddef.h>

#include "effects.h"
#include "vision_params.h"

#define VISION_NAME 16      /* a preset's name, with its NUL: vision_params.preset */
#define VISION_PRESETS 16   /* the five built in and up to eleven more from vision.conf */
#define VISION_TOPIC 64

struct vision_preset {
	char name[VISION_NAME];
	EffectChainConfig chain;
};

struct vision_conf {
	char input[VISION_TOPIC];  /* the frame topic read: cam.decoded on the topside, cam.raw on the vehicle */
	char output[VISION_TOPIC]; /* the frame topic written: cam.pilot */
	struct vision_preset presets[VISION_PRESETS];
	int n_presets;
};

/* The defaults: cam.decoded to cam.pilot, and the five built-in presets
 * normal, auto, edges, sharpen and lowlight (the README says what each is). */
void vision_conf_defaults(struct vision_conf *c);

/* vision.conf's text over the defaults: [Vision] Input=, Output=; [Preset
 * <name>] Auto=yes|no, Chain=<stage> <stage>..., a new preset or one of the
 * built-in five redefined. 0, or -1 with the line and the reason in err. */
int vision_conf_parse(struct vision_conf *c, const char *text, size_t len, char *err, size_t errlen);

/* One chain in vision.conf's form: stages separated by blanks, each an
 * operator with its settings, `blur:strength=2 sobel:threshold=16`. 0, or -1
 * with the reason. */
int vision_chain_parse(const char *text, EffectChainConfig *out, char *err, size_t errlen);

/* The chain the parameters select: the preset's, or, for "custom", the
 * stages as given. 0, or -1 with the reason (an unknown preset). */
int vision_effective_chain(const struct vision_conf *c, const vision_params *p, EffectChainConfig *out, char *err,
			   size_t errlen);

/* The operator's name (`blur`), or "?" for a code past the vocabulary. */
const char *vision_effect_name(EffectType e);

#endif
