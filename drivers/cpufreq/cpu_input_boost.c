// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018 Sultan Alsawaf <sultan@kerneltoast.com>.
 */
// Copyright (C) 2019 GPU input boost, Erik Müller <pappschlumpf@xda>

#define pr_fmt(fmt) "cpu_input_boost: " fmt

#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/msm_drm_notify.h>
#include <linux/input.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/cpu_input_boost.h>
#include <linux/kthread.h>
#include "../gpu/msm/kgsl.h"
#include "../gpu/msm/kgsl_pwrscale.h"
#include "../gpu/msm/kgsl_device.h"

static __read_mostly unsigned int input_boost_freq_lp = CONFIG_INPUT_BOOST_FREQ_LP;
static __read_mostly unsigned int input_boost_freq_hp = CONFIG_INPUT_BOOST_FREQ_PERF;
static __read_mostly unsigned short input_boost_duration = CONFIG_INPUT_BOOST_DURATION_MS;
static __read_mostly unsigned int remove_input_boost_freq_lp = CONFIG_REMOVE_INPUT_BOOST_FREQ_LP;
static __read_mostly unsigned int remove_input_boost_freq_perf = CONFIG_REMOVE_INPUT_BOOST_FREQ_PERF;
static __read_mostly unsigned short flex_boost_duration = CONFIG_FLEX_BOOST_DURATION_MS;
static __read_mostly unsigned int flex_boost_freq_lp = CONFIG_FLEX_BOOST_FREQ_LP;
static __read_mostly unsigned int flex_boost_freq_hp = CONFIG_FLEX_BOOST_FREQ_PERF;
static __read_mostly unsigned int gpu_boost_freq = CONFIG_GPU_BOOST_FREQ;
static __read_mostly unsigned int gpu_min_freq = CONFIG_GPU_MIN_FREQ;
static __read_mostly unsigned int gpu_boost_extender_ms = CONFIG_GPU_BOOST_EXTENDER_MS;
static __read_mostly unsigned int input_thread_prio = CONFIG_INPUT_THREAD_PRIORITY;

#ifdef CONFIG_DYNAMIC_STUNE_BOOST
static __read_mostly short dynamic_stune_boost=20;
static __read_mostly short input_stune_boost_offset = CONFIG_INPUT_BOOST_STUNE_OFFSET;
static __read_mostly short max_stune_boost_offset = CONFIG_MAX_BOOST_STUNE_OFFSET;
static __read_mostly short flex_stune_boost_offset = CONFIG_FLEX_BOOST_STUNE_OFFSET;
static __read_mostly unsigned short stune_boost_extender_ms = CONFIG_STUNE_BOOST_EXTENDER_MS;
static __read_mostly int suspend_stune_boost = CONFIG_SUSPEND_STUNE_BOOST;

module_param(dynamic_stune_boost, short, 0644);
module_param(input_stune_boost_offset, short, 0644);
module_param(max_stune_boost_offset, short, 0644);
module_param(flex_stune_boost_offset, short, 0644);
module_param(stune_boost_extender_ms, short, 0644);
module_param(gpu_boost_freq, uint, 0644);
module_param(gpu_min_freq, uint, 0644);
module_param(gpu_boost_extender_ms, uint, 0644);
#endif

module_param(input_boost_freq_lp, uint, 0644);
module_param(input_boost_freq_hp, uint, 0644);
module_param(flex_boost_freq_lp, uint, 0644);
module_param(flex_boost_freq_hp, uint, 0644);
module_param(input_boost_duration, short, 0644);
module_param(flex_boost_duration, short, 0644);
module_param(remove_input_boost_freq_lp, uint, 0644);
module_param(remove_input_boost_freq_perf, uint, 0644);
module_param(suspend_stune_boost, int, 0644);

