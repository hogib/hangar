// SPDX-License-Identifier: GPL-3.0-or-later
/* hangar-policy: the generic policy program (docs/09-learning.md "Policy
 * modules"), one module per policy, `policy_<name>`, run on its policy spec
 * (/etc/hangar/policies/<name>.policy). It ticks on each message of its
 * dataset's anchor, builds the row's input parts with the pairer's own join
 * code (lib/hg/hg_dataset.h), steps the running model (lib/nn), and
 * publishes policy.<name>.wrench, referencing the anchor and the
 * policy.<name>.active message of the model that computed it. Weights come
 * by load_weights: checks 1 to 7 on a worker thread at SCHED_IDLE (lib/nn's
 * loader), warm-up beside the running model on the tick thread (check 8),
 * the swap between two ticks. current.hgw and previous.hgw are kept in the
 * state directory and reloaded at start; revert swaps back; a non-finite
 * output is never published. modules/policy/README.md lists the deviations. */
#include "mod.h"

#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "blake2s.h"
#include "empty.h"
#include "hg_call.h"
#include "hg_dataset.h"
#include "hg_ini.h"
#include "load_weights_rep.h"
#include "load_weights_req.h"
#include "nn_graph.h"
#include "nn_hgw.h"
#include "policy_active.h"
#include "policy_load.h"
#include "policy_version.h"
#include "recorder_session.h"
#include "runtime_info.h"
#include "wrench.h"

#define WHO "hangar-policy"
#define PATH_LEN 512
#define MAX_IN_BIND 128
#define MAX_OUT_BIND 6
#define MAX_MODEL_IO 16

enum { CAUSE_START = 0, CAUSE_LOAD = 1, CAUSE_REVERT = 2 };
enum { LOAD_NONE = 0, LOAD_CHECKING = 1, LOAD_WARMING = 2, LOAD_DONE = 3, LOAD_REJECTED = 4 };

/* ---- The policy spec ---------------------------------------------------------- */

static struct {
	char name[32];
	char dataset[32];
	char dataset_dir[PATH_LEN];
	uint64_t arch;
	double tick_budget; /* share of the tick period */
	int have_name, have_dataset, have_arch;
} S;

static int spec_section(void *ctx, const char *section, const char *arg, int line, char *err, size_t errlen)
{
	(void)ctx;
	(void)line;
	if (strcmp(section, "Policy") != 0 || (arg && *arg)) {
		mod_fmt(err, errlen, "unknown section [%s%s%s]", section, arg && *arg ? " " : "", arg ? arg : "");
		return -1;
	}
	return 0;
}

static int valid_name(const char *s)
{
	if (!*s || strlen(s) >= sizeof S.name)
		return 0;
	for (; *s; s++)
		if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_'))
			return 0;
	return 1;
}

static int spec_key(void *ctx, const char *key, const char *value, int line, char *err, size_t errlen)
{
	(void)ctx;
	(void)line;
	if (strcmp(key, "Name") == 0) {
		if (!valid_name(value)) {
			mod_fmt(err, errlen, "Name: [a-z0-9_], at most 31 characters");
			return -1;
		}
		mod_fmt(S.name, sizeof S.name, "%s", value);
		S.have_name = 1;
	} else if (strcmp(key, "Dataset") == 0) {
		if (!valid_name(value)) {
			mod_fmt(err, errlen, "Dataset: [a-z0-9_], at most 31 characters");
			return -1;
		}
		mod_fmt(S.dataset, sizeof S.dataset, "%s", value);
		S.have_dataset = 1;
	} else if (strcmp(key, "Arch") == 0) {
		char *end;
		errno = 0;
		unsigned long long v = strtoull(value, &end, 16);
		if (errno || *end || end - value != 16) {
			mod_fmt(err, errlen, "Arch: 16 hex digits");
			return -1;
		}
		S.arch = v;
		S.have_arch = 1;
	} else if (strcmp(key, "TickBudget") == 0) {
		char *end;
		double v = strtod(value, &end);
		if (end == value || strcmp(end, "%") != 0 || v <= 0 || v > 100) {
			mod_fmt(err, errlen, "TickBudget: a share of the tick period, 1%% to 100%%");
			return -1;
		}
		S.tick_budget = v / 100.0;
	} else if (strcmp(key, "DatasetDir") == 0) {
		mod_fmt(S.dataset_dir, sizeof S.dataset_dir, "%s", value);
	} else {
		mod_fmt(err, errlen, "unknown key %s", key);
		return -1;
	}
	return 0;
}

static int spec_load(const char *path, char *err, size_t errlen)
{
	static char text[8192];
	size_t len;
	S.tick_budget = 0.5;
	if (hg_ini_read_file(path, text, sizeof text, &len) < 0) {
		mod_fmt(err, errlen, "%s: %s", path, strerror(errno));
		return -1;
	}
	char why[200];
	if (hg_ini_parse(text, len, spec_section, spec_key, NULL, why, sizeof why) < 0) {
		mod_fmt(err, errlen, "%s: %s", path, why);
		return -1;
	}
	if (!S.have_name || !S.have_dataset || !S.have_arch) {
		mod_fmt(err, errlen, "%s: Name, Dataset and Arch are required", path);
		return -1;
	}
	if (!S.dataset_dir[0]) {
		const char *root = getenv("HANGAR_ROOT");
		mod_fmt(S.dataset_dir, sizeof S.dataset_dir, "%s/etc/hangar/datasets", root ? root : "");
	}
	return 0;
}

/* ---- The dataset: the anchor, the input parts, the row ------------------------- */

static struct hg_ds_spec DS;
static struct hg_ds_row_type *RT;
static struct hg_schema anchor_schema, part_schema[HG_DS_MAX_PARTS];
static hg_sub *sub_anchor, *sub_part[HG_DS_MAX_PARTS], *sub_session;
static struct hg_ds_source src[HG_DS_MAX_PARTS];
static struct hg_ds_state dstate;
static unsigned char boot[16];
static unsigned char *row, *apayload, *scratch;
static int bound;
static uint64_t tick_period_ns;
static const char *input_fields[HG_DS_MAX_MAP];
static size_t n_input_fields;

