/*
 * KMSAN shadow implementation.
 *
 * Copyright (C) 2017-2019 Google LLC
 * Author: Alexander Potapenko <glider@google.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <asm/cpu_entry_area.h>
#include <asm/page.h>
#include <asm/pgtable_64_types.h>
#include <asm/tlbflush.h>
#include <linux/memblock.h>
#include <linux/percpu-defs.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/stddef.h>

#include "kmsan.h"
#include "kmsan_shadow.h"

#define shadow_page_for(page) \
	((page)->shadow)

#define origin_page_for(page) \
	((page)->origin)

#define shadow_ptr_for(page) \
	(page_address((page)->shadow))

#define origin_ptr_for(page) \
	(page_address((page)->origin))

#define has_shadow_page(page) \
	(!!((page)->shadow))

#define has_origin_page(page) \
	(!!((page)->origin))

#define set_no_shadow_page(page) 	\
	do {				\
		(page)->shadow = NULL;	\
	} while(0) /**/

#define set_no_origin_page(page) 	\
	do {				\
		(page)->origin = NULL;	\
	} while(0) /**/

DEFINE_PER_CPU(char[CPU_ENTRY_AREA_SIZE], cpu_entry_area_shadow);
DEFINE_PER_CPU(char[CPU_ENTRY_AREA_SIZE], cpu_entry_area_origin);

/*
 * Dummy load and store pages to be used when the real metadata is unavailable.
 * There are separate pages for loads and stores, so that every load returns a
 * zero, and every store doesn't affect other stores.
 */
char dummy_load_page[PAGE_SIZE] __attribute__((aligned(PAGE_SIZE)));
char dummy_store_page[PAGE_SIZE] __attribute__((aligned(PAGE_SIZE)));

/*
 * Taken from arch/x86/mm/physaddr.h
 * TODO(glider): do we need it?
 */
static inline int my_phys_addr_valid(unsigned long addr)
{
#ifdef CONFIG_PHYS_ADDR_T_64BIT
	return !(addr >> boot_cpu_data.x86_phys_bits);
#else
	return 1;
#endif
}

/*
 * Taken from arch/x86/mm/physaddr.c
 * TODO(glider): do we need it?
 */
static bool my_virt_addr_valid(void *addr)
{
	unsigned long x = (unsigned long)addr;
	unsigned long y = x - __START_KERNEL_map;

	/* use the carry flag to determine if x was < __START_KERNEL_map */
	if (unlikely(x > y)) {
		x = y + phys_base;

		if (y >= KERNEL_IMAGE_SIZE)
			return false;
	} else {
		x = y + (__START_KERNEL_map - PAGE_OFFSET);

		/* carry flag will be set if starting x was >= PAGE_OFFSET */
		if ((x > y) || !my_phys_addr_valid(x))
			return false;
	}

	return pfn_valid(x >> PAGE_SHIFT);
}

static inline bool is_cpu_entry_area_addr(void *addr)
{
        return ((u64)addr >= CPU_ENTRY_AREA_BASE) && ((u64)addr < (CPU_ENTRY_AREA_BASE + CPU_ENTRY_AREA_MAP_SIZE));
}

void *vmalloc_meta(void *addr, bool is_origin)
{
	u64 addr64 = (u64)addr, off;

	BUG_ON(is_origin && !IS_ALIGNED(addr64, ORIGIN_SIZE));
	if (_is_vmalloc_addr(addr)) {
		return (void *)(addr64 + (is_origin ? VMALLOC_ORIGIN_OFFSET
						: VMALLOC_SHADOW_OFFSET));
	}
	if (is_module_addr(addr)) {
		off = addr64 - MODULES_VADDR;
		return (void *)(off + (is_origin ? MODULES_ORIGIN_START
						: MODULES_SHADOW_START));
	}
	return NULL;
}

static void *get_cea_meta_or_null(void *addr, bool is_origin)
{
	int cpu = smp_processor_id();
	int off;
	char *metadata_array;

	if (!is_cpu_entry_area_addr(addr))
		return NULL;
	off = (char*)addr - (char*)get_cpu_entry_area(cpu);
	if ((off < 0) || (off >= CPU_ENTRY_AREA_SIZE))
		return NULL;
	metadata_array = is_origin ? cpu_entry_area_origin :
				     cpu_entry_area_shadow;
	return &per_cpu(metadata_array[off], cpu);
}

static struct page *virt_to_page_or_null(void *vaddr)
{
	if (my_virt_addr_valid(vaddr))
		return virt_to_page(vaddr);
	else
		return NULL;
}