/* Available bits for boost_drv state */
#define SCREEN_AWAKE		BIT(0)
#define INPUT_BOOST		BIT(1)
#define WAKE_BOOST		BIT(2)
#define MAX_BOOST		BIT(3)
#define FLEX_BOOST		BIT(4)
#define INPUT_STUNE_BOOST	BIT(5)
#define MAX_STUNE_BOOST		BIT(6)
#define FLEX_STUNE_BOOST	BIT(7)
#define INPUT_GPU_BOOST		BIT(8)

struct boost_drv {
	struct kthread_worker worker;
	struct task_struct *worker_thread;
	struct kthread_work input_boost;
	struct delayed_work input_unboost;
	struct kthread_work max_boost;
	struct delayed_work max_unboost;
	struct kthread_work flex_boost;
	struct delayed_work flex_unboost;
	struct delayed_work stune_extender_unboost;
	struct delayed_work gpu_extender_unboost;
	struct notifier_block cpu_notif;
	struct notifier_block msm_drm_notif;
	struct kgsl_device *gpu_device;
	struct kgsl_pwrctrl *gpu_pwr;
	atomic64_t max_boost_expires;
	atomic_t max_boost_dur;
	atomic64_t flex_boost_expires;
	atomic_t flex_boost_dur;
	atomic_t state;
	int input_stune_slot;
	int max_stune_slot;
	int root_stune_default;
	int flex_stune_slot;
	int cpu;
};

static struct boost_drv *boost_drv_g __read_mostly;

static u32 get_boost_freq(struct boost_drv *b, u32 cpu, u32 state)
{
	if (state & INPUT_BOOST) {
		if (cpumask_test_cpu(cpu, cpu_lp_mask))
			return input_boost_freq_lp;

		return input_boost_freq_hp;
	}

	if (state & FLEX_BOOST) {
		
		if (cpumask_test_cpu(cpu, cpu_lp_mask))
			return flex_boost_freq_lp;

		return flex_boost_freq_hp;
	}

	if (cpumask_test_cpu(cpu, cpu_lp_mask))
		return flex_boost_freq_lp;

	return flex_boost_freq_hp;
}

static u32 get_min_freq(struct boost_drv *b, u32 cpu)
{
	if (cpumask_test_cpu(cpu, cpu_lp_mask))
		return remove_input_boost_freq_lp;

	return remove_input_boost_freq_perf;
}

static u32 get_boost_state(struct boost_drv *b)
{
	return atomic_read(&b->state);
}

static void set_boost_bit(struct boost_drv *b, u32 state)
{
	atomic_or(state, &b->state);
}

static void clear_boost_bit(struct boost_drv *b, u32 state)
{
	atomic_andnot(state, &b->state);
}

static void update_online_cpu_policy(void)
{
	u32 cpu;

	/* Only one CPU from each cluster needs to be updated */
	get_online_cpus();
	cpu = cpumask_first_and(cpu_lp_mask, cpu_online_mask);
	cpufreq_update_policy(cpu);
	cpu = cpumask_first_and(cpu_perf_mask, cpu_online_mask);
	cpufreq_update_policy(cpu);
	put_online_cpus();
}

static void update_stune_boost(struct boost_drv *b, u32 state, u32 bit, int level,
			    int *slot)
{
	if (level && !(state & bit)) {
		if (!do_stune_boost("top-app", level, slot))
			set_boost_bit(b, bit);
	}
}

static void clear_stune_boost(struct boost_drv *b, u32 state, u32 bit, int slot)
{
	if (state & bit) {
		reset_stune_boost("top-app", slot);
		clear_boost_bit(b, bit);
	}
}

static void update_gpu_boost(struct boost_drv *b, u32 state, u32 bit, int freq)
{
	int level;
	if (freq==0) return;
	if (freq && !(state & bit)) {
		if (freq>=342)
			level=6;
		if (freq==257)
			level=7;
		mutex_lock(&b->gpu_device->mutex);
		b->gpu_pwr->min_pwrlevel=level;
		mutex_unlock(&b->gpu_device->mutex);
		set_boost_bit(b, bit);
	}
}

