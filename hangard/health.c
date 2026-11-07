// SPDX-License-Identifier: GPL-3.0-or-later
/* States and transitions (docs/03 "States"), the health tick, back-off,
 * Burst and OnGiveUp, exits, stops. */
#include "hangard.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define OFF_FLAGS 36
#define OFF_WRITER_PID 152
#define OFF_FIRST_SEQ 168
#define OFF_STALE_AFTER 176
#define OFF_WRITE_SEQ 200
#define OFF_LAST_PUBLISH 208
#define PAGE_INSTANCE 8
#define PAGE_READY 16
#define PAGE_ALIVE 24

void set_sys_state(enum sys_state st, const char *why)
{
	if (G.sys == st && strcmp(G.sys_reason, why ? why : "") == 0)
		return;
	if (G.sys == SYS_FAILSAFE && st != SYS_FAILSAFE)
		return; /* latched: only clear-failsafe leaves it */
	G.sys = st;
	G.sys_since_ns = hg_now_ns();
	mf_fmt(G.sys_reason, sizeof G.sys_reason, "%s", why ? why : "");
	hd_log("system %s: %s", sys_state_names[st], G.sys_reason);
	registry_write();
	request_publish();
}

void recompute_sys_state(void)
{
	if (G.sys == SYS_FAILSAFE)
		return;
	for (int i = 0; i < G.n_modules; i++) {
		const struct module *m = &G.modules[i];
		if (m->st == ST_FAILED && m->gave_up && m->mf && m->mf->giveup == MF_GIVEUP_DEGRADE) {
			char why[128];
			mf_fmt(why, sizeof why, "%s given up", m->name);
			set_sys_state(SYS_DEGRADED, why);
			return;
		}
	}
	if (G.sys == SYS_DEGRADED) {
		G.sys = SYS_NOMINAL;
		G.sys_since_ns = hg_now_ns();
		G.sys_reason[0] = 0;
		hd_log("system nominal");
		registry_write();
		request_publish();
	}
}

void set_state(struct module *m, enum state st, const char *why)
{
	if (why)
		mf_fmt(m->why, sizeof m->why, "%s", why);
	int changed = m->st != st;
	if (changed) {
		m->st = st;
		m->since_ns = hg_now_ns();
		if (st == ST_READY)
			m->ready_ns = m->since_ns;
	}
	if (changed || (why && *why))
		hd_log("%s: %s%s%s", m->name, state_names[st], why && *why ? ": " : "", why && *why ? why : "");
	registry_write();
	request_publish();
	if (changed && (st == ST_FAILED || st == ST_STOPPED || st == ST_STARTING))
		recompute_sys_state();
}

void module_outputs_unmap(struct module *m)
{
	for (int i = 0; i < m->n_outputs; i++) {
		if (m->outputs[i].hdr)
			munmap(m->outputs[i].hdr, 4096);
		m->outputs[i].hdr = NULL;
		m->outputs[i].stale_since_ns = 0;
	}
}

void module_outputs_init(struct module *m)
{
	module_outputs_unmap(m);
	m->n_outputs = 0;
	if (!m->mf)
		return;
	for (int i = 0; i < m->mf->n_pubs && m->n_outputs < MF_MAX_PUBS; i++) {
		const struct mf_pub *p = &m->mf->pubs[i];
		struct output *o = &m->outputs[m->n_outputs++];
		memset(o, 0, sizeof *o);
		mf_fmt(o->topic, sizeof o->topic, "%s", p->topic);
		o->rate_mhz = p->rate_mhz;
		o->stale_after_ns = p->stale_after_ns;
		o->implicit = p->implicit;
	}
}

/* Maps, or re-maps, an output's header so that it is the file this
 * instance writes (its writer_pid is the module's). 1 if mapped and
 * written by this instance. */
