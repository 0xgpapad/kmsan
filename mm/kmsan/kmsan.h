/*
 * KMSAN internal declarations.
 *
 * Copyright (C) 2017-2019 Google LLC
 * Author: Alexander Potapenko <glider@google.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#ifndef __MM_KMSAN_KMSAN_H
#define __MM_KMSAN_KMSAN_H

#include <asm/pgtable_64_types.h>
#include <linux/irqflags.h>
#include <linux/sched.h>
#include <linux/stackdepot.h>
#include <linux/stacktrace.h>
#include <linux/nmi.h>
#include <linux/mm.h>
#include <linux/printk.h>

#include "kmsan_shadow.h"

#define KMSAN_MAGIC_MASK 0xffffffffff00
#define KMSAN_ALLOCA_MAGIC_ORIGIN 0x4110c4071900
#define KMSAN_CHAIN_MAGIC_ORIGIN_FULL 0xd419170cba00

#define KMSAN_POISON_NOCHECK	0x0
#define KMSAN_POISON_CHECK	0x1

#define ORIGIN_SIZE 4

#define KMSAN_NESTED_CONTEXT_MAX (8)
/* [0] for dummy per-CPU context */
DECLARE_PER_CPU(kmsan_context_state[KMSAN_NESTED_CONTEXT_MAX],
		kmsan_percpu_cstate);
/* 0 for task context, |i>0| for kmsan_context_state[i]. */
DECLARE_PER_CPU(int, kmsan_context_level);
DECLARE_PER_CPU(int, kmsan_in_interrupt);
DECLARE_PER_CPU(bool, kmsan_in_softirq);
DECLARE_PER_CPU(bool, kmsan_in_nmi);

extern spinlock_t report_lock;
#define kmsan_pr_err(...) \
	do { \
		if (!is_logbuf_locked()) \
			pr_err(__VA_ARGS__); \
	} while (0)

void kmsan_print_origin(depot_stack_handle_t origin);
void kmsan_report(depot_stack_handle_t origin,
		  void *address, int size, int off_first, int off_last,
		  const void *user_addr, int reason);


enum KMSAN_BUG_REASON
{
	REASON_ANY = 0,
	REASON_COPY_TO_USER = 1,
	REASON_SUBMIT_URB = 2,
};

/*
 * When a compiler hook is invoked, it may make a call to instrumented code
 * and eventually call itself recursively. To avoid that, we protect the
 * runtime entry points with ENTER_RUNTIME()/LEAVE_RUNTIME() macros and exit
 * the hook if IN_RUNTIME() is true. But when an interrupt occurs inside the
 * runtime, the hooks won’t run either, which may lead to errors.
 * Therefore we have to disable interrupts inside the runtime.
 */
DECLARE_PER_CPU(int, kmsan_in_runtime);
DECLARE_PER_CPU(unsigned long, kmsan_runtime_last_caller);
#define IN_RUNTIME()	(this_cpu_read(kmsan_in_runtime))
#define ENTER_RUNTIME(irq_flags) \
	do { \
		preempt_disable(); \
		local_irq_save(irq_flags); \
		stop_nmi();		\
		this_cpu_inc(kmsan_in_runtime); \
		this_cpu_write(kmsan_runtime_last_caller, _THIS_IP_); \
		BUG_ON(this_cpu_read(kmsan_in_runtime) > 1); \
	} while(0)
#define LEAVE_RUNTIME(irq_flags)	\
	do {	\
		this_cpu_dec(kmsan_in_runtime);	\
		if (this_cpu_read(kmsan_in_runtime)) { \
			kmsan_pr_err("kmsan_in_runtime: %d, last_caller: %pF\n", \
				this_cpu_read(kmsan_in_runtime), \
				this_cpu_read(kmsan_runtime_last_caller)); \
			BUG(); \
		}	\
		restart_nmi();		\
		local_irq_restore(irq_flags);	\
		preempt_enable(); } while(0)

void kmsan_memcpy_metadata(void *dst, void *src, size_t n);
void kmsan_memmove_metadata(void *dst, void *src, size_t n);

depot_stack_handle_t kmsan_save_stack(void);
depot_stack_handle_t kmsan_save_stack_with_flags(gfp_t flags);
void kmsan_internal_poison_shadow(void *address, size_t size, gfp_t flags,
				  unsigned int poison_flags);
void kmsan_internal_unpoison_shadow(void *address, size_t size, bool checked);
void kmsan_internal_memset_shadow(void *address, int b, size_t size,
				  bool checked);
depot_stack_handle_t kmsan_internal_chain_origin(depot_stack_handle_t id);

void do_kmsan_task_create(struct task_struct *task);
void kmsan_set_origin(void *address, int size, u32 origin, bool checked);

kmsan_context_state *task_kmsan_context_state(void);

bool metadata_is_contiguous(void *addr, size_t size, bool is_origin);
void kmsan_internal_check_memory(void *addr, size_t size, const void *user_addr,
				 int reason);

struct page *vmalloc_to_page_or_null(void *vaddr);

/* Declared in mm/vmalloc.c */
void __vunmap_page_range(unsigned long addr, unsigned long end);
int __vmap_page_range_noflush(unsigned long start, unsigned long end,
				   pgprot_t prot, struct page **pages);

/*
 * Dummy replacement for __builtin_return_address() which may crash without
 * frame pointers.
 */
static inline void *kmsan_internal_return_address(int arg)
{
#ifdef CONFIG_UNWINDER_FRAME_POINTER
	switch (arg) {
		case 1:
			return __builtin_return_address(1);
		case 2:
			return __builtin_return_address(2);
		default:
			BUG();
	}
#else
	unsigned long entries[1];
	struct stack_trace trace = {
		.nr_entries = 0,
		.entries = entries,
		.max_entries = 1,
		.skip = arg
	};
	save_stack_trace(&trace);
	return entries[0];
#endif
}

static bool is_module_addr(void *vaddr)
{
	return ((u64)vaddr >= MODULES_VADDR) && ((u64)vaddr < MODULES_END);
}

static inline bool _is_vmalloc_addr(void *addr)
{
	return ((u64)addr >= VMALLOC_START) && ((u64)addr < VMALLOC_END);
}

#endif  /* __MM_KMSAN_KMSAN_H */
