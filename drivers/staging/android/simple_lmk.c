/*
 * Copyright (C) 2018, Sultan Alsawaf <sultanxda@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) "simple_lmk: " fmt

#include <linux/cpu_input_boost.h>
#include <linux/devfreq_boost.h>
#include <linux/mm.h>
#include <linux/moduleparam.h>
#include <linux/oom.h>
#include <linux/sched.h>

/* Free at least 64 MiB per low memory event */
#define MIN_FREE_PAGES (64UL * SZ_1M / PAGE_SIZE)

/* Time until LMK can be triggered again after reclaim is finished */
#define POST_KILL_TIMEOUT_MS (20)

/* Duration to boost CPU and DDR bus to the max per low memory event */
#define BOOST_DURATION_MS (100)

/* Pulled from the Android framework */
static const short int adj_prio[] = {
	906, /* CACHED_APP_MAX_ADJ */
	900, /* CACHED_APP_MIN_ADJ */
	800, /* SERVICE_B_ADJ */
	700, /* PREVIOUS_APP_ADJ */
	600, /* HOME_APP_ADJ */
	500, /* SERVICE_ADJ */
	400, /* HEAVY_WEIGHT_APP_ADJ */
	300  /* BACKUP_APP_ADJ */
};

static atomic_t reclaim_refcnt = ATOMIC_INIT(1);
static unsigned long last_reclaim_expires;

static bool test_task_flag(struct task_struct *p, int flag)
{
	struct task_struct *t;

	for_each_thread(p, t) {
		task_lock(t);
		if (test_tsk_thread_flag(t, flag)) {
			task_unlock(t);
			return true;
		}
		task_unlock(t);
	}

	return false;
}

static unsigned long scan_and_kill(int min_adj, int max_adj,
	unsigned long pages_needed)
{
	struct task_struct *tsk;
	unsigned long pages_freed = 0;

	rcu_read_lock();
	for_each_process(tsk) {
		struct task_struct *p;
		unsigned long tasksize;
		short oom_score_adj;

		if (tsk->flags & PF_KTHREAD)
			continue;

		/* Ignore tasks that no longer have any memory */
		if (test_task_flag(tsk, TIF_MM_RELEASED))
			continue;

		p = find_lock_task_mm(tsk);
		if (!p)
			continue;

		oom_score_adj = p->signal->oom_score_adj;
		if (oom_score_adj < min_adj || oom_score_adj > max_adj) {
			task_unlock(p);
			continue;
		}

		tasksize = get_mm_rss(p->mm);
		task_unlock(p);
		if (tasksize <= 0)
			continue;

		do_send_sig_info(SIGKILL, SEND_SIG_FORCED, p, true);

		pages_freed += tasksize;
		if (pages_freed >= pages_needed)
			break;
	}
	rcu_read_unlock();

	return pages_freed;
}

static void do_lmk_reclaim(unsigned long pages_needed)
{
	unsigned long pages_freed = 0;
	int i;

	for (i = 1; i < ARRAY_SIZE(adj_prio); i++) {
		pages_freed += scan_and_kill(adj_prio[i], adj_prio[i - 1],
					pages_needed - pages_freed);
		if (pages_freed >= pages_needed)
			break;
	}

	if (pages_freed)
		pr_info("freed %lu MiB\n", pages_freed * PAGE_SIZE / SZ_1M);
}

void simple_lmk_mem_reclaim(void)
{
	if (time_before(jiffies, last_reclaim_expires))
		return;

	/* Only one LMK event can happen at a time */
	if (!atomic_dec_and_test(&reclaim_refcnt)) {
		atomic_inc(&reclaim_refcnt);
		return;
	}

	last_reclaim_expires = jiffies + msecs_to_jiffies(POST_KILL_TIMEOUT_MS);

	cpu_input_boost_kick_max(BOOST_DURATION_MS);
	devfreq_boost_kick_max(DEVFREQ_MSM_CPUBW, BOOST_DURATION_MS);
	do_lmk_reclaim(MIN_FREE_PAGES);
	atomic_inc(&reclaim_refcnt);
}

/* Needed to prevent Android from thinking there's no LMK and thus rebooting */
#undef MODULE_PARAM_PREFIX
#define MODULE_PARAM_PREFIX "lowmemorykiller."
static int minfree_unused;
module_param_named(minfree, minfree_unused, int, 0200);
