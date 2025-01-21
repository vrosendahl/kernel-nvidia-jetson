#include <asm/kvm_pgtable.h>

#define HCALL_HANDLED 0
#define HCALL_UNHANDLED -1

int __pkvm_register_host_smc_handler(bool (*cb)(struct user_pt_regs *));
int __pkvm_register_default_trap_handler(bool (*cb)(struct user_pt_regs *));
int __pkvm_register_illegal_abt_notifier(void (*cb)(struct user_pt_regs *));
int __pkvm_register_hyp_panic_notifier(void (*cb)(struct user_pt_regs *));

enum pkvm_psci_notification;
int __pkvm_register_psci_notifier(void (*cb)(enum pkvm_psci_notification, struct user_pt_regs *));

int reset_pkvm_priv_hcall_limit(void);

#ifdef CONFIG_MODULES
int __pkvm_init_module(void *module_init);
int __pkvm_register_hcall(unsigned long hfn_hyp_va);
int handle_host_dynamic_hcall(struct kvm_cpu_context *host_ctxt);
int __pkvm_close_late_module_registration(void);
void __pkvm_close_module_registration(void);
#else
static inline int __pkvm_init_module(void *module_init) { return -EOPNOTSUPP; }
static inline int
__pkvm_register_hcall(unsigned long hfn_hyp_va) { return -EOPNOTSUPP; }
static inline int handle_host_dynamic_hcall(struct kvm_cpu_context *host_ctxt)
{
	return HCALL_UNHANDLED;
}
static inline int __pkvm_close_late_module_registration(void) { return -EOPNOTSUPP; }
static inline void __pkvm_close_module_registration(void) { }
#endif
