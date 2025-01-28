// SPDX-License-Identifier: GPL-2.0
/*
 * pKVM hyp driver for the Arm SMMUv3
 *
 * Copyright (C) 2022 Linaro Ltd.
 */
#include "arm_smmu_v3.h"

#include <asm/arm-smmu-v3-regs.h>
#include <asm/kvm_hyp.h>
#include <nvhe/iommu.h>
#include <nvhe/alloc.h>
#include <nvhe/iommu.h>
#include <nvhe/mem_protect.h>
#include <nvhe/mm.h>
#include <nvhe/pkvm.h>
#include <nvhe/trap_handler.h>


#include "arm-smmu-v3-module.h"

#ifdef MODULE
void *memset(void *dst, int c, size_t count)
{
	return CALL_FROM_OPS(memset, dst, c, count);
}

#ifdef CONFIG_LIST_HARDENED
bool __list_add_valid_or_report(struct list_head *new,
				struct list_head *prev,
				struct list_head *next)
{
	return CALL_FROM_OPS(list_add_valid_or_report, new, prev, next);
}

bool __list_del_entry_valid_or_report(struct list_head *entry)
{
	return CALL_FROM_OPS(list_del_entry_valid_or_report, entry);
}
#endif

const struct pkvm_module_ops		*mod_ops;
#endif

#define ARM_SMMU_POLL_TIMEOUT_US	100000 /* 100ms arbitrary timeout */

size_t __ro_after_init kvm_hyp_arm_smmu_v3_count;
struct hyp_arm_smmu_v3_device *kvm_hyp_arm_smmu_v3_smmus;

struct hyp_arm_smmu_v3_domain {
	struct kvm_hyp_iommu_domain     *domain;
	struct kvm_hyp_iommu            *iommu;
};

#define for_each_smmu(smmu) \
	for ((smmu) = kvm_hyp_arm_smmu_v3_smmus; \
	     (smmu) != &kvm_hyp_arm_smmu_v3_smmus[kvm_hyp_arm_smmu_v3_count]; \
	     (smmu)++)

/*
 * Wait until @cond is true.
 * Return 0 on success, or -ETIMEDOUT
 */
#define smmu_wait(_cond)					\
({								\
	int __i = 0;						\
	int __ret = 0;						\
								\
	while (!(_cond)) {					\
		if (++__i > ARM_SMMU_POLL_TIMEOUT_US) {		\
			__ret = -ETIMEDOUT;			\
			break;					\
		}						\
		pkvm_udelay(1);					\
	}							\
	__ret;							\
})

#define smmu_wait_event(_smmu, _cond)				\
({								\
	if ((_smmu)->features & ARM_SMMU_FEAT_SEV) {		\
		while (!(_cond))				\
			wfe();					\
	}							\
	smmu_wait(_cond);					\
})

/* Request non-device memory */
static void *smmu_alloc(size_t size)
{
	void *p;
	struct kvm_hyp_req req;

	p = hyp_alloc(size);
	/* We can't handle any other errors. */
	if (!p) {
		BUG_ON(hyp_alloc_errno() != -ENOMEM);
		req.type = KVM_HYP_REQ_TYPE_MEM;
		req.mem.dest = REQ_MEM_DEST_HYP_ALLOC;
		req.mem.nr_pages = hyp_alloc_missing_donations();
		req.mem.sz_alloc = PAGE_SIZE;
		kvm_iommu_request(&req);
	}

	return p;
}

static int smmu_write_cr0(struct hyp_arm_smmu_v3_device *smmu, u32 val)
{
	writel_relaxed(val, smmu->base + ARM_SMMU_CR0);
	return smmu_wait(readl_relaxed(smmu->base + ARM_SMMU_CR0ACK) == val);
}

#define Q_WRAP(smmu, reg)	((reg) & (1 << (smmu)->cmdq_log2size))
#define Q_IDX(smmu, reg)	((reg) & ((1 << (smmu)->cmdq_log2size) - 1))