shadow_origin_ptr_t kmsan_get_shadow_origin_ptr(void *address, u64 size, bool store)
{
	shadow_origin_ptr_t ret;
	struct page *page;
	u64 pad, offset, o_offset;
	const u64 addr64 = (u64)address;
	u64 o_addr64 = (u64)address;
	void *shadow;

	if (size > PAGE_SIZE) {
		WARN(1, "size too big in kmsan_get_shadow_origin_ptr("
			"%px, %d, %d)\n", address, size, store);
		BUG();
	}
	if (store) {
		ret.s = dummy_store_page;
		ret.o = dummy_store_page;
	} else {
		ret.s = dummy_load_page;
		ret.o = dummy_load_page;
	}
	if (!kmsan_ready || IN_RUNTIME())
		return ret;
	BUG_ON(!metadata_is_contiguous(address, size, /*is_origin*/false));

	if (!IS_ALIGNED(addr64, ORIGIN_SIZE)) {
		pad = addr64 % ORIGIN_SIZE;
		o_addr64 -= pad;
	}

	if (_is_vmalloc_addr(address) || is_module_addr(address)) {
		ret.s = vmalloc_meta(address, /*is_origin*/false);
		ret.o = vmalloc_meta((void *)o_addr64, /*is_origin*/true);
		return ret;
	}

	if (!my_virt_addr_valid(address)) {
		page = vmalloc_to_page_or_null(address);
		if (page)
			goto next;
		shadow = get_cea_meta_or_null(address, /*is_origin*/false);
		if (shadow) {
			ret.s = shadow;
			ret.o = get_cea_meta_or_null((void *)o_addr64, /*is_origin*/true);
			return ret;
		}
	}
	page = virt_to_page_or_null(address);
	if (!page)
		return ret;
next:
        if (!has_shadow_page(page) || !has_origin_page(page))
		return ret;
	offset = addr64 % PAGE_SIZE;
	o_offset = o_addr64 % PAGE_SIZE;

	if (offset + size - 1 > PAGE_SIZE) {
		/*
		 * The access overflows the current page and touches the
		 * subsequent ones. Make sure the shadow/origin pages are also
		 * consequent.
		 */
		BUG_ON(!metadata_is_contiguous(address, size, /*is_origin*/false));
	}

	ret.s = shadow_ptr_for(page) + offset;
	ret.o = origin_ptr_for(page) + o_offset;
	return ret;
}

/*
 * TODO(glider): all other shadow getters are broken, so let's write another
 * one. The semantic is pretty straightforward: either return a valid shadow
 * pointer or NULL. The caller must BUG_ON on NULL if he wants to.
 * The return value of this function should not depend on whether we're in the
 * runtime or not.
 */
__always_inline
void *kmsan_get_metadata_or_null(void *address, size_t size, bool is_origin)
{
	struct page *page;
	void *ret;
	u64 addr = (u64)address, pad, offset;

	if (is_origin && !IS_ALIGNED(addr, ORIGIN_SIZE)) {
		pad = addr % ORIGIN_SIZE;
		addr -= pad;
		size += pad;
	}
	address = (void *)addr;
	if (_is_vmalloc_addr(address) || is_module_addr(address)) {
		return vmalloc_meta(address, is_origin);
	}

	if (!my_virt_addr_valid(address)) {
		page = vmalloc_to_page_or_null(address);
		if (page)
			goto next;
		ret = get_cea_meta_or_null(address, is_origin);
		if (ret)
			return ret;
	}
	page = virt_to_page_or_null(address);
	if (!page)
		return NULL;
next:
        if (!has_shadow_page(page) || !has_origin_page(page))
		return NULL;
	offset = addr % PAGE_SIZE;

	ret = (is_origin ? origin_ptr_for(page) : shadow_ptr_for(page)) + offset;
	return ret;
}

void __init kmsan_init_alloc_meta_for_range(void *start, void *end)
{
	u64 addr, size;
	struct page *page;
	void *shadow, *origin;
	struct page *shadow_p, *origin_p;

	start = (void *)ALIGN_DOWN((u64)start, PAGE_SIZE);
	size = ALIGN((u64)end - (u64)start, PAGE_SIZE);
	shadow = memblock_alloc(size, PAGE_SIZE);
	origin = memblock_alloc(size, PAGE_SIZE);
	for (addr = 0; addr < size; addr += PAGE_SIZE) {
		page = virt_to_page_or_null((char*)start + addr);
		shadow_p = virt_to_page_or_null((char*)shadow + addr);
		shadow_page_for(shadow_p) = NULL;
		origin_page_for(shadow_p) = NULL;
		shadow_page_for(page) = shadow_p;
		origin_p = virt_to_page_or_null((char*)origin + addr);
		shadow_page_for(origin_p) = NULL;
		origin_page_for(origin_p) = NULL;
		origin_page_for(page) = origin_p;
	}
}

