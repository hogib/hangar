// SPDX-License-Identifier: GPL-3.0-or-later
/* neutral: the toy Neutral command: appends one line to the file named by
 * its first argument and exits 0; with --fail it exits 1; with --hang it
 * never exits. Lets the suites see that the manager ran it, as which user,
 * and what it does when the command fails. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	if (argc < 2)
		return 2;
	int fd = open(argv[1], O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
	if (fd < 0)
		return 3;
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	char line[160];
	int n = snprintf(line, sizeof line, "neutral uid=%lu gid=%lu module=%s at=%lld\n", (unsigned long)getuid(),
			 (unsigned long)getgid(), getenv("HANGAR_MODULE") ? getenv("HANGAR_MODULE") : "?",
			 (long long)ts.tv_sec);
	if (write(fd, line, (size_t)n) != n)
		return 3;
	close(fd);
	for (int i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--fail") == 0)
			return 1;
		if (strcmp(argv[i], "--hang") == 0)
			for (;;)
				pause();
	}
	return 0;
}