static bool smmu_cmdq_full(struct hyp_arm_smmu_v3_device *smmu)
{
	u64 cons = readl_relaxed(smmu->base + ARM_SMMU_CMDQ_CONS);

	return Q_IDX(smmu, smmu->cmdq_prod) == Q_IDX(smmu, cons) &&
	       Q_WRAP(smmu, smmu->cmdq_prod) != Q_WRAP(smmu, cons);
}

static bool smmu_cmdq_empty(struct hyp_arm_smmu_v3_device *smmu)
{
	u64 cons = readl_relaxed(smmu->base + ARM_SMMU_CMDQ_CONS);

	return Q_IDX(smmu, smmu->cmdq_prod) == Q_IDX(smmu, cons) &&
	       Q_WRAP(smmu, smmu->cmdq_prod) == Q_WRAP(smmu, cons);
}

static int smmu_add_cmd(struct hyp_arm_smmu_v3_device *smmu,
			struct arm_smmu_cmdq_ent *ent)
{
	int i;
	int ret;
	u64 cmd[CMDQ_ENT_DWORDS] = {};
	int idx = Q_IDX(smmu, smmu->cmdq_prod);
	u64 *slot = smmu->cmdq_base + idx * CMDQ_ENT_DWORDS;

	if (smmu->iommu.power_is_off)
		return -EPIPE;

	ret = smmu_wait_event(smmu, !smmu_cmdq_full(smmu));
	if (ret)
		return ret;

	cmd[0] |= FIELD_PREP(CMDQ_0_OP, ent->opcode);

	switch (ent->opcode) {
	case CMDQ_OP_CFGI_ALL:
		cmd[1] |= FIELD_PREP(CMDQ_CFGI_1_RANGE, 31);
		break;
	case CMDQ_OP_CFGI_STE:
		cmd[0] |= FIELD_PREP(CMDQ_CFGI_0_SID, ent->cfgi.sid);
		cmd[1] |= FIELD_PREP(CMDQ_CFGI_1_LEAF, ent->cfgi.leaf);
		break;
	case CMDQ_OP_TLBI_NSNH_ALL:
		break;
	case CMDQ_OP_TLBI_S12_VMALL:
		cmd[0] |= FIELD_PREP(CMDQ_TLBI_0_VMID, ent->tlbi.vmid);
		break;
	case CMDQ_OP_TLBI_S2_IPA:
		cmd[0] |= FIELD_PREP(CMDQ_TLBI_0_NUM, ent->tlbi.num);
		cmd[0] |= FIELD_PREP(CMDQ_TLBI_0_SCALE, ent->tlbi.scale);
		cmd[0] |= FIELD_PREP(CMDQ_TLBI_0_VMID, ent->tlbi.vmid);
		cmd[1] |= FIELD_PREP(CMDQ_TLBI_1_LEAF, ent->tlbi.leaf);
		cmd[1] |= FIELD_PREP(CMDQ_TLBI_1_TTL, ent->tlbi.ttl);
		cmd[1] |= FIELD_PREP(CMDQ_TLBI_1_TG, ent->tlbi.tg);
		cmd[1] |= ent->tlbi.addr & CMDQ_TLBI_1_IPA_MASK;
		break;
	case CMDQ_OP_CMD_SYNC:
		cmd[0] |= FIELD_PREP(CMDQ_SYNC_0_CS, CMDQ_SYNC_0_CS_SEV);
		break;
	default:
		return -EINVAL;
	}

	for (i = 0; i < CMDQ_ENT_DWORDS; i++)
		slot[i] = cpu_to_le64(cmd[i]);

	smmu->cmdq_prod++;
	writel(Q_IDX(smmu, smmu->cmdq_prod) | Q_WRAP(smmu, smmu->cmdq_prod),
	       smmu->base + ARM_SMMU_CMDQ_PROD);
	return 0;
}

static int smmu_sync_cmd(struct hyp_arm_smmu_v3_device *smmu)
{
	int ret;
	struct arm_smmu_cmdq_ent cmd = {
		.opcode = CMDQ_OP_CMD_SYNC,
	};

	ret = smmu_add_cmd(smmu, &cmd);
	if (ret)
		return ret;

	return smmu_wait_event(smmu, smmu_cmdq_empty(smmu));
}