static void clear_gpu_boost(struct boost_drv *b, u32 state, u32 bit, int freq)
{
	int level;
	if (state & bit) {
		if (freq==342)
			level=6;
		if (freq==257)
			level=7;
		if (freq==180)
			level=8;
		mutex_lock(&b->gpu_device->mutex);
		b->gpu_pwr->min_pwrlevel=level;
		mutex_unlock(&b->gpu_device->mutex);
		clear_boost_bit(b, bit);
	}
}

static void unboost_all_cpus(struct boost_drv *b)
{
	u32 state = get_boost_state(b);

	if (!cancel_delayed_work_sync(&b->input_unboost) &&
		!cancel_delayed_work_sync(&b->flex_unboost) &&
		!cancel_delayed_work_sync(&b->max_unboost))
		return;

	clear_boost_bit(b, INPUT_BOOST | WAKE_BOOST | MAX_BOOST | FLEX_BOOST);
	update_online_cpu_policy();

	clear_stune_boost(b, state, INPUT_STUNE_BOOST, b->input_stune_slot);
	clear_stune_boost(b, state, MAX_STUNE_BOOST, b->max_stune_slot);
	clear_gpu_boost(b, state, INPUT_GPU_BOOST, gpu_min_freq);
}

static void __cpu_input_boost_kick(struct boost_drv *b)
{
	if (!(get_boost_state(b) & SCREEN_AWAKE))
		return;

	kthread_queue_work(&b->worker, &b->input_boost);
}

void cpu_input_boost_kick(void)
{
	struct boost_drv *b = boost_drv_g;

	if (!b)
		return;

	__cpu_input_boost_kick(b);
}

static void __cpu_input_boost_kick_max(struct boost_drv *b,
	unsigned int duration_ms, unsigned int cpu)
{
	unsigned long curr_expires, new_expires;

	if (!(get_boost_state(b) & SCREEN_AWAKE))
		return;

	do {
		if (cpu < 4) 
			b->cpu = 0;
		else
			b->cpu = 4; 			
		curr_expires = atomic64_read(&b->max_boost_expires);
		new_expires = jiffies + msecs_to_jiffies(duration_ms);

		/* Skip this boost if there's a longer boost in effect */
		if (time_after(curr_expires, new_expires))
			return;
	} while (atomic64_cmpxchg(&b->max_boost_expires, curr_expires,
		new_expires) != curr_expires);

	atomic_set(&b->max_boost_dur, duration_ms);
	kthread_queue_work(&b->worker, &b->max_boost);
}

void cpu_input_boost_kick_max(unsigned int duration_ms)
{
	struct boost_drv *b = boost_drv_g;

	if (!b)
		return;
	
	__cpu_input_boost_kick_max(b, duration_ms, 0);
}

void cluster_input_boost_kick_max(unsigned int duration_ms, int cpu)
{
	struct boost_drv *b = boost_drv_g;

	if (!b)
		return;
	
	__cpu_input_boost_kick_max(b, duration_ms, cpu);
}

static void __cpu_input_boost_kick_flex(struct boost_drv *b)
{
	unsigned long curr_expires, new_expires;

	do {
		curr_expires = atomic64_read(&b->flex_boost_expires);
		new_expires = jiffies + msecs_to_jiffies(flex_boost_duration);

		/* Skip this boost if there's a longer boost in effect */
		if (time_after(curr_expires, new_expires))
			return;
	} while (atomic64_cmpxchg(&b->flex_boost_expires, curr_expires,
		new_expires) != curr_expires);

	atomic_set(&b->flex_boost_dur, flex_boost_duration);
	kthread_queue_work(&b->worker, &b->flex_boost);
}

void cpu_input_boost_kick_flex(void)
{
	struct boost_drv *b = boost_drv_g;
	u32 state;

	if (!b)
		return;

	state = get_boost_state(b);

	if (!(state & SCREEN_AWAKE))
		return;

	__cpu_input_boost_kick_flex(b);
}

