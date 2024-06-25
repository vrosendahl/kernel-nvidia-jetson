/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __NVHE_RAMLOG_H
#define __NVHE_RAMLOG_H

#include <generated/autoconf.h>
#ifdef CONFIG_KVM_ARM_HYP_DEBUG_RAMLOG
#include <linux/kernel.h>
#include <linux/string.h>
#include <asm/page-def.h>
#include <hyp/hyp_print.h>
#include <nvhe/chacha.h>

#define LOG_ENTRY_LENGTH 64
#define RAMLOG_SIZE PAGE_SIZE

#define __hyp_read_reg(r)                                    \
	__extension__({                                        \
		uint64_t value;                                    \
		__asm__ __volatile__("mrs	%0, " #r               \
					 : "=r"(value));                       \
		value;                                             \
	})

#ifdef CONFIG_KVM_ARM_HYP_DEBUG_RAMLOG_DIRECT_PRINT
# define hyp_ramlog_ts(fmt, ...) do {				     \
		gettimestamp(&__hyp_ramlog.hts);		     \
		hyp_print("[rl %d.%ld] " fmt, __hyp_ramlog.hts.sec,  \
			  __hyp_ramlog.hts.nsec, __VA_ARGS__);	     \
		hyp_ramlog("[rl %d.%ld] " fmt, __hyp_ramlog.hts.sec, \
			   __hyp_ramlog.hts.nsec, __VA_ARGS__);	     \
} while (0)

#else // CONFIG_KVM_ARM_HYP_DEBUG_RAMLOG_DIRECT_PRINT

# define hyp_ramlog_ts(fmt, ...) do {				     \
		gettimestamp(&__hyp_ramlog.hts);		     \
		hyp_ramlog("[rl %d.%ld] " fmt, __hyp_ramlog.hts.sec, \
			   __hyp_ramlog.hts.nsec, __VA_ARGS__);	     \
} while (0)
#endif // CONFIG_KVM_ARM_HYP_DEBUG_RAMLOG_DIRECT_PRINT

#define hyp_ramlog_reg(reg) \
		hyp_ramlog_ts(#reg "\t- %016llx\n", __hyp_read_reg(reg))

struct hyp_timestamp {
	u64 sec;
	u64 nsec;
};

struct ramlog {
	char buf[RAMLOG_SIZE];
	int rp;
	struct hyp_timestamp hts;
};

extern struct ramlog __hyp_ramlog;

extern int hyp_vsnprintf(char *a, size_t b, const char *c, va_list d);

static inline u64 getcntpct_el0(void)
{
	u64 res;

	isb();
	asm volatile("mrs %0, cntpct_el0" : "=r" (res) :: "memory");
	return res;
}

static inline u64 getcntfrq_el0(void)
{
	u64 res;

	isb();
	asm volatile("mrs %0, cntfrq_el0" : "=r" (res) :: "memory");
	return res;
}

/*
 * poor implementation of the timer from cntpct_el0
 * could be not accurate in lowest orders
 */
static inline void gettimestamp(struct hyp_timestamp *hts)
{
	int i;
	u64 nsec = 0, rem = 0;
	u64 clks = getcntpct_el0();
	u64 freq = getcntfrq_el0();

	hts->sec = clks/freq;
	rem = clks % freq;
	for (i = 0; i < 10; i++) {
		freq /= 10;
		nsec += rem / freq;
		rem = rem % freq;
		nsec *= 10;
	}
	hts->nsec = nsec / 10;
}

static inline char *rlogp_head(void)
{
	return __hyp_ramlog.buf;
}

/* returns a pointer to 64-byte entry */
static inline char *rlogp_entry(int entry)
{
	return &__hyp_ramlog.buf[entry * LOG_ENTRY_LENGTH];
}

static inline int rlog_cur_entry(void)
{
	return __hyp_ramlog.rp / LOG_ENTRY_LENGTH;
}

void hyp_ramlog(const char *fmt, ...);

/**
 * The function decrypts, prints out and encrypts the ramlog back
 */
void print_rlog(void);

/**
 * The function being called by a hvc __hyp_dbg() call handler. Able to
 * just call print_rlog or copy an encrypted ramlog to shared with the host
 * buffer
 *
 * @dump defines will ramlog being printed or copied to the host for to dump
 */
void output_rlog(u64 dump);

#else /* CONFIG_KVM_ARM_HYP_DEBUG_RAMLOG */
#define hyp_ramlog_ts(...)
#define hyp_ramlog_reg(reg)
static inline char *rlogp_head(void) { return NULL; }
static inline char *rlogp_entry(int entry) { return NULL; }
static inline int   rlog_cur_entry(void) { return -1; }
static inline void hyp_ramlog(const char *fmt, ...) {}
static inline void print_rlog(void) {}
static inline void output_rlog(u64) {}
#endif /* CONFIG_KVM_ARM_HYP_DEBUG_RAMLOG */
#endif /* __NVHE_RAMLOG_H */
