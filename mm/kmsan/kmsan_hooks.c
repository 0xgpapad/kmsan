/*
 * KMSAN hooks for kernel subsystems.
 *
 * These functions handle creation of KMSAN metadata for memory allocations.
 *
 * Copyright (C) 2018-2019 Google LLC
 * Author: Alexander Potapenko <glider@google.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <asm/cacheflush.h>
#include <linux/gfp.h>
#include <linux/i2c.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/usb.h>

#include "../slab.h"
#include "kmsan.h"

/* TODO(glider): do we need to export these symbols? */

/*
 * The functions may call back to instrumented code, which, in turn, may call
 * these hooks again. To avoid re-entrancy, we use __GFP_NO_KMSAN_SHADOW.
 * Instrumented functions shouldn't be called under
 * ENTER_RUNTIME()/LEAVE_RUNTIME(), because this will lead to skipping
 * effects of functions like memset() inside instrumented code.
 */
/* Called from kernel/kthread.c, kernel/fork.c */
void kmsan_task_create(struct task_struct *task)
{
	unsigned long irq_flags;

	if (!task)
		return;
	ENTER_RUNTIME(irq_flags);
	kmsan_internal_task_create(task);
	LEAVE_RUNTIME(irq_flags);
}
EXPORT_SYMBOL(kmsan_task_create);


/* Called from kernel/exit.c */
void kmsan_task_exit(struct task_struct *task)
{
	unsigned long irq_flags;
	struct kmsan_task_state *state = &task->kmsan;

	if (!kmsan_ready || IN_RUNTIME())
		return;

	ENTER_RUNTIME(irq_flags);
	state->allow_reporting = false;

	LEAVE_RUNTIME(irq_flags);
}
EXPORT_SYMBOL(kmsan_task_exit);

/* Called from mm/slub.c */
void kmsan_slab_alloc(struct kmem_cache *s, void *object, gfp_t flags)
{
	unsigned long irq_flags;

	if (unlikely(object == NULL))
		return;
	if (!kmsan_ready || IN_RUNTIME())
		return;
	/*
	 * There's a ctor or this is an RCU cache - do nothing. The memory
	 * status hasn't changed since last use.
	 */
	if (s->ctor || (s->flags & SLAB_TYPESAFE_BY_RCU))
		return;

	ENTER_RUNTIME(irq_flags);
	if (flags & __GFP_ZERO) {
		kmsan_internal_unpoison_shadow(object, s->object_size,
					       KMSAN_POISON_CHECK);
	} else {
		kmsan_internal_poison_shadow(object, s->object_size, flags,
					     KMSAN_POISON_CHECK);
	}
	LEAVE_RUNTIME(irq_flags);
}

/* Called from mm/slub.c */
void kmsan_slab_free(struct kmem_cache *s, void *object)
{
	unsigned long irq_flags;

	if (!kmsan_ready || IN_RUNTIME())
		return;
	ENTER_RUNTIME(irq_flags);

	/* RCU slabs could be legally used after free within the RCU period */
	if (unlikely(s->flags & (SLAB_TYPESAFE_BY_RCU | SLAB_POISON)))
		goto leave;
	if (s->ctor)
		goto leave;
	kmsan_internal_poison_shadow(object, s->object_size,
				     GFP_KERNEL,
				     KMSAN_POISON_CHECK | KMSAN_POISON_FREE);
leave:
	LEAVE_RUNTIME(irq_flags);
	return;
}

/* Called from mm/slub.c */
void kmsan_kmalloc_large(const void *ptr, size_t size, gfp_t flags)
{
	unsigned long irq_flags;

	if (unlikely(ptr == NULL))
		return;
	if (!kmsan_ready || IN_RUNTIME())
		return;
	ENTER_RUNTIME(irq_flags);
	if (flags & __GFP_ZERO) {
		kmsan_internal_unpoison_shadow((void *)ptr, size,
					       /*checked*/true);
	} else {
		kmsan_internal_poison_shadow((void *)ptr, size, flags,
					     KMSAN_POISON_CHECK);
	}
	LEAVE_RUNTIME(irq_flags);
}

/* Called from mm/slub.c */
void kmsan_kfree_large(const void *ptr)
{
	struct page *page;
	unsigned long irq_flags;

	if (!kmsan_ready || IN_RUNTIME())
		return;
	ENTER_RUNTIME(irq_flags);
	page = virt_to_head_page((void *)ptr);
	BUG_ON(ptr != page_address(page));
	kmsan_internal_poison_shadow(
		(void *)ptr, PAGE_SIZE << compound_order(page), GFP_KERNEL,
		KMSAN_POISON_CHECK | KMSAN_POISON_FREE);
	LEAVE_RUNTIME(irq_flags);
}


static unsigned long vmalloc_shadow(unsigned long addr)
{
	return (unsigned long)kmsan_get_metadata((void *)addr, 1, META_SHADOW);
}

