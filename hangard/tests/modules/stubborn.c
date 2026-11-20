// SPDX-License-Identifier: GPL-3.0-or-later
/* stubborn: ready via hg_ready(); ignores SIGTERM, so that a stop needs
 * SIGKILL, unless --polite.
 * --no-ready: never calls hg_ready(). --exit N: exits N right after hg_init.
 * --env FILE: appends its HANGAR_* environment and FOO to FILE.
 * --ctl PREFIX: sends the bytes of PREFIX.1, PREFIX.2, ... as raw control
 * requests, as this module's main process, and writes each reply to
 * PREFIX.<n>.reply (a mirrored open by hand); a descriptor that comes with
 * a reply stays open, as a writer's would.
 * --fill-page-exit78 --page FILE: fills the module page's status line and
 * everything after it with 'A' and exits 78. */
#include "toy.h"
#include <fcntl.h>
#include <sys/mman.h>
#include "hg_control.h"

int main(int argc, char **argv)
{
	const char *envf = toy_arg(argc, argv, "--env", NULL);
	if (envf) {
		FILE *f = fopen(envf, "a");
		if (f) {
			fprintf(f, "MODULE=%s CONTROL=%s WIRING=%s FOO=%s\n", getenv("HANGAR_MODULE") ? getenv("HANGAR_MODULE") : "(unset)",
				getenv("HANGAR_CONTROL") ? "set" : "(unset)",
				getenv("HANGAR_WIRING") ? getenv("HANGAR_WIRING") : "(unset)",
				getenv("FOO") ? getenv("FOO") : "(unset)");
			fclose(f);
		}
		/* Without the manager's variables the library would run standalone
		 * against the real paths: stop here instead. */
		if (!getenv("HANGAR_CONTROL") || !getenv("HANGAR_TOPIC_DIR"))
			return 9;
	}
	toy_init("stubborn");
	if (!toy_flag(argc, argv, "--polite"))
		signal(SIGTERM, SIG_IGN);
	if (toy_flag(argc, argv, "--fill-page-exit78")) {
		/* The page is the module's own file: write it through the path. */
		const char *path = toy_arg(argc, argv, "--page", NULL);
		int fd = path ? open(path, O_RDWR) : -1;
		if (fd < 0) {
			perror("stubborn: page");
			return 5;
		}
		char *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (p == MAP_FAILED)
			return 5;
		memset(p + 64, 'A', 4096 - 64);
		return 78;
	}
	const char *ctlp = toy_arg(argc, argv, "--ctl", NULL);
	for (int i = 1; ctlp; i++) {
		char path[1024], out[1100];
		static char req[HG_CONTROL_MAX], rep[HG_CONTROL_MAX];
		snprintf(path, sizeof path, "%s.%d", ctlp, i);
		snprintf(out, sizeof out, "%s.reply", path);
		int fd = open(path, O_RDONLY);
		if (fd < 0)
			break;
		ssize_t n = read(fd, req, sizeof req);
		close(fd);
		int tfd = -1;
		ssize_t r = hg_control_call(NULL, req, (size_t)n, rep, sizeof rep, &tfd, 1);
		FILE *f = fopen(out, "w");
		if (f) {
			fprintf(f, "%s", r < 0 ? strerror(errno) : rep);
			fclose(f);
		}
		/* tfd stays open, as a writer's would */
	}
	if (toy_arg(argc, argv, "--exit", NULL))
		return (int)toy_num(argc, argv, "--exit", 0);
	if (!toy_flag(argc, argv, "--no-ready"))
		hg_ready();
	hg_status("stubborn: running");
	while (!toy_stop || !toy_flag(argc, argv, "--polite"))
		toy_sleep_ms(50);
	return 0;
}
