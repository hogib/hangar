// SPDX-License-Identifier: GPL-3.0-or-later
/* hangard: the module manager of docs/03-manager.md. One process, one thread,
 * an epoll loop. This header is shared by its translation units:
 * hangard.c (the loop, start-up, shutdown, reload, re-exec, its own topics),
 * conf.c (hangard.conf and the $HANGAR_ROOT paths), registry.c (the text
 * registry and /proc), topics.c (topic files on the module's behalf),
 * spawn.c (clone3, the child's steps, Neutral), health.c (states, the tick,
 * back-off), control.c (the control socket). */
#ifndef HANGARD_H
#define HANGARD_H

#define _GNU_SOURCE
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <sys/types.h>

#include "hg_topic.h"
#include "manifest.h"

#define HD_PATH 512
#define HD_MAX_MODULES 128
#define HD_MAX_FAILS 64
#define HD_WIRING_MAX 65536
#define HD_PAGE_BYTES 4096

/* ---- Kernel interfaces without a libc wrapper (lib/hg/hg_sys.h style) --- */

#ifndef SYS_clone3
#define SYS_clone3 435
#endif
#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif
#ifndef SYS_pidfd_send_signal
#define SYS_pidfd_send_signal 424
#endif
#ifndef SYS_pidfd_getfd
#define SYS_pidfd_getfd 438
#endif
#ifndef SYS_close_range
#define SYS_close_range 436
#endif

struct hd_clone_args {
	uint64_t flags;
	uint64_t pidfd;
	uint64_t child_tid;
	uint64_t parent_tid;
	uint64_t exit_signal;
	uint64_t stack;
	uint64_t stack_size;
	uint64_t tls;
	uint64_t set_tid;
	uint64_t set_tid_size;
	uint64_t cgroup;
};
#define HD_CLONE_PIDFD 0x1000u
#define HD_CLOSE_RANGE_CLOEXEC 4u
#define HD_P_PIDFD 3

/* prctl and capability constants, in case the libc headers lack them. */
#ifndef PR_SET_KEEPCAPS
#define PR_SET_KEEPCAPS 8
#endif
#ifndef PR_SET_DUMPABLE
#define PR_SET_DUMPABLE 4
#endif
#ifndef PR_CAP_AMBIENT
#define PR_CAP_AMBIENT 47
#define PR_CAP_AMBIENT_RAISE 2
#endif
#ifndef SO_PEERGROUPS
#define SO_PEERGROUPS 59
#endif
#define HD_CAP_VERSION_3 0x20080522u
struct hd_cap_header {
	uint32_t version;
	int pid;
};
struct hd_cap_data {
	uint32_t effective, permitted, inheritable;
};

/* ---- Configuration ------------------------------------------------------ */

/* Fixed paths, relocated under $HANGAR_ROOT when it is set (tests): /etc/hangar,
 * /run/hangar, /var/lib/hangar, /usr/share/hangar, /dev/shm/hangar and the
 * journald socket /run/systemd/journal/stdout. */
struct paths {
	char root[HD_PATH / 2];
	char etc[HD_PATH];
	char run[HD_PATH];
	char var[HD_PATH];
	char share_msg[HD_PATH];
	char shm[HD_PATH];
	char control[HD_PATH];
	char registry[HD_PATH];
	char registry_tmp[HD_PATH];
	char pages[HD_PATH];     /* <run>/modules */
	char neutral[HD_PATH];   /* <run>/neutral */
	char journal[HD_PATH];
	char conf[HD_PATH];
};

struct conf {
	char role[16];
	char privileged_module[MF_NAME];
	char module_dir[HD_PATH];
	char msg_dir[HD_PATH];
	char topic_dir[HD_PATH];
	char bridge_conf[HD_PATH];
	char dataset_dir[HD_PATH];
	char recording_dir[HD_PATH];
	char log_dir[HD_PATH];
	uint64_t health_tick_ns;
	int max_modules;
	uint64_t log_cap;
};

void paths_init(struct paths *p, const char *conf_file);
/* 0, or -1 with err "<file>:<line>: <reason>". A missing file is an error
 * (Role is required). */
int conf_load(const char *path, const struct paths *pp, struct conf *c, char *err, size_t errlen);

/* ---- Run-time state ------------------------------------------------------ */

enum state {
	ST_STARTING,
	ST_READY,
	ST_UNHEALTHY,
	ST_BACKOFF,
	ST_FAILED,
	ST_STOPPED,
	ST_REFUSED,
	ST_STOPPING, /* SIGTERM sent, waiting for the exit: see README, deviations */
	ST_COUNT
};
extern const char *const state_names[ST_COUNT];
int state_from_name(const char *s); /* -1 if unknown */