static unsigned long vmalloc_origin(unsigned long addr)
{
	return (unsigned long)kmsan_get_metadata((void *)addr, 1, META_ORIGIN);
}

/* Called from mm/vmalloc.c */
void kmsan_vunmap_page_range(unsigned long start, unsigned long end)
{
	__vunmap_page_range(vmalloc_shadow(start), vmalloc_shadow(end));
	__vunmap_page_range(vmalloc_origin(start), vmalloc_origin(end));
}

/* Called from lib/ioremap.c */
/*
 * This function creates new shadow/origin pages for the physical pages mapped
 * into the virtual memory. If those physical pages already had shadow/origin,
 * those are ignored.
 */
void kmsan_ioremap_page_range(unsigned long start, unsigned long end,
	phys_addr_t phys_addr, pgprot_t prot)
{
	unsigned long irq_flags;
	struct page *shadow, *origin;
	int i, nr;
	unsigned long off = 0;
	gfp_t gfp_mask = GFP_KERNEL | __GFP_ZERO | __GFP_NO_KMSAN_SHADOW;

	if (!kmsan_ready || IN_RUNTIME())
		return;

	nr = (end - start) / PAGE_SIZE;
	ENTER_RUNTIME(irq_flags);
	for (i = 0; i < nr; i++, off += PAGE_SIZE) {
		shadow = alloc_pages(gfp_mask, 1);
		origin = alloc_pages(gfp_mask, 1);
		__vmap_page_range_noflush(vmalloc_shadow(start + off),
				vmalloc_shadow(start + off + PAGE_SIZE),
				prot, &shadow);
		__vmap_page_range_noflush(vmalloc_origin(start + off),
				vmalloc_origin(start + off + PAGE_SIZE),
				prot, &origin);
	}
	flush_cache_vmap(vmalloc_shadow(start), vmalloc_shadow(end));
	flush_cache_vmap(vmalloc_origin(start), vmalloc_origin(end));
	LEAVE_RUNTIME(irq_flags);
}

void kmsan_iounmap_page_range(unsigned long start, unsigned long end)
{
	int i, nr;
	struct page *shadow, *origin;
	unsigned long v_shadow, v_origin;
	unsigned long irq_flags;

	if (!kmsan_ready || IN_RUNTIME())
		return;

	nr = (end - start) / PAGE_SIZE;
	ENTER_RUNTIME(irq_flags);
	v_shadow = (unsigned long)vmalloc_shadow(start);
	v_origin = (unsigned long)vmalloc_origin(start);
	for (i = 0; i < nr; i++, v_shadow += PAGE_SIZE, v_origin += PAGE_SIZE) {
		shadow = vmalloc_to_page_or_null((void *)v_shadow);
		origin = vmalloc_to_page_or_null((void *)v_origin);
		__vunmap_page_range(v_shadow, v_shadow + PAGE_SIZE);
		__vunmap_page_range(v_origin, v_origin + PAGE_SIZE);
		if (shadow)
			__free_pages(shadow, 1);
		if (origin)
			__free_pages(origin, 1);
	}
	LEAVE_RUNTIME(irq_flags);
}

/* Called from include/linux/uaccess.h, include/linux/uaccess.h */
void kmsan_copy_to_user(const void *to, const void *from,
			size_t to_copy, size_t left)
{
	void *shadow;

	if (!kmsan_ready || IN_RUNTIME())
		return;
	/*
	 * At this point we've copied the memory already. It's hard to check it
	 * before copying, as the size of actually copied buffer is unknown.
	 */

	/* copy_to_user() may copy zero bytes. No need to check. */
	if (!to_copy)
		return;
	/* Or maybe copy_to_user() failed to copy anything. */
	if (to_copy == left)
		return;
	if ((u64)to < TASK_SIZE) {
		/* This is a user memory access, check it. */
		kmsan_internal_check_memory((void *)from, to_copy - left, to,
						REASON_COPY_TO_USER);
		return;
	}
	/* Otherwise this is a kernel memory access. This happens when a compat
	 * syscall passes an argument allocated on the kernel stack to a real
	 * syscall.
	 * Don't check anything, just copy the shadow of the copied bytes.
	 */
	shadow = kmsan_get_metadata((void *)to, to_copy - left, META_SHADOW);
	if (shadow) {
		kmsan_memcpy_metadata((void *)to, (void *)from, to_copy - left);
	}
}
EXPORT_SYMBOL(kmsan_copy_to_user);

void kmsan_poison_shadow(const volatile void *address, size_t size, gfp_t flags)
{
	unsigned long irq_flags;

	if (!kmsan_ready || IN_RUNTIME())
		return;
	ENTER_RUNTIME(irq_flags);
	/* The users may want to poison/unpoison random memory. */
	kmsan_internal_poison_shadow((void *)address, size, flags,
				     KMSAN_POISON_NOCHECK);
	LEAVE_RUNTIME(irq_flags);
}
EXPORT_SYMBOL(kmsan_poison_shadow);