static void input_boost_worker(struct kthread_work *work)
{
	struct boost_drv *b = container_of(work, typeof(*b), input_boost);
	u32 state = get_boost_state(b);

	if (!cancel_delayed_work_sync(&b->input_unboost)) {
		set_boost_bit(b, INPUT_BOOST);
		update_online_cpu_policy();

		update_stune_boost(b, state, INPUT_STUNE_BOOST, dynamic_stune_boost+input_stune_boost_offset,
			&b->input_stune_slot);		

		update_gpu_boost(b, state, INPUT_GPU_BOOST, gpu_boost_freq);	
	}

	queue_delayed_work(system_power_efficient_wq, &b->input_unboost,
		msecs_to_jiffies(input_boost_duration));
	
}

static void input_unboost_worker(struct work_struct *work)
{
	struct boost_drv *b = container_of(to_delayed_work(work),
					   typeof(*b), input_unboost);

	clear_boost_bit(b, INPUT_BOOST);
	update_online_cpu_policy();

	cancel_delayed_work_sync(&b->stune_extender_unboost);
	queue_delayed_work(system_power_efficient_wq, &b->stune_extender_unboost,
		msecs_to_jiffies(stune_boost_extender_ms));

	cancel_delayed_work_sync(&b->gpu_extender_unboost);
	queue_delayed_work(system_power_efficient_wq, &b->gpu_extender_unboost,
		msecs_to_jiffies(gpu_boost_extender_ms));
}

static void max_boost_worker(struct kthread_work *work)
{
	struct boost_drv *b = container_of(work, typeof(*b), max_boost);
	u32 state = get_boost_state(b);

	if (!cancel_delayed_work_sync(&b->max_unboost)) {
		set_boost_bit(b, MAX_BOOST);
		update_online_cpu_policy();
	
		update_stune_boost(b, state, MAX_STUNE_BOOST, dynamic_stune_boost+max_stune_boost_offset,
			&b->max_stune_slot);
	}

	queue_delayed_work(system_power_efficient_wq, &b->max_unboost,
		msecs_to_jiffies(atomic_read(&b->max_boost_dur)));
}

static void max_unboost_worker(struct work_struct *work)
{
	struct boost_drv *b = container_of(to_delayed_work(work),
					   typeof(*b), max_unboost);
	u32 state = get_boost_state(b);

	clear_boost_bit(b, WAKE_BOOST | MAX_BOOST);
	update_online_cpu_policy();

	clear_stune_boost(b, state, MAX_STUNE_BOOST, b->max_stune_slot);
}

static void flex_boost_worker(struct kthread_work *work)
{
	struct boost_drv *b = container_of(work, typeof(*b), flex_boost);

	u32 state = get_boost_state(b);
	
	if (flex_boost_duration==0) 
		return;
	
	if (!cancel_delayed_work_sync(&b->flex_unboost)) {
		set_boost_bit(b, FLEX_BOOST);
		update_online_cpu_policy();

		if (!(state & MAX_STUNE_BOOST) && !(state & INPUT_STUNE_BOOST) ) {
			update_stune_boost(b, state, FLEX_STUNE_BOOST, dynamic_stune_boost+flex_stune_boost_offset,
				&b->flex_stune_slot);	
		}
	}

	queue_delayed_work(system_power_efficient_wq, &b->flex_unboost,
		msecs_to_jiffies(atomic_read(&b->flex_boost_dur)));
}

static void flex_unboost_worker(struct work_struct *work)
{
	struct boost_drv *b =
		container_of(to_delayed_work(work), typeof(*b), flex_unboost);
	u32 state = get_boost_state(b);

	clear_boost_bit(b, FLEX_BOOST);
	update_online_cpu_policy();

	clear_stune_boost(b, state, FLEX_STUNE_BOOST, b->flex_stune_slot);
}

static void stune_extender_unboost_worker(struct work_struct *work)
{
	struct boost_drv *b =
		container_of(to_delayed_work(work), typeof(*b), stune_extender_unboost);
	u32 state = get_boost_state(b);

#ifdef CONFIG_DYNAMIC_STUNE_BOOST
	clear_stune_boost(b, state, INPUT_STUNE_BOOST, b->input_stune_slot);
#endif
	
}