static int smmu_send_cmd(struct hyp_arm_smmu_v3_device *smmu,
			 struct arm_smmu_cmdq_ent *cmd)
{
	int ret = smmu_add_cmd(smmu, cmd);

	if (ret)
		return ret;

	return smmu_sync_cmd(smmu);
}

static int smmu_sync_ste(struct hyp_arm_smmu_v3_device *smmu, u32 sid)
{
	struct arm_smmu_cmdq_ent cmd = {
		.opcode = CMDQ_OP_CFGI_STE,
		.cfgi.sid = sid,
		.cfgi.leaf = true,
	};

	if (smmu->iommu.power_is_off && smmu->caches_clean_on_power_on)
		return 0;

	return smmu_send_cmd(smmu, &cmd);
}

static int smmu_alloc_l2_strtab(struct hyp_arm_smmu_v3_device *smmu, u32 idx)
{
	void *table;
	u64 l2ptr, span;

	/* Leaf tables must be page-sized */
	if (smmu->strtab_split + ilog2(STRTAB_STE_DWORDS) + 3 != PAGE_SHIFT)
		return -EINVAL;

	span = smmu->strtab_split + 1;
	if (WARN_ON(span < 1 || span > 11))
		return -EINVAL;

	table = kvm_iommu_donate_page();
	if (!table)
		return -ENOMEM;

	l2ptr = hyp_virt_to_phys(table);
	if (l2ptr & (~STRTAB_L1_DESC_L2PTR_MASK | ~PAGE_MASK))
		return -EINVAL;

	/* Ensure the empty stream table is visible before the descriptor write */
	wmb();

	WRITE_ONCE(smmu->strtab_base[idx], l2ptr | span);

	return 0;
}

static u64 *smmu_get_ste_ptr(struct hyp_arm_smmu_v3_device *smmu, u32 sid)
{
	u32 idx;
	int ret;
	u64 l1std, span, *base;

	if (sid >= smmu->strtab_num_entries)
		return NULL;
	sid = array_index_nospec(sid, smmu->strtab_num_entries);

	if (!smmu->strtab_split)
		return smmu->strtab_base + sid * STRTAB_STE_DWORDS;

	idx = sid >> smmu->strtab_split;
	l1std = smmu->strtab_base[idx];
	if (!l1std) {
		ret = smmu_alloc_l2_strtab(smmu, idx);
		if (ret)
			return NULL;
		l1std = smmu->strtab_base[idx];
		if (WARN_ON(!l1std))
			return NULL;
	}

	span = l1std & STRTAB_L1_DESC_SPAN;
	idx = sid & ((1 << smmu->strtab_split) - 1);
	if (!span || idx >= (1 << (span - 1)))
		return NULL;

	base = hyp_phys_to_virt(l1std & STRTAB_L1_DESC_L2PTR_MASK);
	return base + idx * STRTAB_STE_DWORDS;
}

static int smmu_init_registers(struct hyp_arm_smmu_v3_device *smmu)
{
	u64 val, old;
	int ret;

	if (!(readl_relaxed(smmu->base + ARM_SMMU_GBPA) & GBPA_ABORT))
		return -EINVAL;

	/* Initialize all RW registers that will be read by the SMMU */
	ret = smmu_write_cr0(smmu, 0);
	if (ret)
		return ret;

	val = FIELD_PREP(CR1_TABLE_SH, ARM_SMMU_SH_ISH) |
	      FIELD_PREP(CR1_TABLE_OC, CR1_CACHE_WB) |
	      FIELD_PREP(CR1_TABLE_IC, CR1_CACHE_WB) |
	      FIELD_PREP(CR1_QUEUE_SH, ARM_SMMU_SH_ISH) |
	      FIELD_PREP(CR1_QUEUE_OC, CR1_CACHE_WB) |
	      FIELD_PREP(CR1_QUEUE_IC, CR1_CACHE_WB);
	writel_relaxed(val, smmu->base + ARM_SMMU_CR1);
	writel_relaxed(CR2_PTM, smmu->base + ARM_SMMU_CR2);

	val = readl_relaxed(smmu->base + ARM_SMMU_GERROR);
	old = readl_relaxed(smmu->base + ARM_SMMU_GERRORN);
	/* Service Failure Mode is fatal */
	if ((val ^ old) & GERROR_SFM_ERR)
		return -EIO;
	/* Clear pending errors */
	writel_relaxed(val, smmu->base + ARM_SMMU_GERRORN);

	return 0;
}

