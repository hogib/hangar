#pragma once
#include "hg_pool.h"

/*
 * The thread pool the operators run their loops on (lib/common/hg_pool.h),
 * in place of OpenMP's implicit team. One pointer for the process, set once
 * at start by whoever owns the effects thread -- eeye's effects_loop, the
 * vision module, tools/equivalence/equiv.c -- and read by every operator
 * on entry. NULL (the default) runs every loop inline on the caller, which
 * is what the suites that never set one get.
 *
 * A global rather than a parameter on every operator, because the
 * operators already are one-caller-at-a-time: conv.c keeps its blur, LoG
 * and Canny scratch in process-wide buffers, so a second thread calling
 * into lib/vision was never supported, and a pool pointer kept in the same
 * place adds no restriction that was not there. A parameter would have
 * changed seven public signatures and every caller -- the chain, four
 * suites, equiv.c (and its v0.3.1 build) and eeye -- to pass down a value
 * that is the same for the life of the process; the thread count is a
 * @restart parameter of the vision module (docs/11-electric-eye.md).
 *
 * The thread count comes with the pool: a caller that wants OpenMP's rule
 * creates it with hg_pool_default_threads().
 */
void vision_set_pool(struct hg_pool *pool);
struct hg_pool *vision_pool(void);