void kmsan_unpoison_shadow(const volatile void *address, size_t size)
{
	unsigned long irq_flags;

	if (!kmsan_ready || IN_RUNTIME())
		return;

	ENTER_RUNTIME(irq_flags);
	/* The users may want to poison/unpoison random memory. */
	kmsan_internal_unpoison_shadow((void *)address, size,
				       KMSAN_POISON_NOCHECK);
	LEAVE_RUNTIME(irq_flags);
}
EXPORT_SYMBOL(kmsan_unpoison_shadow);

void kmsan_check_memory(const volatile void *addr, size_t size)
{
	return kmsan_internal_check_memory((void *)addr, size, /*user_addr*/ 0,
					   REASON_ANY);
}
EXPORT_SYMBOL(kmsan_check_memory);

void kmsan_gup_pgd_range(struct page **pages, int nr)
{
	int i;
	void *page_addr;

	/*
	 * gup_pgd_range() has just created a number of new pages that KMSAN
	 * treats as uninitialized. In the case they belong to the userspace
	 * memory, unpoison the corresponding kernel pages.
	 */
	for (i = 0; i < nr; i++) {
		page_addr = page_address(pages[i]);
		if (((u64)page_addr < TASK_SIZE) &&
		    ((u64)page_addr + PAGE_SIZE < TASK_SIZE))
			kmsan_unpoison_shadow(page_addr, PAGE_SIZE);
	}

}
EXPORT_SYMBOL(kmsan_gup_pgd_range);

/* Helper function to check an SKB. */
void kmsan_check_skb(const struct sk_buff *skb)
{
	int start = skb_headlen(skb);
	struct sk_buff *frag_iter;
	int i, copy = 0;
	skb_frag_t *f;
	u32 p_off, p_len, copied;
	struct page *p;
	u8 *vaddr;

	if (!skb || !skb->len)
		return;

	kmsan_internal_check_memory(skb->data, skb_headlen(skb), 0, REASON_ANY);
	if (skb_is_nonlinear(skb)) {
		for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
			f = &skb_shinfo(skb)->frags[i];

			skb_frag_foreach_page(f,
					      skb_frag_off(f)  - start,
					      copy, p, p_off, p_len, copied) {

				vaddr = kmap_atomic(p);
				kmsan_internal_check_memory(vaddr + p_off,
						p_len, /*user_addr*/ 0,
						REASON_ANY);
				kunmap_atomic(vaddr);
			}
		}
	}
	skb_walk_frags(skb, frag_iter)
		kmsan_check_skb(frag_iter);
}
EXPORT_SYMBOL(kmsan_check_skb);

/* Helper function to check an URB. */
void kmsan_handle_urb(const struct urb *urb, bool is_out)
{
	if (!urb)
		return;
	if (is_out)
		kmsan_internal_check_memory(urb->transfer_buffer,
					    urb->transfer_buffer_length,
					    /*user_addr*/ 0, REASON_SUBMIT_URB);
	else
		kmsan_internal_unpoison_shadow(urb->transfer_buffer,
					       urb->transfer_buffer_length,
					       /*checked*/false);
}
EXPORT_SYMBOL(kmsan_handle_urb);

/* Helper function to check I2C-transferred data. */
void kmsan_handle_i2c_transfer(struct i2c_msg *msgs, int num)
{
	int i;

	if (!msgs)
		return;
	for (i = 0; i < num; i++) {
		if (msgs[i].flags & I2C_M_RD)
			kmsan_internal_unpoison_shadow(msgs[i].buf,
						       msgs[i].len,
						       /*checked*/false);
		else
			kmsan_internal_check_memory(msgs[i].buf, msgs[i].len,
						    /*user_addr*/0,
						    REASON_ANY);
	}
}

/*
 * Helper function to check printk.
 * Some optimizations may cause *fmt to be uninitialized, that's why it's
 * passed by reference.
 */
void kmsan_handle_vprintk(const char **fmt, va_list args)
{
	unsigned long irq_flags;
	size_t fmt_size;

	if (!kmsan_ready || IN_RUNTIME())
		return;
	ENTER_RUNTIME(irq_flags);
	fmt_size = strlen(*fmt);
	LEAVE_RUNTIME(irq_flags);
	/* TODO(glider): check |args|. */
	kmsan_internal_check_memory(fmt, sizeof(char*), /*user_addr*/0,
				    REASON_ANY);
	kmsan_internal_check_memory((void *)(*fmt), fmt_size, /*user_addr*/0,
				    REASON_ANY);
}
EXPORT_SYMBOL(kmsan_handle_vprintk);