/* Transfer ownership of structures from host to hyp */
static void *smmu_take_pages(u64 phys, size_t size)
{
	WARN_ON(!PAGE_ALIGNED(phys) || !PAGE_ALIGNED(size));
	if (__pkvm_host_donate_hyp(phys >> PAGE_SHIFT, size >> PAGE_SHIFT))
		return NULL;

	return hyp_phys_to_virt(phys);
}

static int smmu_init_cmdq(struct hyp_arm_smmu_v3_device *smmu)
{
	u64 cmdq_base;
	size_t cmdq_nr_entries, cmdq_size;
	int ret;
	enum kvm_pgtable_prot prot = PAGE_HYP;

	cmdq_base = readq_relaxed(smmu->base + ARM_SMMU_CMDQ_BASE);
	if (cmdq_base & ~(Q_BASE_RWA | Q_BASE_ADDR_MASK | Q_BASE_LOG2SIZE))
		return -EINVAL;

	smmu->cmdq_log2size = cmdq_base & Q_BASE_LOG2SIZE;
	cmdq_nr_entries = 1 << smmu->cmdq_log2size;
	cmdq_size = cmdq_nr_entries * CMDQ_ENT_DWORDS * 8;

	cmdq_base &= Q_BASE_ADDR_MASK;

	if (!(smmu->features & ARM_SMMU_FEAT_COHERENCY))
		prot |= KVM_PGTABLE_PROT_NC;

	ret = ___pkvm_host_donate_hyp_prot(cmdq_base >> PAGE_SHIFT,
					   PAGE_ALIGN(cmdq_size) >> PAGE_SHIFT,
					   false, prot);
	if (ret)
		return ret;

	smmu->cmdq_base = hyp_phys_to_virt(cmdq_base);

	memset(smmu->cmdq_base, 0, cmdq_size);
	writel_relaxed(0, smmu->base + ARM_SMMU_CMDQ_PROD);
	writel_relaxed(0, smmu->base + ARM_SMMU_CMDQ_CONS);

	return 0;
}

static int smmu_init_strtab(struct hyp_arm_smmu_v3_device *smmu)
{
	u64 strtab_base;
	size_t strtab_size;
	u32 strtab_cfg, fmt;
	int split, log2size;

	strtab_base = readq_relaxed(smmu->base + ARM_SMMU_STRTAB_BASE);
	if (strtab_base & ~(STRTAB_BASE_ADDR_MASK | STRTAB_BASE_RA))
		return -EINVAL;

	strtab_cfg = readl_relaxed(smmu->base + ARM_SMMU_STRTAB_BASE_CFG);
	if (strtab_cfg & ~(STRTAB_BASE_CFG_FMT | STRTAB_BASE_CFG_SPLIT |
			   STRTAB_BASE_CFG_LOG2SIZE))
		return -EINVAL;

	fmt = FIELD_GET(STRTAB_BASE_CFG_FMT, strtab_cfg);
	split = FIELD_GET(STRTAB_BASE_CFG_SPLIT, strtab_cfg);
	log2size = FIELD_GET(STRTAB_BASE_CFG_LOG2SIZE, strtab_cfg);

	smmu->strtab_split = split;
	smmu->strtab_num_entries = 1 << log2size;

	switch (fmt) {
	case STRTAB_BASE_CFG_FMT_LINEAR:
		if (split)
			return -EINVAL;
		smmu->strtab_num_l1_entries = smmu->strtab_num_entries;
		strtab_size = smmu->strtab_num_l1_entries *
			      STRTAB_STE_DWORDS * 8;
		break;
	case STRTAB_BASE_CFG_FMT_2LVL:
		if (split != 6 && split != 8 && split != 10)
			return -EINVAL;
		smmu->strtab_num_l1_entries = 1 << max(0, log2size - split);
		strtab_size = smmu->strtab_num_l1_entries *
			      STRTAB_L1_DESC_DWORDS * 8;
		break;
	default:
		return -EINVAL;
	}

	strtab_base &= STRTAB_BASE_ADDR_MASK;
	smmu->strtab_base = smmu_take_pages(strtab_base, strtab_size);
	if (!smmu->strtab_base)
		return -EINVAL;

	/* Disable all STEs */
	memset(smmu->strtab_base, 0, strtab_size);
	return 0;
}

