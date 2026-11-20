// SPDX-License-Identifier: GPL-3.0-or-later
/* The regression runner of a fuzz harness: main() for a build without the
 * fuzzer engine. Every argument is a file or a directory of files; each file's
 * bytes go to LLVMFuzzerTestOneInput once. `make check-fuzz-corpus` runs every
 * harness over fuzz/corpus/<name>/ this way, with the compiler of the day, so
 * an input that once crashed is replayed by `make check` for good. */
#define _GNU_SOURCE
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static int n_run;

static int run_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		fprintf(stderr, "%s: cannot open\n", path);
		return -1;
	}
	size_t cap = 1 << 16, len = 0, k;
	uint8_t *buf = malloc(cap);
	while (buf && (k = fread(buf + len, 1, cap - len, f)) > 0) {
		len += k;
		if (len == cap) {
			uint8_t *nb = realloc(buf, cap *= 2);
			if (!nb)
				free(buf);
			buf = nb;
		}
	}
	fclose(f);
	if (!buf) {
		fprintf(stderr, "%s: out of memory\n", path);
		return -1;
	}
	/* The exact length in its own allocation, so that a read one byte past
	 * the input is one byte past a heap block (what ASan and valgrind see). */
	uint8_t *exact = malloc(len ? len : 1);
	if (!exact) {
		free(buf);
		return -1;
	}
	memcpy(exact, buf, len);
	free(buf);
	LLVMFuzzerTestOneInput(exact, len);
	free(exact);
	n_run++;
	return 0;
}

static int by_name(const struct dirent **a, const struct dirent **b)
{
	return strcmp((*a)->d_name, (*b)->d_name);
}

int main(int argc, char **argv)
{
	int bad = 0;
	for (int i = 1; i < argc; i++) {
		struct stat st;
		if (stat(argv[i], &st) < 0) {
			fprintf(stderr, "%s: no such file or directory\n", argv[i]);
			bad = 1;
			continue;
		}
		if (!S_ISDIR(st.st_mode)) {
			bad |= run_file(argv[i]) < 0;
			continue;
		}
		struct dirent **names;
		int n = scandir(argv[i], &names, NULL, by_name);
		for (int k = 0; k < n; k++) {
			char path[4096];
			if (names[k]->d_name[0] != '.') {
				snprintf(path, sizeof path, "%s/%s", argv[i], names[k]->d_name);
				bad |= run_file(path) < 0;
			}
			free(names[k]);
		}
		if (n >= 0)
			free(names);
	}
	printf("%d inputs replayed\n", n_run);
	return bad || n_run == 0;
}