static int output_current(struct module *m, struct output *o)
{
	if (o->hdr && (pid_t)hdr_u32(o->hdr, OFF_WRITER_PID) == m->pid && !(hdr_u32(o->hdr, OFF_FLAGS) & HG_FLAG_REPLACED))
		return 1;
	if (o->hdr) {
		munmap(o->hdr, 4096);
		o->hdr = NULL;
	}
	char path[HD_PATH];
	mf_fmt(path, sizeof path, "%s/%s", G.conf.topic_dir, o->topic);
	o->hdr = topic_map_header(path, &o->ino);
	if (!o->hdr)
		return 0;
	if ((pid_t)hdr_u32(o->hdr, OFF_WRITER_PID) != m->pid) {
		munmap(o->hdr, 4096);
		o->hdr = NULL;
		return 0;
	}
	return 1;
}

static int page_ready(const struct module *m)
{
	if (!m->page)
		return 0;
	uint64_t inst, ready;
	memcpy(&inst, m->page + PAGE_INSTANCE, 8);
	memcpy(&ready, m->page + PAGE_READY, 8);
	return inst != 0 && ready == inst;
}

static uint64_t page_alive(const struct module *m)
{
	if (!m->page)
		return 0;
	uint64_t a;
	memcpy(&a, m->page + PAGE_ALIVE, 8);
	return a;
}

/* Ready is having published once on every rated output, or hg_ready(). */
static int module_is_ready(struct module *m)
{
	if (page_ready(m))
		return 1;
	int rated = 0;
	for (int i = 0; i < m->n_outputs; i++) {
		struct output *o = &m->outputs[i];
		if (!o->rate_mhz)
			continue;
		rated++;
		if (!output_current(m, o))
			return 0;
		if (hdr_u64(o->hdr, OFF_WRITE_SEQ) <= hdr_u64(o->hdr, OFF_FIRST_SEQ))
			return 0;
	}
	return rated > 0;
}

void module_start(struct module *m)
{
	if (m->pid || !m->mf || m->mf->refused)
		return;
	m->gave_up = 0;
	m->stop_deadline = 0;
	module_outputs_init(m);
	if (module_spawn(m) < 0) {
		if (m->mf->policy == MF_RESTART_NEVER)
			set_state(m, ST_STOPPED, "cannot start");
		else
			schedule_restart(m, "cannot start");
		return;
	}
	if (m->mf->legacy)
		set_state(m, ST_READY, "legacy: ready while it runs");
	else
		set_state(m, ST_STARTING, "");
}

static void apply_giveup(struct module *m, const char *why)
{
	m->gave_up = 1;
	m->delay_ns = 0;
	set_state(m, ST_FAILED, why);
	if (m->mf && m->mf->giveup == MF_GIVEUP_FAILSAFE) {
		char r[128];
		mf_fmt(r, sizeof r, "%s given up", m->name);
		set_sys_state(SYS_FAILSAFE, r);
	} else {
		recompute_sys_state();
	}
}

void schedule_restart(struct module *m, const char *why)
{
	const struct mf_module *mf = m->mf;
	uint64_t now = hg_now_ns();
	m->failures++;
	if (m->n_fail_times == HD_MAX_FAILS) {
		memmove(m->fail_times, m->fail_times + 1, (HD_MAX_FAILS - 1) * sizeof m->fail_times[0]);
		m->n_fail_times--;
	}
	m->fail_times[m->n_fail_times++] = now;
	unsigned in_period = 0;
	for (int i = 0; i < m->n_fail_times; i++)
		if (now - m->fail_times[i] <= mf->burst_period_ns)
			in_period++;
	if (in_period > mf->burst_count) {
		char r[160];
		mf_fmt(r, sizeof r, "%u failures within %llu s: given up (%s)", in_period,
			 (unsigned long long)(mf->burst_period_ns / 1000000000u), why);
		apply_giveup(m, r);
		return;
	}
	m->delay_ns = m->delay_ns ? m->delay_ns * 2 : mf->backoff_first_ns;
	if (m->delay_ns > mf->backoff_max_ns)
		m->delay_ns = mf->backoff_max_ns;
	m->restart_at = now + m->delay_ns;
	char r[160];
	mf_fmt(r, sizeof r, "%s; restarting in %g s (failure %u)", why, (double)m->delay_ns / 1e9, m->failures);
	set_state(m, ST_BACKOFF, r);
}