static int smmu_reset_device(struct hyp_arm_smmu_v3_device *smmu)
{
	int ret;
	struct arm_smmu_cmdq_ent cfgi_cmd = {
		.opcode = CMDQ_OP_CFGI_ALL,
	};
	struct arm_smmu_cmdq_ent tlbi_cmd = {
		.opcode = CMDQ_OP_TLBI_NSNH_ALL,
	};

	/* Invalidate all cached configs and TLBs */
	ret = smmu_write_cr0(smmu, CR0_CMDQEN);
	if (ret)
		return ret;

	ret = smmu_add_cmd(smmu, &cfgi_cmd);
	if (ret)
		goto err_disable_cmdq;

	ret = smmu_add_cmd(smmu, &tlbi_cmd);
	if (ret)
		goto err_disable_cmdq;

	ret = smmu_sync_cmd(smmu);
	if (ret)
		goto err_disable_cmdq;

	/* Enable translation */
	return smmu_write_cr0(smmu, CR0_SMMUEN | CR0_CMDQEN | CR0_ATSCHK | CR0_EVTQEN);

err_disable_cmdq:
	return smmu_write_cr0(smmu, 0);
}

static struct hyp_arm_smmu_v3_device *to_smmu(struct kvm_hyp_iommu *iommu)
{
	return container_of(iommu, struct hyp_arm_smmu_v3_device, iommu);
}

static void smmu_tlb_flush_all(void *cookie)
{
	struct kvm_hyp_iommu_domain *domain = cookie;
	struct hyp_arm_smmu_v3_domain *smmu_domain = domain->priv;
	struct hyp_arm_smmu_v3_device *smmu = to_smmu(smmu_domain->iommu);
	struct arm_smmu_cmdq_ent cmd = {
		.opcode = CMDQ_OP_TLBI_S12_VMALL,
		.tlbi.vmid = domain->domain_id,
	};

	hyp_spin_lock(&smmu->iommu.lock);
	if (smmu->iommu.power_is_off && smmu->caches_clean_on_power_on) {
		hyp_spin_unlock(&smmu->iommu.lock);
		return;
	}

	WARN_ON(smmu_send_cmd(smmu, &cmd));
	hyp_spin_unlock(&smmu->iommu.lock);
}

static void smmu_tlb_inv_range(struct kvm_hyp_iommu_domain *domain,
			       unsigned long iova, size_t size, size_t granule,
			       bool leaf)
{
	struct hyp_arm_smmu_v3_domain *smmu_domain = domain->priv;
	struct hyp_arm_smmu_v3_device *smmu = to_smmu(smmu_domain->iommu);
	unsigned long end = iova + size;
	struct arm_smmu_cmdq_ent cmd = {
		.opcode = CMDQ_OP_TLBI_S2_IPA,
		.tlbi.vmid = domain->domain_id,
		.tlbi.leaf = leaf,
	};

	hyp_spin_lock(&smmu->iommu.lock);

	if (smmu->iommu.power_is_off && smmu->caches_clean_on_power_on) {
		hyp_spin_unlock(&smmu->iommu.lock);
		return;
	}

	/*
	 * There are no mappings at high addresses since we don't use TTB1, so
	 * no overflow possible.
	 */
	BUG_ON(end < iova);

	while (iova < end) {
		cmd.tlbi.addr = iova;
		WARN_ON(smmu_send_cmd(smmu, &cmd));
		BUG_ON(iova + granule < iova);
		iova += granule;
	}
	hyp_spin_unlock(&smmu->iommu.lock);
}

