#ifndef __MM_KMSAN_KMSAN_H
#define __MM_KMSAN_KMSAN_H

#include <asm/current.h>
#include <linux/irqflags.h>
#include <linux/sched.h>
#include <linux/stackdepot.h>
#include <linux/stacktrace.h>
#include <linux/nmi.h>

#define KMSAN_MAGIC_MASK 0xffffffffff00
#define KMSAN_ALLOCA_MAGIC_ORIGIN 0x4110c4071900
#define KMSAN_CHAIN_MAGIC_ORIGIN_FULL 0xd419170cba00
#define KMSAN_CHAIN_MAGIC_ORIGIN_FRAME 0xed41917ddd00


#define KMSAN_NESTED_CONTEXT_MAX (8)
DECLARE_PER_CPU(kmsan_context_state[KMSAN_NESTED_CONTEXT_MAX], kmsan_percpu_cstate);  // [0] for dummy per-CPU context
DECLARE_PER_CPU(int, kmsan_context_level);  // 0 for task context, |i>0| for kmsan_context_state[i]
DECLARE_PER_CPU(int, kmsan_in_interrupt);
DECLARE_PER_CPU(bool, kmsan_in_softirq);
DECLARE_PER_CPU(bool, kmsan_in_nmi);

extern spinlock_t report_lock;
bool is_logbuf_locked(void);
#define kmsan_pr_err(...) \
	do { \
		if (!is_logbuf_locked()) \
			pr_err(__VA_ARGS__); \
	} while (0)

enum KMSAN_BUG_REASON
{
	REASON_ANY = 0,
	REASON_COPY_TO_USER = 1,
};

#define min_num(x,y) ((x) < (y) ? x : y)


/*
 * When a compiler hook is invoked, it may make a call to instrumented code
 * and eventually call itself recursively. To avoid that, we protect the
 * runtime entry points with ENTER_RUNTIME()/LEAVE_RUNTIME() macros and exit
 * the hook if IN_RUNTIME() is true. But when an interrupt occurs inside the
 * runtime, the hooks won’t run either, which may lead to errors.
 * Therefore we have to disable interrupts inside the runtime.
 */
#define IN_RUNTIME()	(current->kmsan.in_runtime)
#define ENTER_RUNTIME(irq_flags) \
	do { \
		preempt_disable(); \
		local_irq_save(irq_flags); \
		stop_nmi();		\
		current->kmsan.in_runtime++; \
		BUG_ON(current->kmsan.in_runtime > 1); \
	} while(0)
#define LEAVE_RUNTIME(irq_flags)	\
	do {	\
		current->kmsan.in_runtime--;	\
		BUG_ON(current->kmsan.in_runtime); \
		restart_nmi();		\
		local_irq_restore(irq_flags);	\
		preempt_enable(); } while(0)
void *kmsan_get_shadow_address(u64 addr, size_t size, bool checked, bool is_store);
void *kmsan_get_shadow_address_noruntime(u64 addr, size_t size, bool checked);
void *kmsan_get_origin_address(u64 addr, size_t size, bool checked, bool is_store);
void *kmsan_get_origin_address_noruntime(u64 addr, size_t size, bool checked);

void kmsan_memcpy_shadow(u64 dst, u64 src, size_t n);
void kmsan_memmove_shadow(u64 dst, u64 src, size_t n);
void kmsan_memcpy_shadow_to_mem(u64 dst, u64 src, size_t n);
void kmsan_memcpy_origin_to_mem(u64 dst, u64 src, size_t n);
void kmsan_memcpy_mem_to_shadow(u64 dst, u64 src, size_t n);
void kmsan_memcpy_mem_to_origin(u64 dst, u64 src, size_t n);


void kmsan_store_arg_shadow_origin(u64 dst_shadow, u64 dst_origin, u64 src, u64 size);
void kmsan_memcpy_origins(u64 dst, u64 src, size_t n);
void kmsan_memmove_origins(u64 dst, u64 src, size_t n);

extern char dummy_shadow_load_page[PAGE_SIZE];
extern char dummy_origin_load_page[PAGE_SIZE];
extern char dummy_shadow_store_page[PAGE_SIZE];
extern char dummy_origin_store_page[PAGE_SIZE];



extern void *kmsan_dummy_retval_tls[];
extern u64 kmsan_dummy_va_arg_overflow_size_tls;
extern void *kmsan_dummy_va_arg_tls[];
extern void *kmsan_dummy_va_arg_origin_tls[];
extern void *kmsan_dummy_param_tls[];
extern depot_stack_handle_t kmsan_dummy_origin_tls;
extern depot_stack_handle_t kmsan_dummy_param_origin_tls[];
extern depot_stack_handle_t kmsan_dummy_retval_origin_tls;

inline depot_stack_handle_t kmsan_save_stack(void);
inline depot_stack_handle_t kmsan_save_stack_with_flags(gfp_t flags);
void kmsan_internal_poison_shadow(void *address, size_t size, gfp_t flags);
void kmsan_internal_unpoison_shadow(void *address, size_t size);
void kmsan_internal_memset_shadow(u64 address, int b, size_t size);
depot_stack_handle_t kmsan_internal_chain_origin(depot_stack_handle_t id, bool full);

void do_kmsan_thread_create(struct task_struct *task);
void kmsan_set_origin(u64 address, int size, u32 origin);
inline void kmsan_report(void *caller, depot_stack_handle_t origin,
			u64 address, int size,
			int off_first, int off_last, bool deep, int reason);

int kmsan_internal_alloc_meta_for_pages(struct page *page, unsigned int order,
				unsigned int actual_size, gfp_t flags, int node);

kmsan_context_state *task_kmsan_context_state(void);

bool metadata_is_contiguous(u64 addr, size_t size, bool is_origin);

struct page *vmalloc_to_page_or_null(const void *vaddr);
struct page *virt_to_page_or_null(const void *vaddr);
void *get_cea_shadow_or_null(const void *addr);
void *get_cea_origin_or_null(const void *addr);

#endif