void module_stop(struct module *m, enum state next, int count_failure, const char *why)
{
	if (!m->pid) {
		if (next == ST_BACKOFF)
			schedule_restart(m, why);
		else if (next == ST_STARTING)
			module_start(m);
		else
			set_state(m, next, why);
		return;
	}
	m->after_stop = next;
	m->count_failure = count_failure;
	m->stop_deadline = hg_now_ns() + (m->mf ? m->mf->stop_timeout_ns : 5000000000ull);
	if (m->st != ST_UNHEALTHY)
		set_state(m, ST_STOPPING, why);
	module_signal_group(m, SIGTERM);
}

void on_module_exit(struct module *m)
{
	siginfo_t si;
	memset(&si, 0, sizeof si);
	int known = 0;
	if (!m->adopted) {
		if (syscall(SYS_waitid, HD_P_PIDFD, m->pidfd, &si, WEXITED | WNOWAIT, NULL) < 0) {
			if (errno == EAGAIN || errno == EINTR)
				return;
		} else {
			known = 1;
		}
	}
	/* Before anything else: the hardware to neutral. */
	if (m->mf && m->mf->has_neutral)
		neutral_run(m->name);
	/* Its readers learn that the writer died. */
	topics_mark_writer_died(m->name);
	/* Anything left in its cgroup or group; the unreaped leader keeps the
	 * group id from being reused. */
	module_kill_group(m);
	if (!m->adopted) {
		siginfo_t si2;
		while (syscall(SYS_waitid, HD_P_PIDFD, m->pidfd, &si2, WEXITED, NULL) < 0 && errno == EINTR)
			;
	}
	char why[128];
	if (!known) {
		m->exit_known = 0;
		mf_fmt(why, sizeof why, "pid %d exited, status unknown (adopted)", (int)m->pid);
	} else if (si.si_code == CLD_EXITED) {
		m->exit_known = 1;
		m->exit_status = si.si_status;
		mf_fmt(why, sizeof why, "pid %d exited with status %d", (int)m->pid, si.si_status);
	} else {
		m->exit_known = 1;
		m->exit_status = -si.si_status;
		mf_fmt(why, sizeof why, "pid %d killed by signal %d (%s)", (int)m->pid, si.si_status,
			 strsignal(si.si_status));
	}
	hd_log("%s: %s", m->name, why);
	epoll_del(m->pidfd);
	close(m->pidfd);
	m->pidfd = -1;
	m->pid = 0;
	int was_stopping = m->stop_deadline != 0;
	m->stop_deadline = 0;
	module_outputs_unmap(m);

	if (G.shutting_down) {
		set_state(m, ST_STOPPED, why);
		return;
	}
	if (!m->mf) {
		set_state(m, ST_STOPPED, why);
		G.need_prune = 1;
		return;
	}
	if (was_stopping) {
		switch (m->after_stop) {
		case ST_BACKOFF:
			if (m->count_failure)
				schedule_restart(m, m->why[0] ? m->why : why);
			else
				module_start(m);
			break;
		case ST_STARTING:
			module_start(m); /* a restart on request: no failure counted */
			break;
		default:
			set_state(m, m->after_stop, why);
			break;
		}
		return;
	}
	/* An exit of its own. */
	if (known && si.si_code == CLD_EXITED && si.si_status == 78) {
		char r[256];
		mf_fmt(r, sizeof r, "exit status 78: refused its configuration: %s", m->page ? (char *)m->page + 64 : "");
		apply_giveup(m, r);
		return;
	}
	if (m->mf->policy == MF_RESTART_NEVER ||
	    (m->mf->policy == MF_RESTART_ON_FAILURE && known && si.si_code == CLD_EXITED && si.si_status == 0)) {
		set_state(m, ST_STOPPED, why);
		return;
	}
	schedule_restart(m, why);
}