static void gpu_extender_unboost_worker(struct work_struct *work)
{
	struct boost_drv *b =
		container_of(to_delayed_work(work), typeof(*b), gpu_extender_unboost);
	u32 state = get_boost_state(b);

	clear_gpu_boost(b, state, INPUT_GPU_BOOST, gpu_min_freq);	
}

static int cpu_notifier_cb(struct notifier_block *nb,
			   unsigned long action, void *data)
{
	struct boost_drv *b = container_of(nb, typeof(*b), cpu_notif);
	struct cpufreq_policy *policy = data;
	u32 boost_freq, min_freq, state;

	if (action != CPUFREQ_ADJUST)
		return NOTIFY_OK;

	state = get_boost_state(b);

	/* Boost CPU to max frequency for max boost */
	if (state & MAX_BOOST) {
		if (b->cpu == policy->cpu) {
			policy->min = policy->max;
			b->cpu = 9;
			return NOTIFY_OK;
		}
	}

	/*
	 * Boost to policy->max if the boost frequency is higher. When
	 * unboosting, set policy->min to the absolute min freq for the CPU.
	 */
	if (state & INPUT_BOOST ||state & FLEX_BOOST) {
		boost_freq = get_boost_freq(b, policy->cpu, state);
		policy->min = min(policy->max, boost_freq);
	} else {
		min_freq = get_min_freq(b, policy->cpu);
		policy->min = max(policy->cpuinfo.min_freq, min_freq);
	}

	return NOTIFY_OK;
}

static int msm_drm_notifier_cb(struct notifier_block *nb,
	unsigned long event, void *data)
{
	struct boost_drv *b = container_of(nb, typeof(*b), msm_drm_notif);
	struct msm_drm_notifier *evdata = data;
	int blank;

	blank = *(int *)(evdata->data);	

	/* Parse framebuffer blank events as soon as they occur */
	if (event != MSM_DRM_EARLY_EVENT_BLANK)
		return NOTIFY_OK;

	/* Boost when the screen turns on and unboost when it turns off */
	if (blank == MSM_DRM_BLANK_UNBLANK_CUST) {
		set_boost_bit(b, SCREEN_AWAKE);
		set_stune_boost("/", b->root_stune_default, NULL);
		__cpu_input_boost_kick_max(b, CONFIG_WAKE_BOOST_DURATION_MS, 0);
		
	} else {
		clear_boost_bit(b, SCREEN_AWAKE);
		unboost_all_cpus(b);
		set_stune_boost("/", suspend_stune_boost,
			&b->root_stune_default);
	}

	return NOTIFY_OK;
}

static void cpu_input_boost_input_event(struct input_handle *handle,
					unsigned int type, unsigned int code,
					int value)
{
	struct boost_drv *b = handle->handler->private;

	__cpu_input_boost_kick(b);
}

static int cpu_input_boost_input_connect(struct input_handler *handler,
					 struct input_dev *dev,
					 const struct input_device_id *id)
{
	struct input_handle *handle;
	int ret;

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "cpu_input_boost_handle";

	ret = input_register_handle(handle);
	if (ret)
		goto free_handle;

	ret = input_open_device(handle);
	if (ret)
		goto unregister_handle;

	return 0;

unregister_handle:
	input_unregister_handle(handle);
free_handle:
	kfree(handle);
	return ret;
}