enum sys_state { SYS_NOMINAL, SYS_DEGRADED, SYS_FAILSAFE };
extern const char *const sys_state_names[3];

/* One output of a running module, watched for staleness and readiness. */
struct output {
	char topic[MF_TOPIC];
	uint32_t rate_mhz;
	uint64_t stale_after_ns;
	int implicit;
	unsigned char *hdr;      /* the file's header, mapped read-only; NULL if not yet */
	ino_t ino;
	uint64_t stale_since_ns; /* 0: not stale */
};

struct module {
	char name[MF_NAME];
	struct mf_module *mf;    /* NULL for a module only the registry knows */
	enum state st;
	uint64_t since_ns;       /* when st was entered */
	pid_t pid;               /* 0 when not running */
	int pidfd;
	unsigned long long starttime; /* /proc/<pid>/stat field 22 */
	int adopted;             /* not our child: no exit status */
	int from_registry;       /* its state came from the registry, not a fresh start */
	uint64_t started_ns;     /* the instance: CLOCK_MONOTONIC at spawn */
	uint64_t ready_ns;       /* when it became ready, for ResetAfter */
	uint64_t delay_ns;       /* current back-off; 0 before the first failure */
	unsigned failures;
	uint64_t fail_times[HD_MAX_FAILS];
	int n_fail_times;
	uint64_t restart_at;     /* ST_BACKOFF */
	uint64_t stop_deadline;  /* ST_STOPPING: SIGKILL at this time */
	enum state after_stop;   /* what the exit leads to */
	int count_failure;       /* the pending stop counts as a failure */
	int exit_known;
	int exit_status;         /* if exit_known: >= 0 status, < 0 -signal */
	int wiring_fd;           /* the sealed memfd, or -1 */
	char *wiring;
	size_t wiring_len;
	uint64_t wiring_hash;
	uint64_t manifest_hash;
	int page_fd;
	unsigned char *page;     /* mapped read-write, HD_PAGE_BYTES */
	int n_outputs;
	struct output outputs[MF_MAX_PUBS];
	char why[128];           /* the last health or exit reason */
	int gave_up;             /* in ST_FAILED with OnGiveUp applied */
	uint64_t last_alive_ns;
};

struct hangard {
	struct paths paths;
	struct conf conf;
	struct mf_set set;
	int n_modules;
	struct module modules[HD_MAX_MODULES];
	int epfd, sigfd, timfd, ctlfd;
	int notify_fd;           /* $NOTIFY_SOCKET, or -1 */
	int is_root;
	char cgroup_base[HD_PATH]; /* "" when no delegated cgroup */
	unsigned char boot_id[16];
	enum sys_state sys;
	uint64_t sys_since_ns;
	char sys_reason[128];
	int shutting_down;       /* 1: neutral modules stopping; 2: the rest */
	uint64_t next_tick_ns, next_publish_ns, next_watchdog_ns;
	int publish_now;         /* a state changed: publish before the second is up */
	hg_pub *pub_system, *pub_modules, *pub_admin;
	uid_t uid;
	gid_t gid_hangar, gid_admin; /* (gid_t)-1 if the group does not exist */
	int have_gid_hangar, have_gid_admin;
	uint64_t registry_written_ns;
	int reexec_state_fd;     /* the state handed to us by our predecessor, or -1 */
	int need_prune;          /* a removed module stopped: drop its entry */
	uint64_t next_call_id;   /* of hangar.<role>.admin.req; continued across a re-exec */
};

extern struct hangard G;

/* ---- Logging ------------------------------------------------------------- */

void hd_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void hd_die(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));

/* ---- registry.c ---------------------------------------------------------- */

unsigned long long proc_starttime(pid_t pid);
int proc_tracer_pid(pid_t pid); /* -1 if unknown */
void registry_write(void);

struct reg_entry {
	char name[MF_NAME];
	pid_t pid;
	unsigned long long starttime;
	enum state st;
	unsigned failures;
	uint64_t delay_ns;
	uint64_t manifest_hash, wiring_hash;
};
/* Reads the registry file. The number of module entries, or -1. */
int registry_read(const char *path, struct reg_entry *out, int max, enum sys_state *sys,
		  uint64_t *since, char *reason, size_t reasonlen);

/* Users and groups from /etc/passwd and /etc/group, read by hand. */
int lookup_user(const char *name, uid_t *uid, gid_t *gid);
int lookup_group(const char *name, gid_t *gid);
/* The name of uid, or its number as text when /etc/passwd has no line. */
void lookup_user_name(uid_t uid, char *out, size_t len);