static int sub_info(hg_sub *s, struct hg_schema *sch, struct hg_ds_topic_info *out)
{
	struct hg_topic_meta m;
	uint32_t slen;
	/* A topic absent at hg_sub_open attaches on a read, and nothing reads a
	 * part before the bind: hg_skip settles it (the parts are read by seq
	 * and stamp, never by hg_next, so skipping loses nothing). */
	if (hg_sub_meta(s, &m) < 0)
		(void)hg_skip(s);
	const char *text = hg_sub_schema(s, &slen);
	if (!text || hg_sub_meta(s, &m) < 0)
		return -1;
	if (hg_schema_parse(text, slen, sch, NULL, 0) < 0)
		return -1;
	out->type = &sch->type;
	out->topic_id = m.topic_id;
	out->rate_mhz = m.rate_mhz;
	out->stale_after_ns = m.stale_after_ns;
	return 0;
}

/* Once the anchor and every input part exist: the row type, which is the
 * pairer's for the same spec and topics. Parts that are not inputs are not
 * subscribed: the policy acts on every tick, whoever is driving. */
static void try_bind(void)
{
	struct hg_ds_topic_info ai, pi[HG_DS_MAX_PARTS];
	memset(pi, 0, sizeof pi);
	if (sub_info(sub_anchor, &anchor_schema, &ai) < 0)
		return;
	for (int i = 0; i < DS.n_parts; i++) {
		if (DS.parts[i].role != HG_DS_INPUT) {
			/* A placeholder type for a label or filter part: the row
			 * type is built over every part, as the pairer's, so that the
			 * offsets agree; the policy never joins these. */
			if (sub_info(sub_part[i], &part_schema[i], &pi[i]) < 0)
				return;
			continue;
		}
		if (sub_info(sub_part[i], &part_schema[i], &pi[i]) < 0)
			return;
	}
	char err[300];
	if (hg_ds_bind(&DS, &ai, pi, RT, err, sizeof err) < 0) {
		mod_log(WHO, "%s: %s", DS.name, err);
		exit(78);
	}
	RT->schema.type.name = "dataset";
	apayload = malloc(ai.type->payload_bytes + 8);
	scratch = malloc(RT->scratch_bytes + 8);
	row = calloc(1, RT->row_bytes + 8);
	if (!apayload || !scratch || !row) {
		mod_log(WHO, "out of memory");
		exit(3);
	}
	for (int i = 0; i < DS.n_parts; i++)
		if (sub_part[i])
			src[i] = hg_ds_source_sub(sub_part[i]);
	struct hg_topic_meta am;
	hg_sub_meta(sub_anchor, &am);
	int zero = 1;
	for (int i = 0; i < 16; i++)
		zero &= am.origin_boot[i] == 0;
	if (zero)
		hg_ds_boot_id(boot);
	else
		memcpy(boot, am.origin_boot, 16);
	tick_period_ns = am.rate_mhz ? (uint64_t)(1e12 / am.rate_mhz) : 0;
	/* The fields a model may bind: the input parts' and the anchor's when
	 * it is an input (role=input in the row schema), and the missing bits. */
	n_input_fields = 0;
	for (uint32_t f = 0; f < RT->schema.type.n_fields; f++) {
		const struct hg_field *fd = &RT->schema.type.fields[f];
		if (fd->count != 1 || fd->kind == HG_CHAR)
			continue;
		if (strncmp(fd->comment, "role=input", 10) == 0 || strcmp(fd->name, "missing") == 0)
			input_fields[n_input_fields++] = fd->name;
	}
	bound = 1;
	mod_log(WHO, "%s: bound, %u-byte rows, %zu input fields, tick %llu us, row type %016llx", DS.name, RT->row_bytes,
		n_input_fields, (unsigned long long)(tick_period_ns / 1000), (unsigned long long)RT->schema.type.hash);
}

/* ---- Models --------------------------------------------------------------------- */

struct ibind {
	uint32_t row_off;
	enum hg_field_kind kind;
	uint32_t input, elem, dtype;
};

struct obind {
	uint32_t output, elem, off;
};

struct model {
	nn_model *m;
	uint8_t digest[32];
	uint64_t hash;
	uint32_t version;
	nn_hgw_header hdr;
	int n_ib, n_ob;
	struct ibind ib[MAX_IN_BIND];
	struct obind ob[MAX_OUT_BIND];
	void *in[MAX_MODEL_IO];
	float *out[MAX_MODEL_IO];
	double step_ns; /* the mean step time, measured on the ticks */
	uint64_t steps;
};

static void model_free(struct model *md)
{
	for (int i = 0; i < MAX_MODEL_IO; i++) {
		free(md->in[i]);
		free(md->out[i]);
	}
	if (md->m)
		nn_free(md->m);
	memset(md, 0, sizeof *md);
}

static uint64_t digest_hash(const uint8_t d[32])
{
	uint64_t h;
	memcpy(&h, d, 8);
	return h;
}

/* The model's bindings resolved against the row type and the wrench: a
 * row field and a dtype for each input element, a wrench field for each
 * output element. -1 with the reason (check 4's names were already checked
 * by the loader; this resolves them). */
