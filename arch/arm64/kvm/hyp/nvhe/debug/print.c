// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2013-2014, ARM Limited and Contributors. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * Neither the name of ARM nor the names of its contributors may be used
 * to endorse or promote products derived from this software without specific
 * prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include <linux/kernel.h>
#include <hyp/hyp_print.h>
#include <hyp/hyp_debug.h>

#include <asm/kvm_mmu.h>
#define HYP_PL011_BASE_PHYS	CONFIG_KVM_ARM_HYP_DEBUG_UART_ADDR
#define HYP_PL011_UARTFR	0x18

#define HYP_PL011_UARTFR_BUSY	3
#define HYP_PL011_UARTFR_FULL	5
/* Choose max of 128 chars for now. */
#define PRINT_BUFFER_SIZE 128
static inline void *__hyp_pl011_base(void)
{
	unsigned long ioaddr;

	asm volatile(ALTERNATIVE_CB(
		"movz	%0, #0\n"
		"movk	%0, #0, lsl #16\n"
		"movk	%0, #0, lsl #32\n"
		"movk	%0, #0, lsl #48",
		kvm_hyp_debug_uart_set_basep)
		: "=r" (ioaddr));

	return *((void **)kern_hyp_va(ioaddr));
}

static inline unsigned int __hyp_readw(void *ioaddr)
{
	unsigned int val;

	asm volatile("ldr %w0, [%1]" : "=r" (val) : "r" (ioaddr));
	return val;
}

static inline void __hyp_writew(unsigned int val, void *ioaddr)
{
	asm volatile("str %w0, [%1]" : : "r" (val), "r" (ioaddr));
}

static inline void hyp_putc(char c)
{
	unsigned int val;
	void *base = __hyp_pl011_base();

	do {
		val = __hyp_readw(base + HYP_PL011_UARTFR);
	} while (val & (1U << HYP_PL011_UARTFR_FULL));

	__hyp_writew(c, base);

	do {
		val = __hyp_readw(base + HYP_PL011_UARTFR);
	} while (val & (1U << HYP_PL011_UARTFR_BUSY));
}

int hyp_print(const char *fmt, ...)
{
	va_list args;
	char buf[PRINT_BUFFER_SIZE];
	int count;

	va_start(args, fmt);
	hyp_vsnprintf(buf, sizeof(buf) - 1, fmt, args);
	va_end(args);

	/* Use putchar directly as 'puts()' adds a newline. */
	buf[PRINT_BUFFER_SIZE - 1] = '\0';
	count = 0;
	while (buf[count]) {
		hyp_putc(buf[count]);
		count++;
	}

	return count;
}

int hyp_snprint(char *s, size_t slen, const char *format, ...)
{
	int retval;
	va_list ap;

	va_start(ap, format);
	retval = hyp_vsnprintf(s, slen, format, ap);
	va_end(ap);
	return retval;
}

#ifdef CONFIG_KVM_ARM_HYP_DEBUG_HYP_CALLS
int hyp_dbg_print(const char *fmt, ...)
{
	va_list args;
	char buf[PRINT_BUFFER_SIZE];
	int count;
	int maxlen;

	if (dbg_buffer) {
		if (dbg_buffer->datalen >= dbg_buffer->size)
			return 0;
		maxlen = dbg_buffer->size - dbg_buffer->datalen;
		va_start(args, fmt);
		count = hyp_vsnprintf(&dbg_buffer->data[dbg_buffer->datalen], maxlen, fmt, args);
		va_end(args);
		dbg_buffer->datalen += count;
	} else {
		va_start(args, fmt);
		hyp_vsnprintf(buf, sizeof(buf) - 1, fmt, args);
		va_end(args);
		/* Use putchar directly as 'puts()' adds a newline. */
		buf[PRINT_BUFFER_SIZE - 1] = '\0';
		count = 0;
		while (buf[count]) {
			hyp_putc(buf[count]);
			count++;
		}
	}
	return count;
}
#endif