static void cpu_input_boost_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id cpu_input_boost_ids[] = {
	/* Multi-touch touchscreen */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		.absbit = { [BIT_WORD(ABS_MT_POSITION_X)] =
			BIT_MASK(ABS_MT_POSITION_X) |
			BIT_MASK(ABS_MT_POSITION_Y) }
	},
	/* Touchpad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_KEYBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.keybit = { [BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH) },
		.absbit = { [BIT_WORD(ABS_X)] =
			BIT_MASK(ABS_X) | BIT_MASK(ABS_Y) }
	},
	/* Keypad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_KEY) }
	},
	{ }
};

static struct input_handler cpu_input_boost_input_handler = {
	.event		= cpu_input_boost_input_event,
	.connect	= cpu_input_boost_input_connect,
	.disconnect	= cpu_input_boost_input_disconnect,
	.name		= "cpu_input_boost_handler",
	.id_table	= cpu_input_boost_ids
};

static int __init cpu_input_boost_init(void)
{
	struct boost_drv *b;
	int ret, c;
	struct sched_param param = { .sched_priority = input_thread_prio};
	cpumask_t sys_bg_mask;

	b = kzalloc(sizeof(*b), GFP_KERNEL);
	if (!b)
		return -ENOMEM;

	kthread_init_worker(&b->worker);
	b->worker_thread = kthread_run(kthread_worker_fn, &b->worker,
				       "cpu_input_boost_thread");
	if (IS_ERR(b->worker_thread)) {
		ret = PTR_ERR(b->worker_thread);
		pr_err("Failed to start kworker, err: %d\n", ret);
		goto free_b;
	}

	ret = sched_setscheduler(b->worker_thread, SCHED_FIFO, &param);
	if (ret)
		pr_err("Failed to set SCHED_FIFO on kworker, err: %d\n", ret);

	for (c = 0; c < 4; c++) 
		cpumask_set_cpu(c, &sys_bg_mask);

	/* Bind it to the cpumask */
	kthread_bind_mask(b->worker_thread, &sys_bg_mask);

	/* Wake it up */
	wake_up_process(b->worker_thread);

	atomic64_set(&b->max_boost_expires, 0);
	kthread_init_work(&b->input_boost, input_boost_worker);
	INIT_DELAYED_WORK(&b->input_unboost, input_unboost_worker);
	kthread_init_work(&b->max_boost, max_boost_worker);
	INIT_DELAYED_WORK(&b->max_unboost, max_unboost_worker);
	kthread_init_work(&b->flex_boost, flex_boost_worker);
	INIT_DELAYED_WORK(&b->flex_unboost, flex_unboost_worker);
	INIT_DELAYED_WORK(&b->stune_extender_unboost, stune_extender_unboost_worker);
	INIT_DELAYED_WORK(&b->gpu_extender_unboost, gpu_extender_unboost_worker);
	atomic_set(&b->state, 0);
	b->root_stune_default = 0;

	b->cpu_notif.notifier_call = cpu_notifier_cb;
	b->cpu_notif.priority = INT_MAX - 2;
	ret = cpufreq_register_notifier(&b->cpu_notif, CPUFREQ_POLICY_NOTIFIER);
	if (ret) {
		pr_err("Failed to register cpufreq notifier, err: %d\n", ret);
		goto destroy_wq;
	}

	cpu_input_boost_input_handler.private = b;
	ret = input_register_handler(&cpu_input_boost_input_handler);
	if (ret) {
		pr_err("Failed to register input handler, err: %d\n", ret);
		goto unregister_cpu_notif;
	}

	b->msm_drm_notif.notifier_call = msm_drm_notifier_cb;
	b->msm_drm_notif.priority = INT_MAX;
	ret = msm_drm_register_client(&b->msm_drm_notif);
	if (ret) {
		pr_err("Failed to register msm_drm_notifier, err: %d\n", ret);
		goto unregister_handler;
	}

	/* Allow global boost config access for external boosts */
	boost_drv_g = b;
	set_boost_bit(b, SCREEN_AWAKE);
	b->gpu_device = kgsl_get_device(KGSL_DEVICE_3D0);
	if (IS_ERR_OR_NULL(b->gpu_device))
		return 0;
	b->gpu_pwr = &b->gpu_device->pwrctrl;
	
	return 0;

unregister_handler:
	input_unregister_handler(&cpu_input_boost_input_handler);
unregister_cpu_notif:
	cpufreq_unregister_notifier(&b->cpu_notif, CPUFREQ_POLICY_NOTIFIER);
destroy_wq:
	kthread_destroy_worker(&b->worker);
free_b:
	kfree(b);
	return ret;
}
late_initcall(cpu_input_boost_init);
