/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __ARM64_KVM_PKVM_MODULE_H__
#define __ARM64_KVM_PKVM_MODULE_H__

#include <asm/kvm_pgtable.h>
#include <linux/export.h>

typedef void (*dyn_hcall_t)(struct user_pt_regs *);

#ifdef CONFIG_MODULES
enum pkvm_psci_notification {
	PKVM_PSCI_CPU_SUSPEND,
	PKVM_PSCI_SYSTEM_SUSPEND,
	PKVM_PSCI_CPU_ENTRY,
};

struct pkvm_module_ops {
	int (*create_private_mapping)(phys_addr_t phys, size_t size,
				      enum kvm_pgtable_prot prot,
				      unsigned long *haddr);
	int (*register_serial_driver)(void (*hyp_putc_cb)(char));
	void (*putc)(char c);
	void (*puts)(const char *s);
	void (*putx64)(u64 x);
	void *(*fixmap_map)(phys_addr_t phys);
	void (*fixmap_unmap)(void);
	void *(*linear_map_early)(phys_addr_t phys, size_t size, enum kvm_pgtable_prot prot);
	void (*linear_unmap_early)(void *addr, size_t size);
	void (*flush_dcache_to_poc)(void *addr, size_t size);
	int (*register_host_perm_fault_handler)(int (*cb)(struct user_pt_regs *regs, u64 esr, u64 addr));
	int (*protect_host_page)(u64 pfn, enum kvm_pgtable_prot prot);
	int (*register_host_smc_handler)(bool (*cb)(struct user_pt_regs *));
	int (*register_default_trap_handler)(bool (*cb)(struct user_pt_regs *));
	int (*register_illegal_abt_notifier)(void (*cb)(struct user_pt_regs *));
	int (*register_psci_notifier)(void (*cb)(enum pkvm_psci_notification, struct user_pt_regs *));
	int (*register_hyp_panic_notifier)(void (*cb)(struct user_pt_regs *));
	int (*host_donate_hyp)(u64 pfn, u64 nr_pages);
	int (*hyp_donate_host)(u64 pfn, u64 nr_pages);
	void* (*memcpy)(void *to, const void *from, size_t count);
	void* (*memset)(void *dst, int c, size_t count);
	phys_addr_t (*hyp_pa)(void *x);
	unsigned long (*kern_hyp_va)(unsigned long x);
};

int __pkvm_load_el2_module(struct module *this, unsigned long *token);

int __pkvm_register_el2_call(unsigned long hfn_hyp_va);
#else
static inline int __pkvm_load_el2_module(struct module *this,
					 unsigned long *token)
{
	return -ENOSYS;
}

static inline int __pkvm_register_el2_call(unsigned long hfn_hyp_va)
{
	return -ENOSYS;
}
#endif /* CONFIG_MODULES */

#ifdef MODULE
/*
 * Convert an EL2 module addr from the kernel VA to the hyp VA
 */
#define pkvm_el2_mod_va(kern_va, token)					\
({									\
	unsigned long hyp_text_kern_va =				\
		(unsigned long)THIS_MODULE->arch.hyp.text.start;	\
	unsigned long offset;						\
									\
	offset = (unsigned long)kern_va - hyp_text_kern_va;		\
	token + offset;							\
})

#define pkvm_load_el2_module(init_fn, token)				\
({									\
	THIS_MODULE->arch.hyp.init = init_fn;				\
	__pkvm_load_el2_module(THIS_MODULE, token);			\
})

#define pkvm_register_el2_mod_call(hfn, token)				\
({									\
	__pkvm_register_el2_call(pkvm_el2_mod_va(hfn, token));		\
})

#define pkvm_el2_mod_call(id, ...)					\
	({								\
		struct arm_smccc_res res;				\
									\
		arm_smccc_1_1_hvc(KVM_HOST_SMCCC_ID(id),		\
				  ##__VA_ARGS__, &res);			\
		WARN_ON(res.a0 != SMCCC_RET_SUCCESS);			\
									\
		res.a1;							\
	})
#endif
#endif