static int model_bind(struct model *md, char *reason, size_t rlen)
{
	nn_model *m = md->m;
	const nn_hgw_file *f = &m->file;
	if (m->n_inputs > MAX_MODEL_IO || m->n_outputs > MAX_MODEL_IO) {
		mod_fmt(reason, rlen, "more than %d model inputs or outputs", MAX_MODEL_IO);
		return -1;
	}
	for (uint32_t i = 0; i < m->n_inputs; i++) {
		md->in[i] = calloc(1, nn_input_bytes(m, i) + 8);
		if (!md->in[i])
			return -1;
	}
	for (uint32_t j = 0; j < m->n_outputs; j++) {
		md->out[j] = calloc(nn_output_count(m, j) + 1, sizeof(float));
		if (!md->out[j])
			return -1;
	}
	md->n_ib = 0;
	for (uint32_t b = 0; b < f->n_inputs; b++) {
		const nn_hgw_binding *bd = &f->inputs[b];
		char field[NN_HGW_FIELD_LEN + 1];
		memcpy(field, bd->field, NN_HGW_FIELD_LEN);
		field[NN_HGW_FIELD_LEN] = 0;
		const struct hg_field *fd = hg_type_field(&RT->schema.type, field);
		uint32_t in = UINT32_MAX;
		for (uint32_t i = 0; i < m->n_inputs; i++)
			if (m->input_buf[i] == bd->buffer)
				in = i;
		if (!fd || in == UINT32_MAX || md->n_ib == MAX_IN_BIND) {
			mod_fmt(reason, rlen, "input binding %s: not a row field of %s", field, DS.name);
			return -1;
		}
		md->ib[md->n_ib++] = (struct ibind){ .row_off = fd->offset, .kind = fd->kind, .input = in,
						     .elem = bd->element, .dtype = nn_input_desc(m, in)->dtype };
	}
	md->n_ob = 0;
	for (uint32_t b = 0; b < f->n_outputs; b++) {
		const nn_hgw_binding *bd = &f->outputs[b];
		char field[NN_HGW_FIELD_LEN + 1];
		memcpy(field, bd->field, NN_HGW_FIELD_LEN);
		field[NN_HGW_FIELD_LEN] = 0;
		const struct hg_field *fd = hg_type_field(&WRENCH_TYPE, field);
		uint32_t out = UINT32_MAX;
		for (uint32_t j = 0; j < m->n_outputs; j++)
			if (m->output_buf[j] == bd->buffer)
				out = j;
		if (!fd || fd->kind != HG_F32 || out == UINT32_MAX || md->n_ob == MAX_OUT_BIND) {
			mod_fmt(reason, rlen, "output binding %s: not a field of the wrench", field);
			return -1;
		}
		md->ob[md->n_ob++] = (struct obind){ .output = out, .elem = bd->element, .off = fd->offset };
	}
	memcpy(&md->hdr, f->hdr, sizeof md->hdr);
	return 0;
}

static double row_value(const unsigned char *p, enum hg_field_kind k)
{
	switch (k) {
	case HG_U8: return *(const uint8_t *)p;
	case HG_I8: return *(const int8_t *)p;
	case HG_U16: { uint16_t v; memcpy(&v, p, 2); return v; }
	case HG_I16: { int16_t v; memcpy(&v, p, 2); return v; }
	case HG_U32: { uint32_t v; memcpy(&v, p, 4); return v; }
	case HG_I32: { int32_t v; memcpy(&v, p, 4); return v; }
	case HG_U64: { uint64_t v; memcpy(&v, p, 8); return (double)v; }
	case HG_I64: { int64_t v; memcpy(&v, p, 8); return (double)v; }
	case HG_F32: { float v; memcpy(&v, p, 4); return v; }
	case HG_F64: { double v; memcpy(&v, p, 8); return v; }
	default: return 0;
	}
}

static void put_value(void *buf, uint32_t elem, uint32_t dtype, double v)
{
	unsigned char *p = buf;
	switch (dtype) {
	case NN_F32: { float x = (float)v; memcpy(p + 4 * elem, &x, 4); break; }
	case NN_F64: memcpy(p + 8 * elem, &v, 8); break;
	case NN_I8: ((int8_t *)p)[elem] = (int8_t)v; break;
	case NN_U8: ((uint8_t *)p)[elem] = (uint8_t)v; break;
	case NN_I16: { int16_t x = (int16_t)v; memcpy(p + 2 * elem, &x, 2); break; }
	case NN_U16: { uint16_t x = (uint16_t)v; memcpy(p + 2 * elem, &x, 2); break; }
	case NN_I32: { int32_t x = (int32_t)v; memcpy(p + 4 * elem, &x, 4); break; }
	case NN_U32: { uint32_t x = (uint32_t)v; memcpy(p + 4 * elem, &x, 4); break; }
	default: break;
	}
}

static void model_inputs(struct model *md, const unsigned char *r)
{
	for (int b = 0; b < md->n_ib; b++)
		put_value(md->in[md->ib[b].input], md->ib[b].elem, md->ib[b].dtype,
			  row_value(r + md->ib[b].row_off, md->ib[b].kind));
}

/* One step, timed. NN_OK with *w filled, or NN_ERR_NONFINITE (the state
 * reset by the runtime, w untouched). */
static int model_step(struct model *md, struct wrench *w, uint64_t *took)
{
	uint64_t t0 = hg_now_ns();
	int r = nn_step(md->m, (const void *const *)md->in, md->out);
	uint64_t dt = hg_now_ns() - t0;
	if (took)
		*took = dt;
	md->steps++;
	md->step_ns = md->steps == 1 ? (double)dt : 0.9 * md->step_ns + 0.1 * (double)dt;
	if (r != NN_OK)
		return r;
	memset(w, 0, sizeof *w);
	for (int b = 0; b < md->n_ob; b++)
		memcpy((unsigned char *)w + md->ob[b].off, &md->out[md->ob[b].output][md->ob[b].elem], 4);
	return NN_OK;
}

/* ---- State --------------------------------------------------------------------- */

static char state_dir[PATH_LEN], incoming_dir[PATH_LEN + 16];
static struct model run, prev, cand; /* running, kept for revert, being loaded */
static int have_run, have_prev;
static uint32_t next_version = 1;
static hg_pub *pub_out, *pub_active, *pub_load;
static uint32_t active_topic_id;
static uint64_t active_seq; /* of the active message the running model was announced by */
static uint64_t nonfinite, ticks;
/* Tests only: <state dir>/test-warm-delay-us, read when a load starts, delays
 * each warm-up step of that load (an overrun on demand). */
