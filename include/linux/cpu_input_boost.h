// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018 Sultan Alsawaf <sultan@kerneltoast.com>.
 */
#ifndef _CPU_INPUT_BOOST_H_
#define _CPU_INPUT_BOOST_H_

#ifdef CONFIG_CPU_INPUT_BOOST
extern unsigned long last_input_jiffies;
extern unsigned int kgsl_cpu;
static __read_mostly unsigned int kick_frame_boost_suspend_ms = CONFIG_KICK_FRAME_BOOST_SUSPEND_MS;
void cpu_input_boost_kick(void);
void cpu_input_boost_kick_max(unsigned int duration_ms);
void cluster_input_boost_kick_max(unsigned int duration_ms, int cpu);
void cpu_input_boost_kick_general(unsigned int duration_ms);
void cpu_input_boost_kick_flex(void);
static inline bool kick_stune_frame_boost(void)
{
	return time_before(jiffies,
			   last_input_jiffies + msecs_to_jiffies(kick_frame_boost_suspend_ms));
}
#else
static inline void cpu_input_boost_kick(void)
{
}
static inline void cpu_input_boost_kick_max(unsigned int duration_ms)
{
}
void core_input_boost_kick_max(unsigned int duration_ms, unsigned int cpu)
{
}
static inline void cpu_input_boost_kick_wake(void)
{
}
static inline void cpu_input_boost_kick_general(unsigned int duration_ms)
{
}
static inline void cpu_input_boost_kick_flex(void)
{
}

static inline bool should_kick_frame_boost(void)
{
	return false;
}
#endif

#endif /* _CPU_INPUT_BOOST_H_ */
