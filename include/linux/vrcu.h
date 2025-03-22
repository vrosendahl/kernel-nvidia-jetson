#ifndef __VRCU_H__
#define __VRCU_H__

#include <linux/ktime.h>

void set_cpu_crazy(int cpu);

bool cpu_is_crazy(int cpu);

void viktor_init_crazy(void);

void print_vrcu_data(int cpu, const char *append);

void print_ref(void);

void vrcu_inc_loop_iter(int cpu);

void vrcu_irq_inc(void);

void vrcu_irq_dec(void);

void vrcu_idle_inc(void);

void vrcu_idle_dec(void);

void vrcu_debug_clock_irq(unsigned long j);

void vrcu_debug_stop_tick(long j, ktime_t expires);

void vrcu_debug_broadcast_expired(int value);

void vrcu_debug_send_broadcast(int cpu);

#endif