static long test_warm_delay_us;

/* The load in progress: checks on the worker, warm-up on the tick thread. */
static struct {
	int state;           /* LOAD_* */
	int cause;           /* CAUSE_START or CAUSE_LOAD */
	uint32_t version;
	uint8_t digest[32];
	char path[PATH_LEN + 128];
	struct hg_input req; /* the load_weights request message, for the active message's references */
	int have_req;
	uint32_t warm, warm_needed;
	int check;
	char reason[64];
} L;

/* The worker: one job at a time, under wl. */
static pthread_mutex_t wl = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wc = PTHREAD_COND_INITIALIZER;
enum { JOB_NONE, JOB_CHECK, JOB_KEEP_LOAD, JOB_KEEP_REVERT, JOB_QUIT };
static struct {
	int job;
	int done;            /* JOB_CHECK finished: result below */
	nn_model *m;
	nn_report rep;
	double budget_ns;
	char path[PATH_LEN + 128];
	uint8_t digest[32];
	int have_digest;
	char keep_from[PATH_LEN + 128]; /* JOB_KEEP_LOAD: the new current.hgw's source */
	uint64_t keep_hash;
	char session[256];
} W;

static void publish_load(int state, int check, const char *reason)
{
	struct policy_load m;
	memset(&m, 0, sizeof m);
	m.version = L.version;
	m.state = (uint8_t)state;
	m.check = (uint8_t)check;
	m.hash = digest_hash(L.digest);
	m.since_ns = hg_now_ns();
	if (reason)
		mod_fmt(m.reason, sizeof m.reason, "%s", reason);
	if (hg_publish_with(pub_load, &m, sizeof m, 0, HG_NO_REFS, NULL, 0) < 0)
		mod_log(WHO, "publish %s.load: %s", S.name, strerror(errno));
}

/* Announces the running model, referencing the request that asked for it
 * when there was one (docs/09 "Lineage"). */
static void publish_active(int cause, const struct hg_input *req)
{
	struct policy_active a;
	memset(&a, 0, sizeof a);
	a.version = run.version;
	a.cause = (uint8_t)cause;
	a.hash = run.hash;
	a.arch = run.hdr.arch_hash;
	a.swapped_ns = hg_now_ns();
	memcpy(a.dataset, run.hdr.dataset, sizeof a.dataset < sizeof run.hdr.dataset ? sizeof a.dataset : sizeof run.hdr.dataset);
	a.dataset[sizeof a.dataset - 1] = 0;
	a.dataset_hash = run.hdr.dataset_hash;
	memcpy(a.newest_row_boot, run.hdr.newest_row_boot, 16);
	a.newest_row_stamp_ns = run.hdr.newest_row_stamp_ns;
	a.rows = run.hdr.rows;
	active_seq = hg_pub_next_seq(pub_active);
	if (req)
		hg_publish_with(pub_active, &a, sizeof a, 0, 0, req, 1);
	else
		hg_publish_with(pub_active, &a, sizeof a, 0, HG_NO_REFS, NULL, 0);
	mod_log(WHO, "%s: version %u running (%s), hash %016llx", S.name, run.version,
		cause == CAUSE_START ? "start" : cause == CAUSE_LOAD ? "load" : "revert", (unsigned long long)run.hash);
}

static int file_digest(const char *path, uint8_t out[32])
{
	char why[64];
	size_t size;
	const void *p = nn_hgw_map(path, &size, why, sizeof why);
	if (!p)
		return -1;
	nn_blake2s(out, 32, p, size, NULL, 0);
	nn_hgw_unmap(p, size);
	return 0;
}

static int copy_file(const char *from, const char *to)
{
	int in = open(from, O_RDONLY | O_CLOEXEC);
	if (in < 0)
		return -1;
	char tmp[PATH_LEN + 160];
	mod_fmt(tmp, sizeof tmp, "%s.tmp", to);
	int out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0640);
	if (out < 0) {
		close(in);
		return -1;
	}
	char buf[65536];
	ssize_t n;
	int rc = 0;
	while ((n = read(in, buf, sizeof buf)) > 0)
		if (write(out, buf, (size_t)n) != n) {
			rc = -1;
			break;
		}
	if (n < 0)
		rc = -1;
	close(in);
	if (close(out) < 0)
		rc = -1;
	if (rc == 0 && rename(tmp, to) < 0)
		rc = -1;
	if (rc < 0)
		unlink(tmp);
	return rc;
}

