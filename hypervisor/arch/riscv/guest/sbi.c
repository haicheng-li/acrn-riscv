/*
 * Copyright (C) 2023-2024 Intel Corporation. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <lib/types.h>
#include <asm/lib/bits.h>
#include <asm/cpu.h>
#include <asm/irq.h>
#include <asm/tlb.h>
#include <asm/smp.h>
#include <asm/notify.h>
#include <asm/cache.h>
#include <asm/guest/vcpu.h>
#include <asm/guest/vm.h>
#include "sbi.h"

static void sbi_ecall_base_probe(unsigned long id, unsigned long *out_val)
{
	*out_val = 0;
	switch (id) {
	case SBI_ID_BASE:
	case SBI_ID_IPI:
	case SBI_ID_RFENCE:
		*out_val = 1;
		break;
	default:
		break;
	}

	return;
}

static void sbi_base_handler(struct acrn_vcpu *vcpu, struct cpu_regs *regs)
{
	int *ret = &regs->a0;
	bool fail = false;
	unsigned long funcid = regs->a6;
	unsigned long *out_val = &regs->a1;

	switch (funcid) {
	case SBI_TYPE_BASE_GET_SPEC_VERSION:
		*out_val = SBI_SPEC_VERSION_MAJOR << 24;
		*out_val = *out_val | SBI_SPEC_VERSION_MINOR;
		break;
	case SBI_TYPE_BASE_GET_IMP_ID:
		*out_val = SBI_ACRN_IMPID;
		break;
	case SBI_TYPE_BASE_GET_IMP_VERSION:
		*out_val = SBI_ACRN_VERSION_MAJOR << 24;
		*out_val = *out_val | SBI_ACRN_VERSION_MINOR;
		break;
	case SBI_TYPE_BASE_GET_MVENDORID:
		*out_val = cpu_csr_read(mvendorid);
		break;
	case SBI_TYPE_BASE_GET_MARCHID:
		*out_val = cpu_csr_read(marchid);
		break;
	case SBI_TYPE_BASE_GET_MIMPID:
		*out_val = cpu_csr_read(mimpid);
		break;
	case SBI_TYPE_BASE_PROBE_EXT:
		sbi_ecall_base_probe(regs->a0, out_val);
		break;
	default:
		fail = true;
		break;
	}

	if (fail)
		*ret = SBI_ENOTSUPP;
	else
		*ret = SBI_SUCCESS;

	return;
}

static void sbi_timer_handler(struct acrn_vcpu *vcpu, struct cpu_regs *regs)
{
	int *ret = &regs->a0;
	unsigned long funcid = regs->a6;
	unsigned long *out_val = &regs->a1;

	if (funcid == SBI_TYPE_TIME_SET_TIMER) {
//		sbi_timer_event_start((u64)regs->a0);
		*ret = SBI_SUCCESS;
	} else {
		*ret = SBI_ENOTSUPP;
	}

	return;
}

static void send_vipi_mask(struct acrn_vcpu *vcpu, uint64_t mask, uint64_t base)
{
	uint16_t offset;

	offset = ffs64(mask);

	while ((offset + base) < vcpu->vm->hw.created_vcpus) {
		clear_bit(offset, &mask);
		send_single_swi(vcpu->vm->hw.vcpu[base + offset].pcpu_id,
				NOTIFY_VCPU_SWI);
		offset = ffs64(mask);
	}
}

static void sbi_ipi_handler(struct acrn_vcpu *vcpu, struct cpu_regs *regs)
{
	int *ret = &regs->a0;
	unsigned long funcid = regs->a6;
	unsigned long *out_val = &regs->a1;

	if (funcid == SBI_TYPE_IPI_SEND_IPI) {
		send_vipi_mask(vcpu, regs->a0, regs->a1);
		*ret = SBI_SUCCESS;
	} else {
		*ret = SBI_ENOTSUPP;
	}

	return;
}

static void sbi_rcall_sfence_vma(struct sbi_rfence_call *rcall)
{
	uint64_t base = rcall->base;
	uint64_t size = rcall->size;
	uint64_t i;

	if ((base == 0 && size == 0) || (size == SBI_RFENCE_FLUSH_ALL)) {
		flush_guest_tlb_local();
		return;
	}

	for (i = 0; i < size; i += PAGE_SIZE)
		flush_tlb_addr(base + i);
}

static void sbi_rcall_sfence_vma_asid(struct sbi_rfence_call *rcall)
{
	uint64_t base = rcall->base;
	uint64_t size  = rcall->size;
	uint64_t asid  = rcall->asid;
	uint64_t i;

	if (base == 0 && size == 0) {
		flush_guest_tlb_local();
		return;
	}

	if (size == SBI_RFENCE_FLUSH_ALL) {
		flush_tlb_asid(asid);
		return;
	}

	for (i = 0; i < size; i += PAGE_SIZE)
		flush_tlb_addr_asid(base + i, asid);
}

static void sbi_rcall_fence_i(struct sbi_rfence_call *rcall)
{
	invalidate_icache_local();
}

static void sbi_rfence_handler(struct acrn_vcpu *vcpu, struct cpu_regs *regs)
{
	int *ret = &regs->a0;
	uint64_t funcid = regs->a6;
	uint64_t *out_val = &regs->a1;
	uint64_t mask = regs->a0;
	uint64_t base = regs->a1;
	uint64_t rcall_mask = 0;
	smp_call_func_t func = NULL;
	struct sbi_rfence_call rcall;
	uint16_t offset;

	*ret = SBI_SUCCESS;
	switch (funcid) {
	case SBI_TYPE_RFENCE_FNECE_I:
		func = (smp_call_func_t)sbi_rcall_fence_i;
		break;
	case SBI_TYPE_RFENCE_SFNECE_VMA:
		func = (smp_call_func_t)sbi_rcall_sfence_vma;
		rcall.base = regs->a2;
		rcall.size = regs->a3;
		break;
	case SBI_TYPE_RFENCE_SFNECE_VMA_ASID:
		func = (smp_call_func_t)sbi_rcall_sfence_vma_asid;
		rcall.base = regs->a2;
		rcall.size = regs->a3;
		rcall.asid = regs->a4;
		break;
	default:
		*ret = SBI_ENOTSUPP;
		break;
	}

	if (func == NULL)
		return;
	offset = ffs64(mask);
	while ((offset + base) < vcpu->vm->hw.created_vcpus) {
		clear_bit(offset, &mask);
		set_bit(base + offset, &rcall_mask);
		offset = ffs64(mask);
	}
	smp_call_function(rcall_mask, func, (void *)&rcall);

	return;
}

static void sbi_hsm_handler(struct acrn_vcpu *vcpu, struct cpu_regs *regs)
{
	regs->a0 = SBI_ENOTSUPP;

	return;
}

static void sbi_srst_handler(struct acrn_vcpu *vcpu, struct cpu_regs *regs)
{
	regs->a0 = SBI_ENOTSUPP;

	return;
}

static void sbi_pmu_handler(struct acrn_vcpu *vcpu, struct cpu_regs *regs)
{
	regs->a0 = SBI_ENOTSUPP;

	return;
}

static void sbi_undefined_handler(struct acrn_vcpu *vcpu, struct cpu_regs *regs)
{
	regs->a0 = SBI_ENOTSUPP;

	return;
}

static const struct sbi_ecall_dispatch sbi_dispatch_table[NR_HX_EXIT_REASONS] = {
	[SBI_TYPE_BASE] = {
		.ext_id = SBI_ID_BASE,
		.handler = sbi_base_handler},
	[SBI_TYPE_TIMER] = {
		.ext_id = SBI_ID_TIMER,
		.handler = sbi_timer_handler},
	[SBI_TYPE_IPI] = {
		.ext_id = SBI_ID_IPI,
		.handler = sbi_ipi_handler},
	[SBI_TYPE_RFENCE] = {
		.ext_id = SBI_ID_RFENCE,
		.handler = sbi_rfence_handler},
	[SBI_TYPE_HSM] = {
		.ext_id = SBI_ID_HSM,
		.handler = sbi_hsm_handler},
	[SBI_TYPE_SRST] = {
		.ext_id = SBI_ID_SRST,
		.handler = sbi_srst_handler},
	[SBI_TYPE_PMU] = {
		.ext_id = SBI_ID_PMU,
		.handler = sbi_pmu_handler},
	[SBI_MAX_TYPES] = {
		.ext_id = SBI_VENDOR_START,
		.handler = sbi_undefined_handler},
};

int sbi_ecall_handler(struct acrn_vcpu *vcpu)
{
	struct run_context *ctx =
		&vcpu->arch.contexts[vcpu->arch.cur_context].run_ctx;
	struct cpu_regs *regs = &ctx->cpu_gp_regs.regs;
	uint32_t id = regs->a7;
	struct sbi_ecall_dispatch *d = &sbi_dispatch_table[SBI_MAX_TYPES];

	for (uint32_t i = 0; i < SBI_MAX_TYPES; i++) {
		if (id == sbi_dispatch_table[i].ext_id) {
			d = &sbi_dispatch_table[i];
			break;
		}
	}

	d->handler(vcpu, regs);

	return 0;
}