/* ---- topics.c ------------------------------------------------------------ */

struct topic_params {
	const char *name;
	const char *writer;
	pid_t writer_pid;
	uint64_t type_hash;
	uint32_t payload_bytes;
	const char *schema;
	uint32_t schema_bytes;
	uint32_t slots;
	uint32_t rate_mhz;
	uint64_t stale_after_ns;
	const unsigned char *origin_boot; /* NULL for a local topic */
	uint64_t first_seq;               /* the bridge's: used when the boot differs */
};
/* Creates the topic file by docs/01 "Opening" and returns a writable
 * descriptor; -1 with errno and err. */
int topic_create(const struct topic_params *tp, char *err, size_t errlen);
/* Flag bit 1 on the file, then the wake. 0 or -1. */
int topic_mark_died(const char *path);
/* Flags every topic in the directory whose header names writer. */
void topics_mark_writer_died(const char *writer);
/* The wake file (created once per boot) and the directory word. */
int topic_dir_prepare(void);
void topic_dir_bump(void);
/* Maps a topic's 4 KB header read-only; NULL with errno. */
unsigned char *topic_map_header(const char *path, ino_t *ino);
/* Header fields, by offset. */
uint64_t hdr_u64(const unsigned char *h, size_t off);
uint32_t hdr_u32(const unsigned char *h, size_t off);
/* The pid of the process with a topic open for writing, or 0: the header's
 * writer_pid if that process is still alive and is a module of ours. */
int topic_other_writer(const char *name, pid_t asker, char *who, size_t wholen);

/* ---- spawn.c ------------------------------------------------------------- */

/* Starts m's program: the module page is rewritten, the registry entry and
 * the Neutral record are written before the child is released. 0, or -1
 * when the program could not be started (logged; the caller schedules). */
int module_spawn(struct module *m);
/* Writes/reads/removes <run>/hangar/neutral/<name>. */
int neutral_record_write(const struct module *m);
void neutral_record_remove(const char *name);
/* Runs the module's Neutral command from its record, as the module's user,
 * with a 1 s limit; 0 if it ran and exited 0, -1 otherwise (logged). A
 * module without a record returns 1. */
int neutral_run(const char *name);
/* Start-up step 1: every Neutral record whose registry entry is dead. */
void neutral_run_all(const struct reg_entry *dead, int n);
/* SIGTERM to the group; SIGKILL to the cgroup or the group. */
void module_signal_group(struct module *m, int sig);
void module_kill_group(struct module *m);
/* The module page: (re)created with a fresh instance, the rest cleared. */
int page_prepare(struct module *m, uint64_t instance);
int page_open_existing(struct module *m);
int cgroup_for(const struct module *m, char *out, size_t len);

/* ---- health.c ------------------------------------------------------------ */

void set_state(struct module *m, enum state st, const char *why);
void set_sys_state(enum sys_state st, const char *why);
void module_outputs_init(struct module *m);
void module_outputs_unmap(struct module *m);
/* An exit: status read, Neutral run, topics flagged, group killed, reaped. */
void on_module_exit(struct module *m);
/* Stop it (SIGTERM, StopTimeout): next is the state after the exit. */
void module_stop(struct module *m, enum state next, int count_failure, const char *why);
void schedule_restart(struct module *m, const char *why);
void health_tick(uint64_t now);
/* The earliest time anything in health.c needs the loop awake, or 0. */
uint64_t health_next_deadline(void);
/* Starts a module now (from a request or a reload). */
void module_start(struct module *m);
/* Whether the health tick has work: any running module with a rated output,
 * a watchdog, or in its start grace. */
int health_tick_needed(void);
void recompute_sys_state(void);

/* ---- control.c ----------------------------------------------------------- */

int control_open(void);
void control_accept(void);
/* Readable connection fds are tracked by control.c; tag >= TAG_CONN. */
void control_handle(int fd);

/* ---- hangard.c ------------------------------------------------------------ */

#define TAG_SIGNAL 0
#define TAG_TIMER 1
#define TAG_CONTROL 2
#define TAG_MODULE 0x10000
#define TAG_CONN 0x20000

struct module *module_find(const char *name);
void arm_timer(void);
void publish_own_topics(void);
void request_publish(void);
int do_reload(char *err, size_t errlen);
int do_reexec(char *err, size_t errlen);
void begin_shutdown(void);
int epoll_add(int fd, uint64_t tag);
void epoll_del(int fd);

#endif
