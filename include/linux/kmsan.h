/* KMSAN */
#ifndef LINUX_KMSAN_H
#define LINUX_KMSAN_H

#include <linux/stackdepot.h>
#include <linux/types.h>
#include <linux/vmalloc.h>

struct page;
struct kmem_cache;
struct task_struct;
struct vm_struct;


extern bool kmsan_ready;

#ifdef CONFIG_KMSAN
void __init kmsan_initialize_shadow(void);
#else
static inline void __init kmsan_initialize_shadow(void) { }
#endif


#ifdef CONFIG_KMSAN

typedef struct kmsan_thread_s kmsan_thread_state;
typedef struct kmsan_context_s kmsan_context_state;


// TODO(glider): Factor out params, origins etc. into a separate
// struct kmsan_context_state. Then make those for IRQs and exceptions per-cpu,
// not per-task.
// These constants are defined in the MSan LLVM instrumentation pass.
#define RETVAL_SIZE 800
#define KMSAN_PARAM_SIZE 800

struct kmsan_context_s {
	char param_tls[KMSAN_PARAM_SIZE];
	char retval_tls[RETVAL_SIZE];
	char va_arg_tls[KMSAN_PARAM_SIZE];
	char va_arg_origin_tls[KMSAN_PARAM_SIZE];
	u64 va_arg_overflow_size_tls;
	depot_stack_handle_t param_origin_tls[KMSAN_PARAM_SIZE / sizeof(depot_stack_handle_t)];
	depot_stack_handle_t retval_origin_tls;
	depot_stack_handle_t origin_tls;
};

struct kmsan_thread_s {
	bool enabled;
	bool initialization;
	bool allow_reporting;
	bool is_reporting;
	// TODO(glider): When in_runtime is 1, IRQs are disabled.
	int in_runtime;
	bool is_switching;
	bool debug;

	kmsan_context_state cstate;
};

extern kmsan_context_state kmsan_dummy_state;

// TODO(glider): rename to kmsan_task_create()
void kmsan_thread_create(struct task_struct *task);
void kmsan_task_exit(struct task_struct *task);
void kmsan_alloc_shadow_for_region(void *start, size_t size);
void kmsan_prep_pages(struct page *page, unsigned int order);
int kmsan_alloc_page(struct page *page, unsigned int order, gfp_t flags);
void kmsan_acpi_map(void *vaddr, unsigned long size);
void kmsan_acpi_unmap(void *vaddr, unsigned long size);
void kmsan_free_page(struct page *page, unsigned int order);
void kmsan_split_page(struct page *page, unsigned int order);
void kmsan_clear_user_page(struct page *page);
void kmsan_copy_page_meta(struct page *dst, struct page *src);

void kmsan_poison_slab(struct page *page, gfp_t flags);
void kmsan_kmalloc_large(const void *ptr, size_t size, gfp_t flags);
void kmsan_kfree_large(const void *ptr);
void kmsan_kmalloc(struct kmem_cache *s, const void *object, size_t size,
		  gfp_t flags);
void kmsan_slab_alloc(struct kmem_cache *s, void *object, gfp_t flags);
bool kmsan_slab_free(struct kmem_cache *s, void *object);

void kmsan_slab_setup_object(struct kmem_cache *s, void *object);
void kmsan_post_alloc_hook(struct kmem_cache *s, gfp_t flags,
			size_t size, void *object);

void kmsan_wipe_params_shadow_origin(void);
void kmsan_record_future_shadow_range(u64 start, u64 end);

void kmsan_vprintk_func(const char *fmt, va_list args);

// Vmap
void kmsan_vmap(struct vm_struct *area,
		struct page **pages, unsigned int count, unsigned long flags,
		pgprot_t prot, void *caller);
void kmsan_vunmap(const void *addr, struct vm_struct *area, int deallocate_pages);
bool kmsan_vmalloc_area_node(struct vm_struct *area, gfp_t alloc_flags, gfp_t nested_gfp, gfp_t highmem_mask, pgprot_t prot, int node);

void kmsan_softirq_enter(void);
void kmsan_softirq_exit(void);
#else

static inline void kmsan_thread_create(struct task_struct *task) {}
static inline void kmsan_task_exit(struct task_struct *task) {}
static inline void kmsan_alloc_shadow_for_region(void *start, size_t size) {}
static inline void kmsan_prep_pages(struct page *page, unsigned int order) {}
static inline int kmsan_alloc_page(
	struct page *page, unsigned int order, gfp_t flags)
{
	return 0;
}
static inline void kmsan_acpi_map(void *vaddr, unsigned long size) {}
static inline void kmsan_acpi_unmap(void *vaddr, unsigned long size) {}
static inline void kmsan_free_page(struct page *page, unsigned int order) {}
static inline void kmsan_split_page(struct page *page, unsigned int order) {}
static inline void kmsan_clear_user_page(struct page *page) {}
static inline void kmsan_copy_page_meta(struct page *dst, struct page *src) {}

static inline void kmsan_poison_slab(struct page *page, gfp_t flags) {}
static inline void kmsan_kmalloc_large(
	const void *ptr, size_t size, gfp_t flags) {}
static inline void kmsan_kfree_large(const void *ptr) {}
static inline void kmsan_kmalloc(
	struct kmem_cache *s, const void *object, size_t size, gfp_t flags) {}
static inline void kmsan_slab_alloc(
	struct kmem_cache *s, void *object, gfp_t flags) {}
// TODO(glider): make it return void.
static inline bool kmsan_slab_free(struct kmem_cache *s, void *object)
{
	return false;
}

static inline void kmsan_slab_setup_object(
	struct kmem_cache *s, void *object) {}
static inline void kmsan_post_alloc_hook(struct kmem_cache *s, gfp_t flags,
	size_t size, void *object) {}
static inline void kmsan_wipe_params_shadow_origin(void) {}
static inline void kmsan_record_future_shadow_range(u64 start, u64 end) {}

static inline void kmsan_vprintk_func(const char *fmt, va_list args) {}

static inline void kmsan_vmap(struct vm_struct *area,
		struct page **pages, unsigned int count, unsigned long flags,
		pgprot_t prot, void *caller) {}
static inline void kmsan_vunmap(const void *addr, struct vm_struct *area, int deallocate_pages) {}
static inline bool kmsan_vmalloc_area_node(struct vm_struct *area, gfp_t alloc_flags, gfp_t nested_gfp, gfp_t highmem_mask, pgprot_t prot, int node)
{
	return true;
}

static inline void kmsan_softirq_enter(void) {}
static inline void kmsan_softirq_exit(void) {}


#endif

#endif /* LINUX_KMSAN_H */