int health_tick_needed(void)
{
	for (int i = 0; i < G.n_modules; i++) {
		const struct module *m = &G.modules[i];
		if (!m->pid || !m->mf)
			continue;
		if (m->st == ST_STARTING)
			return 1;
		if (m->mf->watchdog_ns)
			return 1;
		for (int k = 0; k < m->n_outputs; k++)
			if (m->outputs[k].rate_mhz)
				return 1;
	}
	return 0;
}

uint64_t health_next_deadline(void)
{
	uint64_t at = 0;
	for (int i = 0; i < G.n_modules; i++) {
		const struct module *m = &G.modules[i];
		if (m->st == ST_BACKOFF && (!at || m->restart_at < at))
			at = m->restart_at;
		if (m->stop_deadline && (!at || m->stop_deadline < at))
			at = m->stop_deadline;
	}
	return at;
}

void health_tick(uint64_t now)
{
	for (int i = 0; i < G.n_modules; i++) {
		struct module *m = &G.modules[i];
		const struct mf_module *mf = m->mf;
		if (m->st == ST_BACKOFF && !G.shutting_down && now >= m->restart_at) {
			module_start(m);
			continue;
		}
		if (m->pid && m->stop_deadline && now >= m->stop_deadline) {
			hd_log("%s: still running %g s after SIGTERM: SIGKILL to its %s", m->name,
			       (double)(mf ? mf->stop_timeout_ns : 0) / 1e9,
			       G.cgroup_base[0] && G.is_root ? "cgroup" : "process group");
			module_kill_group(m);
			m->stop_deadline = 0;
			continue;
		}
		if (!m->pid || !mf || m->stop_deadline || m->st == ST_UNHEALTHY || mf->legacy)
			continue;
		if (m->st == ST_STARTING) {
			if (module_is_ready(m)) {
				set_state(m, ST_READY, "");
			} else if (now - m->started_ns > mf->start_grace_ns) {
				char why[128];
				mf_fmt(why, sizeof why, "not ready %g s after start", (double)mf->start_grace_ns / 1e9);
				set_state(m, ST_UNHEALTHY, why);
				module_stop(m, ST_BACKOFF, 1, why);
			}
			continue;
		}
		if (m->st != ST_READY)
			continue;
		/* Staleness, from each rated topic's own header. */
		for (int k = 0; k < m->n_outputs; k++) {
			struct output *o = &m->outputs[k];
			if (!o->rate_mhz || !output_current(m, o))
				continue;
			uint64_t last = hdr_u64(o->hdr, OFF_LAST_PUBLISH);
			uint64_t stale_after = hdr_u64(o->hdr, OFF_STALE_AFTER);
			if (!last || !stale_after)
				continue;
			if (now > last && now - last > stale_after + mf->stale_grace_ns) {
				char why[128];
				mf_fmt(why, sizeof why, "%s stale for %llu ms", o->topic,
					 (unsigned long long)((now - last - stale_after) / 1000000u));
				set_state(m, ST_UNHEALTHY, why);
				module_stop(m, ST_BACKOFF, 1, why);
				break;
			}
		}
		if (m->st != ST_READY)
			continue;
		if (mf->watchdog_ns) {
			uint64_t alive = page_alive(m);
			uint64_t base = alive > m->ready_ns ? alive : m->ready_ns;
			if (now > base && now - base > mf->watchdog_ns) {
				char why[128];
				mf_fmt(why, sizeof why, "watchdog: no hg_alive() for %llu ms",
					 (unsigned long long)((now - base) / 1000000u));
				set_state(m, ST_UNHEALTHY, why);
				module_stop(m, ST_BACKOFF, 1, why);
				continue;
			}
		}
		/* Healthy for ResetAfter: the delay and the count start over. */
		if ((m->failures || m->delay_ns) && now - m->ready_ns >= mf->reset_after_ns) {
			m->failures = 0;
			m->delay_ns = 0;
			m->n_fail_times = 0;
			hd_log("%s: healthy for %g s: failure count reset", m->name, (double)mf->reset_after_ns / 1e9);
			registry_write();
			request_publish();
		}
	}
}