/* Called from mm/memory.c */
void kmsan_copy_page_meta(struct page *dst, struct page *src)
{
	unsigned long irq_flags;

	if (!kmsan_ready || IN_RUNTIME())
		return;
	if (!has_shadow_page(src)) {
		/* TODO(glider): are we leaking pages here? */
		set_no_shadow_page(dst);
		set_no_origin_page(dst);
		return;
	}
	if (!has_shadow_page(dst))
		return;

	ENTER_RUNTIME(irq_flags);
	__memcpy(shadow_ptr_for(dst), shadow_ptr_for(src),
		PAGE_SIZE);
	BUG_ON(!has_origin_page(src) || !has_origin_page(dst));
	__memcpy(origin_ptr_for(dst), origin_ptr_for(src),
		PAGE_SIZE);
	LEAVE_RUNTIME(irq_flags);
}
EXPORT_SYMBOL(kmsan_copy_page_meta);

/* Helper function to allocate page metadata. */
static int kmsan_internal_alloc_meta_for_pages(struct page *page, unsigned int order,
					unsigned int actual_size, gfp_t flags, int node)
{
	struct page *shadow, *origin;
	int pages = 1 << order;
	int i;
	bool initialized = (flags & __GFP_ZERO) || !kmsan_ready;
	depot_stack_handle_t handle;

	// If |actual_size| is non-zero, we allocate |1 << order| metadata pages
	// for |actual_size| bytes of memory. We can't set shadow for more than
	// |actual_size >> PAGE_SHIFT| pages in that case.
	if (actual_size)
		pages = ALIGN(actual_size, PAGE_SIZE) >> PAGE_SHIFT;

	if (flags & __GFP_NO_KMSAN_SHADOW) {
		for (i = 0; i < pages; i++) {
			set_no_shadow_page(&page[i]);
			set_no_origin_page(&page[i]);
		}
		return 0;
	}

	flags = GFP_ATOMIC;  // TODO(glider)
	if (initialized)
		flags |= __GFP_ZERO;
	shadow = alloc_pages_node(node, flags | __GFP_NO_KMSAN_SHADOW, order);
	if (!shadow) {
		for (i = 0; i < pages; i++) {
			set_no_shadow_page(&page[i]);
			set_no_shadow_page(&page[i]);
		}
		return -ENOMEM;
	}
	if (!initialized)
		__memset(page_address(shadow), -1, PAGE_SIZE * pages);

	origin = alloc_pages_node(node, flags | __GFP_NO_KMSAN_SHADOW, order);
	// Assume we've allocated the origin.
	if (!origin) {
		__free_pages(shadow, order);
		for (i = 0; i < pages; i++) {
			set_no_shadow_page(&page[i]);
			set_no_origin_page(&page[i]);
		}
		return -ENOMEM;
	}

	if (!initialized) {
		handle = kmsan_save_stack_with_flags(flags);
		// Addresses are page-aligned, pages are contiguous, so it's ok
		// to just fill the origin pages with |handle|.
		for (i = 0; i < PAGE_SIZE * pages / sizeof(handle); i++) {
			((depot_stack_handle_t*)page_address(origin))[i] = handle;
		}
	}

	for (i = 0; i < pages; i++) {
		// TODO(glider): sometimes shadow_page_for(&page[i]) is initialized. Let's skip the check for now.
		///if (shadow_page_for(&page[i])) continue;
		shadow_page_for(&page[i]) = &shadow[i];
		set_no_shadow_page(shadow_page_for(&page[i]));
		set_no_origin_page(shadow_page_for(&page[i]));
		origin_page_for(&page[i]) = &origin[i];
		set_no_shadow_page(origin_page_for(&page[i]));
		set_no_origin_page(origin_page_for(&page[i]));
	}
	return 0;
}

/* Called from mm/page_alloc.c */
int kmsan_alloc_page(struct page *page, unsigned int order, gfp_t flags)
{
	unsigned long irq_flags;
	int ret;

	if (IN_RUNTIME())
		return 0;
	ENTER_RUNTIME(irq_flags);
	ret = kmsan_internal_alloc_meta_for_pages(
		page, order, /*actual_size*/ 0, flags, -1);
	LEAVE_RUNTIME(irq_flags);
	return ret;
}

