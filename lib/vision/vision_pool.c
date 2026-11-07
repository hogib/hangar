#include "vision_pool.h"

// Plain, not atomic: set before the operators run and read by the thread
// that runs them (see vision_pool.h).
static struct hg_pool *current_pool;

void vision_set_pool(struct hg_pool *pool) { current_pool = pool; }

struct hg_pool *vision_pool(void) { return current_pool; }