static void smmu_tlb_flush_walk(unsigned long iova, size_t size,
				size_t granule, void *cookie)
{
	smmu_tlb_inv_range(cookie, iova, size, granule, false);
}

static void smmu_tlb_add_page(struct iommu_iotlb_gather *gather,
			      unsigned long iova, size_t granule,
			      void *cookie)
{
	smmu_tlb_inv_range(cookie, iova, granule, granule, true);
}

static const struct iommu_flush_ops smmu_tlb_ops = {
	.tlb_flush_all	= smmu_tlb_flush_all,
	.tlb_flush_walk = smmu_tlb_flush_walk,
	.tlb_add_page	= smmu_tlb_add_page,
};

static int smmu_init_device(struct hyp_arm_smmu_v3_device *smmu)
{
	int ret;

	if (!PAGE_ALIGNED(smmu->mmio_addr | smmu->mmio_size))
		return -EINVAL;

	ret = ___pkvm_host_donate_hyp(smmu->mmio_addr >> PAGE_SHIFT,
				      smmu->mmio_size >> PAGE_SHIFT,
				      /* accept_mmio */ true);
	if (ret)
		return ret;

	smmu->base = hyp_phys_to_virt(smmu->mmio_addr);
	smmu->pgtable_cfg.tlb = &smmu_tlb_ops;

	ret = smmu_init_registers(smmu);
	if (ret)
		return ret;

	ret = smmu_init_cmdq(smmu);
	if (ret)
		return ret;

	ret = smmu_init_strtab(smmu);
	if (ret)
		return ret;

	ret = smmu_reset_device(smmu);
	if (ret)
		return ret;

	return kvm_iommu_init_device(&smmu->iommu);
}

static int smmu_init(unsigned long init_arg)
{
	int ret;
	struct hyp_arm_smmu_v3_device *smmu;
	int smmu_arr_size = PAGE_ALIGN(sizeof(*kvm_hyp_arm_smmu_v3_smmus) * kvm_hyp_arm_smmu_v3_count);

	kvm_hyp_arm_smmu_v3_smmus = kern_hyp_va(kvm_hyp_arm_smmu_v3_smmus);

	WARN_ON(!smmu_take_pages(hyp_virt_to_phys(kvm_hyp_arm_smmu_v3_smmus), smmu_arr_size));

	for_each_smmu(smmu) {
		ret = smmu_init_device(smmu);
		if (ret)
			return ret;
	}

	return 0;
}

static struct kvm_hyp_iommu *smmu_id_to_iommu(pkvm_handle_t smmu_id)
{
	if (smmu_id >= kvm_hyp_arm_smmu_v3_count)
		return NULL;
	smmu_id = array_index_nospec(smmu_id, kvm_hyp_arm_smmu_v3_count);

	return &kvm_hyp_arm_smmu_v3_smmus[smmu_id].iommu;
}

int smmu_domain_finalise(struct kvm_hyp_iommu_domain *domain)
{
	int ret;
	struct hyp_arm_smmu_v3_domain *smmu_domain = domain->priv;
	struct hyp_arm_smmu_v3_device *smmu = to_smmu(smmu_domain->iommu);

	domain->pgtable = kvm_arm_io_pgtable_alloc(&smmu->pgtable_cfg,
						   domain, &ret);

	return ret;
}