static void *worker(void *arg)
{
	(void)arg;
	/* SCHED_IDLE: the checks never delay a tick (docs/09 "Loading weights"). */
	struct sched_param sp = { .sched_priority = 0 };
	pthread_setschedparam(pthread_self(), SCHED_IDLE, &sp);
	pthread_mutex_lock(&wl);
	for (;;) {
		while (W.job == JOB_NONE)
			pthread_cond_wait(&wc, &wl);
		int job = W.job;
		if (job == JOB_QUIT)
			break;
		char path[sizeof W.path], keep_from[sizeof W.keep_from], session[sizeof W.session];
		uint8_t digest[32];
		int have_digest = W.have_digest;
		double budget = W.budget_ns;
		uint64_t keep_hash = W.keep_hash;
		memcpy(path, W.path, sizeof path);
		memcpy(keep_from, W.keep_from, sizeof keep_from);
		memcpy(session, W.session, sizeof session);
		memcpy(digest, W.digest, 32);
		pthread_mutex_unlock(&wl);

		nn_model *m = NULL;
		nn_report rep;
		memset(&rep, 0, sizeof rep);
		if (job == JOB_CHECK) {
			nn_load_req req = {
				.file_hash = have_digest ? digest : NULL,
				.arch = S.arch,
				.tick_period_ns = 0, /* the model's own period is checked against the anchor's below */
				.dataset = S.dataset,
				.dataset_hash = RT->schema.type.hash,
				.input_fields = input_fields,
				.n_input_fields = n_input_fields,
				.output_fields = NULL,
				.n_output_fields = 0,
				.step_budget_ns = budget,
			};
			static const char *wf[] = { "surge", "sway", "heave", "roll", "pitch", "yaw" };
			req.output_fields = wf;
			req.n_output_fields = 6;
			nn_load(path, &req, &m, &rep);
		} else if (job == JOB_KEEP_LOAD) {
			/* The files: the old current.hgw becomes previous.hgw, the new
			 * file current.hgw; and a copy into the recorder's session. */
			char cur[PATH_LEN + 32], old[PATH_LEN + 32];
			mod_fmt(cur, sizeof cur, "%s/current.hgw", state_dir);
			mod_fmt(old, sizeof old, "%s/previous.hgw", state_dir);
			if (access(cur, F_OK) == 0 && strcmp(keep_from, cur) != 0 && rename(cur, old) < 0)
				mod_log(WHO, "keep: %s -> %s: %s", cur, old, strerror(errno));
			if (strcmp(keep_from, cur) != 0 && rename(keep_from, cur) < 0 && copy_file(keep_from, cur) < 0)
				mod_log(WHO, "keep: %s -> %s: %s", keep_from, cur, strerror(errno));
			if (session[0]) {
				char dir[300], to[400];
				mod_fmt(dir, sizeof dir, "%s/weights", session);
				mkdir(dir, 02770);
				mod_fmt(to, sizeof to, "%s/%016llx.hgw", dir, (unsigned long long)keep_hash);
				if (copy_file(cur, to) < 0)
					mod_log(WHO, "keep: a copy into %s: %s", dir, strerror(errno));
			}
		} else if (job == JOB_KEEP_REVERT) {
			char cur[PATH_LEN + 32], old[PATH_LEN + 32], tmp[PATH_LEN + 32];
			mod_fmt(cur, sizeof cur, "%s/current.hgw", state_dir);
			mod_fmt(old, sizeof old, "%s/previous.hgw", state_dir);
			mod_fmt(tmp, sizeof tmp, "%s/swap.hgw", state_dir);
			if (rename(cur, tmp) < 0 || rename(old, cur) < 0 || rename(tmp, old) < 0)
				mod_log(WHO, "revert: exchanging the files: %s", strerror(errno));
		}

		pthread_mutex_lock(&wl);
		if (job == JOB_CHECK) {
			W.m = m;
			W.rep = rep;
			W.done = 1;
		}
		W.job = JOB_NONE;
	}
	pthread_mutex_unlock(&wl);
	return NULL;
}

static void worker_post(int job)
{
	pthread_mutex_lock(&wl);
	W.job = job;
	pthread_cond_signal(&wc);
	pthread_mutex_unlock(&wl);
}

static int worker_busy(void)
{
	pthread_mutex_lock(&wl);
	int busy = W.job != JOB_NONE;
	pthread_mutex_unlock(&wl);
	return busy;
}

/* The budget left for a new model's step: TickBudget of the tick period,
 * less the running model's mean step (check 7). 0: no budget (an event
 * anchor, whose period the policy does not know). */
static double step_budget(void)
{
	if (!tick_period_ns)
		return 0;
	double b = S.tick_budget * (double)tick_period_ns - (have_run ? run.step_ns : 0);
	return b > 1 ? b : 1;
}

static void load_reject(int check, const char *reason)
{
	L.check = check;
	mod_fmt(L.reason, sizeof L.reason, "%s", reason);
	publish_load(LOAD_REJECTED, check, reason);
	mod_log(WHO, "%s: load of version %u rejected, check %d: %s", S.name, L.version, check, reason);
	model_free(&cand);
	L.state = LOAD_NONE;
}

static void load_start(int cause, const char *path, const uint8_t *digest, const struct hg_input *req)
{
	memset(&L, 0, sizeof L);
	L.state = LOAD_CHECKING;
	L.cause = cause;
	L.version = next_version++;
	mod_fmt(L.path, sizeof L.path, "%s", path);
	if (digest)
		memcpy(L.digest, digest, 32);
	else
		file_digest(path, L.digest);
	if (req) {
		L.req = *req;
		L.have_req = 1;
	}
	publish_load(LOAD_CHECKING, 0, NULL);
	char tpath[PATH_LEN + 32];
	mod_fmt(tpath, sizeof tpath, "%s/test-warm-delay-us", state_dir);
	FILE *tf = fopen(tpath, "r");
	test_warm_delay_us = 0;
	if (tf) {
		if (fscanf(tf, "%ld", &test_warm_delay_us) != 1)
			test_warm_delay_us = 0;
		fclose(tf);
	}
	pthread_mutex_lock(&wl);
	mod_fmt(W.path, sizeof W.path, "%s", path);
	memcpy(W.digest, L.digest, 32);
	W.have_digest = digest != NULL;
	W.budget_ns = step_budget();
	W.done = 0;
	W.m = NULL;
	W.job = JOB_CHECK;
	pthread_cond_signal(&wc);
	pthread_mutex_unlock(&wl);
}

/* The worker's verdict on checks 1 to 7, taken on the tick thread. */
static void load_poll(void)
{
	if (L.state != LOAD_CHECKING)
		return;
	pthread_mutex_lock(&wl);
	int done = W.done;
	nn_model *m = W.m;
	nn_report rep = W.rep;
	if (done) {
		W.done = 0;
		W.m = NULL;
	}
	pthread_mutex_unlock(&wl);
	if (!done)
		return;
	if (!m) {
		load_reject(rep.check ? rep.check : 3, rep.reason);
		return;
	}
	memset(&cand, 0, sizeof cand);
	cand.m = m;
	memcpy(cand.digest, L.digest, 32);
	cand.hash = digest_hash(L.digest);
	cand.version = L.version;
	char why[128];
	if (model_bind(&cand, why, sizeof why) < 0) {
		load_reject(4, why);
		return;
	}
	cand.step_ns = rep.step_ns;
	L.state = LOAD_WARMING;
	L.warm_needed = cand.hdr.warmup_ticks ? cand.hdr.warmup_ticks : 1;
	publish_load(LOAD_WARMING, 0, NULL);
}

