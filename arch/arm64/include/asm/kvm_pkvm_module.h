/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __ARM64_KVM_PKVM_MODULE_H__
#define __ARM64_KVM_PKVM_MODULE_H__

#include <asm/kvm_pgtable.h>
#include <linux/export.h>

typedef void (*dyn_hcall_t)(struct user_pt_regs *);

#ifdef CONFIG_MODULES
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
	void (*flush_dcache_to_poc)(void *addr, size_t size);
};

int __pkvm_load_el2_module(struct module *this, unsigned long *token);

int __pkvm_register_el2_call(dyn_hcall_t hfn, unsigned long token,
			     unsigned long hyp_text_kern_va);
#else
static inline int __pkvm_load_el2_module(struct module *this,
					 unsigned long *token)
{
	return -ENOSYS;
}

static inline int __pkvm_register_el2_call(dyn_hcall_t hfn, unsigned long token,
					   unsigned long hyp_text_kern_va)
{
	return -ENOSYS;
}
#endif /* CONFIG_MODULES */

#ifdef MODULE
#define pkvm_load_el2_module(init_fn, token)				\
({									\
	THIS_MODULE->arch.hyp.init = init_fn;				\
	__pkvm_load_el2_module(THIS_MODULE, token);			\
})

#define pkvm_register_el2_mod_call(hfn, token)				\
({									\
	unsigned long hyp_text_kern_va;					\
	hyp_text_kern_va = THIS_MODULE->arch.hyp.text.start;	 	\
	__pkvm_register_el2_call(hfn, token, hyp_text_kern_va);		\
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
