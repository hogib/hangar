#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The effect vocabulary: what an effect chain is made of. Shared by the
 * chain itself (effect_chain.h) and by whatever parses or publishes a
 * chain -- eeye's config.c today, hangar's parameter struct later
 * (docs/05-parameters.md) -- so it lives with the operators rather than
 * with any one program's configuration.
 */

typedef enum {
  EFFECT_NONE = 0,
  EFFECT_GRAYSCALE,
  EFFECT_INVERT,
  EFFECT_THRESHOLD,
  EFFECT_TINT,
  EFFECT_SOBEL,
  EFFECT_BLUR,
  EFFECT_CONTRAST,
  EFFECT_LIGHT,
  EFFECT_LOG,
  EFFECT_CANNY,
} EffectType;

// One entry in an effect chain. Every stage carries its own parameters
// (rather than one shared set for the whole chain) so e.g. two EFFECT_TINT
// stages in the same chain, or a threshold before a tint, each get their
// own values.
typedef struct {
  EffectType effect;
  uint8_t threshold_value; // used when effect == EFFECT_THRESHOLD
  uint8_t tint_u;          // used when effect == EFFECT_TINT
  uint8_t tint_v;
  uint8_t tint_strength;

  // used when effect == EFFECT_SOBEL: gradient magnitudes below this are
  // clamped to 0 -- raises the bar for what counts as an edge, so faint
  // sensor noise doesn't show up as a sea of dim edge pixels. 0 (the
  // default) keeps every magnitude sobel_edges() would otherwise produce,
  // identical to not having this field at all.
  uint8_t sobel_threshold;

  // used when effect == EFFECT_BLUR: how many times to repeat the 5-tap
  // pass. 0 and 1 both mean a single pass (today's blur, unchanged); each
  // additional pass roughly doubles that stage's cost, so this is the
  // knob for "soft focus" vs. "heavily smoothed" rather than a free
  // parameter to max out.
  uint8_t blur_strength;

  // used when effect == EFFECT_LIGHT: a single dial over both brightness
  // (Y) and color saturation (U/V distance from neutral), moved together
  // -- unlike EFFECT_CONTRAST this is a fixed function of the level, not
  // the frame's actual content, so it composes into effect_chain.c's
  // point-op LUT fusion instead of needing its own full-frame pass. 128 =
  // neutral (also the default when the config key is omitted -- see
  // parse_effect_stage in eeye's config.c, which seeds this field to 128
  // before parsing, unlike every other field here which defaults to a
  // zero-initialized 0). Below 128: darker and desaturated, down to fully
  // black/gray at 0. Above 128: brighter and more saturated, up to roughly
  // double saturation at 255.
  uint8_t light_level;

  // used when effect == EFFECT_LOG: Gaussian passes before the Laplacian,
  // so this sets sigma (growing as sqrt(strength)). 0 and 1 both mean a
  // single pass, same as blur_strength.
  uint8_t log_strength;

  // used when effect == EFFECT_LOG: how steep the response must be across
  // a zero-crossing to count as an edge. 0 marks every sign change,
  // including noise grazing zero in flat regions.
  uint8_t log_threshold;

  // used when effect == EFFECT_CANNY: Gaussian passes before the gradient.
  // Unlike the other strength knobs this is never zero -- 0 means 1 --
  // because the smoothing bounds hysteresis's cost rather than just
  // setting quality (see conv.h).
  uint8_t canny_strength;

  // used when effect == EFFECT_CANNY: hysteresis thresholds. At or above
  // canny_high is an edge outright; between the two only when connected to
  // one. Defaults (0) would produce an empty map, so the config parser
  // seeds them to 40/90 the way it seeds light_level to 128.
  uint8_t canny_low;
  uint8_t canny_high;
} EffectStage;

// Bounds the chain to a fixed-size array rather than something dynamically
// allocated: the chain's config is copied by value (into eeye's
// ConfigWatcher ring buffer on every reload, see config.c; into hangar's
// parameter topic later), and keeping it a plain, pointer-free POD struct
// is what makes that safe without any per-slot allocation or cleanup. 8 is
// comfortably past any chain that makes visual sense.
enum { max_chain_stages = 8 };

// Everything apply_effect_chain*() reads: the stages, how many of them,
// and whether auto visibility (auto_vis.h) runs ahead of them. eeye keeps
// these same three fields in its Config and copies them into one of these
// per frame (video_threads.c), so that the chain needs nothing of eeye's.
typedef struct {
  EffectStage stages[max_chain_stages];
  size_t stage_count; // may be 0: an empty chain is a valid pass-through

  // Auto visibility (auto_vis.h): continuously corrects colour cast,
  // brightness range and haze on the pilot's picture, ahead of the chain.
  // Off by default -- it changes what the pilot sees, so it is switched on
  // deliberately (the page's big toggle).
  bool auto_visibility;
} EffectChainConfig;