/* The open session's directory, "" with none. hg_latest gives a message once:
 * the last one is kept, so a second load in the same session finds it too. */
static void session_path(char *out, size_t len)
{
	static struct recorder_session s;
	static struct hg_info info;
	static int have;
	out[0] = 0;
	if (!sub_session)
		return;
	if (mod_latest(sub_session, &s, sizeof s, &info, &have) && s.active)
		mod_fmt(out, len, "%s", s.path);
}

/* Between two ticks: the warmed-up model becomes the running one. */
static void swap_in(void)
{
	int cause = L.cause;
	if (have_run) {
		if (have_prev)
			model_free(&prev);
		prev = run;
		have_prev = 1;
	}
	run = cand;
	memset(&cand, 0, sizeof cand);
	have_run = 1;
	publish_active(cause, L.have_req ? &L.req : NULL);
	publish_load(LOAD_DONE, 0, NULL);
	if (cause == CAUSE_LOAD) {
		pthread_mutex_lock(&wl);
		mod_fmt(W.keep_from, sizeof W.keep_from, "%s", L.path);
		W.keep_hash = run.hash;
		session_path(W.session, sizeof W.session);
		pthread_mutex_unlock(&wl);
		worker_post(JOB_KEEP_LOAD);
	}
	L.state = LOAD_NONE;
}

/* ---- The tick -------------------------------------------------------------------- */

static void publish_output(const struct wrench *w, const struct hg_info *anchor, int stale, int with_active)
{
	struct hg_input refs[HG_MAX_REFS];
	int n = 0;
	refs[n++] = (struct hg_input){ .topic_id = RT->anchor_topic_id, .seq = anchor->seq };
	if (with_active)
		refs[n++] = (struct hg_input){ .topic_id = active_topic_id, .seq = active_seq };
	for (int i = 0; i < DS.n_parts && n < HG_MAX_REFS; i++) {
		if (DS.parts[i].role != HG_DS_INPUT)
			continue;
		uint64_t seq;
		memcpy(&seq, row + RT->parts[i].hdr_off, 8);
		if (seq)
			refs[n++] = (struct hg_input){ .topic_id = RT->parts[i].topic_id, .seq = seq };
	}
	unsigned flags = stale ? HG_STALE : 0;
	if (hg_publish_with(pub_out, w, sizeof *w, anchor->stamp_ns, flags, refs, n) < 0 &&
	    hg_publish_with(pub_out, w, sizeof *w, anchor->stamp_ns, flags | HG_NO_REFS, NULL, 0) < 0)
		mod_log(WHO, "publish policy.%s.wrench: %s", S.name, strerror(errno));
}

static int pending_gap;

static void tick(const struct hg_info *anchor)
{
	ticks++;
	struct hg_ds_joined jr[HG_DS_MAX_PARTS];
	memset(jr, 0, sizeof jr);
	hg_ds_row_begin(RT, row, anchor, apayload, boot);
	int stale = 0;
	for (int i = 0; i < DS.n_parts; i++) {
		if (DS.parts[i].role != HG_DS_INPUT)
			continue;
		hg_ds_join(RT, i, row, anchor, &src[i], scratch, &jr[i]);
		stale |= jr[i].missing || jr[i].stale;
	}
	/* The reset rule, the pairer's: only input parts count in it, so the
	 * policy resets its state at the rows whose reset bit is set. */
	hg_ds_row_finish(RT, &dstate, row, jr, pending_gap, anchor->epoch);
	pending_gap = 0;
	uint32_t rflags;
	memcpy(&rflags, row + HG_DS_OFF_FLAGS, 4);
	int reset = (rflags & HG_DS_ROW_RESET) != 0;

	uint64_t t_run = 0;
	if (!have_run) {
		struct wrench zero = { 0 };
		publish_output(&zero, anchor, 1, 0);
	} else {
		if (reset)
			nn_reset(run.m);
		model_inputs(&run, row);
		struct wrench w;
		if (model_step(&run, &w, &t_run) == NN_OK) {
			publish_output(&w, anchor, stale, 1);
		} else {
			/* Never published; the state was reset by the runtime; the mux
			 * sees a skipped tick and demotes (docs/09 "Numerics"). */
			nonfinite++;
		}
	}

	if (L.state == LOAD_WARMING) {
		if (reset)
			nn_reset(cand.m);
		model_inputs(&cand, row);
		struct wrench w;
		uint64_t t_new;
		int r = model_step(&cand, &w, &t_new);
		if (test_warm_delay_us > 0) {
			usleep((useconds_t)test_warm_delay_us);
			t_new += (uint64_t)test_warm_delay_us * 1000u;
		}
		/* A tick whose two steps together overran the tick period stops the
		 * load (docs/09, check 8): the running model's output was late. */
		if (r != NN_OK) {
			load_reject(8, "warm-up: a non-finite output");
		} else if (tick_period_ns && t_run + t_new > tick_period_ns) {
			load_reject(8, "warm-up overran");
		} else if (++L.warm >= L.warm_needed) {
			swap_in();
		}
	}
}

/* A revert asked for: at the next tick boundary, which is now. */
static int on_revert(const hg_call_ctx *c, const void *req, void *rep_, char *reason)
{
	(void)req;
	struct policy_version *rep = rep_;
	if (!have_prev) {
		mod_fmt(reason, HG_CALL_REASON, "no previous model to revert to");
		return HG_E_REFUSED;
	}
	if (L.state != LOAD_NONE) {
		mod_fmt(reason, HG_CALL_REASON, "a load is in progress (version %u)", L.version);
		return HG_E_REFUSED;
	}
	struct model t = run;
	run = prev;
	prev = t;
	nn_reset(run.m);
	run.version = next_version++;
	publish_active(CAUSE_REVERT, &c->req);
	worker_post(JOB_KEEP_REVERT);
	rep->version = run.version;
	rep->hash = run.hash;
	return HG_OK;
}