/* Called from mm/page_alloc.c */
void kmsan_free_page(struct page *page, unsigned int order)
{
	struct page *shadow, *origin, *cur_page;
	int pages = 1 << order;
	int i;
	unsigned long irq_flags;

	if (!shadow_page_for(page)) {
		for (i = 0; i < pages; i++) {
			cur_page = &page[i];
			BUG_ON(shadow_page_for(cur_page));
		}
		return;
	}

	if (!kmsan_ready) {
		for (i = 0; i < pages; i++) {
			cur_page = &page[i];
			set_no_shadow_page(cur_page);
			set_no_origin_page(cur_page);
		}
		return;
	}

	if (IN_RUNTIME()) {
		/* TODO(glider): looks legit. depot_save_stack() may call
		 * free_pages().
		 */
		return;
	}

	ENTER_RUNTIME(irq_flags);
	if (!has_shadow_page(&page[0])) {
		/* TODO(glider): can we free a page without a shadow?
		 * Maybe if it was allocated at boot time?
		 * Anyway, all shadow pages must be NULL then.
		 */
		for (i = 0; i < pages; i++)
			if (has_shadow_page(&page[i])) {
				current->kmsan.is_reporting = true;
				for (i = 0; i < pages; i++)
					kmsan_pr_err("shadow_page_for(&page[%d])=%px\n",
							i, shadow_page_for(&page[i]));
				current->kmsan.is_reporting = false;
				break;
			}
		LEAVE_RUNTIME(irq_flags);
		return;
	}

	shadow = shadow_page_for(&page[0]);
	origin = origin_page_for(&page[0]);

	/* TODO(glider): this is racy. */
	for (i = 0; i < pages; i++) {
		BUG_ON(has_shadow_page(shadow_page_for(&page[i])));
		set_no_shadow_page(&page[i]);
		BUG_ON(has_shadow_page(origin_page_for(&page[i])));
		set_no_origin_page(&page[i]);
	}
	BUG_ON(has_shadow_page(shadow));
	__free_pages(shadow, order);

	BUG_ON(has_shadow_page(origin));
	__free_pages(origin, order);
	LEAVE_RUNTIME(irq_flags);
}
EXPORT_SYMBOL(kmsan_free_page);

/* Called from mm/page_alloc.c */
void kmsan_split_page(struct page *page, unsigned int order)
{
	struct page *shadow, *origin;
	unsigned long irq_flags;

	if (!kmsan_ready || IN_RUNTIME())
		return;

	ENTER_RUNTIME(irq_flags);
	if (!has_shadow_page(&page[0])) {
		BUG_ON(has_origin_page(&page[0]));
		LEAVE_RUNTIME(irq_flags);
		return;
	}
	shadow = shadow_page_for(&page[0]);
	split_page(shadow, order);

	origin = origin_page_for(&page[0]);
	split_page(origin, order);
	LEAVE_RUNTIME(irq_flags);
}
EXPORT_SYMBOL(kmsan_split_page);

/* Called from include/linux/highmem.h */
void kmsan_clear_page(void *page_addr)
{
	struct page *page;

	if (!kmsan_ready || IN_RUNTIME())
		return;
	BUG_ON(!IS_ALIGNED((u64)page_addr, PAGE_SIZE));
	page = vmalloc_to_page_or_null(page_addr);
	if (!page)
		page = virt_to_page_or_null(page_addr);
	if (!page || !has_shadow_page(page))
		return;
	__memset(shadow_ptr_for(page), 0, PAGE_SIZE);
	BUG_ON(!has_origin_page(page));
	__memset(origin_ptr_for(page), 0, PAGE_SIZE);
}
EXPORT_SYMBOL(kmsan_clear_page);

/* Called from mm/vmalloc.c */
void kmsan_vmap_page_range_noflush(unsigned long start, unsigned long end,
				   pgprot_t prot, struct page **pages)
{
	int nr, i, mapped;
	struct page **s_pages, **o_pages;
	unsigned long irq_flags;

	if (!kmsan_ready || IN_RUNTIME())
		return;
	if (!vmalloc_meta(start, /*is_origin*/false))
		return;

	BUG_ON(start >= end);
	nr = (end - start) / PAGE_SIZE;
	s_pages = kzalloc(sizeof(struct page *) * nr, GFP_KERNEL);
	o_pages = kzalloc(sizeof(struct page *) * nr, GFP_KERNEL);
	if (!s_pages || !o_pages)
		goto ret;
	for (i = 0; i < nr; i++) {
		s_pages[i] = shadow_page_for(pages[i]);
		o_pages[i] = origin_page_for(pages[i]);
	}
	prot = __pgprot(pgprot_val(prot) | _PAGE_NX);
	prot = PAGE_KERNEL;
	mapped = __vmap_page_range_noflush(vmalloc_meta(start, /*is_origin*/false), vmalloc_meta(end, /*is_origin*/false), prot, s_pages);
	BUG_ON(mapped != nr);
	flush_tlb_kernel_range(vmalloc_meta(start, /*is_origin*/false), vmalloc_meta(end, /*is_origin*/false));
	mapped = __vmap_page_range_noflush(vmalloc_meta(start, /*is_origin*/true), vmalloc_meta(end, /*is_origin*/true), prot, o_pages);
	BUG_ON(mapped != nr);
	flush_tlb_kernel_range(vmalloc_meta(start, /*is_origin*/true), vmalloc_meta(end, /*is_origin*/true));
ret:
	if (s_pages)
		kfree(s_pages);
	if (o_pages)
		kfree(o_pages);
}