static int smmu_attach_dev(struct kvm_hyp_iommu *iommu, struct kvm_hyp_iommu_domain *domain,
			   u32 sid, u32 pasid, u32 pasid_bits)
{
	int i;
	int ret = -EINVAL;
	u64 *dst;
	struct io_pgtable_cfg *cfg;
	u64 ts, sl, ic, oc, sh, tg, ps;
	u64 ent[STRTAB_STE_DWORDS] = {};
	struct hyp_arm_smmu_v3_device *smmu = to_smmu(iommu);
	struct hyp_arm_smmu_v3_domain *smmu_domain = domain->priv;

	hyp_spin_lock(&iommu->lock);
	dst = smmu_get_ste_ptr(smmu, sid);
	if (!dst || dst[0])
		goto out_unlock;

	/*
	 * First attach to the domain, this is over protected by the all domain locks,
	 * as there is no per-domain lock now, this can be improved later.
	 * However, as this operation is not on the hot path, it should be fine.
	 */
	if (!domain->pgtable) {
		smmu_domain->iommu = iommu;
		ret = smmu_domain_finalise(domain);
		if (ret)
			goto out_unlock;
	}

	if (smmu_domain->iommu != iommu) {
		ret = -EBUSY;
		goto out_unlock;
	}

	cfg = &domain->pgtable->cfg;
	ps = cfg->arm_lpae_s2_cfg.vtcr.ps;
	tg = cfg->arm_lpae_s2_cfg.vtcr.tg;
	sh = cfg->arm_lpae_s2_cfg.vtcr.sh;
	oc = cfg->arm_lpae_s2_cfg.vtcr.orgn;
	ic = cfg->arm_lpae_s2_cfg.vtcr.irgn;
	sl = cfg->arm_lpae_s2_cfg.vtcr.sl;
	ts = cfg->arm_lpae_s2_cfg.vtcr.tsz;

	ent[0] = STRTAB_STE_0_V |
		 FIELD_PREP(STRTAB_STE_0_CFG, STRTAB_STE_0_CFG_S2_TRANS);
	ent[1] = FIELD_PREP(STRTAB_STE_1_SHCFG, STRTAB_STE_1_SHCFG_INCOMING);
	ent[2] = FIELD_PREP(STRTAB_STE_2_VTCR,
			FIELD_PREP(STRTAB_STE_2_VTCR_S2PS, ps) |
			FIELD_PREP(STRTAB_STE_2_VTCR_S2TG, tg) |
			FIELD_PREP(STRTAB_STE_2_VTCR_S2SH0, sh) |
			FIELD_PREP(STRTAB_STE_2_VTCR_S2OR0, oc) |
			FIELD_PREP(STRTAB_STE_2_VTCR_S2IR0, ic) |
			FIELD_PREP(STRTAB_STE_2_VTCR_S2SL0, sl) |
			FIELD_PREP(STRTAB_STE_2_VTCR_S2T0SZ, ts)) |
		 FIELD_PREP(STRTAB_STE_2_S2VMID, domain->domain_id) |
		 STRTAB_STE_2_S2AA64 | STRTAB_STE_2_S2R;
	ent[3] = cfg->arm_lpae_s2_cfg.vttbr & STRTAB_STE_3_S2TTB_MASK;

	/*
	 * The SMMU may cache a disabled STE.
	 * Initialize all fields, sync, then enable it.
	 */
	for (i = 1; i < STRTAB_STE_DWORDS; i++)
		dst[i] = cpu_to_le64(ent[i]);

	ret = smmu_sync_ste(smmu, sid);
	if (ret)
		goto out_unlock;

	WRITE_ONCE(dst[0], cpu_to_le64(ent[0]));
	ret = smmu_sync_ste(smmu, sid);
	WARN_ON(ret);

out_unlock:
	hyp_spin_unlock(&iommu->lock);
	return ret;
}

static int smmu_detach_dev(struct kvm_hyp_iommu *iommu, struct kvm_hyp_iommu_domain *domain,
			   u32 sid, u32 pasid)
{
	u64 *dst;
	int i, ret = -ENODEV;
	struct hyp_arm_smmu_v3_device *smmu = to_smmu(iommu);

	hyp_spin_lock(&iommu->lock);
	dst = smmu_get_ste_ptr(smmu, sid);
	if (!dst)
		goto out_unlock;

	dst[0] = 0;
	ret = smmu_sync_ste(smmu, sid);
	if (ret)
		goto out_unlock;

	for (i = 1; i < STRTAB_STE_DWORDS; i++)
		dst[i] = 0;

	ret = smmu_sync_ste(smmu, sid);
out_unlock:
	hyp_spin_unlock(&iommu->lock);
	return ret;
}

int smmu_alloc_domain(struct kvm_hyp_iommu_domain *domain)
{
	struct hyp_arm_smmu_v3_domain *smmu_domain;

	smmu_domain = smmu_alloc(sizeof(struct hyp_arm_smmu_v3_domain));
	if (!smmu_domain)
		return -ENOMEM;

	/* Can't do much without the IOMMU. */
	smmu_domain->domain = domain;
	domain->priv = (void *)smmu_domain;

	return 0;
}