static int on_reset_state(const hg_call_ctx *c, const void *req, void *rep, char *reason)
{
	(void)c;
	(void)req;
	(void)rep;
	(void)reason;
	if (have_run)
		nn_reset(run.m);
	if (L.state == LOAD_WARMING)
		nn_reset(cand.m);
	return HG_OK;
}

static int on_runtime_info(const hg_call_ctx *c, const void *req, void *rep_, char *reason)
{
	(void)c;
	(void)req;
	(void)reason;
	struct runtime_info *rep = rep_;
	rep->format_version = NN_HGW_FORMAT_VERSION;
	rep->max_inputs = MAX_MODEL_IO;
	rep->max_outputs = MAX_MODEL_IO;
	rep->arch = S.arch;
	mod_fmt(rep->dataset, sizeof rep->dataset, "%s", S.dataset);
	rep->dataset_hash = bound ? RT->schema.type.hash : 0;
	rep->tick_period_ns = tick_period_ns;
	size_t u = 0;
	for (size_t i = 0; i < nn_op_registry_len && u + 32 < sizeof rep->ops; i++) {
		u += (size_t)mod_fmt(rep->ops + u, sizeof rep->ops - u, "%s%s:%u", u ? " " : "",
				     nn_op_registry[i].type, nn_op_registry[i].version);
		rep->n_ops++;
	}
	return HG_OK;
}

