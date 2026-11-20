// SPDX-License-Identifier: GPL-3.0-or-later
/* A topic file as a subscriber reads it (lib/hg/hg_topic.c: read_layout,
 * attach, hg_latest, hg_next, hg_read_seq, hg_find_stamp, hg_sub_meta,
 * hg_sub_schema): the header and ring of a file that is hostile or damaged.
 * A subscriber maps the file read-only and must take nothing from it on trust:
 * sizes, counts, offsets and sequence numbers are another process's, which may
 * be a module of lower privilege than the reader (docs/01).
 *
 * Input: the bytes of the topic file. It is written to a directory of this
 * process's own ($TMPDIR or /tmp), opened as `fuzz.t` without a manager, and
 * read the ways a module, the recorder, the bridge and `hangar echo` read. */
#include "fuzz.h"
#include "hg_topic.h"
#include "hg_type.h"

#include <fcntl.h>
#include <sys/stat.h>

static char dir[256], path[300];
static struct hg_schema sch;

static void cleanup(void)
{
	char p[320];
	static const char *const names[] = { "fuzz.t", ".fuzz.t.wake", ".dir" };
	for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
		snprintf(p, sizeof p, "%s/%s", dir, names[i]);
		unlink(p);
	}
	rmdir(dir);
}

static void setup(void)
{
	const char *tmp = getenv("TMPDIR");
	snprintf(dir, sizeof dir, "%s/hg-fuzz-topic-XXXXXX", tmp && *tmp ? tmp : "/tmp");
	if (!mkdtemp(dir))
		abort();
	snprintf(path, sizeof path, "%s/fuzz.t", dir);
	/* The topic's wake file, which its writer makes beside it: 64 bytes. */
	char wake[320];
	snprintf(wake, sizeof wake, "%s/.fuzz.t.wake", dir);
	int wfd = open(wake, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (wfd < 0 || ftruncate(wfd, 64) < 0)
		abort();
	close(wfd);
	setenv("HANGAR_TOPIC_DIR", dir, 1);
	unsetenv("HANGAR_CONTROL");
	unsetenv("HANGAR_WIRING");
	unsetenv("HANGAR_WIRING_FILE");
	unsetenv("HANGAR_PEER_TOPIC_DIR");
	atexit(cleanup);
}

#define BUF (1u << 16)

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (!dir[0])
		setup();
	/* A new file each time (a new inode), as a replaced topic is. */
	unlink(path);
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	if (fd < 0)
		abort();
	if (size && write(fd, data, size) != (ssize_t)size)
		abort();
	close(fd);

	for (int oldest = 0; oldest < 2; oldest++) {
		hg_sub *s = hg_sub_open_from("fuzz.t", NULL, oldest ? HG_OLDEST : HG_NEWEST);
		if (!s)
			continue;
		FZ_NOTE("payload %u bytes", hg_sub_payload_bytes(s));
		struct hg_info info;
		struct hg_topic_meta meta;
		uint32_t pb = hg_sub_payload_bytes(s);
		/* A buffer of exactly the payload, on the heap: a copy that takes
		 * more than the validated size writes past it. */
		size_t blen = pb && pb <= BUF ? pb : 8;
		unsigned char *exact = malloc(blen);
		if (!exact)
			abort();
		enum hg_result r = hg_latest(s, exact, blen, &info);
		FZ_NOTE("latest: result %d seq %llu", (int)r, (unsigned long long)info.seq);
		if (r == HG_MSG) {
			FZ_CHECK(info.n_inputs <= HG_MAX_REFS);
			(void)hg_read_seq(s, info.seq, exact, blen, &info);
			(void)hg_read_seq(s, info.seq - 1, exact, blen, &info);
			(void)hg_find_stamp(s, info.stamp_ns, exact, blen, &info);
		}
		for (int i = 0; i < 6; i++)
			(void)hg_next(s, exact, blen, &info);
		(void)hg_skip(s);
		(void)hg_read_seq(s, 1, exact, blen, &info);
		(void)hg_read_seq(s, UINT64_MAX, exact, blen, &info);
		(void)hg_find_stamp(s, UINT64_MAX, exact, blen, &info);
		(void)hg_find_stamp(s, 0, exact, blen, &info);
		/* One too short is refused, not copied into. */
		if (pb > 8 && pb <= BUF)
			FZ_CHECK(hg_latest(s, exact, pb - 8, &info) != HG_MSG);
		if (hg_sub_meta(s, &meta) == 0) {
			uint32_t sl = 0;
			const char *schema = hg_sub_schema(s, &sl);
			if (schema) {
				FZ_CHECK(sl <= HG_SCHEMA_MAX);
				char err[160];
				(void)hg_schema_parse(schema, sl, &sch, err, sizeof err);
			}
		}
		hg_sub *one[1] = { s };
		(void)hg_wait(one, 1, 0);
		free(exact);
		hg_sub_close(s);
	}
	return 0;
}