void smmu_free_domain(struct kvm_hyp_iommu_domain *domain)
{
	/*
	 * As page table allocation is decoupled from alloc_domain, free_domain can
	 * be called with a domain that have never been attached.
	 */
	if (domain->pgtable)
		kvm_arm_io_pgtable_free(domain->pgtable);

	hyp_free(domain->priv);
}

bool smmu_dabt_device(struct hyp_arm_smmu_v3_device *smmu,
		      struct kvm_cpu_context *host_ctxt, u64 esr, u32 off)
{
	bool is_write = esr & ESR_ELx_WNR;
	unsigned int len = BIT((esr & ESR_ELx_SAS) >> ESR_ELx_SAS_SHIFT);
	int rd = (esr & ESR_ELx_SRT_MASK) >> ESR_ELx_SRT_SHIFT;
	const u32 no_access  = 0;
	const u32 read_write = (u32)(-1);
	const u32 read_only = is_write ? no_access : read_write;
	u32 mask = no_access;

	/*
	 * Only handle MMIO access with u32 size and alignment.
	 * We don't need to change 64-bit registers for now.
	 */
	if ((len != sizeof(u32)) || (off & (sizeof(u32) - 1)))
		return false;

	switch (off) {
	case ARM_SMMU_EVTQ_PROD + SZ_64K:
		mask = read_write;
		break;
	case ARM_SMMU_EVTQ_CONS + SZ_64K:
		mask = read_write;
		break;
	case ARM_SMMU_GERROR:
		mask = read_only;
		break;
	case ARM_SMMU_GERRORN:
		mask = read_write;
		break;
	};

	if (!mask)
		return false;
	if (is_write)
		writel_relaxed(cpu_reg(host_ctxt, rd) & mask, smmu->base + off);
	else
		cpu_reg(host_ctxt, rd) = readl_relaxed(smmu->base + off);

	return true;
}

bool smmu_dabt_handler(struct kvm_cpu_context *host_ctxt, u64 esr, u64 addr)
{
	struct hyp_arm_smmu_v3_device *smmu;

	for_each_smmu(smmu) {
		if (addr < smmu->mmio_addr || addr >= smmu->mmio_addr + smmu->mmio_size)
			continue;
		return smmu_dabt_device(smmu, host_ctxt, esr, addr - smmu->mmio_addr);
	}
	return false;
}

int smmu_suspend(struct kvm_hyp_iommu *iommu)
{
	struct hyp_arm_smmu_v3_device *smmu = to_smmu(iommu);

	/*
	 * Disable translation, GBPA is validated at probe to be set, so all transaltion
	 * would be aborted when SMMU is disabled.
	 */
	if (iommu->power_domain.type == KVM_POWER_DOMAIN_HOST_HVC)
		return smmu_write_cr0(smmu, 0);
	return 0;
}

int smmu_resume(struct kvm_hyp_iommu *iommu)
{
	struct hyp_arm_smmu_v3_device *smmu = to_smmu(iommu);

	/*
	 * Re-enable and clean all caches.
	 */
	if (iommu->power_domain.type == KVM_POWER_DOMAIN_HOST_HVC)
		return smmu_reset_device(smmu);
	return 0;
}

#ifdef MODULE
int smmu_init_hyp_module(const struct pkvm_module_ops *ops)
{
	if (!ops)
		return -EINVAL;

	mod_ops = ops;
	return 0;
}
#endif

struct kvm_iommu_ops smmu_ops = {
	.init				= smmu_init,
	.get_iommu_by_id		= smmu_id_to_iommu,
	.alloc_domain			= smmu_alloc_domain,
	.free_domain			= smmu_free_domain,
	.attach_dev			= smmu_attach_dev,
	.detach_dev			= smmu_detach_dev,
	.dabt_handler			= smmu_dabt_handler,
	.suspend			= smmu_suspend,
	.resume				= smmu_resume,
};