static int hexval(char c)
{
	return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static int on_load_weights(const hg_call_ctx *c, const void *req_, void *rep_, char *reason)
{
	const struct load_weights_req *req = req_;
	struct load_weights_rep *rep = rep_;
	char name[sizeof req->name + 1];
	memcpy(name, req->name, sizeof req->name);
	name[sizeof req->name] = 0;
	if (!bound) {
		mod_fmt(reason, HG_CALL_REASON, "dataset %s not bound yet (its topics are absent)", S.dataset);
		return HG_E_REFUSED;
	}
	if (L.state != LOAD_NONE || worker_busy()) {
		mod_fmt(reason, HG_CALL_REASON, "a load is in progress (version %u)", L.version);
		return HG_E_REFUSED;
	}
	if (!name[0] || strchr(name, '/') || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
		mod_fmt(reason, HG_CALL_REASON, "name: a file name in the incoming directory, no '/'");
		return HG_E_REFUSED;
	}
	uint8_t digest[32];
	for (int i = 0; i < 32; i++) {
		int hi = hexval(req->blake2s[2 * i]), lo = hexval(req->blake2s[2 * i + 1]);
		if (hi < 0 || lo < 0) {
			mod_fmt(reason, HG_CALL_REASON, "blake2s: 64 hex digits");
			return HG_E_REFUSED;
		}
		digest[i] = (uint8_t)(hi << 4 | lo);
	}
	char path[PATH_LEN + 128];
	mod_fmt(path, sizeof path, "%s/%s", incoming_dir, name);
	if (access(path, R_OK) < 0) {
		mod_fmt(reason, HG_CALL_REASON, "%s: %s", name, strerror(errno));
		return HG_E_REFUSED;
	}
	load_start(CAUSE_LOAD, path, digest, &c->req);
	rep->version = L.version;
	mod_log(WHO, "%s: load_weights %s by %s: version %u, checking", S.name, name, c->caller, L.version);
	return HG_OK;
}

/* ---- Start ------------------------------------------------------------------------ */

/* current.hgw and previous.hgw, at start: previous.hgw checked and kept in
 * memory for revert at once; current.hgw through the worker and the warm-up,
 * announced with cause start. Both skip check 1 (no request names a hash). */
static void reload_at_start(void)
{
	char cur[PATH_LEN + 32], old[PATH_LEN + 32];
	mod_fmt(cur, sizeof cur, "%s/current.hgw", state_dir);
	mod_fmt(old, sizeof old, "%s/previous.hgw", state_dir);
	if (access(old, R_OK) == 0) {
		nn_load_req req = { .arch = S.arch, .dataset = S.dataset, .dataset_hash = RT->schema.type.hash,
				    .input_fields = input_fields, .n_input_fields = n_input_fields };
		nn_model *m = NULL;
		nn_report rep;
		if (nn_load(old, &req, &m, &rep) == 0 && m) {
			memset(&prev, 0, sizeof prev);
			prev.m = m;
			file_digest(old, prev.digest);
			prev.hash = digest_hash(prev.digest);
			prev.version = next_version++;
			char why[128];
			if (model_bind(&prev, why, sizeof why) == 0)
				have_prev = 1;
			else
				model_free(&prev);
		} else {
			mod_log(WHO, "%s: previous.hgw not kept: check %d: %s", S.name, rep.check, rep.reason);
		}
	}
	if (access(cur, R_OK) == 0)
		load_start(CAUSE_START, cur, NULL, NULL);
}

static void status_line(void)
{
	char line[256];
	if (have_run)
		mod_fmt(line, sizeof line, "%s: version %u, hash %016llx, step %.1f us, %llu ticks, %llu non-finite%s", S.name,
			run.version, (unsigned long long)run.hash, run.step_ns / 1000.0, (unsigned long long)ticks,
			(unsigned long long)nonfinite,
			L.state == LOAD_CHECKING ? "; checking a load" : L.state == LOAD_WARMING ? "; warming a load" : "");
	else
		mod_fmt(line, sizeof line, "%s: no model (%s), %llu ticks, neutral output", S.name,
			bound ? (L.state ? "loading" : "none loaded") : "dataset not bound", (unsigned long long)ticks);
	hg_status(line);
}

int main(int argc, char **argv)
{
	mod_init(WHO);
	if (argc != 2) {
		mod_log(WHO, "usage: hangar-policy <spec.policy>");
		return 64;
	}
	char err[400];
	if (spec_load(argv[1], err, sizeof err) < 0) {
		mod_log(WHO, "%s", err);
		hg_status(err);
		return 78;
	}
	char module[64];
	mod_fmt(module, sizeof module, "policy_%s", S.name);
	if (hg_module_name() && strcmp(hg_module_name(), module) != 0) {
		mod_log(WHO, "the module is %s; the spec's Name makes it %s", hg_module_name(), module);
		return 78;
	}
	char dpath[PATH_LEN + 64];
	mod_fmt(dpath, sizeof dpath, "%s/%s.dataset", S.dataset_dir, S.dataset);
	if (hg_ds_read_file(dpath, &DS, err, sizeof err) < 0) {
		mod_log(WHO, "%s: %s", dpath, err);
		return 78;
	}
	char waits[200];
	if (!hg_ds_policy_usable(&DS, waits, sizeof waits)) {
		mod_log(WHO, "%s: input joins that would have to wait (offset + Lateness > 0): %s", S.dataset, waits);
		return 78;
	}
	RT = calloc(1, sizeof *RT);
	if (!RT)
		return 3;
	sub_anchor = hg_sub_open(DS.anchor, NULL);
	if (!sub_anchor) {
		mod_log(WHO, "subscribe %s: %s (the manifest needs [Subscribe %s])", DS.anchor, strerror(errno), DS.anchor);
		return 78;
	}
	for (int i = 0; i < DS.n_parts; i++) {
		sub_part[i] = hg_sub_open(DS.parts[i].topic, NULL);
		/* Every part, not only the inputs: the row type, whose hash a
		 * weights file is checked against (check 4), is built over all
		 * of them, as the pairer's. Only the inputs are ever read. */
		if (!sub_part[i]) {
			mod_log(WHO, "subscribe %s: %s (each of the dataset's parts needs [Subscribe %s])",
				DS.parts[i].topic, strerror(errno), DS.parts[i].topic);
			return 78;
		}
	}
	sub_session = hg_sub_open("recorder.session", &RECORDER_SESSION_TYPE); /* optional */
	char t_out[96], t_active[96], t_load[96];
	mod_fmt(t_out, sizeof t_out, "policy.%s.wrench", S.name);
	mod_fmt(t_active, sizeof t_active, "policy.%s.active", S.name);
	mod_fmt(t_load, sizeof t_load, "policy.%s.load", S.name);
	pub_out = hg_pub_open(t_out, &WRENCH_TYPE, NULL);
	pub_active = hg_pub_open(t_active, &POLICY_ACTIVE_TYPE, NULL);
	pub_load = hg_pub_open(t_load, &POLICY_LOAD_TYPE, NULL);
	if (!pub_out || !pub_active || !pub_load) {
		mod_log(WHO, "the outputs %s, %s and %s: %s", t_out, t_active, t_load, strerror(errno));
		return 78;
	}
	active_topic_id = hg_topic_id(t_active);
	if (hg_serve_method("load_weights", on_load_weights, NULL) < 0 || hg_serve_method("revert", on_revert, NULL) < 0 ||
	    hg_serve_method("runtime_info", on_runtime_info, NULL) < 0)
		mod_log(WHO, "serving load_weights, revert, runtime_info: %s", strerror(errno));
	(void)hg_serve_method("reset_state", on_reset_state, NULL); /* optional in the manifest */
	if (mod_state_dir(state_dir, sizeof state_dir) < 0) {
		mod_log(WHO, "state directory %s: %s", state_dir, strerror(errno));
		return 73;
	}
	mod_fmt(incoming_dir, sizeof incoming_dir, "%s/incoming", state_dir);
	mkdir(incoming_dir, 0770);

	pthread_t th;
	if (pthread_create(&th, NULL, worker, NULL) != 0) {
		mod_log(WHO, "worker thread: %s", strerror(errno));
		return 71;
	}

	hg_sub *subs[HG_MAX_WAIT];
	int n = 0;
	subs[n++] = sub_anchor;
	int nc = hg_call_subs(subs + n, HG_MAX_WAIT - n);
	if (nc > 0)
		n += nc;
	mod_log(WHO, "%s on %s (anchor %s), Arch %016llx, TickBudget %.0f%%, state %s", S.name, S.dataset, DS.anchor,
		(unsigned long long)S.arch, S.tick_budget * 100, state_dir);
	uint64_t last_status = 0;
	int started = 0;
	while (!mod_stop) {
		/* The anchor wakes the tick; a load in progress is polled at 20 Hz
		 * (the worker's verdict). Nothing in the set goes unread: the
		 * anchor is read to its end below, the call topics by hg_serve. */
		unsigned r = hg_wait(subs, n, L.state == LOAD_CHECKING ? 50 : 500);
		if (r & HG_READY_INTERRUPT)
			break;
		if (!bound) {
			try_bind();
			if (bound && !started) {
				started = 1;
				reload_at_start();
			}
		}
		hg_serve();
		load_poll();
		if (bound) {
			struct hg_info info;
			for (int k = 0; k < 4096; k++) {
				enum hg_result rr = hg_next(sub_anchor, apayload, RT->anchor_payload_bytes + 8, &info);
				if (rr != HG_MSG)
					break;
				if (info.gap)
					pending_gap = 1;
				tick(&info);
			}
		} else {
			(void)hg_skip(sub_anchor); /* not bound yet: nothing to tick on (the lesson of hangar#1) */
		}
		uint64_t now = hg_now_ns();
		if (now - last_status > 1000000000u) {
			last_status = now;
			status_line();
			hg_alive();
		}
	}
	worker_post(JOB_QUIT);
	pthread_join(th, NULL);
	mod_log(WHO, "%s: stopping", S.name);
	return 0;
}
